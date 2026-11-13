#include "core/VersionParser.h"

#include "core/JsonUtil.h"
#include "core/Mirror.h"

#include <QJsonArray>
#include <QJsonObject>

namespace md {

namespace {

const char *const kOfficialLibraries = "https://libraries.minecraft.net/";

QString ensureSlash(QString s)
{
    if (!s.isEmpty() && !s.endsWith(u'/'))
        s += u'/';
    return s;
}

qint64 jsonSize(const QJsonObject &o)
{
    if (!o.contains(QStringLiteral("size")))
        return -1;
    return static_cast<qint64>(o.value(QStringLiteral("size")).toDouble());
}

// 从 downloads 里的某个 artifact 对象造 spec；path 字段优先（GUIDE.md 明确要求）。
DownloadSpec specFromArtifact(const QJsonObject &artifact, const QString &coord,
                              const QString &classifier, const QString &fallbackBase = QString())
{
    DownloadSpec spec;
    if (artifact.isEmpty())
        return spec;

    QString rel = artifact.value(QStringLiteral("path")).toString();
    if (rel.isEmpty())
        rel = mavenRelPath(coord, classifier);
    if (rel.isEmpty())
        return spec;

    spec.relPath = QStringLiteral("libraries/") + rel;
    spec.url = artifact.value(QStringLiteral("url")).toString();
    if (spec.url.isEmpty()) {
        const QString base = fallbackBase.isEmpty() ? QString::fromLatin1(kOfficialLibraries)
                                                    : ensureSlash(fallbackBase);
        spec.url = base + rel;
    }
    spec.sha1 = artifact.value(QStringLiteral("sha1")).toString();
    spec.size = jsonSize(artifact);
    return spec;
}

} // namespace

QString mavenRelPath(const QString &coordinate, const QString &classifier)
{
    const QStringList parts = coordinate.split(u':');
    if (parts.size() < 3)
        return QString();

    QString group = parts.at(0);
    const QString artifact = parts.at(1);
    const QString version = parts.at(2);

    QString cls = classifier;
    if (cls.isEmpty() && parts.size() >= 4)
        cls = parts.at(3);

    QString file = artifact + u'-' + version;
    if (!cls.isEmpty())
        file += u'-' + cls;
    file += QStringLiteral(".jar");

    group.replace(u'.', u'/');
    return group + u'/' + artifact + u'/' + version + u'/' + file;
}

bool parseVersionJson(const QByteArray &json, const ParseOptions &opt, VersionMeta *out,
                      QString *err)
{
    QJsonDocument doc;
    if (!parseJsonBytes(json, &doc, err, QStringLiteral("版本 JSON")))
        return false;

    const QJsonObject root = doc.object();
    VersionMeta meta;
    meta.id = root.value(QStringLiteral("id")).toString();
    meta.type = root.value(QStringLiteral("type")).toString();
    meta.mainClass = root.value(QStringLiteral("mainClass")).toString();
    if (meta.id.isEmpty()) {
        if (err)
            *err = QStringLiteral("版本 JSON 缺少 id");
        return false;
    }

    // ---- 客户端主 jar ----
    const QJsonObject downloads = root.value(QStringLiteral("downloads")).toObject();
    const QJsonObject client = downloads.value(QStringLiteral("client")).toObject();
    meta.client.url = client.value(QStringLiteral("url")).toString();
    meta.client.sha1 = client.value(QStringLiteral("sha1")).toString();
    meta.client.size = jsonSize(client);
    meta.client.relPath = QStringLiteral("versions/%1/%1.jar").arg(meta.id);

    // ---- 资源索引 ----
    // 现代版本把 assetIndex 放在根节点，极老的版本放在 downloads 里。
    QJsonObject assetIndex = root.value(QStringLiteral("assetIndex")).toObject();
    if (assetIndex.isEmpty())
        assetIndex = downloads.value(QStringLiteral("assetIndex")).toObject();
    meta.assetIndexId = assetIndex.value(QStringLiteral("id")).toString();
    if (meta.assetIndexId.isEmpty())
        meta.assetIndexId = root.value(QStringLiteral("assets")).toString();
    if (meta.assetIndexId.isEmpty())
        meta.assetIndexId = QStringLiteral("legacy");
    meta.assetIndex.url = assetIndex.value(QStringLiteral("url")).toString();
    meta.assetIndex.sha1 = assetIndex.value(QStringLiteral("sha1")).toString();
    meta.assetIndex.size = jsonSize(assetIndex);
    meta.assetIndex.relPath = QStringLiteral("assets/indexes/%1.json").arg(meta.assetIndexId);
    meta.assetTotalSize = assetIndex.value(QStringLiteral("totalSize")).toInteger(-1);

    // ---- 日志配置 ----
    const QJsonObject loggingClient = root.value(QStringLiteral("logging"))
                                          .toObject()
                                          .value(QStringLiteral("client"))
                                          .toObject();
    const QJsonObject logFile = loggingClient.value(QStringLiteral("file")).toObject();
    const QString logId = logFile.value(QStringLiteral("id")).toString();
    if (!logId.isEmpty()) {
        meta.logConfig.url = logFile.value(QStringLiteral("url")).toString();
        meta.logConfig.sha1 = logFile.value(QStringLiteral("sha1")).toString();
        meta.logConfig.size = jsonSize(logFile);
        meta.logConfig.relPath = QStringLiteral("assets/log_configs/%1").arg(logId);
    }

    // ---- Java 版本（只用于提示，不自动下载）----
    const QJsonObject javaVersion = root.value(QStringLiteral("javaVersion")).toObject();
    if (javaVersion.contains(QStringLiteral("majorVersion")))
        meta.javaMajor = javaVersion.value(QStringLiteral("majorVersion")).toInt();
    meta.javaComponent = javaVersion.value(QStringLiteral("component")).toString();

    // ---- 依赖库 ----
    const QJsonArray libraries = root.value(QStringLiteral("libraries")).toArray();
    meta.libraries.reserve(libraries.size());

    for (const QJsonValue &v : libraries) {
        const QJsonObject lib = v.toObject();
        const QJsonArray rules = lib.value(QStringLiteral("rules")).toArray();
        if (!rulesAllow(rules, opt.os))
            continue;

        const QString coord = lib.value(QStringLiteral("name")).toString();
        const QJsonObject libDownloads = lib.value(QStringLiteral("downloads")).toObject();
        const QString customBase = lib.value(QStringLiteral("url")).toString();

        // 1) 老版本 native：natives 键映射到 classifiers
        if (opt.includeNatives) {
            const QJsonObject natives = lib.value(QStringLiteral("natives")).toObject();
            if (!natives.isEmpty()) {
                QString classifier = natives.value(QStringLiteral("windows")).toString();
                classifier.replace(QStringLiteral("${arch}"),
                                   opt.os.arch == QStringLiteral("x86") ? QStringLiteral("32")
                                                                        : QStringLiteral("64"));
                if (!classifier.isEmpty()) {
                    const QJsonObject classifiers =
                        libDownloads.value(QStringLiteral("classifiers")).toObject();
                    const DownloadSpec spec =
                        specFromArtifact(classifiers.value(classifier).toObject(), coord, classifier,
                                         customBase);
                    if (!spec.relPath.isEmpty()) {
                        LibrarySpec ls;
                        ls.name = coord;
                        ls.relPath = spec.relPath;
                        ls.url = spec.url;
                        ls.sha1 = spec.sha1;
                        ls.size = spec.size;
                        ls.isNative = true;
                        meta.libraries.push_back(ls);
                    }
                }
            }
        }

        // 2) 普通 artifact（含新版本把 native 当普通 artifact 的情况）
        const DownloadSpec spec =
            specFromArtifact(libDownloads.value(QStringLiteral("artifact")).toObject(), coord,
                             QString(), customBase);
        if (!spec.relPath.isEmpty()) {
            LibrarySpec ls;
            ls.name = coord;
            ls.relPath = spec.relPath;
            ls.url = spec.url;
            ls.sha1 = spec.sha1;
            ls.size = spec.size;
            meta.libraries.push_back(ls);
            continue;
        }

        // 3) 既没有 downloads 也没有 natives：老版本 + 自定义仓库，只能自己拼 Maven 路径
        //    注意：有 natives 但本地没有匹配分类器时不能瞎拼路径，那会造出一个空的假任务。
        if (!libDownloads.isEmpty() || !lib.value(QStringLiteral("natives")).toObject().isEmpty())
            continue;
        const QString rel = mavenRelPath(coord, QString());
        if (rel.isEmpty())
            continue;
        LibrarySpec ls;
        ls.name = coord;
        ls.relPath = QStringLiteral("libraries/") + rel;
        ls.url = ensureSlash(customBase.isEmpty() ? QString::fromLatin1(kOfficialLibraries)
                                                  : customBase)
                 + rel;
        meta.libraries.push_back(ls);
    }

    // ---- 统一过一遍镜像替换 ----
    // 镜像返回的 JSON 里 url 可能已经是镜像域名，applyMirror 对两种情况都幂等。
    meta.client.url = applyMirror(meta.client.url, opt.mirrorBase);
    meta.assetIndex.url = applyMirror(meta.assetIndex.url, opt.mirrorBase);
    meta.logConfig.url = applyMirror(meta.logConfig.url, opt.mirrorBase);
    for (LibrarySpec &ls : meta.libraries)
        ls.url = applyMirror(ls.url, opt.mirrorBase);

    *out = meta;
    return true;
}

} // namespace md
