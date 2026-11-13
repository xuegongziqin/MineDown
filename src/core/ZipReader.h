#pragma once

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace md {

// 极简 ZIP 读取器（stored + deflate），用来读加载器安装器和 processor 的 jar。
//
// 为什么自己写：环境里只有 zlib，没有 quazip；而我们只需要「列条目 + 取一个条目 + 全量解压」
// 三件事，ZIP 的中央目录解析也就百来行，比引一个第三方库省事。
// 不支持：zip64、加密、多卷（加载器安装器都远小于 4 GB）。
class ZipReader
{
public:
    struct Entry {
        QString name;
        quint16 method = 0;   // 0 = stored, 8 = deflate
        quint32 crc = 0;
        qint64 compSize = 0;
        qint64 uncompSize = 0;
        qint64 localOffset = 0;
    };

    // 打开并读完中央目录。失败返回 false，err 里是中文原因。
    bool open(const QString &path, QString *err);
    void close();
    bool isOpen() const { return m_file.isOpen(); }
    QString path() const { return m_file.fileName(); }

    QStringList entries() const;
    bool contains(const QString &name) const { return m_index.contains(name); }

    // 取出单个条目（原始文件名，区分大小写）。
    bool read(const QString &name, QByteArray *out, QString *err) const;

    // 全部解压到 destDir；filter 返回 false 的条目跳过。
    bool extractAll(const QString &destDir, QString *err,
                    const std::function<bool(const QString &)> &filter = {}) const;

    // 只解压 prefix 开头的条目到 destDir，并去掉该前缀（例如 "maven/"）。
    bool extractUnder(const QString &prefix, const QString &destDir, int *count,
                      QString *err) const;

private:
    bool loadDirectory(QString *err);
    bool readEntry(int index, QByteArray *out, QString *err) const;

    QFile m_file;
    QVector<Entry> m_entries;
    QHash<QString, int> m_index;
};

// 把字节写成文件（自动建父目录）。失败时 err 带路径。
bool writeFileBytes(const QString &path, const QByteArray &data, QString *err);

} // namespace md
