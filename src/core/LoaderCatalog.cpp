#include "core/LoaderCatalog.h"

#include "core/HttpFetcher.h"
#include "core/Mirror.h"
#include "core/Settings.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QXmlStreamReader>

#include <algorithm>

namespace md {
namespace {

const QString kForgeMetadata =
    QStringLiteral("https://maven.minecraftforge.net/net/minecraftforge/forge/maven-metadata.xml");
const QString kNeoForgeMetadata =
    QStringLiteral("https://maven.neoforged.net/releases/net/neoforged/neoforge/maven-metadata.xml");
const QString kNeoForgeLegacyMetadata =
    QStringLiteral("https://maven.neoforged.net/releases/net/neoforged/forge/maven-metadata.xml");

// 判断某个 NeoForge 版本是不是给 mcVersion 用的。前后有两代写法：
//   1.21.x 时代：加载器版本 "21.5.75" 对应原版 "1.21.5"（第一段是原版的次版本号）
//   26.x 时代： 加载器版本 "26.3.0.86" 对应原版 "26.3"，
//                "26.1.2.123" 对应原版 "26.1.2"（原版版本后面跟 patch 占位/build 号）
// 所以先按新写法比「去掉最后一段（build 号）后的前缀」，再退回老写法。
bool neoForgeIsForMc(const QString &loaderVersion, const QString &mcVersion)
{
    const QStringList parts = loaderVersion.split(u'.');
    if (parts.size() < 3)
        return false;

    // 新写法：去掉 build 号后正好等于原版版本，或者多一个 .0 占位（26.3.0.86 -> 26.3）
    const QString withoutBuild = parts.mid(0, parts.size() - 1).join(u'.');
    if (withoutBuild == mcVersion)
        return true;
    if (withoutBuild.endsWith(QStringLiteral(".0"))
        && withoutBuild.left(withoutBuild.size() - 2) == mcVersion)
        return true;

    // 老写法：1.21.5 对应 21.5.75
    const QString legacy = QStringLiteral("1.") + parts.at(0) + u'.' + parts.at(1);
    return legacy == mcVersion;
}

QStringList parseMetadataVersions(const QByteArray &xml)
{
    QStringList out;
    QXmlStreamReader r(xml);
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement() && r.name() == QLatin1String("version")) {
            const QString v = r.readElementText().trimmed();
            if (!v.isEmpty())
                out.append(v);
        }
    }
    return out;
}

} // namespace

QString loaderKindKey(LoaderKind kind)
{
    switch (kind) {
    case LoaderKind::Fabric:
        return QStringLiteral("fabric");
    case LoaderKind::Forge:
        return QStringLiteral("forge");
    case LoaderKind::NeoForge:
        return QStringLiteral("neoforge");
    }
    return QStringLiteral("fabric");
}

QString loaderKindName(LoaderKind kind)
{
    switch (kind) {
    case LoaderKind::Fabric:
        return QStringLiteral("Fabric");
    case LoaderKind::Forge:
        return QStringLiteral("Forge");
    case LoaderKind::NeoForge:
        return QStringLiteral("NeoForge");
    }
    return QString();
}

bool loaderKindFromKey(const QString &key, LoaderKind *out)
{
    if (!out)
        return false;
    const QString k = key.trimmed().toLower();
    if (k == QLatin1String("fabric")) {
        *out = LoaderKind::Fabric;
        return true;
    }
    if (k == QLatin1String("forge")) {
        *out = LoaderKind::Forge;
        return true;
    }
    if (k == QLatin1String("neoforge")) {
        *out = LoaderKind::NeoForge;
        return true;
    }
    return false;
}

QString LoaderVersion::label() const
{
    if (kind == LoaderKind::Fabric)
        return stable ? loaderVersion : loaderVersion + QStringLiteral("（测试版）");
    return loaderVersion;
}

