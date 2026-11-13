#include "core/AssetIndexParser.h"

#include "core/JsonUtil.h"

#include <QJsonObject>

namespace md {

QHash<QString, qint64> AssetIndex::uniqueObjects() const
{
    QHash<QString, qint64> uniq;
    uniq.reserve(objects.size());
    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        if (!uniq.contains(it.value().hash))
            uniq.insert(it.value().hash, it.value().size);
    }
    return uniq;
}

qint64 AssetIndex::totalBytes() const
{
    qint64 sum = 0;
    const QHash<QString, qint64> uniq = uniqueObjects();
    for (auto it = uniq.cbegin(); it != uniq.cend(); ++it) {
        if (it.value() > 0)
            sum += it.value();
    }
    return sum;
}

bool parseAssetIndex(const QByteArray &json, AssetIndex *out, QString *err)
{
    QJsonDocument doc;
    if (!parseJsonBytes(json, &doc, err, QStringLiteral("资源索引")))
        return false;

    const QJsonObject root = doc.object();
    const QJsonObject objects = root.value(QStringLiteral("objects")).toObject();
    if (objects.isEmpty()) {
        if (err)
            *err = QStringLiteral("资源索引里没有 objects");
        return false;
    }

    AssetIndex index;
    index.mapToResources = root.value(QStringLiteral("map_to_resources")).toBool(false);
    index.virtualDir = root.value(QStringLiteral("virtual")).toString();
    index.objects.reserve(objects.size());

    for (auto it = objects.begin(); it != objects.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        AssetObject obj;
        obj.hash = o.value(QStringLiteral("hash")).toString();
        if (obj.hash.size() != 40)
            continue;
        if (o.contains(QStringLiteral("size")))
            obj.size = static_cast<qint64>(o.value(QStringLiteral("size")).toDouble());
        index.objects.insert(it.key(), obj);
    }

    if (index.objects.isEmpty()) {
        if (err)
            *err = QStringLiteral("资源索引里没有有效对象");
        return false;
    }

    *out = index;
    return true;
}

} // namespace md
