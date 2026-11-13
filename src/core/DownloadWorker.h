#pragma once

#include "core/Types.h"

#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QVector>

namespace md {

// 下载工作单元：运行在独立 QThread 里，独占一个 QNetworkAccessManager
// （这样每个线程有自己的连接池和 keep-alive，才能真正并发）。
//
// 它的职责：校验 -> 断点续传下载 -> 校验 -> 落盘。
// 一次只处理一个任务；完成后发 finished，由 DownloadManager 派发下一个。
//
// 大文件（>= 8 MiB）首次尝试时会切成几段并行下载：
// 实测单连接的吞吐被压得很低（官方源 ~0.04 MB/s），多连接能到 1 MB/s 以上。
// 分段下载的状态只在本进程内有效——一旦暂停，.part 被丢弃，下次重新下。
class DownloadWorker : public QObject
{
    Q_OBJECT

public:
    explicit DownloadWorker(int index, QObject *parent = nullptr);
    ~DownloadWorker() override;

public slots:
    void init();                              // 在所属线程里创建 QNAM
    void assign(int id, const md::DownloadTask &task);
    void abortCurrent();                      // 暂停/取消：中断当前传输但保留 .part

signals:
    void finished(int id, const md::TaskResult &result);
    void interrupted(int id, const md::DownloadTask &task);
    void progress(int id, qint64 received, qint64 total);
    void logLine(const QString &text);

private:
    struct Chunk
    {
        qint64 start = 0;
        qint64 end = -1;          // 含尾
        qint64 written = 0;       // 本段已写入的字节数
        int attempts = 0;
        QNetworkReply *reply = nullptr;
    };

    // 单连接路径
    void startAttempt();
    void onMetaDataChanged();
    void onReadyRead();
    void onReplyFinished();

    // 多连接（分块）路径
    void startChunked();
    void startChunk(int index);
    void onChunkReadyRead(int index);
    void onChunkFinished(int index);
    bool chunksAllDone() const;
    qint64 chunksReceived() const;

    void cleanupReply();
    void cleanupChunks();
    void closePartFile();
    bool promotePartFile(QString *err);
    void fail(const QString &err, bool retryable, bool discardPartial);
    void finishTransfer();                    // 传输结束后的统一校验/落盘
    void complete(TaskState state, const QString &err = QString());
    void emitProgress(qint64 total);

    int m_index = 0;
    QNetworkAccessManager *m_nam = nullptr;

    int m_id = -1;
    DownloadTask m_task;
    QNetworkReply *m_reply = nullptr;
    QFile *m_file = nullptr;
    QString m_partPath;

    qint64 m_haveOnDisk = 0;   // 本次尝试开始时 .part 中已有的字节数
    qint64 m_total = -1;       // 期望总字节数（-1 = 未知）
    qint64 m_written = 0;      // 本次尝试已写入的字节数
    int m_attempt = 0;         // 已尝试次数
    int m_redirects = 0;       // 本条任务已手动跟随的跳转次数
    bool m_requestedRange = false;
    bool m_busy = false;
    bool m_abortRequested = false;
    bool m_partVerified = false;   // .part 已通过 SHA1：失败时绝不能顺手删掉
    bool m_destBlocked = false;    // 目标文件无法替换（拒绝访问），重试没有意义
    QString m_writeError;      // 本地写入错误，优先于网络错误上报
    qint64 m_lastProgressMs = 0;

    // 分块下载状态
    QVector<Chunk> m_chunks;
    int m_activeChunks = 0;
    bool m_chunked = false;
    bool m_chunkingUnsupported = false;  // 服务器忽略 Range，别再切块了
    qint64 m_retryDelayOverride = 0;     // 429 之类的限流要用更长的退避
};

} // namespace md
