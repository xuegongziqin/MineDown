#include "core/LoaderInstaller.h"

#include "core/DownloadManager.h"
#include "core/JavaLocator.h"
#include "core/MavenUtil.h"
#include "core/Mirror.h"
#include "core/RulesEval.h"
#include "core/Settings.h"
#include "core/VersionParser.h"
#include "core/ZipReader.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QTimer>

namespace md {
namespace {

constexpr int kProcessorTimeoutMs = 15 * 60 * 1000;

QString stripLeadingSlash(QString s)
{
    while (s.startsWith(u'/') || s.startsWith(u'\\'))
        s.remove(0, 1);
    return s;
}

// jar 的 META-INF/MANIFEST.MF 里取 Main-Class（注意 72 字节折行，续行以空格开头）。
QString manifestMainClass(const QByteArray &bytes)
{
    QStringList logical;
    const QStringList raw = QString::fromUtf8(bytes).split(u'\n');
    for (const QString &line0 : raw) {
        QString line = line0;
        if (line.endsWith(u'\r'))
            line.chop(1);
        if (!line.isEmpty() && line.startsWith(u' ') && !logical.isEmpty())
            logical.last() += line.mid(1);
        else
            logical.append(line);
    }
    for (const QString &line : logical) {
        if (line.startsWith(QLatin1String("Main-Class:"), Qt::CaseInsensitive))
            return line.mid(11).trimmed();
    }
    return {};
}

// 加载器的库默认挂在哪个仓库（install_profile 里偶尔缺 url）。
QString defaultRepoFor(const QString &coord)
{
    if (coord.startsWith(QLatin1String("net.minecraftforge:")))
        return forgeMavenBase();
    if (coord.startsWith(QLatin1String("net.neoforged:")))
        return neoForgeMavenBase();
    if (coord.startsWith(QLatin1String("net.fabricmc:")))
        return fabricMavenBase();
    return QStringLiteral("https://libraries.minecraft.net");
}

QString baseVersionJar(const QString &root, const QString &mc)
{
    return root + QStringLiteral("/versions/") + mc + u'/' + mc + QStringLiteral(".jar");
}

} // namespace

LoaderInstaller::LoaderInstaller(QObject *parent) : QObject(parent)
{
    m_dm = new DownloadManager(this);
    connect(m_dm, &DownloadManager::drained, this, [this]() {
        auto next = m_onDrained;
        m_onDrained = nullptr;
        if (next)
            next();
    });
    connect(m_dm, &DownloadManager::statsChanged, this,
            [this](const DownloadStats &s) { emit progress(s.doneBytes, s.totalBytes); });
    // 安装期的依赖列表和启动期依赖大量重叠，去重提示会刷屏，这里只记数不逐个打印。
    connect(m_dm, &DownloadManager::message, this, [this](const QString &m) {
        if (m.contains(QStringLiteral("跳过重复项"))) {
            ++m_deduped;
            return;
        }
        log(m);
    });
    connect(m_dm, &DownloadManager::taskFinished, this, [this](const TaskResult &r) {
        if (r.state == TaskState::Failed) {
            ++m_failed;
            log(QStringLiteral("下载失败：%1 —— %2")
                    .arg(r.task.label.isEmpty() ? r.task.dest : r.task.label, r.error));
        }
    });

    m_procTimer = new QTimer(this);
    m_procTimer->setSingleShot(true);
    connect(m_procTimer, &QTimer::timeout, this, [this]() {
        if (m_proc && m_proc->state() != QProcess::NotRunning)
            m_proc->kill();
    });
}

LoaderInstaller::~LoaderInstaller() = default;

void LoaderInstaller::setConcurrency(int n)
{
    m_concurrency = qBound(1, n, 32);
    if (m_dm)
        m_dm->setConcurrency(m_concurrency);
}

QString LoaderInstaller::mirrorBase() const
{
    return Settings::instance().mirrorBase();
}

QString LoaderInstaller::mirrorUrl(const QString &official) const
{
    return applyMirror(official, mirrorBase());
}

void LoaderInstaller::start(const Request &req)
{
    if (m_active)
        return;

    m_req = req;
    m_active = true;
    m_canceled = false;
    m_procIndex = 0;
    m_failed = 0;
    m_deduped = 0;
    m_dataValues.clear();
    m_processors = QJsonArray();
    m_profile = QJsonObject();
    m_versionJson = QJsonObject();
    m_procOutput.clear();
    m_targetId = req.versionId;

    if (m_req.rootDir.isEmpty()) {        fail(QStringLiteral("还没有设置游戏目录"));
        return;
    }
    if (m_req.mcVersion.isEmpty()) {
        fail(QStringLiteral("还没有选择原版版本"));
        return;
    }
    if (m_req.loaderVersion.isEmpty()) {
        fail(QStringLiteral("还没有选择加载器版本"));
        return;
    }
    if (m_targetId.isEmpty())
        m_targetId = req.mcVersion;

    QDir().mkpath(m_req.rootDir);
    QDir().mkpath(m_req.rootDir + QStringLiteral("/versions"));
    QDir().mkpath(m_req.rootDir + QStringLiteral("/libraries"));

    m_dm->reset();
    m_dm->setConcurrency(m_concurrency);

    log(QStringLiteral("开始安装 %1 %2（原版 %3）")
            .arg(loaderKindName(m_req.kind), m_req.loaderVersion, m_req.mcVersion));

    if (m_req.kind == LoaderKind::Fabric)
        startFabric();
    else
        startForge();
}

void LoaderInstaller::cancel()
{
    if (!m_active)
        return;
    m_canceled = true;
    if (m_dm)
        m_dm->cancel();
    if (m_proc && m_proc->state() != QProcess::NotRunning)
        m_proc->kill();
    m_active = false;
    cleanupTemp();
    emit finished(false, QStringLiteral("已取消"), m_targetId);
}

void LoaderInstaller::setStage(const QString &text)
{
    emit stageChanged(text);
}

void LoaderInstaller::log(const QString &text)
{
    emit logLine(text);
}

void LoaderInstaller::fail(const QString &message)
{
    if (!m_active && m_canceled)
        return;
    m_active = false;
    if (m_proc && m_proc->state() != QProcess::NotRunning)
        m_proc->kill();
    if (m_dm)
        m_dm->cancel();
    cleanupTemp();
    emit finished(false, message, m_targetId);
}

void LoaderInstaller::succeed(const QString &message)
{
    m_active = false;
    if (m_deduped > 0)
        log(QStringLiteral("（另有 %1 个依赖与已下载文件重复，已跳过）").arg(m_deduped));
    cleanupTemp();
    emit finished(true, message, m_targetId);
}

void LoaderInstaller::cleanupTemp()
{
    if (m_extractDir.isEmpty())
        return;
    QDir dir(m_extractDir);
    if (dir.exists())
        dir.removeRecursively();
}

void LoaderInstaller::submit(QVector<DownloadTask> tasks,
                            const std::function<void()> &next)
{
    if (tasks.isEmpty()) {
        // 没有要下的文件：下一拍再推进，避免在信号回调里递归太深。
        QTimer::singleShot(0, this, [this, next]() {
            if (m_active && !m_canceled && next)
                next();
        });
        return;
    }
    m_onDrained = next;
    m_dm->submit(tasks);
    m_dm->start();
}

QJsonObject LoaderInstaller::readJsonFile(const QString &path, QString *err) const
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = QStringLiteral("读不到 %1").arg(path);
        return {};
    }
    const QByteArray data = f.readAll();
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (err)
            *err = QStringLiteral("%1 不是有效的 JSON（%2）").arg(path, pe.errorString());
        return {};
    }
    return doc.object();
}

