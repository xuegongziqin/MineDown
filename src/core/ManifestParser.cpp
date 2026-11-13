#include "core/ManifestParser.h"

#include "core/JsonUtil.h"

#include <QJsonArray>
#include <QJsonObject>

namespace md {

const VersionEntry *VersionManifest::find(const QString &id) const
{
    for (const VersionEntry &v : versions) {
        if (v.id == id)
            return &v;
    }
    return nullptr;
}

bool parseVersionManifest(const QByteArray &json, VersionManifest *out, QString *err)
{
    QJsonDocument doc;
    if (!parseJsonBytes(json, &doc, err, QStringLiteral("版本清单")))
        return false;

    const QJsonObject root = doc.object();
    const QJsonObject latest = root.value(QStringLiteral("latest")).toObject();

    VersionManifest manifest;
    manifest.latestRelease = latest.value(QStringLiteral("release")).toString();
    manifest.latestSnapshot = latest.value(QStringLiteral("snapshot")).toString();

    const QJsonArray versions = root.value(QStringLiteral("versions")).toArray();
    if (versions.isEmpty()) {
        if (err)
            *err = QStringLiteral("版本清单里没有任何版本");
        return false;
    }

    manifest.versions.reserve(versions.size());
    for (const QJsonValue &v : versions) {
        const QJsonObject o = v.toObject();
        VersionEntry e;
        e.id = o.value(QStringLiteral("id")).toString();
        e.type = o.value(QStringLiteral("type")).toString();
        e.url = o.value(QStringLiteral("url")).toString();
        e.sha1 = o.value(QStringLiteral("sha1")).toString();
        if (o.contains(QStringLiteral("size")))
            e.size = static_cast<qint64>(o.value(QStringLiteral("size")).toDouble());
        e.releaseTime = o.value(QStringLiteral("releaseTime")).toString();
        if (e.id.isEmpty() || e.url.isEmpty())
            continue;
        manifest.versions.push_back(e);
    }

    if (manifest.versions.isEmpty()) {
        if (err)
            *err = QStringLiteral("版本清单里没有可用条目");
        return false;
    }

    *out = manifest;
    return true;
}

} // namespace md
