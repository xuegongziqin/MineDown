#include "core/DownloadManager.h"

#include "core/DownloadWorker.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>

namespace md {

namespace {

// 区分「等一会儿重试就好」和「权限根本不对」用的：后者在从旧系统 / 备份拷来的
// .minecraft 里很常见，重试多少次都一样，只能提示用户去修目录权限。
bool looksLikePermissionProblem(const QString &err)
{
    return err.contains(QStringLiteral("拒绝访问"))
           || err.contains(QStringLiteral("无法删除"))
           || err.contains(QStringLiteral("无法替换"))
           || err.contains(QStringLiteral("Access is denied"), Qt::CaseInsensitive)
           || err.contains(QStringLiteral("Permission denied"), Qt::CaseInsensitive);
}

} // namespace

DownloadManager::DownloadManager(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<md::DownloadTask>("md::DownloadTask");
    qRegisterMetaType<md::TaskResult>("md::TaskResult");
    qRegisterMetaType<md::DownloadStats>("md::DownloadStats");
    qRegisterMetaType<md::TaskState>("md::TaskState");

    m_tick = new QTimer(this);
    m_tick->setInterval(1000);
    connect(m_tick, &QTimer::timeout, this, [this]() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 delta = m_stats.doneBytes - m_lastBytes;
        const qint64 dt = now - m_lastTickMs;
        m_stats.bytesThisSecond = dt > 0 ? delta * 1000 / dt : 0;
        m_lastBytes = m_stats.doneBytes;
        m_lastTickMs = now;
        emitStats();
    });
}

DownloadManager::~DownloadManager()
{
    destroyPool();
}

int DownloadManager::slotOf(int id) const
{
    for (int i = 0; i < m_workers.size(); ++i) {
        if (m_workers.at(i).currentId == id)
            return i;
    }
    return -1;
}

void DownloadManager::buildPool(int n)
{
    destroyPool();
    if (n < 1) n = 1;
    if (n > 32) n = 32;

    for (int i = 0; i < n; ++i) {
        Slot slot;
        slot.thread = new QThread(this);
        slot.thread->setObjectName(QStringLiteral("md-dl-%1").arg(i));
        slot.worker = new DownloadWorker(i);
        slot.worker->moveToThread(slot.thread);

        connect(slot.thread, &QThread::finished, slot.worker, &QObject::deleteLater);
        connect(slot.worker, &DownloadWorker::finished, this, &DownloadManager::onWorkerFinished);
        connect(slot.worker, &DownloadWorker::interrupted, this, &DownloadManager::onWorkerInterrupted);
        connect(slot.worker, &DownloadWorker::progress, this, &DownloadManager::onWorkerProgress);
        connect(slot.worker, &DownloadWorker::logLine, this, &DownloadManager::message);

        slot.thread->start();
        QMetaObject::invokeMethod(slot.worker, "init", Qt::QueuedConnection);
        m_workers.push_back(slot);
    }
}

void DownloadManager::destroyPool()
{
    // 先把调度停掉：不然 abort 之后 worker 把任务还回来，dispatch() 又会派新任务。
    m_running = false;
    m_paused = false;
    m_queue.clear();
    if (m_tick)
        m_tick->stop();

    for (Slot &slot : m_workers) {
        QThread *thread = slot.thread;
        if (!thread)
            continue;
        // 在途传输要先掐断，否则线程的事件循环里还挂着网络请求，quit() 后 wait 等不到
        if (slot.worker && slot.currentId != 0)
            QMetaObject::invokeMethod(slot.worker, "abortCurrent", Qt::QueuedConnection);
        thread->quit();
        if (thread->wait(15000)) {
            // 线程确实停了才 delete：worker 由 thread->finished -> deleteLater 回收。
            delete thread;
        } else {
            // 还卡着就别硬删（删一个还在跑的 QThread 是未定义行为），
            // 让它自己结束后由 Qt 回收。
            thread->disconnect(this);
            QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        }
        slot.thread = nullptr;
        slot.worker = nullptr;
    }
    m_workers.clear();
    m_inflight.clear();
    m_bytes.clear();
    m_stats.bytesThisSecond = 0;
}

void DownloadManager::setConcurrency(int n)
{
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    if (m_running) {
        m_pendingConcurrency = n;
        return;
    }
    buildPool(n);
}

void DownloadManager::submit(const QVector<md::DownloadTask> &tasks)
{
    for (const DownloadTask &t : tasks) {
        // 同一个目标文件可能被清单里多条记录指向（老版本 natives、重复 artifact 等），
        // 多线程同时写同一个 .part 会互相截断，这里按路径去重。
        const QString key = QDir::cleanPath(t.dest);
        if (m_seenDests.contains(key)) {
            emit message(QStringLiteral("[%1] 与已有任务目标相同，跳过重复项").arg(t.label));
            continue;
        }
        m_seenDests.insert(key);
        // 目录一律在主线程先建好：多个 worker 并发 mkpath 同一棵目录树时，
        // Windows 上偶尔会瞬时失败，导致后面 open 报「找不到指定的路径」。
        QDir().mkpath(QFileInfo(t.dest).absolutePath());
        m_queue.push_back(t);
        m_stats.totalBytes += qMax<qint64>(0, t.size);
        m_stats.totalFiles += 1;
    }
    emitStats();
}