// —— Fabric ——

bool LoaderInstaller::buildFabricJson(QJsonObject *out, QString *err) const
{
    const QJsonObject entry = m_req.fabricEntry;
    const QJsonObject launcherMeta = entry.value(QStringLiteral("launcherMeta")).toObject();
    const QString mainClass = launcherMeta.value(QStringLiteral("mainClass"))
                                  .toObject()
                                  .value(QStringLiteral("client"))
                                  .toString();
    if (mainClass.isEmpty()) {
        if (err)
            *err = QStringLiteral("Fabric 的版本信息里没有 mainClass（列表可能没加载完）");
        return false;
    }

    QJsonArray libs;
    auto appendLibraries = [&libs](const QJsonArray &arr) {
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            MavenCoord coord;
            if (!parseMavenCoord(o.value(QStringLiteral("name")).toString(), &coord))
                continue;
            QString repo = o.value(QStringLiteral("url")).toString();
            if (repo.isEmpty())
                repo = fabricMavenBase();

            QJsonObject artifact;
            artifact.insert(QStringLiteral("path"), coord.relPath());
            artifact.insert(QStringLiteral("url"), mavenRemoteUrl(repo, coord));
            const QString sha1 = o.value(QStringLiteral("sha1")).toString();
            if (!sha1.isEmpty())
                artifact.insert(QStringLiteral("sha1"), sha1);
            const qint64 size = o.value(QStringLiteral("size")).toInteger(-1);
            if (size > 0)
                artifact.insert(QStringLiteral("size"), size);

            QJsonObject downloads;
            downloads.insert(QStringLiteral("artifact"), artifact);
            QJsonObject lib;
            lib.insert(QStringLiteral("name"), coord.coordinate());
            lib.insert(QStringLiteral("downloads"), downloads);
            libs.append(lib);
        }
    };

    const QJsonObject libGroup = launcherMeta.value(QStringLiteral("libraries")).toObject();
    appendLibraries(libGroup.value(QStringLiteral("common")).toArray());
    appendLibraries(libGroup.value(QStringLiteral("client")).toArray());

    // loader 与 intermediary 不在 common 列表里，必须各自补一条，
    // 否则启动时找不到 net.fabricmc.loader —— 这是 Fabric 最容易漏的一步。
    auto appendCoord = [&libs](const QString &coordText) {
        MavenCoord coord;
        if (!parseMavenCoord(coordText, &coord))
            return;
        QJsonObject artifact;
        artifact.insert(QStringLiteral("path"), coord.relPath());
        artifact.insert(QStringLiteral("url"), mavenRemoteUrl(fabricMavenBase(), coord));
        QJsonObject downloads;
        downloads.insert(QStringLiteral("artifact"), artifact);
        QJsonObject lib;
        lib.insert(QStringLiteral("name"), coord.coordinate());
        lib.insert(QStringLiteral("downloads"), downloads);
        libs.append(lib);
    };
    appendCoord(entry.value(QStringLiteral("loader"))
                    .toObject()
                    .value(QStringLiteral("maven"))
                    .toString());
    appendCoord(entry.value(QStringLiteral("intermediary"))
                    .toObject()
                    .value(QStringLiteral("maven"))
                    .toString());

    if (libs.isEmpty()) {
        if (err)
            *err = QStringLiteral("Fabric 的版本信息里没有依赖库");
        return false;
    }

    QJsonObject json;
    json.insert(QStringLiteral("id"), m_targetId);
    json.insert(QStringLiteral("inheritsFrom"), m_req.mcVersion);
    json.insert(QStringLiteral("type"), QStringLiteral("release"));
    json.insert(QStringLiteral("mainClass"), mainClass);
    json.insert(QStringLiteral("libraries"), libs);
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    json.insert(QStringLiteral("time"), now);
    json.insert(QStringLiteral("releaseTime"), now);

    *out = json;
    return true;
}