int compareVersionStrings(const QString &a, const QString &b)
{
    auto split = [](const QString &s) {
        QStringList out;
        QString cur;
        for (const QChar ch : s) {
            if (ch.isLetterOrNumber()) {
                cur += ch;
            } else if (!cur.isEmpty()) {
                out.append(cur);
                cur.clear();
            }
        }
        if (!cur.isEmpty())
            out.append(cur);
        return out;
    };

    const QStringList pa = split(a);
    const QStringList pb = split(b);
    const int n = qMin(pa.size(), pb.size());
    for (int i = 0; i < n; ++i) {
        bool oka = false;
        bool okb = false;
        const qlonglong na = pa.at(i).toLongLong(&oka);
        const qlonglong nb = pb.at(i).toLongLong(&okb);
        if (oka && okb) {
            if (na != nb)
                return na < nb ? -1 : 1;
        } else {
            const int cmp = QString::compare(pa.at(i), pb.at(i), Qt::CaseInsensitive);
            if (cmp != 0)
                return cmp;
        }
    }
    if (pa.size() == pb.size())
        return 0;
    return pa.size() < pb.size() ? -1 : 1;
}

LoaderCatalog::LoaderCatalog(QObject *parent) : QObject(parent)
{
    m_fetcher = new HttpFetcher(this);
    connect(m_fetcher, &HttpFetcher::done, this,
            [this](const QByteArray &data, const QUrl &) { onData(data); });
    connect(m_fetcher, &HttpFetcher::error, this, &LoaderCatalog::onError);
}

QString LoaderCatalog::mirrorBase() const
{
    return Settings::instance().mirrorBase();
}

void LoaderCatalog::abort()
{
    m_fetcher->abort();
}

void LoaderCatalog::fetch(LoaderKind kind, const QString &mcVersion)
{
    m_kind = kind;
    m_mc = mcVersion.trimmed();
    m_neoTriedLegacy = false;
    m_neoTriedNew = false;
    m_pendingError.clear();

    if (m_mc.isEmpty()) {
        QTimer::singleShot(0, this, [this]() { emit failed(QStringLiteral("还没有选择原版版本")); });
        return;
    }

    QString official;
    switch (kind) {
    case LoaderKind::Fabric:
        // Fabric 的 Meta API 没有镜像，applyMirror 里会强制走官方。
        official = QStringLiteral("https://meta.fabricmc.net/v2/versions/loader/%1/").arg(m_mc);
        break;
    case LoaderKind::Forge:
        official = kForgeMetadata;
        break;
    case LoaderKind::NeoForge: {
        // 1.20.2 起 NeoForge 才独立发版；更早的版本挂在 net.neoforged:forge 下。
        const bool legacy = compareVersionStrings(m_mc, QStringLiteral("1.20.2")) < 0;
        official = legacy ? kNeoForgeLegacyMetadata : kNeoForgeMetadata;
        m_neoTriedLegacy = legacy;
        m_neoTriedNew = !legacy;
        break;
    }
    }

    const QString url = applyMirror(official, mirrorBase());
    m_fetcher->get(QUrl(url), QString(), QUrl(fallbackForUrl(url)));
}

void LoaderCatalog::onData(const QByteArray &data)
{
    if (m_kind == LoaderKind::Fabric) {
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isArray()) {
            emit failed(QStringLiteral("Fabric 版本列表格式不对（不是 JSON 数组）"));
            return;
        }
        QVector<LoaderVersion> list;
        const QJsonArray arr = doc.array();
        list.reserve(arr.size());
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            const QJsonObject loader = o.value(QStringLiteral("loader")).toObject();
            const QString ver = loader.value(QStringLiteral("version")).toString();
            if (ver.isEmpty())
                continue;

            LoaderVersion lv;
            lv.kind = LoaderKind::Fabric;
            lv.mcVersion = m_mc;
            lv.loaderVersion = ver;
            lv.versionId = QStringLiteral("fabric-loader-%1-%2").arg(ver, m_mc);
            lv.stable = loader.value(QStringLiteral("stable")).toBool(true);
            lv.raw = o;
            list.append(lv);
        }
        if (list.isEmpty()) {
            emit failed(QStringLiteral("Fabric 没有为 %1 提供加载器版本").arg(m_mc));
            return;
        }
        emit ready(list);
        return;
    }

    parseForgeXml(data, m_neoTriedLegacy);
}

