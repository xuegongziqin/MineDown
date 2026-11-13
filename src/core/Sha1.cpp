#include "core/Sha1.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

namespace md {

QString sha1File(const QString &path, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = QStringLiteral("无法打开文件: %1").arg(f.errorString());
        return QString();
    }

    QCryptographicHash hash(QCryptographicHash::Sha1);
    constexpr qint64 kChunk = 1 << 20; // 1 MiB
    while (!f.atEnd()) {
        const QByteArray chunk = f.read(kChunk);
        if (chunk.isEmpty()) {
            if (f.error() != QFileDevice::NoError) {
                if (err)
                    *err = QStringLiteral("读取失败: %1").arg(f.errorString());
                return QString();
            }
            break;
        }
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool verifyFile(const QString &path, const QString &sha1, qint64 expectedSize, QString *err)
{
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        if (err)
            *err = QStringLiteral("文件不存在");
        return false;
    }
    // 先比大小，能省掉大量无谓的哈希计算。
    if (expectedSize >= 0 && fi.size() != expectedSize) {
        if (err)
            *err = QStringLiteral("大小不符（本地 %1 / 期望 %2）").arg(fi.size()).arg(expectedSize);
        return false;
    }
    if (sha1.size() != 40)
        return true; // 没有 SHA1 可校验，大小对上就算通过

    const QString actual = sha1File(path, err);
    if (actual.isEmpty())
        return false;
    if (actual.compare(sha1, Qt::CaseInsensitive) != 0) {
        if (err)
            *err = QStringLiteral("SHA1 不符（本地 %1 / 期望 %2）").arg(actual, sha1);
        return false;
    }
    return true;
}

} // namespace md
