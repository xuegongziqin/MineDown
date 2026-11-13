#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

namespace md {

// 一个待下载文件的类别，用于统计、日志与界面分组。
enum class TaskKind {
    Unknown = 0,
    Manifest,     // version_manifest_v2.json
    VersionJson,  // <id>.json
    ClientJar,    // <id>.jar
    Library,      // libraries/**
    AssetIndex,   // assets/indexes/<id>.json
    Asset,        // assets/objects/**
    LogConfig,    // assets/log_configs/**
};

QString taskKindName(TaskKind kind);

// 一个下载任务：URL 已是替换过镜像的最终地址。
struct DownloadTask {
    QString url;
    QString sha1;              // 期望的小写十六进制 SHA1；为空表示只做大小校验
    qint64 size = -1;          // 期望字节数；-1 表示未知
    QString dest;              // 本地绝对路径
    TaskKind kind = TaskKind::Unknown;
    QString label;             // 界面展示用的短名称
    QString fallbackUrl;       // 镜像缺文件时回退的官方地址（可为空）

    bool hasSha1() const { return sha1.size() == 40; }
};

enum class TaskState {
    Pending = 0,
    Running,
    Done,      // 下载完成且校验通过
    Skipped,   // 本地已存在且校验通过，无需下载
    Failed,
    Canceled,
};

QString taskStateName(TaskState state);

struct TaskResult {
    DownloadTask task;
    TaskState state = TaskState::Pending;
    QString error;
    qint64 downloaded = 0;   // 本次实际写入的字节数
    int attempts = 0;        // 尝试次数（含首次）
};

// 汇总统计，界面与日志共用。
struct DownloadStats {
    qint64 totalBytes = 0;    // 计划下载的总字节
    qint64 doneBytes = 0;     // 已完成字节（含跳过文件）
    int totalFiles = 0;
    int doneFiles = 0;
    int skippedFiles = 0;
    int failedFiles = 0;
    qint64 startedMs = 0;     // QDateTime::currentMSecsSinceEpoch()
    qint64 bytesThisSecond = 0;

    double speedBytesPerSec() const;
    int percent() const;
};

} // namespace md

Q_DECLARE_METATYPE(md::DownloadTask)
Q_DECLARE_METATYPE(md::TaskResult)
Q_DECLARE_METATYPE(md::DownloadStats)
