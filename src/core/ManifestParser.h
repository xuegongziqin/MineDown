#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace md {

struct VersionEntry {
    QString id;
    QString type;        // release / snapshot / old_beta / old_alpha
    QString url;
    QString sha1;
    qint64 size = -1;
    QString releaseTime; // ISO8601

    bool isRelease() const { return type == QStringLiteral("release"); }
};

struct VersionManifest {
    QString latestRelease;
    QString latestSnapshot;
    QVector<VersionEntry> versions;   // 保持 JSON 中的原始顺序（新版本在前）

    const VersionEntry *find(const QString &id) const;
};

bool parseVersionManifest(const QByteArray &json, VersionManifest *out, QString *err);

} // namespace md