void LoaderInstaller::startFabric()
{
    setStage(QStringLiteral("生成 Fabric 版本 JSON…"));

    QJsonObject json;
    QString err;
    if (!buildFabricJson(&json, &err)) {
        fail(err);
        return;
    }
    if (!writeVersionJson(json, &err)) {
        fail(err);
        return;
    }
    log(QStringLiteral("版本 JSON 已写入 versions/%1/%1.json").arg(m_targetId));

    QVector<DownloadTask> tasks = tasksFromVersionJson(json, &err);
    if (!err.isEmpty()) {
        fail(err);
        return;
    }
    ensureBaseVersion();
    setStage(QStringLiteral("下载 Fabric 依赖库…"));
    submit(tasks, [this]() { afterVersionLibs(); });
}

// —— Forge / NeoForge ——

QString LoaderInstaller::installerRelPath() const
{
    MavenCoord c;
    if (m_req.kind == LoaderKind::Forge) {
        c.group = QStringLiteral("net.minecraftforge");
        c.artifact = QStringLiteral("forge");
        c.version = m_req.mcVersion + u'-' + m_req.loaderVersion;
    } else if (m_req.kind == LoaderKind::NeoForge) {
        // 1.20.2 起是 net.neoforged:neoforge，之前是 net.neoforged:forge。
        const bool legacy = compareVersionStrings(m_req.mcVersion, QStringLiteral("1.20.2")) < 0;
        c.group = QStringLiteral("net.neoforged");
        c.artifact = legacy ? QStringLiteral("forge") : QStringLiteral("neoforge");
        c.version = legacy ? m_req.mcVersion + u'-' + m_req.loaderVersion : m_req.loaderVersion;
    }
    c.classifier = QStringLiteral("installer");
    return c.relPath();
}

