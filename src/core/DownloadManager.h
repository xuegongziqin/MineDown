#pragma once

#include "core/Types.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVector>

#include <deque>

class QThread;

namespace md {

class DownloadWorker;

// 下载调度器（主线程）：持有一组常驻工作线程，把任务队列分派给空闲 worker。
// 负责：并发控制、暂停/继续/取消、汇总进度、失败统计。
class DownloadManager : public QObject
{
    Q_OBJECT

public:
    explicit DownloadManager(QObject *parent = nullptr);
    ~DownloadManager() override;

    // 并发数（1..32）。仅在空闲时生效，运行中调用会排队到结束之后。
    void setConcurrency(int n);
    int concurrency() const { return m_workers.size(); }

    void submit(const QVector<md::DownloadTask> &tasks);
    void start();
    void pause();
    void cancel();
    void reset();                 // 清空队列 + 统计

    bool running() const { return m_running; }
    bool paused() const { return m_paused; }
    int pendingCount() const { return static_cast<int>(m_queue.size()); }
    int doneCount() const { return m_stats.doneFiles; }
    int totalCount() const { return m_stats.totalFiles; }
    DownloadStats stats() const { return m_stats; }
    // 因「拒绝访问」（文件属于别的账户/受系统保护）失败的文件数：这种失败重试无用，
    // 主界面据此给出修复提示。
    int permissionDeniedFiles() const { return m_permDenied; }

signals:
    void statsChanged(const md::DownloadStats &s);
    void taskFinished(const md::TaskResult &r);
    void taskProgress(const QString &dest, qint64 received, qint64 total);
    void drained();               // 队列和在途任务都清空
    void message(const QString &text);

private slots:
    void onWorkerFinished(int id, const md::TaskResult &result);
    void onWorkerInterrupted(int id, const md::DownloadTask &task);
    void onWorkerProgress(int id, qint64 received, qint64 total);

private:
    struct Slot {
        QThread *thread = nullptr;
        DownloadWorker *worker = nullptr;
        int currentId = 0;        // 0 = 空闲
    };

    void buildPool(int n);
    void destroyPool();
    void stopAll(bool notify);
    void dispatch();
    void maybeDrained();
    void emitStats();
    int slotOf(int id) const;

    QVector<Slot> m_workers;
    std::deque<DownloadTask> m_queue;
    QHash<int, DownloadTask> m_inflight;
    QHash<int, qint64> m_bytes;
    QSet<QString> m_seenDests;     // 同一次下载里按目标路径去重

    int m_nextId = 1;
    bool m_running = false;
    bool m_paused = false;
    int m_pendingConcurrency = 0;

    qint64 m_baseBytes = 0;       // 已完成任务的字节数
    qint64 m_lastBytes = 0;
    qint64 m_lastTickMs = 0;
    int m_permDenied = 0;         // 权限失败计数
    DownloadStats m_stats;
    QTimer *m_tick = nullptr;
};

} // namespace md
