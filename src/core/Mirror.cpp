#include "core/Mirror.h"

#include <QUrl>

namespace md {

namespace {

const QString kOfficialMeta = QStringLiteral("https://piston-meta.mojang.com");
const QString kOfficialData = QStringLiteral("https://piston-data.mojang.com");
const QString kOfficialLibs = QStringLiteral("https://libraries.minecraft.net");
const QString kOfficialRes = QStringLiteral("https://resources.download.minecraft.net");

// 加载器（Forge / NeoForge / Fabric）自己的 Maven 仓库。
const QString kForgeMaven = QStringLiteral("https://maven.minecraftforge.net");
const QString kNeoForgeMaven = QStringLiteral("https://maven.neoforged.net/releases");
const QString kFabricMaven = QStringLiteral("https://maven.fabricmc.net");
const QString kFabricMeta = QStringLiteral("https://meta.fabricmc.net");

// 地址里的资源类型 —— 决定镜像侧要插什么路径前缀。
enum class Kind {
    Unknown,
    Meta,       // 版本清单 / 版本 JSON / 资源索引 JSON
    Objects,    // piston-data 的 v1/objects（客户端 jar、日志配置…）
    Libraries,  // 游戏依赖库（libraries.minecraft.net）
    Asset,      // 资源对象（无扩展名，按 SHA1 命名）
    LoaderMaven,// 加载器仓库（Forge / NeoForge / Fabric）
    FabricMeta, // Fabric 的 Meta API（没有镜像，永远走官方）
};

struct Classified {
    Kind kind = Kind::Unknown;
    QString rest; // 去掉前缀后剩下的路径（不含开头斜杠）
};

QString normalizeBase(QString base)
{
    base = base.trimmed();
    while (base.size() > 1 && base.endsWith(u'/'))
        base.chop(1);
    if (!base.isEmpty() && !base.contains(QLatin1String("://")))
        base.prepend(QLatin1String("https://"));
    return base;
}

bool splitUrl(const QString &url, QString *host, QString *path)
{
    const QUrl u(url);
    if (!u.isValid() || u.host().isEmpty())
        return false;
    *host = u.host().toLower();
    *path = u.path();
    return true;
}

bool isKnownMirrorHost(const QString &host)
{
    static const QVector<MirrorInfo> list = builtinMirrors();
    for (const MirrorInfo &m : list) {
        if (m.base.isEmpty())
            continue;
        if (QUrl(m.base).host().toLower() == host)
            return true;
    }
    return false;
}

// 按 Maven 仓库里的 group 路径判断这条地址属于哪个加载器仓库。
// 只认三家的 group，避免把 MC 自己的库（at/yawk/... 之类）误判成加载器库。
QString loaderRepoForPath(const QString &rest)
{
    if (rest.startsWith(QLatin1String("net/minecraftforge/")))
        return kForgeMaven;
    if (rest.startsWith(QLatin1String("net/neoforged/")))
        return kNeoForgeMaven;
    if (rest.startsWith(QLatin1String("net/fabricmc/")))
        return kFabricMaven;
    return {};
}

Classified classify(const QString &host, const QString &path)
{
    QString rest = path;
    while (rest.startsWith(u'/'))
        rest.remove(0, 1);

    if (host == QLatin1String("piston-meta.mojang.com")
        || host == QLatin1String("launchermeta.mojang.com")
        || host == QLatin1String("launcher.mojang.com")) {
        return {Kind::Meta, rest};
    }
    if (host == QLatin1String("piston-data.mojang.com")) {
        if (rest.startsWith(QLatin1String("v1/objects/")))
            return {Kind::Objects, rest.mid(11)};
        return {Kind::Meta, rest};
    }
    if (host == QLatin1String("libraries.minecraft.net"))
        return {Kind::Libraries, rest};
    if (host == QLatin1String("resources.download.minecraft.net"))
        return {Kind::Asset, rest};

    // 加载器仓库本体。NeoForge 的仓库根带一层 releases/，镜像侧没有这一层。
    if (host == QLatin1String("maven.minecraftforge.net"))
        return {Kind::LoaderMaven, rest};
    if (host == QLatin1String("maven.neoforged.net")) {
        if (rest.startsWith(QLatin1String("releases/")))
            rest.remove(0, 9);
        return {Kind::LoaderMaven, rest};
    }
    if (host == QLatin1String("maven.fabricmc.net"))
        return {Kind::LoaderMaven, rest};
    if (host == QLatin1String("meta.fabricmc.net"))
        return {Kind::FabricMeta, rest};

    // 已知镜像主机：按路径前缀反推资源类型（这样换镜像时也能正确重写）。
    if (!isKnownMirrorHost(host))
        return {};
    if (rest.startsWith(QLatin1String("v1/objects/")))
        return {Kind::Objects, rest.mid(11)};
    if (rest.startsWith(QLatin1String("libraries/")))
        return {Kind::Libraries, rest.mid(10)};
    if (rest.startsWith(QLatin1String("maven/"))) {
        const QString inner = rest.mid(6);
        // hkmc 把 MC 的库也放在 /maven/ 下，所以先按 group 区分是谁的库。
        if (!loaderRepoForPath(inner).isEmpty())
            return {Kind::LoaderMaven, inner};
        return {Kind::Libraries, inner};
    }
    if (rest.startsWith(QLatin1String("assets/")))
        return {Kind::Asset, rest.mid(7)};
    if (rest.startsWith(QLatin1String("v1/packages/"))
        || rest.startsWith(QLatin1String("mc/game/")))
        return {Kind::Meta, rest};
    return {};
}

QString buildUrl(const QString &base0, MirrorLayout layout, Kind kind, const QString &rest)
{
    const QString base = normalizeBase(base0);
    if (base.isEmpty())
        return {};
    switch (kind) {
    case Kind::Meta:
    case Kind::FabricMeta:
        return base + u'/' + rest;
    case Kind::Objects:
        return base + QLatin1String("/v1/objects/") + rest;
    case Kind::Libraries: {
        QString prefix;
        if (layout == MirrorLayout::Bmclapi)
            prefix = QStringLiteral("libraries/");
        else if (layout == MirrorLayout::Hkmc)
            prefix = QStringLiteral("maven/");
        return base + u'/' + prefix + rest;
    }
    case Kind::LoaderMaven: {
        // BMCLAPI / hkmc 都把加载器仓库挂在 /maven/ 下；只换域名的自定义镜像保留路径。
        const QString prefix =
            layout == MirrorLayout::HostOnly ? QString() : QStringLiteral("maven/");
        return base + u'/' + prefix + rest;
    }
    case Kind::Asset: {
        const QString prefix = layout == MirrorLayout::HostOnly ? QString()
                                                                : QStringLiteral("assets/");
        return base + u'/' + prefix + rest;
    }
    default:
        return {};
    }
}

} // namespace

QVector<MirrorInfo> builtinMirrors()
{
    return {
        {MirrorId::Official, QStringLiteral("official"), QStringLiteral("官方源（Mojang）"),
         kOfficialMeta, MirrorLayout::HostOnly},
        {MirrorId::Bmclapi, QStringLiteral("bmclapi"), QStringLiteral("BMCLAPI（国内加速）"),
         QStringLiteral("https://bmclapi2.bangbang93.com"), MirrorLayout::Bmclapi},
        {MirrorId::HkmcMain, QStringLiteral("hkmc"), QStringLiteral("MC 光速主线路（HKMC）"),
         QStringLiteral("https://bsmirror.hkmc.online"), MirrorLayout::Hkmc},
        {MirrorId::HkmcEo, QStringLiteral("hkmc-eo"), QStringLiteral("MC 光速 EO 线路（HKMC）"),
         QStringLiteral("https://mirror.hkmc.online"), MirrorLayout::Hkmc},
        {MirrorId::Custom, QStringLiteral("custom"), QStringLiteral("自定义镜像"),
         QString(), MirrorLayout::HostOnly},
    };
}

const MirrorInfo *findMirror(const QString &key)
{
    static const QVector<MirrorInfo> list = builtinMirrors();
    for (const MirrorInfo &m : list) {
        if (m.key == key)
            return &m;
    }
    return nullptr;
}

MirrorLayout layoutForBase(const QString &base)
{
    const QString h = QUrl(normalizeBase(base)).host().toLower();
    if (h.contains(QLatin1String("bangbang93")) || h.contains(QLatin1String("bmclapi")))
        return MirrorLayout::Bmclapi;
    if (h.contains(QLatin1String("hkmc")))
        return MirrorLayout::Hkmc;
    return MirrorLayout::HostOnly;
}

bool isOfficialHost(const QString &url)
{
    QString host, path;
    if (!splitUrl(url, &host, &path))
        return false;
    return host == QLatin1String("piston-meta.mojang.com")
        || host == QLatin1String("piston-data.mojang.com")
        || host == QLatin1String("libraries.minecraft.net")
        || host == QLatin1String("resources.download.minecraft.net")
        || host == QLatin1String("launchermeta.mojang.com")
        || host == QLatin1String("launcher.mojang.com")
        || host == QLatin1String("maven.minecraftforge.net")
        || host == QLatin1String("maven.neoforged.net")
        || host == QLatin1String("maven.fabricmc.net")
        || host == QLatin1String("meta.fabricmc.net");
}

QString officialUrlFor(const QString &url)
{
    QString host, path;
    if (!splitUrl(url, &host, &path))
        return url;
    const Classified c = classify(host, path);
    switch (c.kind) {
    case Kind::Meta:
        return buildUrl(kOfficialMeta, MirrorLayout::HostOnly, c.kind, c.rest);
    case Kind::Objects:
        return buildUrl(kOfficialData, MirrorLayout::HostOnly, c.kind, c.rest);
    case Kind::Libraries:
        return buildUrl(kOfficialLibs, MirrorLayout::HostOnly, c.kind, c.rest);
    case Kind::Asset:
        return buildUrl(kOfficialRes, MirrorLayout::HostOnly, c.kind, c.rest);
    case Kind::LoaderMaven: {
        const QString repo = loaderRepoForPath(c.rest);
        if (repo.isEmpty())
            return url;
        return buildUrl(repo, MirrorLayout::HostOnly, c.kind, c.rest);
    }
    case Kind::FabricMeta:
        return buildUrl(kFabricMeta, MirrorLayout::HostOnly, c.kind, c.rest);
    default:
        return url;
    }
}

QString fallbackForUrl(const QString &url)
{
    const QString official = officialUrlFor(url);
    return official == url ? QString() : official;
}

QString applyMirror(const QString &url, const QString &base)
{    QString host, path;
    if (!splitUrl(url, &host, &path))
        return url;

    const Classified c = classify(host, path);
    if (c.kind == Kind::Unknown)
        return url; // 自定义 Maven 仓库等：绝不动

    // Fabric 的 Meta API 没有镜像，只能走官方（实测 BMCLAPI 的 /fabric-meta/ 是 404）。
    if (c.kind == Kind::FabricMeta)
        return officialUrlFor(url);

    if (normalizeBase(base).isEmpty())
        return officialUrlFor(url);

    const QString rewritten = buildUrl(base, layoutForBase(base), c.kind, c.rest);
    return rewritten.isEmpty() ? url : rewritten;
}

QString forgeMavenBase()
{
    return kForgeMaven;
}

QString neoForgeMavenBase()
{
    return kNeoForgeMaven;
}

QString fabricMavenBase()
{
    return kFabricMaven;
}

QString fabricMetaBase()
{
    return kFabricMeta;
}

} // namespace md