QString LoaderInstaller::installerOfficialUrl() const
{
    const QString repo = m_req.kind == LoaderKind::Forge ? forgeMavenBase() : neoForgeMavenBase();
    return repo + u'/' + installerRelPath();
}

int LoaderInstaller::requiredJavaMajor() const
{
    // 先看原版版本 JSON 声明的 javaVersion，再看加载器自己的，最后按年代猜。
    const QString baseJson =
        m_req.rootDir + QStringLiteral("/versions/") + m_req.mcVersion + u'/' + m_req.mcVersion
        + QStringLiteral(".json");
    if (QFileInfo::exists(baseJson)) {
        QString err;
        const QJsonObject o = readJsonFile(baseJson, &err);
        const int major = o.value(QStringLiteral("javaVersion"))
                              .toObject()
                              .value(QStringLiteral("majorVersion"))
                              .toInt(-1);
        if (major > 0)
            return major;
    }
    const int major = m_versionJson.value(QStringLiteral("javaVersion"))
                          .toObject()
                          .value(QStringLiteral("majorVersion"))
                          .toInt(-1);
    if (major > 0)
        return major;

    // 26.x 换了新版本号体系，原版 26.3 的版本 JSON 里 javaVersion 是 25；
    // 只在本地连原版 JSON 都没有时才走到这里，所以按年代兜底猜。
    if (compareVersionStrings(m_req.mcVersion, QStringLiteral("26")) >= 0)
        return 25;
    if (compareVersionStrings(m_req.mcVersion, QStringLiteral("1.20.5")) >= 0)
        return 21;
    if (compareVersionStrings(m_req.mcVersion, QStringLiteral("1.17")) >= 0)
        return 17;
    return 8;
}

void LoaderInstaller::ensureBaseVersion()
{
    if (!QFileInfo::exists(baseVersionJar(m_req.rootDir, m_req.mcVersion))) {
        log(QStringLiteral("提示：原版 %1 还没下载，装完加载器后请先安装原版再启动。")
                .arg(m_req.mcVersion));
    }
}

void LoaderInstaller::startForge()
{
    m_librariesDir = m_req.rootDir + QStringLiteral("/libraries");
    m_installerPath = m_librariesDir + u'/' + installerRelPath();
    m_extractDir = m_req.rootDir + QStringLiteral("/.minedown/loader/") + m_targetId;

    // 安装器已经不在了才下载；已经有的话还要确认它是个能读的 zip（半截文件要重下）。
    if (QFileInfo::exists(m_installerPath)) {
        ZipReader probe;
        QString zerr;
        if (probe.open(m_installerPath, &zerr)) {
            log(QStringLiteral("已有安装器，跳过下载：%1").arg(m_installerPath));
            afterInstallerDownloaded();
            return;
        }
        log(QStringLiteral("已有安装器但读不开（%1），重新下载").arg(zerr));
    }

    setStage(QStringLiteral("下载安装器…"));
    DownloadTask t;
    const QString official = installerOfficialUrl();
    t.url = mirrorUrl(official);
    if (t.url != official)
        t.fallbackUrl = official;
    t.dest = m_installerPath;
    t.kind = TaskKind::Library;
    t.label = QStringLiteral("安装器 %1").arg(QFileInfo(m_installerPath).fileName());
    submit({t}, [this]() { afterInstallerDownloaded(); });
}

