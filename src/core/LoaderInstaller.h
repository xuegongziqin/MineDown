#pragma once

#include "core/LoaderCatalog.h"
#include "core/Types.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QVector>

#include <functional>

class QTimer;

namespace md {

class DownloadManager;
class ZipReader;

// 加载器安装编排（Fabric / Forge / NeoForge）。
//
// 三条路线：
//   Fabric     —— 把 Meta API 给的库和 mainClass 直接合成版本 JSON，不需要 Java。
//   Forge/新   —— 下载安装器 jar -> 解压 -> 读 install_profile.json -> 下载安装期依赖
//                 -> 顺序执行 processors（java -cp）-> 写 version.json -> 补齐启动期依赖。
//   Forge/旧   —— 安装器里直接带 maven/ 目录，解到 libraries/ 再写 version.json，不用 Java。
//
// 全程只写 versions/<loader-id>/ 与 libraries/，绝不碰原版目录。
class LoaderInstaller : public QObject
{
    Q_OBJECT

public:
    struct Request {
        LoaderKind kind = LoaderKind::Fabric;
        QString mcVersion;      // 原版版本 id
        QString loaderVersion;  // 加载器版本号（Forge: 47.4.29 等）
        QString versionId;      // 预计的版本目录名；安装时会被安装器里的 version 字段覆盖
        QString rootDir;        // .minecraft 目录
        QJsonObject fabricEntry;// Fabric 用：Meta API 的原始条目
    };

    explicit LoaderInstaller(QObject *parent = nullptr);
    ~LoaderInstaller() override;

    void setConcurrency(int n);
    void start(const Request &req);
    void cancel();
    bool active() const { return m_active; }
    QString targetId() const { return m_targetId; }

signals:
    void stageChanged(const QString &text);
    void logLine(const QString &text);
    void progress(qint64 done, qint64 total);
    void finished(bool ok, const QString &message, const QString &versionId);

private:
    // —— 三条路线 ——
    void startFabric();
    void startForge();

    // —— Forge / NeoForge 的流水线 ——
    void afterInstallerDownloaded();
    void afterProfileLibs();
    void runProcessors();
    void runProcessorAt(int index);
    void onProcessorFinished(int exitCode, QProcess::ExitStatus status);
    void afterProcessors();
    void afterVersionLibs();
    void finishIfDownloadsOk();

    // —— 辅助 ——
    bool buildFabricJson(QJsonObject *out, QString *err) const;
    QVector<DownloadTask> tasksFromVersionJson(const QJsonObject &json, QString *err);
    QVector<DownloadTask> tasksFromLibraryArray(const QJsonArray &libs, QString *err);
    QString resolveDataValue(const QString &value, QString *err) const;
    QString resolveArg(const QString &arg, QString *err) const;
    bool writeVersionJson(const QJsonObject &json, QString *err);
    QString installerRelPath() const;
    QString installerOfficialUrl() const;
    int requiredJavaMajor() const;
    void ensureBaseVersion();
    void ensureDataDirs();
    void submit(QVector<DownloadTask> tasks, const std::function<void()> &next);
    void setStage(const QString &text);
    void log(const QString &text);
    void fail(const QString &message);
    void succeed(const QString &message);
    void cleanupTemp();
    QString mirrorBase() const;
    QString mirrorUrl(const QString &official) const;
    QJsonObject readJsonFile(const QString &path, QString *err) const;

    DownloadManager *m_dm = nullptr;
    QProcess *m_proc = nullptr;
    QTimer *m_procTimer = nullptr;

    Request m_req;
    int m_concurrency = 4;
    bool m_active = false;
    bool m_canceled = false;

    QString m_targetId;
    QString m_extractDir;
    QString m_installerPath;
    QString m_librariesDir;

    QJsonObject m_profile;
    QJsonObject m_versionJson;
    QJsonArray m_processors;
    int m_procIndex = 0;
    int m_failed = 0; // 下载失败的文件数（最后汇总用）
    int m_deduped = 0; // 与已有任务目标重复而被跳过的文件数（只统计，不逐个打日志）
    QMap<QString, QString> m_dataValues;
    QString m_procOutput;

    std::function<void()> m_onDrained;
};

} // namespace md
