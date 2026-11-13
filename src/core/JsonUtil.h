#pragma once

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QString>

namespace md {

// 解析内存中的 JSON 文本。失败时返回 false 并填充 *err。
inline bool parseJsonBytes(const QByteArray &data, QJsonDocument *doc, QString *err,
                           const QString &what = QString())
{
    QJsonParseError pe{};
    *doc = QJsonDocument::fromJson(data, &pe);
    if (pe.error != QJsonParseError::NoError) {
        if (err) {
            *err = QStringLiteral("%1 JSON 解析失败（偏移 %2）：%3")
                       .arg(what.isEmpty() ? QStringLiteral("文件") : what)
                       .arg(pe.offset)
                       .arg(pe.errorString());
        }
        return false;
    }
    return true;
}

// 读取并解析本地 JSON 文件。失败时返回 false 并填充 *err。
inline bool loadJsonFile(const QString &path, QJsonDocument *doc, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = QStringLiteral("无法打开 %1：%2").arg(path, f.errorString());
        return false;
    }
    return parseJsonBytes(f.readAll(), doc, err, path);
}

} // namespace md