void LoaderInstaller::afterInstallerDownloaded()
{
    if (!m_active || m_canceled)
        return;

    setStage(QStringLiteral("解压安装器…"));
    QString err;

    ZipReader zip;
    if (!zip.open(m_installerPath, &err)) {
        fail(QStringLiteral("安装器不是有效的 ZIP：%1").arg(err));
        return;
    }

    if (m_extractDir.isEmpty())
        m_extractDir = m_req.rootDir + QStringLiteral("/.minedown/loader/") + m_targetId;
    QDir().mkpath(m_extractDir);
    if (!zip.extractAll(m_extractDir, &err)) {
        fail(QStringLiteral("解压安装器失败：%1").arg(err));
        return;
    }

    m_profile = readJsonFile(m_extractDir + QStringLiteral("/install_profile.json"), &err);
    if (m_profile.isEmpty()) {
        fail(QStringLiteral("安装器里没有 install_profile.json：%1").arg(err));
        return;
    }

    m_versionJson = readJsonFile(m_extractDir + QStringLiteral("/version.json"), &err);
    if (m_versionJson.isEmpty()) {
        const QString p = stripLeadingSlash(m_profile.value(QStringLiteral("json")).toString());
        if (!p.isEmpty() && p != QStringLiteral("version.json"))
            m_versionJson = readJsonFile(m_extractDir + u'/' + p, &err);
    }
    if (m_versionJson.isEmpty()) {
        fail(QStringLiteral("安装器里没有版本 JSON：%1").arg(err));
        return;
    }

    // 目标版本目录名以安装器声明的为准（NeoForge 的 id 不带原版前缀，不能猜）。
    const QString declared = m_profile.value(QStringLiteral("version")).toString();
    if (!declared.isEmpty())
        m_targetId = declared;
    else if (!m_versionJson.value(QStringLiteral("id")).toString().isEmpty())
        m_targetId = m_versionJson.value(QStringLiteral("id")).toString();

    log(QStringLiteral("安装器版本：%1（模板 id=%2，原版=%3）")
            .arg(m_targetId, m_versionJson.value(QStringLiteral("id")).toString(),
                 m_profile.value(QStringLiteral("minecraft")).toString()));

    // data：客户端安装只取 client 一侧的值，解析成「本地绝对路径」或「安装器内文件」。
    const QJsonObject data = m_profile.value(QStringLiteral("data")).toObject();
    for (auto it = data.begin(); it != data.end(); ++it) {
        const QJsonObject sides = it.value().toObject();
        QString value = sides.value(QStringLiteral("client")).toString();
        if (value.isEmpty())
            value = sides.value(QStringLiteral("server")).toString();
        if (value.isEmpty())
            continue;
        QString rerr;
        const QString resolved = resolveDataValue(value, &rerr);
        if (resolved.isEmpty()) {
            fail(QStringLiteral("无法解析 data 值 %1：%2").arg(it.key(), rerr));
            return;
        }
        m_dataValues.insert(it.key(), resolved);
    }
    ensureDataDirs();

    m_processors = m_profile.value(QStringLiteral("processors")).toArray();
    m_procIndex = 0;

    QVector<DownloadTask> tasks = tasksFromLibraryArray(
        m_profile.value(QStringLiteral("libraries")).toArray(), &err);
    if (!err.isEmpty()) {
        fail(err);
        return;
    }
    ensureBaseVersion();

    if (m_processors.isEmpty()) {
        // 旧版（1.12.2 及更早）：安装器里直接带 maven/ 目录，解出来就行，不用跑 Java。
        log(QStringLiteral("旧版安装器（没有 processors），直接解出内置库"));
        int count = 0;
        if (!zip.extractUnder(QStringLiteral("maven/"), m_librariesDir, &count, &err)) {
            fail(QStringLiteral("解出内置库失败：%1").arg(err));
            return;
        }
        log(QStringLiteral("已解出 %1 个库文件").arg(count));

        if (!writeVersionJson(m_versionJson, &err)) {
            fail(err);
            return;
        }

        QVector<DownloadTask> versionTasks = tasksFromVersionJson(m_versionJson, &err);
        if (!err.isEmpty()) {
            fail(err);
            return;
        }
        tasks += versionTasks;
        setStage(QStringLiteral("下载版本依赖…"));
        submit(tasks, [this]() { afterVersionLibs(); });
        return;
    }

    log(QStringLiteral("安装器需要 %1 个 processor 步骤").arg(m_processors.size()));
    setStage(QStringLiteral("下载安装期依赖…"));
    submit(tasks, [this]() { afterProfileLibs(); });
}

void LoaderInstaller::afterProfileLibs()
{
    if (!m_active || m_canceled)
        return;

    const int major = requiredJavaMajor();
    const JavaInfo java = findJava(major);
    if (!java.isValid()) {
        fail(QStringLiteral("没有找到 Java。安装器需要 Java %1 或更高版本，"
                            "请先安装 JDK/JRE 并加入 PATH。")
                 .arg(major));
        return;
    }
    if (java.major < major) {
        log(QStringLiteral("注意：需要 Java %1，但只找到 %2，processor 可能跑不起来。")
                .arg(major)
                .arg(javaVersionText(java)));
    }
    log(QStringLiteral("使用 %1：%2").arg(javaVersionText(java), java.path));

    setStage(QStringLiteral("执行安装步骤…"));
    m_procIndex = 0;
    runProcessors();
}