void LoaderCatalog::parseForgeXml(const QByteArray &data, bool neoForgeLegacy)
{
    QXmlStreamReader check(data);
    bool sawVersions = false;
    while (!check.atEnd()) {
        check.readNext();
        if (check.isStartElement() && check.name() == QLatin1String("versions")) {
            sawVersions = true;
            break;
        }
    }
    if (check.hasError() || !sawVersions) {
        emit failed(QStringLiteral("版本列表解析失败：不是有效的 maven-metadata.xml"));
        return;
    }

    const QStringList all = parseMetadataVersions(data);
    QVector<LoaderVersion> list;

    for (const QString &raw : all) {
        LoaderVersion lv;
        lv.kind = m_kind;
        lv.mcVersion = m_mc;

        if (m_kind == LoaderKind::Forge) {
            // 形如 "1.20.1-47.4.29"
            const QString prefix = m_mc + u'-';
            if (!raw.startsWith(prefix))
                continue;
            lv.loaderVersion = raw.mid(prefix.size());
            lv.versionId = m_mc + QStringLiteral("-forge-") + lv.loaderVersion;
        } else if (neoForgeLegacy) {
            // 旧坐标：形如 "1.20.1-47.1.106"
            const QString prefix = m_mc + u'-';
            if (!raw.startsWith(prefix))
                continue;
            lv.loaderVersion = raw.mid(prefix.size());
            lv.versionId = m_mc + QStringLiteral("-forge-") + lv.loaderVersion;
        } else {
            // 新坐标：版本号里带着原版版本（21.5.75 / 26.3.0.86），需要还原出对应的原版版本
            if (!neoForgeIsForMc(raw, m_mc))
                continue;
            lv.loaderVersion = raw;
            lv.versionId = QStringLiteral("neoforge-") + raw;
        }
        list.append(lv);
    }

    if (list.isEmpty()) {
        // NeoForge 的坐标在 1.20.2 前后换过一次，某一边为空时试另一边。
        if (m_kind == LoaderKind::NeoForge && !neoForgeLegacy && !m_neoTriedLegacy) {
            m_neoTriedLegacy = true;
            const QString url = applyMirror(kNeoForgeLegacyMetadata, mirrorBase());
            m_fetcher->get(QUrl(url), QString(), QUrl(fallbackForUrl(url)));
            return;
        }
        if (m_kind == LoaderKind::NeoForge && neoForgeLegacy && !m_neoTriedNew) {
            m_neoTriedNew = true;
            const QString url = applyMirror(kNeoForgeMetadata, mirrorBase());
            m_fetcher->get(QUrl(url), QString(), QUrl(fallbackForUrl(url)));
            return;
        }
        emit failed(QStringLiteral("%1 没有适用于 %2 的版本").arg(loaderKindName(m_kind), m_mc));
        return;
    }

    std::sort(list.begin(), list.end(),
              [](const LoaderVersion &a, const LoaderVersion &b) {
                  return compareVersionStrings(a.loaderVersion, b.loaderVersion) > 0;
              });
    emit ready(list);
}

void LoaderCatalog::onError(const QString &message)
{
    // NeoForge 两套坐标：一套取不到时再试另一套，别直接失败。
    if (m_kind == LoaderKind::NeoForge) {
        if (!m_neoTriedLegacy) {
            m_neoTriedLegacy = true;
            const QString url = applyMirror(kNeoForgeLegacyMetadata, mirrorBase());
            m_fetcher->get(QUrl(url), QString(), QUrl(fallbackForUrl(url)));
            return;
        }
        if (!m_neoTriedNew && m_neoTriedLegacy) {
            m_neoTriedNew = true;
            const QString url = applyMirror(kNeoForgeMetadata, mirrorBase());
            m_fetcher->get(QUrl(url), QString(), QUrl(fallbackForUrl(url)));
            return;
        }
    }
    emit failed(message);
}

} // namespace md
