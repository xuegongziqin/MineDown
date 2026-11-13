#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

namespace md {

struct AssetObject {
    QString hash;
    qint64 size = -1;
};

// assets/indexes/<id>.json
struct AssetIndex {
    QHash<QString, AssetObject> objects;   // 逻辑名 -> 对象
    bool mapToResources = false;           // 仅 1.6 及更早的老索引
    QString virtualDir;                    // 老索引的 virtual 目录名

    // 去重后的对象（同一 hash 可能被多个逻辑名引用）
    QHash<QString, qint64> uniqueObjects() const;
    qint64 totalBytes() const;
};

bool parseAssetIndex(const QByteArray &json, AssetIndex *out, QString *err);

} // namespace md