void LoaderInstaller::runProcessors()
{
    if (!m_active || m_canceled)
        return;
    if (m_procIndex >= m_processors.size()) {
        afterProcessors();
        return;
    }

    const QJsonObject p = m_processors.at(m_procIndex).toObject();
    const QJsonArray sides = p.value(QStringLiteral("sides")).toArray();
    if (!sides.isEmpty()) {
        bool client = false;
        for (const QJsonValue &v : sides) {
            if (v.toString() == QLatin1String("client"))
                client = true;
        }
        if (!client) {
            ++m_procIndex;
            runProcessors();
            return;
        }
    }
    runProcessorAt(m_procIndex);
}

void LoaderInstaller::runProcessorAt(int index)
{
    const QJsonObject p = m_processors.at(index).toObject();
    QString err;

    MavenCoord jarCoord;
    if (!parseMavenCoord(p.value(QStringLiteral("jar")).toString(), &jarCoord)) {
        fail(QStringLiteral("processor 的 jar 字段无法解析：%1")
                 .arg(p.value(QStringLiteral("jar")).toString()));
        return;
    }
    const QString jarPath = mavenLocalPath(m_req.rootDir, jarCoord);
    if (!QFileInfo::exists(jarPath)) {
        fail(QStringLiteral("缺少 processor 依赖：%1").arg(jarCoord.coordinate()));
        return;
    }

    QStringList classpath;
    for (const QJsonValue &v : p.value(QStringLiteral("classpath")).toArray()) {
        MavenCoord c;
        if (!parseMavenCoord(v.toString(), &c))
            continue;
        classpath.append(mavenLocalPath(m_req.rootDir, c));
    }
    classpath.append(jarPath);

    ZipReader pz;
    if (!pz.open(jarPath, &err)) {
        fail(QStringLiteral("打不开 processor 的 jar：%1").arg(err));
        return;
    }
    QByteArray manifest;
    if (!pz.read(QStringLiteral("META-INF/MANIFEST.MF"), &manifest, &err)) {
        fail(QStringLiteral("processor 的 jar 里没有 MANIFEST.MF：%1").arg(err));
        return;
    }
    const QString mainClass = manifestMainClass(manifest);
    if (mainClass.isEmpty()) {
        fail(QStringLiteral("processor 的 jar 里没有 Main-Class：%1").arg(jarPath));
        return;
    }

    QStringList argv{QStringLiteral("-cp"), classpath.join(u';'), mainClass};
    for (const QJsonValue &v : p.value(QStringLiteral("args")).toArray()) {
        QString rerr;
        const QString arg = resolveArg(v.toString(), &rerr);
        if (!rerr.isEmpty()) {
            fail(QStringLiteral("processor 参数解析失败：%1").arg(rerr));
            return;
        }
        argv.append(arg);
    }

    const JavaInfo java = findJava(requiredJavaMajor());
    if (!java.isValid()) {
        fail(QStringLiteral("没有找到 Java，无法执行安装步骤"));
        return;
    }

    log(QStringLiteral("执行 processor %1/%2：%3")
            .arg(index + 1)
            .arg(m_processors.size())
            .arg(mainClass));

    m_procOutput.clear();
    m_proc = new QProcess(this);
    m_proc->setWorkingDirectory(m_req.rootDir);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_proc, &QProcess::readyRead, this, [this]() {
        if (!m_proc)
            return;
        const QString chunk = QString::fromLocal8Bit(m_proc->readAll());
        m_procOutput += chunk;
        if (m_procOutput.size() > 64 * 1024)
            m_procOutput = m_procOutput.right(32 * 1024);
    });
    connect(m_proc, &QProcess::finished, this, &LoaderInstaller::onProcessorFinished);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            const QString msg = m_proc ? m_proc->errorString() : QString();
            fail(QStringLiteral("无法启动 java：%1").arg(msg));
        }
    });

    m_procTimer->start(kProcessorTimeoutMs);
    m_proc->start(java.path, argv);
    if (!m_proc->waitForStarted(20000)) {
        const QString msg = m_proc->errorString();
        m_procTimer->stop();
        m_proc->deleteLater();
        m_proc = nullptr;
        fail(QStringLiteral("无法启动 java：%1").arg(msg));
    }
}

