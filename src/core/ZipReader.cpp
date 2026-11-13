#include "core/ZipReader.h"

#include <QDir>
#include <QFileInfo>

#include <zlib.h>

namespace md {
namespace {

constexpr quint32 kSigLocal = 0x04034b50;
constexpr quint32 kSigCentral = 0x02014b50;
constexpr quint32 kSigEocd = 0x06054b50;

quint16 readU16(const QByteArray &buf, int off)
{
    return static_cast<quint16>(static_cast<quint8>(buf.at(off)))
        | static_cast<quint16>(static_cast<quint8>(buf.at(off + 1)) << 8);
}

quint32 readU32(const QByteArray &buf, int off)
{
    return static_cast<quint32>(static_cast<quint8>(buf.at(off)))
        | (static_cast<quint32>(static_cast<quint8>(buf.at(off + 1))) << 8)
        | (static_cast<quint32>(static_cast<quint8>(buf.at(off + 2))) << 16)
        | (static_cast<quint32>(static_cast<quint8>(buf.at(off + 3))) << 24);
}

// 目录层级里出现 .. 或绝对路径的条目一律拒绝（防 zip-slip）。
bool safeEntryName(const QString &name)
{
    if (name.isEmpty())
        return false;
    if (name.startsWith(u'/') || name.startsWith(u'\\') || name.contains(u":"))
        return false;
    const QStringList parts = name.split(u'/', Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        if (p == QStringLiteral(".."))
            return false;
    }
    return true;
}

QString normalizeDir(const QString &dir)
{
    QString d = dir;
    while (d.endsWith(u'/') || d.endsWith(u'\\'))
        d.chop(1);
    return d;
}

} // namespace

bool writeFileBytes(const QString &path, const QByteArray &data, QString *err)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (err)
            *err = QStringLiteral("无法创建目录：%1").arg(info.absolutePath());
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err)
            *err = QStringLiteral("无法写入 %1：%2").arg(path, f.errorString());
        return false;
    }
    if (f.write(data) != data.size()) {
        if (err)
            *err = QStringLiteral("写入不完整：%1").arg(path);
        return false;
    }
    f.close();
    return true;
}

bool ZipReader::open(const QString &path, QString *err)
{
    close();
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::ReadOnly)) {
        if (err)
            *err = QStringLiteral("无法打开 %1：%2").arg(path, m_file.errorString());
        return false;
    }
    if (!loadDirectory(err)) {
        close();
        return false;
    }
    return true;
}

void ZipReader::close()
{
    m_entries.clear();
    m_index.clear();
    if (m_file.isOpen())
        m_file.close();
}

QStringList ZipReader::entries() const
{
    QStringList list;
    list.reserve(m_entries.size());
    for (const Entry &e : m_entries)
        list.append(e.name);
    return list;
}

bool ZipReader::loadDirectory(QString *err)
{
    const qint64 size = m_file.size();
    if (size < 22) {
        if (err)
            *err = QStringLiteral("文件太小，不是有效的 ZIP：%1").arg(m_file.fileName());
        return false;
    }

    // EOCD 在文件末尾，最多带 65535 字节注释；从后往前找签名。
    const qint64 tailLen = qMin<qint64>(size, 66000);
    const qint64 tailOff = size - tailLen;
    m_file.seek(tailOff);
    const QByteArray tail = m_file.read(tailLen);

    int eocd = -1;
    for (int i = tail.size() - 22; i >= 0; --i) {
        if (readU32(tail, i) == kSigEocd) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        if (err)
            *err = QStringLiteral("找不到 ZIP 结尾记录（文件可能损坏或不完整）：%1")
                       .arg(m_file.fileName());
        return false;
    }

    const int total = readU16(tail, eocd + 10);
    const quint32 cdSize = readU32(tail, eocd + 12);
    const quint32 cdOff = readU32(tail, eocd + 16);

    if (cdOff == 0xFFFFFFFFu || cdSize == 0xFFFFFFFFu) {
        if (err)
            *err = QStringLiteral("不支持 zip64 格式：%1").arg(m_file.fileName());
        return false;
    }
    if (static_cast<qint64>(cdOff) + cdSize > size) {
        if (err)
            *err = QStringLiteral("ZIP 中央目录越界（文件可能不完整）：%1").arg(m_file.fileName());
        return false;
    }

    m_file.seek(cdOff);
    QByteArray cd = m_file.read(cdSize);

    int off = 0;
    m_entries.reserve(total);
    for (int i = 0; i < total; ++i) {
        if (off + 46 > cd.size() || readU32(cd, off) != kSigCentral)
            break;
        Entry e;
        e.method = readU16(cd, off + 10);
        e.crc = readU32(cd, off + 16);
        e.compSize = readU32(cd, off + 20);
        e.uncompSize = readU32(cd, off + 24);
        const int nameLen = readU16(cd, off + 28);
        const int extraLen = readU16(cd, off + 30);
        const int commentLen = readU16(cd, off + 32);
        e.localOffset = readU32(cd, off + 42);

        if (off + 46 + nameLen > cd.size())
            break;
        e.name = QString::fromUtf8(cd.constData() + off + 46, nameLen);
        e.name.replace(u'\\', u'/');

        off += 46 + nameLen + extraLen + commentLen;

        if (e.name.isEmpty() || e.name.endsWith(u'/'))
            continue;
        if (!m_index.contains(e.name)) {
            m_index.insert(e.name, m_entries.size());
            m_entries.append(e);
        }
    }

    if (m_entries.isEmpty()) {
        if (err)
            *err = QStringLiteral("ZIP 里没有可读取的文件：%1").arg(m_file.fileName());
        return false;
    }
    return true;
}

