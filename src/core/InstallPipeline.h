#pragma once

#include "core/AssetIndexParser.h"
#include "core/ManifestParser.h"
#include "core/Types.h"
#include "core/VersionParser.h"

#include <QObject>
#include <QVector>

namespace md {

class DownloadManager;
class HttpFetcher;

// 安装编排：清单 -> 版本 JSON -> 资源索引 -> 规划 -> 分组下载 -> 收尾。
// 只负责「要下什么、什么时候下」，具体下载交给 DownloadManager。
class InstallPipeline : public QObject
{
    Q_OBJECT

public:
    enum class Stage {
        Idle = 0,
        VersionJson,   // 取版本 JSON
        AssetIndex,    // 取资源索引
        Client,        // 下载客户端 jar
        Libraries,     // 下载依赖库
        Assets,        // 下载资源文件
        Done,
        Failed,
    };
    Q_ENUM(Stage)

    explicit InstallPipeline(DownloadManager *dm, QObject *parent = nullptr);

    void setRootDir(const QString &root) { m_root = root; }
    QString rootDir() const { return m_root; }
    void setManifest(const VersionManifest &manifest) { m_manifest = manifest; }
    const VersionManifest &manifest() const { return m_manifest; }

    bool start(const QString &versionId);
    void pause();
    void resume();
    void cancel();

    Stage stage() const { return m_stage; }
    bool active() const { return m_active; }
    bool paused() const { return m_paused; }
    QString versionId() const { return m_versionId; }
    qint64 plannedBytes() const { return m_plannedBytes; }
    int plannedFiles() const { return m_plannedFiles; }

signals:
    void stageChanged(md::InstallPipeline::Stage stage, const QString &text);
    void planReset();
    void taskPlanned(const md::DownloadTask &task);
    void planned(qint64 totalBytes, int totalFiles);
    // canceled 用来区分「用户点了取消」和「下完了但有失败」，别用文案去猜。
    void finished(bool ok, const QString &summary, bool canceled);
    void failed(const QString &message);
    void logLine(const QString &text);

private:
    void fetchVersionJson();
    void onVersionJson(const QByteArray &data);
    void fetchAssetIndex();
    void onAssetIndex(const QByteArray &data);
    void buildPlan();
    void startStage(Stage stage);
    void onDrained();
    void finishOk();
    void fail(const QString &message);
    void setStage(Stage stage, const QString &text);
    void addTask(const DownloadTask &task, QVector<DownloadTask> *group);
    QString mirrorBase() const;

    DownloadManager *m_dm = nullptr;
    HttpFetcher *m_fetcher = nullptr;

    VersionManifest m_manifest;
    QString m_root;
    QString m_versionId;
    VersionMeta m_meta;
    AssetIndex m_index;

    QVector<DownloadTask> m_clientGroup;
    QVector<DownloadTask> m_libraryGroup;
    QVector<DownloadTask> m_assetGroup;

    Stage m_stage = Stage::Idle;
    bool m_active = false;
    bool m_paused = false;
    qint64 m_plannedBytes = 0;
    int m_plannedFiles = 0;
};

} // namespace md