void DownloadManager::start()
{
    if (m_workers.isEmpty())
        buildPool(16);
    if (!m_running) {
        m_running = true;
        if (m_stats.startedMs == 0)
            m_stats.startedMs = QDateTime::currentMSecsSinceEpoch();
        m_lastTickMs = QDateTime::currentMSecsSinceEpoch();
        m_lastBytes = m_stats.doneBytes;
        m_tick->start();
    }
    m_paused = false;
    emitStats();
    dispatch();
}

void DownloadManager::pause()
{
    if (!m_running || m_paused)
        return;
    m_paused = true;
    m_tick->stop();
    m_stats.bytesThisSecond = 0;
    for (const Slot &slot : m_workers) {
        if (slot.currentId != 0)
            QMetaObject::invokeMethod(slot.worker, "abortCurrent", Qt::QueuedConnection);
    }
    emitStats();
    emit message(QStringLiteral("已暂停"));
}

void DownloadManager::cancel()
{
    stopAll(true);
}

// 真正停下来的部分；notify=false 用于「换版本重新开始」时的静默重置，
// 免得日志里凭空冒出一句「已取消」。
void DownloadManager::stopAll(bool notify)
{
    m_running = false;
    m_paused = false;
    m_queue.clear();
    m_tick->stop();
    m_stats.bytesThisSecond = 0;
    for (const Slot &slot : m_workers) {
        if (slot.currentId != 0)
            QMetaObject::invokeMethod(slot.worker, "abortCurrent", Qt::QueuedConnection);
    }
    if (notify)
        emit message(QStringLiteral("已取消"));
    emitStats();
}

void DownloadManager::reset()
{
    stopAll(false);
    m_seenDests.clear();
    m_inflight.clear();
    m_bytes.clear();
    m_baseBytes = 0;
    m_permDenied = 0;
    m_stats = DownloadStats();
    emitStats();
}

void DownloadManager::dispatch()
{
    if (!m_running || m_paused)
        return;

    for (Slot &slot : m_workers) {
        if (slot.currentId != 0)
            continue;
        if (m_queue.empty())
            break;

        const DownloadTask task = m_queue.front();
        m_queue.pop_front();

        const int id = m_nextId++;
        slot.currentId = id;
        m_inflight.insert(id, task);
        m_bytes.insert(id, 0);
        QMetaObject::invokeMethod(slot.worker, "assign", Qt::QueuedConnection,
                                  Q_ARG(int, id), Q_ARG(md::DownloadTask, task));
    }
    maybeDrained();
}

void DownloadManager::onWorkerFinished(int id, const md::TaskResult &result)
{
    const int slotIdx = slotOf(id);
    if (slotIdx >= 0)
        m_workers[slotIdx].currentId = 0;

    const qint64 received = m_bytes.take(id);
    m_inflight.remove(id);

    switch (result.state) {
    case TaskState::Done:
        m_stats.doneFiles += 1;
        m_baseBytes += received > 0 ? received : qMax<qint64>(0, result.task.size);
        break;
    case TaskState::Skipped:
        m_stats.doneFiles += 1;
        m_stats.skippedFiles += 1;
        m_baseBytes += qMax<qint64>(0, result.task.size);
        break;
    case TaskState::Failed:
        m_stats.failedFiles += 1;
        if (looksLikePermissionProblem(result.error))
            ++m_permDenied;
        break;
    default:
        break;
    }

    emit taskFinished(result);
    emitStats();
    dispatch();
}

void DownloadManager::onWorkerInterrupted(int id, const md::DownloadTask &task)
{
    const int slotIdx = slotOf(id);
    if (slotIdx >= 0)
        m_workers[slotIdx].currentId = 0;

    m_inflight.remove(id);
    m_bytes.remove(id);

    if (m_running && m_paused) {
        // 暂停：任务放回队首，继续时重下（.part 已保留）
        m_queue.push_front(task);
    }
    emitStats();
    dispatch();
}

void DownloadManager::onWorkerProgress(int id, qint64 received, qint64 total)
{
    if (!m_bytes.contains(id))
        return;
    m_bytes[id] = received;
    qint64 inFlight = 0;
    for (auto it = m_bytes.cbegin(); it != m_bytes.cend(); ++it)
        inFlight += it.value();
    m_stats.doneBytes = m_baseBytes + inFlight;

    const auto it = m_inflight.constFind(id);
    if (it != m_inflight.cend())
        emit taskProgress(it.value().dest, received, total);

    emitStats();
}

void DownloadManager::maybeDrained()
{
    if (!m_running)
        return;
    if (!m_queue.empty())
        return;
    for (const Slot &slot : m_workers) {
        if (slot.currentId != 0)
            return;
    }

    m_running = false;
    m_paused = false;
    m_tick->stop();
    m_stats.bytesThisSecond = 0;
    if (m_pendingConcurrency > 0) {
        const int n = m_pendingConcurrency;
        m_pendingConcurrency = 0;
        buildPool(n);
    }
    emitStats();
    emit drained();
}

void DownloadManager::emitStats()
{
    m_stats.doneFiles = qMin(m_stats.doneFiles, m_stats.totalFiles);
    emit statsChanged(m_stats);
}

} // namespace md