void LoaderInstaller::onProcessorFinished(int exitCode, QProcess::ExitStatus status)
{
    m_procTimer->stop();
    if (m_proc) {
        m_procOutput += QString::fromLocal8Bit(m_proc->readAll());
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    if (!m_active || m_canceled)
        return;

    if (status != QProcess::NormalExit || exitCode != 0) {
        QString tail = m_procOutput.trimmed();
        if (tail.size() > 800)
            tail = tail.right(800);
        fail(QStringLiteral("processor 执行失败（退出码 %1）：\n%2").arg(exitCode).arg(tail));
        return;
    }

    ++m_procIndex;
    QTimer::singleShot(0, this, &LoaderInstaller::runProcessors);
}

void LoaderInstaller::afterProcessors()
{
    if (!m_active || m_canceled)
        return;

    setStage(QStringLiteral("写入版本 JSON…"));
    QString err;
    if (!writeVersionJson(m_versionJson, &err)) {
        fail(err);
        return;
    }
    log(QStringLiteral("版本 JSON 已写入 versions/%1/%1.json").arg(m_targetId));

    QVector<DownloadTask> tasks = tasksFromVersionJson(m_versionJson, &err);
    if (!err.isEmpty()) {
        fail(err);
        return;
    }
    setStage(QStringLiteral("下载版本依赖…"));
    submit(tasks, [this]() { afterVersionLibs(); });
}

void LoaderInstaller::afterVersionLibs()
{
    if (!m_active || m_canceled)
        return;
    finishIfDownloadsOk();
}

void LoaderInstaller::finishIfDownloadsOk()
{
    if (m_failed > 0) {
        fail(QStringLiteral("有 %1 个文件下载失败，安装没有完成。可以再试一次（已完成的文件会跳过）。")
                 .arg(m_failed));
        return;
    }
    succeed(QStringLiteral("已安装 %1（版本目录 versions/%1）").arg(m_targetId));
}

// —— 任务与路径 ——

QVector<DownloadTask> LoaderInstaller::tasksFromVersionJson(const QJsonObject &json,
                                                            QString *err)
{
    QVector<DownloadTask> tasks;

    VersionMeta meta;
    QString perr;
    const QByteArray bytes = QJsonDocument(json).toJson();
    ParseOptions opt;
    opt.mirrorBase = mirrorBase();
    opt.os = OsInfo::current();
    opt.includeNatives = true;
    if (!parseVersionJson(bytes, opt, &meta, &perr)) {
        if (err)
            *err = QStringLiteral("解析版本 JSON 失败：%1").arg(perr);
        return tasks;
    }

    if (!meta.client.url.isEmpty() && !meta.client.relPath.isEmpty()) {
        DownloadTask t;
        t.url = meta.client.url;
        t.fallbackUrl = fallbackForUrl(t.url);
        t.sha1 = meta.client.sha1;
        t.size = meta.client.size;
        t.dest = m_req.rootDir + u'/' + meta.client.relPath;
        t.kind = TaskKind::ClientJar;
        t.label = QStringLiteral("客户端 %1").arg(m_targetId);
        tasks.append(t);
    }

    for (const LibrarySpec &lib : meta.libraries) {
        if (lib.url.isEmpty() || lib.relPath.isEmpty())
            continue;
        DownloadTask t;
        t.url = lib.url;
        t.fallbackUrl = fallbackForUrl(t.url);
        t.sha1 = lib.sha1;
        t.size = lib.size;
        t.dest = m_req.rootDir + u'/' + lib.relPath;
        t.kind = TaskKind::Library;
        t.label = lib.name.isEmpty() ? lib.relPath : lib.name;
        tasks.append(t);
    }
    return tasks;
}

QVector<DownloadTask> LoaderInstaller::tasksFromLibraryArray(const QJsonArray &libs, QString *err)
{
    QVector<DownloadTask> tasks;
    const OsInfo os = OsInfo::current();

    for (const QJsonValue &value : libs) {
        const QJsonObject lib = value.toObject();

        if (lib.contains(QStringLiteral("rules"))
            && !rulesAllow(lib.value(QStringLiteral("rules")).toArray(), os)) {
            continue;
        }
        // install_profile 里用 clientreq 标记「客户端是否需要」，不是 rules 那种语义。
        if (lib.contains(QStringLiteral("clientreq"))
            && !lib.value(QStringLiteral("clientreq")).toBool(true)) {
            continue;
        }

        const QJsonObject artifact = lib.value(QStringLiteral("downloads"))
                                         .toObject()
                                         .value(QStringLiteral("artifact"))
                                         .toObject();
        QString relPath = artifact.value(QStringLiteral("path")).toString();

        const QString name = lib.value(QStringLiteral("name")).toString();
        MavenCoord coord;
        const bool hasCoord = parseMavenCoord(name, &coord);
        if (relPath.isEmpty()) {
            if (!hasCoord)
                continue;
            relPath = coord.relPath();
        }
        if (relPath.isEmpty())
            continue;

        QString official = artifact.value(QStringLiteral("url")).toString();
        if (official.isEmpty()) {
            const QString repo = defaultRepoFor(name);
            if (repo.isEmpty())
                continue;
            official = repo + u'/' + relPath;
        }

        DownloadTask t;
        t.url = mirrorUrl(official);
        if (t.url != official)
            t.fallbackUrl = official;
        t.sha1 = artifact.value(QStringLiteral("sha1")).toString();
        t.size = artifact.value(QStringLiteral("size")).toInteger(-1);
        t.dest = m_librariesDir + u'/' + relPath;
        t.kind = TaskKind::Library;
        t.label = name.isEmpty() ? relPath : name;
        tasks.append(t);
    }
    Q_UNUSED(err);
    return tasks;
}

QString LoaderInstaller::resolveDataValue(const QString &value, QString *err) const
{
    const QString v = value.trimmed();

    // 'xxx' 是字面量（例如 PATCHED_SHA 的哈希值）
    if (v.size() >= 2 && v.startsWith(u'\'') && v.endsWith(u'\''))
        return v.mid(1, v.size() - 2);

    // /data/client.lzma 之类指向安装器内部解压出来的文件
    if (v.startsWith(u'/'))
        return m_extractDir + v;

    // [group:artifact:version:classifier] 是 maven 坐标，落到 libraries/ 下
    if (isBracketCoord(v)) {
        MavenCoord c;
        if (!parseMavenCoord(v, &c)) {
            if (err)
                *err = QStringLiteral("不是合法的 Maven 坐标：%1").arg(v);
            return {};
        }
        return mavenLocalPath(m_req.rootDir, c);
    }

    return v;
}

QString LoaderInstaller::resolveArg(const QString &arg, QString *err) const
{
    QString s = arg;

    // {KEY} 可能出现在参数的任意位置（有时整个参数就是一个坐标）。
    for (auto it = m_dataValues.constBegin(); it != m_dataValues.constEnd(); ++it)
        s.replace(u'{' + it.key() + u'}', it.value());

    // 安装器内置的占位符
    s.replace(QStringLiteral("{SIDE}"), QStringLiteral("client"));
    s.replace(QStringLiteral("{MINECRAFT_JAR}"), baseVersionJar(m_req.rootDir, m_req.mcVersion));
    s.replace(QStringLiteral("{ROOT}"), m_req.rootDir);
    s.replace(QStringLiteral("{INSTALLER}"), m_installerPath);
    s.replace(QStringLiteral("{LIBRARY_DIR}"), m_librariesDir);
    s.replace(QStringLiteral("{MINECRAFT_VERSION}"), m_req.mcVersion);

    // 替换完之后仍是 [坐标] 形式的，转成本地路径。
    if (isBracketCoord(s)) {
        MavenCoord c;
        if (!parseMavenCoord(s, &c)) {
            if (err)
                *err = QStringLiteral("参数里的坐标无法解析：%1").arg(arg);
            return {};
        }
        return mavenLocalPath(m_req.rootDir, c);
    }
    return s;
}

void LoaderInstaller::ensureDataDirs()
{
    for (auto it = m_dataValues.constBegin(); it != m_dataValues.constEnd(); ++it) {
        const QString path = it.value();
        if (path.isEmpty())
            continue;
        // 只对看起来像路径的值建目录（字面量哈希值不建）
        if (path.contains(u'/') || path.contains(u'\\'))
            QDir().mkpath(QFileInfo(path).absolutePath());
    }
}

bool LoaderInstaller::writeVersionJson(const QJsonObject &json, QString *err)
{
    QJsonObject out = json;
    out.insert(QStringLiteral("id"), m_targetId);
    if (out.value(QStringLiteral("inheritsFrom")).toString().isEmpty())
        out.insert(QStringLiteral("inheritsFrom"), m_req.mcVersion);
    if (out.value(QStringLiteral("type")).toString().isEmpty())
        out.insert(QStringLiteral("type"), QStringLiteral("release"));

    const QString dir = m_req.rootDir + QStringLiteral("/versions/") + m_targetId;
    const QString path = dir + u'/' + m_targetId + QStringLiteral(".json");
    return writeFileBytes(path, QJsonDocument(out).toJson(QJsonDocument::Indented), err);
}

} // namespace md
