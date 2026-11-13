#include "core/InstallPipeline.h"

#include "core/DownloadManager.h"
#include "core/HttpFetcher.h"
#include "core/Mirror.h"
#include "core/RulesEval.h"
#include "core/Settings.h"
#include "core/VersionParser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

namespace md {


InstallPipeline::InstallPipeline(DownloadManager *dm, QObject *parent)
    : QObject(parent), m_dm(dm), m_fetcher(new HttpFetcher(this))
{
    connect(m_fetcher, &HttpFetcher::done, this, [this](const QByteArray &data, const QUrl &) {
        if (!m_active)
            return;
        switch (m_stage) {
        case Stage::VersionJson:
            onVersionJson(data);
            break;
        case Stage::AssetIndex:
            onAssetIndex(data);
            break;
        default:
            break;
        }
    });
    connect(m_fetcher, &HttpFetcher::error, this, [this](const QString &msg) {
        if (m_active)
            fail(QStringLiteral("获取元数据失败：%1").arg(msg));
    });
    connect(m_dm, &DownloadManager::drained, this, &InstallPipeline::onDrained);
}

QString InstallPipeline::mirrorBase() const
{
    return Settings::instance().mirrorBase();
}

bool InstallPipeline::start(const QString &versionId)
{
    if (m_active)
        return false;
    if (m_root.isEmpty()) {
        emit failed(QStringLiteral("尚未设置游戏目录"));
        return false;
    }

    const VersionEntry *entry = m_manifest.find(versionId);
    if (!entry) {
        emit failed(QStringLiteral("版本清单里找不到 %1").arg(versionId));
        return false;
    }

    m_versionId = versionId;
    m_active = true;
    m_paused = false;
    m_clientGroup.clear();
    m_libraryGroup.clear();
    m_assetGroup.clear();
    m_plannedBytes = 0;
    m_plannedFiles = 0;
    m_meta = VersionMeta();
    m_index = AssetIndex();

    emit planReset();
    m_dm->reset();

    setStage(Stage::VersionJson, QStringLiteral("正在获取版本信息…"));
    const QUrl url(applyMirror(entry->url, mirrorBase()));
    emit logLine(QStringLiteral("开始安装 %1").arg(versionId));
    emit logLine(QStringLiteral("版本 JSON：%1").arg(url.toString()));
    m_fetcher->get(url, entry->sha1, QUrl(fallbackForUrl(url.toString())));
    return true;
}

void InstallPipeline::onVersionJson(const QByteArray &data)
{
    const QString dir = QDir(m_root).filePath(QStringLiteral("versions/%1").arg(m_versionId));
    if (!QDir().mkpath(dir)) {
        fail(QStringLiteral("无法创建目录：%1").arg(dir));
        return;
    }
    const QString jsonPath = QDir(dir).filePath(m_versionId + QStringLiteral(".json"));
    {
        QFile f(jsonPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            fail(QStringLiteral("无法写入 %1：%2").arg(jsonPath, f.errorString()));
            return;
        }
        if (f.write(data) != data.size()) {
            fail(QStringLiteral("写入 %1 不完整").arg(jsonPath));
            return;
        }
    }

    ParseOptions opt;
    opt.mirrorBase = mirrorBase();
    opt.os = OsInfo::current();
    opt.includeNatives = true;

    QString err;
    if (!parseVersionJson(data, opt, &m_meta, &err)) {
        fail(QStringLiteral("解析版本 JSON 失败：%1").arg(err));
        return;
    }
    if (m_meta.client.url.isEmpty()) {
        fail(QStringLiteral("版本 JSON 里没有客户端下载地址"));
        return;
    }

    emit logLine(QStringLiteral("版本类型 %1，依赖库 %2 个")
                     .arg(m_meta.type.isEmpty() ? QStringLiteral("未知") : m_meta.type)
                     .arg(m_meta.libraries.size()));

    fetchAssetIndex();
}

void InstallPipeline::fetchAssetIndex()
{
    if (m_meta.assetIndex.url.isEmpty()) {
        emit logLine(QStringLiteral("该版本没有资源索引，跳过资源阶段"));
        buildPlan();
        return;
    }

    setStage(Stage::AssetIndex, QStringLiteral("正在获取资源索引…"));
    emit logLine(QStringLiteral("资源索引：%1").arg(m_meta.assetIndex.url));
    m_fetcher->get(QUrl(m_meta.assetIndex.url), m_meta.assetIndex.sha1,
                   QUrl(fallbackForUrl(m_meta.assetIndex.url)));
}

void InstallPipeline::onAssetIndex(const QByteArray &data)
{
    QString id = m_meta.assetIndexId;
    if (id.isEmpty())
        id = m_versionId;

    const QString dir = QDir(m_root).filePath(QStringLiteral("assets/indexes"));
    if (!QDir().mkpath(dir)) {
        fail(QStringLiteral("无法创建目录：%1").arg(dir));
        return;
    }
    const QString indexPath = QDir(dir).filePath(id + QStringLiteral(".json"));
    {
        QFile f(indexPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(data);
    }

    QString err;
    if (!parseAssetIndex(data, &m_index, &err)) {
        fail(QStringLiteral("解析资源索引失败：%1").arg(err));
        return;
    }

    emit logLine(QStringLiteral("资源索引 %1：%2 个对象，去重后 %3 个文件")
                     .arg(id)
                     .arg(m_index.objects.size())
                     .arg(m_index.uniqueObjects().size()));

    buildPlan();
}

void InstallPipeline::buildPlan()
{
    const QDir root(m_root);

    // 1) 客户端 jar
    {
        DownloadTask t;
        t.kind = TaskKind::ClientJar;
        t.url = m_meta.client.url;
        t.sha1 = m_meta.client.sha1;
        t.size = m_meta.client.size;
        t.dest = root.filePath(m_meta.client.relPath);
        t.label = QStringLiteral("客户端 %1.jar").arg(m_versionId);
        t.fallbackUrl = fallbackForUrl(t.url);
        addTask(t, &m_clientGroup);
    }

    // 2) 依赖库 + 日志配置
    for (const LibrarySpec &lib : m_meta.libraries) {
        DownloadTask t;
        t.kind = TaskKind::Library;
        t.url = lib.url;
        t.sha1 = lib.sha1;
        t.size = lib.size;
        t.dest = root.filePath(lib.relPath);
        t.label = lib.name;
        t.fallbackUrl = fallbackForUrl(t.url);
        addTask(t, &m_libraryGroup);
    }
    if (!m_meta.logConfig.url.isEmpty()) {
        DownloadTask t;
        t.kind = TaskKind::LogConfig;
        t.url = m_meta.logConfig.url;
        t.sha1 = m_meta.logConfig.sha1;
        t.size = m_meta.logConfig.size;
        t.dest = root.filePath(m_meta.logConfig.relPath);
        t.label = QStringLiteral("日志配置");
        t.fallbackUrl = fallbackForUrl(t.url);
        addTask(t, &m_libraryGroup);
    }

    // 3) 资源对象（按 hash 去重）
    const QHash<QString, qint64> uniq = m_index.uniqueObjects();
    for (auto it = uniq.cbegin(); it != uniq.cend(); ++it) {
        const QString &hash = it.key();
        const QString sub = hash.left(2);
        DownloadTask t;
        t.kind = TaskKind::Asset;
        t.sha1 = hash;
        t.size = it.value();
        t.url = applyMirror(QStringLiteral("https://resources.download.minecraft.net/%1/%2")
                                .arg(sub, hash),
                            mirrorBase());
        t.dest = root.filePath(QStringLiteral("assets/objects/%1/%2").arg(sub, hash));
        t.label = QStringLiteral("资源 %1…").arg(hash.left(10));
        t.fallbackUrl = fallbackForUrl(t.url);
        addTask(t, &m_assetGroup);
    }

    emit planned(m_plannedBytes, m_plannedFiles);
    emit logLine(QStringLiteral("计划完成：%1 个文件，约 %2 MB")
                     .arg(m_plannedFiles)
                     .arg(m_plannedBytes / (1024.0 * 1024.0), 0, 'f', 1));

    startStage(Stage::Client);
}

void InstallPipeline::addTask(const DownloadTask &task, QVector<DownloadTask> *group)
{
    group->append(task);
    m_plannedFiles += 1;
    if (task.size > 0)
        m_plannedBytes += task.size;
    emit taskPlanned(task);
}

void InstallPipeline::startStage(Stage stage)
{
    const QVector<DownloadTask> *group = nullptr;
    QString text;
    switch (stage) {
    case Stage::Client:
        group = &m_clientGroup;
        text = QStringLiteral("正在下载客户端…");
        break;
    case Stage::Libraries:
        group = &m_libraryGroup;
        text = QStringLiteral("正在下载依赖库…");
        break;
    case Stage::Assets:
        group = &m_assetGroup;
        text = QStringLiteral("正在下载资源文件…");
        break;
    default:
        return;
    }

    setStage(stage, text);
    if (group->isEmpty()) {
        QTimer::singleShot(0, this, &InstallPipeline::onDrained);
        return;
    }

    m_dm->submit(*group);
    m_dm->start();
}

void InstallPipeline::onDrained()
{
    if (!m_active)
        return;

    switch (m_stage) {
    case Stage::Client:
        startStage(Stage::Libraries);
        break;
    case Stage::Libraries:
        startStage(Stage::Assets);
        break;
    case Stage::Assets:
        finishOk();
        break;
    default:
        break;
    }
}

void InstallPipeline::finishOk()
{
    m_active = false;
    m_paused = false;

    const DownloadStats s = m_dm->stats();
    const bool ok = s.failedFiles == 0;
    setStage(ok ? Stage::Done : Stage::Failed,
             ok ? QStringLiteral("下载完成") : QStringLiteral("下载结束，但部分文件失败"));

    // doneFiles 把「跳过」也算进去了，这里减掉，免得「完成 129，跳过 129」这种加起来超过总数的怪数
    const QString summary =
        QStringLiteral("共 %1 个文件：完成 %2，跳过 %3，失败 %4")
            .arg(s.totalFiles)
            .arg(qMax(0, s.doneFiles - s.skippedFiles))
            .arg(s.skippedFiles)
            .arg(s.failedFiles);
    emit logLine(summary);
    const int perm = m_dm->permissionDeniedFiles();
    if (perm > 0) {
        emit logLine(QStringLiteral("其中 %1 个文件是「拒绝访问」导致的失败：这些文件属于其它账户"
                                    "或受系统保护，重试不会好转，需要管理员权限修复目录权限。")
                         .arg(perm));
    }
    emit finished(ok, summary, false);
}

void InstallPipeline::fail(const QString &message)
{
    m_active = false;
    m_paused = false;
    m_fetcher->abort();
    m_dm->cancel();
    setStage(Stage::Failed, message);
    emit logLine(QStringLiteral("失败：%1").arg(message));
    emit failed(message);
}

void InstallPipeline::setStage(Stage stage, const QString &text)
{
    m_stage = stage;
    emit stageChanged(stage, text);
}

void InstallPipeline::pause()
{
    if (!m_active || m_paused)
        return;
    m_paused = true;
    m_dm->pause();
    emit logLine(QStringLiteral("已暂停，进度已保留"));
}

void InstallPipeline::resume()
{
    if (!m_active || !m_paused)
        return;
    m_paused = false;
    m_dm->start();
    emit logLine(QStringLiteral("继续下载"));
}

void InstallPipeline::cancel()
{
    m_fetcher->abort();
    m_active = false;
    m_paused = false;
    m_dm->cancel();
    m_dm->reset();
    setStage(Stage::Idle, QStringLiteral("已取消"));
    emit logLine(QStringLiteral("已取消"));
    emit finished(false, QStringLiteral("已取消"), true);
}

} // namespace md