bool ZipReader::readEntry(int index, QByteArray *out, QString *err) const
{
    if (index < 0 || index >= m_entries.size() || !m_file.isOpen()) {
        if (err)
            *err = QStringLiteral("ZIP 未打开或条目下标越界");
        return false;
    }
    const Entry &e = m_entries.at(index);

    QFile &file = const_cast<QFile &>(m_file);
    if (!file.seek(e.localOffset)) {
        if (err)
            *err = QStringLiteral("读取失败：%1").arg(e.name);
        return false;
    }
    const QByteArray head = file.read(30);
    if (head.size() < 30 || readU32(head, 0) != kSigLocal) {
        if (err)
            *err = QStringLiteral("本地文件头损坏：%1").arg(e.name);
        return false;
    }
    const int nameLen = readU16(head, 26);
    const int extraLen = readU16(head, 28);
    const qint64 dataOff = e.localOffset + 30 + nameLen + extraLen;

    if (!file.seek(dataOff)) {
        if (err)
            *err = QStringLiteral("读取失败：%1").arg(e.name);
        return false;
    }
    const QByteArray raw = file.read(e.compSize);
    if (raw.size() != e.compSize) {
        if (err)
            *err = QStringLiteral("数据不完整：%1").arg(e.name);
        return false;
    }

    if (e.method == 0) {
        *out = raw;
    } else if (e.method == 8) {
        QByteArray result(static_cast<int>(e.uncompSize), Qt::Uninitialized);
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
            if (err)
                *err = QStringLiteral("zlib 初始化失败：%1").arg(e.name);
            return false;
        }
        zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(raw.constData()));
        zs.avail_in = static_cast<uInt>(raw.size());
        zs.next_out = reinterpret_cast<Bytef *>(result.data());
        zs.avail_out = static_cast<uInt>(result.size());
        const int rc = inflate(&zs, Z_FINISH);
        const uLong produced = zs.total_out;
        inflateEnd(&zs);
        if (rc != Z_STREAM_END) {
            if (err)
                *err = QStringLiteral("解压失败（zlib %1）：%2").arg(rc).arg(e.name);
            return false;
        }
        if (static_cast<qint64>(produced) != e.uncompSize) {
            if (err)
                *err = QStringLiteral("解压后大小不符：%1").arg(e.name);
            return false;
        }
        *out = result;
    } else {
        if (err)
            *err = QStringLiteral("不支持的压缩方式 %1：%2").arg(e.method).arg(e.name);
        return false;
    }

    const uLong crc = crc32(0L, reinterpret_cast<const Bytef *>(out->constData()),
                            static_cast<uInt>(out->size()));
    if (static_cast<quint32>(crc) != e.crc) {
        if (err)
            *err = QStringLiteral("CRC 校验失败：%1").arg(e.name);
        return false;
    }
    return true;
}

bool ZipReader::read(const QString &name, QByteArray *out, QString *err) const
{
    const auto it = m_index.constFind(name);
    if (it == m_index.constEnd()) {
        if (err)
            *err = QStringLiteral("ZIP 里没有 %1").arg(name);
        return false;
    }
    return readEntry(it.value(), out, err);
}

bool ZipReader::extractAll(const QString &destDir, QString *err,
                           const std::function<bool(const QString &)> &filter) const
{
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &e = m_entries.at(i);
        if (filter && !filter(e.name))
            continue;
        if (!safeEntryName(e.name))
            continue;

        QByteArray data;
        QString readErr;
        if (!readEntry(i, &data, &readErr)) {
            if (err)
                *err = readErr;
            return false;
        }
        const QString target = normalizeDir(destDir) + u'/' + e.name;
        if (!writeFileBytes(target, data, err))
            return false;
    }
    return true;
}

bool ZipReader::extractUnder(const QString &prefixIn, const QString &destDir, int *count,
                             QString *err) const
{
    QString prefix = prefixIn;
    if (!prefix.endsWith(u'/'))
        prefix += u'/';

    int n = 0;
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &e = m_entries.at(i);
        if (!e.name.startsWith(prefix))
            continue;

        const QString rel = e.name.mid(prefix.size());
        if (rel.isEmpty() || !safeEntryName(rel))
            continue;

        QByteArray data;
        QString readErr;
        if (!readEntry(i, &data, &readErr)) {
            if (err)
                *err = readErr;
            return false;
        }
        const QString target = normalizeDir(destDir) + u'/' + rel;
        if (!writeFileBytes(target, data, err))
            return false;
        ++n;
    }
    if (count)
        *count = n;
    return true;
}

} // namespace md
