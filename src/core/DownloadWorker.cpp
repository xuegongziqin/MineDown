#include "core/DownloadWorker.h"

#include "core/Sha1.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>
#include <QUrl>

namespace md {

namespace {
constexpr int kMaxAttempts = 4;      // 首次 + 3 次重试
constexpr int kProgressIntervalMs = 100;
constexpr int kTransferTimeoutMs = 45000;

// 大文件切段并行下载的参数。实测单连接吞吐被压得很低（官方源 0.04 MB/s，
// BMCLAPI 高峰也在 0.5 MB/s 上下），同时开 4 段能到 1 MB/s 以上。
constexpr qint64 kChunkThresholdBytes = 8LL * 1024 * 1024;   // 8 MiB 以上才切段
constexpr qint64 kChunkTargetBytes = 4LL * 1024 * 1024;      // 每段目标 4 MiB
constexpr int kMaxChunksPerFile = 4;

constexpr int kRateLimitBaseDelayMs = 5000;                  // 429 的基础退避
constexpr int kMaxRedirects = 8;                             // 手动跟随跳转的上限

// 「拒绝访问」到底是哪一种：权限错误，还是纯粹的临时占用。
// QFile::rename 失败时报的是 OpenError（"Cannot open for output: 拒绝访问。"），
// 不是 PermissionsError，所以还得看错误文本。
bool isAccessDenied(QFileDevice &file)
{
    if (file.error() == QFileDevice::PermissionsError)
        return true;
    const QString s = file.errorString();
    return s.contains(QStringLiteral("拒绝访问"))
        || s.contains(QStringLiteral("Access is denied"), Qt::CaseInsensitive)
        || s.contains(QStringLiteral("Permission denied"), Qt::CaseInsensitive);
}
} // namespace

DownloadWorker::DownloadWorker(int index, QObject *parent)
    : QObject(parent), m_index(index)
{
}

DownloadWorker::~DownloadWorker()
{
    closePartFile();
    cleanupReply();
    cleanupChunks();
}

void DownloadWorker::init()
{
    if (m_nam)
        return;
    m_nam = new QNetworkAccessManager(this);
    m_nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    m_nam->setTransferTimeout(kTransferTimeoutMs);
}

void DownloadWorker::assign(int id, const md::DownloadTask &task)
{
    m_id = id;
    m_task = task;
    m_attempt = 0;
    m_busy = true;
    m_abortRequested = false;
    m_writeError.clear();
    m_requestedRange = false;
    m_haveOnDisk = 0;
    m_written = 0;
    m_total = task.size;
    m_lastProgressMs = 0;
    m_partPath = task.dest + QStringLiteral(".part");

    m_chunks.clear();
    m_activeChunks = 0;
    m_chunked = false;
    m_chunkingUnsupported = false;
    m_retryDelayOverride = 0;
    m_redirects = 0;
    m_partVerified = false;
    m_destBlocked = false;

    QDir().mkpath(QFileInfo(task.dest).absolutePath());
    // 1) 本地已有成品文件：校验通过就跳过（跨版本复用资源时这是最省时的一步）
    if (QFile::exists(task.dest)) {
        QString why;
        if (verifyFile(task.dest, task.sha1, task.size, &why)) {
            complete(TaskState::Skipped);
            return;
        }
        // 校验没过（内容不对，或者根本读不动）就删掉重下。删不掉的情况在从旧系统 /
        // 备份拷过来的 .minecraft 里很常见：那批文件属于别的账户 SID，当前账户
        // 既不能读也不能删。这时硬着头皮下载只会在最后改名时失败，白下一整份。
        QFile existing(task.dest);
        if (!existing.remove() && existing.exists()) {
            complete(TaskState::Failed,
                     QStringLiteral("目标文件已存在但无法删除（%1）：%2\n"
                                    "该文件多半属于其它账户或受系统保护，"
                                    "需要用管理员权限修复目录的所有权/权限。")
                         .arg(existing.errorString(), task.dest));
            return;
        }
    }

    // 2) .part 是否已经完整、目标文件改名是否被占用，都在 startAttempt 里判断：
    //    它能保证「已完整的 .part」不会被重复下载。
    startAttempt();
}

void DownloadWorker::startAttempt()
{
    if (!m_nam)
        init();
    if (!m_nam || !m_busy)
        return;

    m_attempt += 1;
    m_requestedRange = false;
    m_written = 0;
    m_partVerified = false;
    // 期望大小未知时，m_total 是上一次尝试用 Content-Length 猜的；换了响应就必须重新猜，
    // 否则会拿错误页的长度（比如 124 字节）去比对真正的文件。
    if (m_task.size < 0)
        m_total = -1;

    // .part 已经是完整而正确的文件（上次改名被占用打断了）：只重试改名，别重下。
    const qint64 partSize = QFileInfo(m_partPath).size();
    if (m_task.size >= 0 && partSize == m_task.size) {
        QString why;
        if (verifyFile(m_partPath, m_task.sha1, m_task.size, &why)) {
            m_partVerified = true;
            if (promotePartFile(&why)) {
                emitProgress(m_task.size);
                complete(TaskState::Done);
                return;
            }
            // 权限问题（目标文件属于别的账户、删不掉也写不进）重试多少次都一样，
            // 直接如实失败；只有「被临时占用」才值得再等一轮。
            fail(QStringLiteral("写入目标文件失败：%1").arg(why), !m_destBlocked, false);
            return;
        }
        QFile::remove(m_partPath);
    }

    const qint64 have = QFileInfo(m_partPath).size();
    // 只有知道期望大小、且已有部分才续传；否则从头写。
    const bool resume = have > 0 && (m_task.size < 0 || have < m_task.size);

    // 首次尝试且文件够大：切段并行。暂停过后（有 .part）就走单连接续传。
    if (!resume && !m_chunkingUnsupported && m_attempt == 1 && m_task.size >= kChunkThresholdBytes) {
        startChunked();
        return;
    }

    m_haveOnDisk = resume ? have : 0;

    m_file = new QFile(m_partPath, this);
    bool opened = m_file->open(resume ? (QIODevice::ReadWrite)
                                      : (QIODevice::WriteOnly | QIODevice::Truncate));
    if (!opened) {
        // 目录可能在主线程建好之后才被清掉，或者并发建目录时失败：再试一次
        QDir().mkpath(QFileInfo(m_partPath).absolutePath());
        opened = m_file->open(resume ? (QIODevice::ReadWrite)
                                     : (QIODevice::WriteOnly | QIODevice::Truncate));
    }
    if (!opened) {
        const QString err = QStringLiteral("无法写入 %1：%2").arg(m_partPath, m_file->errorString());
        delete m_file;
        m_file = nullptr;
        complete(TaskState::Failed, err);
        return;
    }
    if (resume)
        m_file->seek(have);

    QNetworkRequest req{QUrl(m_task.url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MineDown/0.1.0"));
    req.setRawHeader("Accept-Encoding", "identity");
    if (m_haveOnDisk > 0) {
        req.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(m_haveOnDisk) + '-');
        m_requestedRange = true;
    }

    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::metaDataChanged, this, &DownloadWorker::onMetaDataChanged);
    connect(m_reply, &QNetworkReply::readyRead, this, &DownloadWorker::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &DownloadWorker::onReplyFinished);

    if (m_haveOnDisk > 0)
        emitProgress(m_total);
}

void DownloadWorker::startChunked()
{
    if (!m_nam)
        init();
    if (!m_nam || !m_busy)
        return;

    m_chunked = true;
    m_haveOnDisk = 0;
    m_written = 0;

    m_file = new QFile(m_partPath, this);
    bool opened = m_file->open(QIODevice::WriteOnly | QIODevice::Truncate);
    if (!opened) {
        QDir().mkpath(QFileInfo(m_partPath).absolutePath());
        opened = m_file->open(QIODevice::WriteOnly | QIODevice::Truncate);
    }
    if (!opened) {
        const QString err = QStringLiteral("无法写入 %1：%2").arg(m_partPath, m_file->errorString());
        delete m_file;
        m_file = nullptr;
        m_chunked = false;
        complete(TaskState::Failed, err);
        return;
    }
    // 预分配：每段按自己的偏移直接写，不需要彼此等待
    if (!m_file->resize(m_task.size)) {
        const QString err = QStringLiteral("无法分配磁盘空间（需要 %1 字节）：%2")
                                .arg(m_task.size)
                                .arg(m_file->errorString());
        closePartFile();
        m_chunked = false;
        QFile::remove(m_partPath);
        complete(TaskState::Failed, err);
        return;
    }

    int count = int((m_task.size + kChunkTargetBytes - 1) / kChunkTargetBytes);
    if (count < 1)
        count = 1;
    if (count > kMaxChunksPerFile)
        count = kMaxChunksPerFile;

    m_chunks.clear();
    m_chunks.resize(count);
    const qint64 per = m_task.size / count;
    for (int i = 0; i < count; ++i) {
        m_chunks[i].start = i * per;
        m_chunks[i].end = (i == count - 1) ? (m_task.size - 1) : ((i + 1) * per - 1);
    }

    emit logLine(QStringLiteral("[%1] 分 %2 段并行下载（共 %3）")
                     .arg(m_task.label)
                     .arg(count)
                     .arg(QString::number(m_task.size)));

    for (int i = 0; i < count; ++i)
        startChunk(i);
}

void DownloadWorker::startChunk(int index)
{
    if (!m_busy || !m_nam || index < 0 || index >= m_chunks.size())
        return;

    Chunk &c = m_chunks[index];
    if (c.reply)
        return;

    const qint64 from = c.start + c.written;
    if (from > c.end)
        return;

    QNetworkRequest req{QUrl(m_task.url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MineDown/0.1.0"));
    req.setRawHeader("Accept-Encoding", "identity");

    const bool wholeFile = (from == 0 && c.end == m_task.size - 1);
    if (!wholeFile) {
        req.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(from)
                                      + '-' + QByteArray::number(c.end));
    }

    QNetworkReply *reply = m_nam->get(req);
    c.reply = reply;
    ++m_activeChunks;

    connect(reply, &QNetworkReply::readyRead, this, [this, index]() { onChunkReadyRead(index); });
    connect(reply, &QNetworkReply::finished, this, [this, index]() { onChunkFinished(index); });
}

void DownloadWorker::onChunkReadyRead(int index)
{
    if (!m_file || index < 0 || index >= m_chunks.size())
        return;
    Chunk &c = m_chunks[index];
    if (!c.reply)
        return;

    const int status = c.reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool wholeFile = (c.end == m_task.size - 1);
    // 4xx/5xx 是错误页；200 说明服务器没理 Range，两种情况都不该往 .part 里写
    if (status >= 400 || (status == 200 && !wholeFile)) {
        c.reply->readAll();
        return;
    }

    const QByteArray data = c.reply->readAll();
    if (data.isEmpty())
        return;

    m_file->seek(c.start + c.written);
    qint64 off = 0;
    while (off < data.size()) {
        const qint64 n = m_file->write(data.constData() + off, data.size() - off);
        if (n <= 0) {
            m_writeError = QStringLiteral("写入失败：%1").arg(m_file->errorString());
            c.reply->abort(); // 走 onChunkFinished 统一上报
            return;
        }
        off += n;
    }
    c.written += off;
    emitProgress(m_task.size);
}

void DownloadWorker::onChunkFinished(int index)
{
    if (index < 0 || index >= m_chunks.size())
        return;
    Chunk &c = m_chunks[index];
    QNetworkReply *reply = c.reply;
    if (!reply)
        return;

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError nerr = reply->error();
    const QString errText = reply->errorString();

    // 收尾：把残余数据写完
    if (m_file && reply->isOpen() && status < 400) {
        const QByteArray rest = reply->readAll();
        if (!rest.isEmpty()) {
            m_file->seek(c.start + c.written);
            const qint64 n = m_file->write(rest);
            if (n > 0)
                c.written += n;
        }
    }

    reply->disconnect(this);
    reply->deleteLater();
    c.reply = nullptr;
    --m_activeChunks;

    // 暂停：等所有段都停下来再上报，避免把 .part 写坏
    if (m_abortRequested) {
        if (m_activeChunks > 0)
            return;
        closePartFile();
        QFile::remove(m_partPath); // 分段数据不完整，续传无从下手，直接重来
        m_chunks.clear();
        m_chunked = false;
        const int id = m_id;
        m_busy = false;
        m_id = -1;
        m_abortRequested = false;
        emit interrupted(id, m_task);
        return;
    }

    if (!m_writeError.isEmpty()) {
        const QString err = m_writeError;
        fail(err, true, true);
        return;
    }

    const qint64 need = c.end - c.start + 1;
    const bool ok = (nerr == QNetworkReply::NoError) && status >= 200 && status < 300
                    && c.written == need;
    if (ok) {
        if (chunksAllDone())
            finishTransfer();
        return;
    }

    // 服务器忽略 Range：整份文件退回单连接下载
    const bool wholeFile = (c.start == 0 && c.end == m_task.size - 1);
    if (status == 200 && !wholeFile) {
        m_chunkingUnsupported = true;
        fail(QStringLiteral("服务器忽略 Range 请求，改用单连接下载"), true, true);
        return;
    }

    // 4xx 是资源真不存在，重试没意义
    if (status == 401 || status == 403 || status == 404 || status == 410) {
        fail(QStringLiteral("HTTP %1 %2").arg(status).arg(errText), false, true);
        return;
    }

    c.attempts += 1;
    if (c.attempts >= kMaxAttempts) {
        fail(QStringLiteral("HTTP %1 %2").arg(status).arg(errText), true, true);
        return;
    }

    qint64 delay = 1000 * (1 << (c.attempts - 1));
    if (status == 429) {
        delay = kRateLimitBaseDelayMs * c.attempts;
        const QByteArray ra = reply->rawHeader("Retry-After");
        if (!ra.isEmpty()) {
            bool numberOk = false;
            const int secs = ra.toInt(&numberOk);
            if (numberOk && secs > 0)
                delay = qMin<qint64>(secs, 60) * 1000;
        }
    }
    emit logLine(QStringLiteral("[%1] 第 %2 段失败（HTTP %3），%4 ms 后重试")
                     .arg(m_task.label)
                     .arg(index + 1)
                     .arg(status)
                     .arg(delay));
    QTimer::singleShot(int(delay), this, [this, index]() {
        if (m_busy && !m_abortRequested && m_chunked)
            startChunk(index);
    });
}

bool DownloadWorker::chunksAllDone() const
{
    if (m_chunks.isEmpty())
        return false;
    for (const Chunk &c : m_chunks) {
        if (c.reply)
            return false;
        if (c.written != c.end - c.start + 1)
            return false;
    }
    return true;
}

qint64 DownloadWorker::chunksReceived() const
{
    qint64 sum = 0;
    for (const Chunk &c : m_chunks)
        sum += c.written;
    return sum;
}

void DownloadWorker::onMetaDataChanged()
{
    if (!m_reply)
        return;
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // 302/404 之类也会触发 metaDataChanged，它们的 Content-Length 是跳转页/错误页的
    // 长度（BMCLAPI 的 302 正文只有百来字节），拿它当文件大小会一直校验失败。
    if (status != 200 && status != 206)
        return;

    // 服务器不支持 Range，返回了 200 全量：必须清空 .part 从头写，
    // 否则会把整个文件追加到旧数据后面。
    if (m_requestedRange && status == 200) {
        if (m_file) {
            m_file->resize(0);
            m_file->seek(0);
        }
        m_haveOnDisk = 0;
        m_written = 0;
        m_requestedRange = false;
        logLine(QStringLiteral("服务器忽略 Range，改为完整下载：%1").arg(m_task.label));
    }

    // 期望大小未知时，用响应头里的 Content-Length 推算总量
    if (m_total < 0) {
        const qint64 len = m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (len > 0)
            m_total = m_haveOnDisk + len;
    }
}

void DownloadWorker::onReadyRead()
{
    if (!m_reply || !m_file)
        return;
    // 4xx/5xx 的响应体是错误页（HTML/文本），绝不能写进 .part
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // 非 2xx 的响应体是错误页/跳转页，一律丢弃，绝不写进 .part
    if (status < 200 || status >= 300) {
        m_reply->readAll();
        return;
    }
    const QByteArray chunk = m_reply->readAll();
    if (chunk.isEmpty())
        return;
    const qint64 n = m_file->write(chunk);
    if (n < 0) {
        m_writeError = QStringLiteral("写入失败：%1").arg(m_file->errorString());
        m_reply->abort(); // 走 onReplyFinished 统一上报
        return;
    }
    m_written += n;
    emitProgress(m_total);
}

void DownloadWorker::emitProgress(qint64 total)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastProgressMs != 0 && now - m_lastProgressMs < kProgressIntervalMs)
        return;
    m_lastProgressMs = now;
    const qint64 done = m_chunked ? chunksReceived() : (m_haveOnDisk + m_written);
    emit progress(m_id, done, total);
}

void DownloadWorker::onReplyFinished()
{
    if (!m_reply)
        return;

    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError nerr = m_reply->error();
    const QString errText = m_reply->errorString();
    const QByteArray retryAfter = m_reply->rawHeader("Retry-After");
    const QByteArray location = m_reply->rawHeader("Location");
    const QUrl replyUrl = m_reply->url();

    // 把残余数据写完（只对 2xx；跳转页/错误页的正文不能落到 .part 里）
    if (m_file && m_reply->isOpen()) {
        const QByteArray rest = m_reply->readAll();
        if (!rest.isEmpty() && status >= 200 && status < 300) {
            const qint64 n = m_file->write(rest);
            if (n > 0)
                m_written += n;
        }
    }

    const bool aborted = m_abortRequested;
    const QString writeError = m_writeError;
    cleanupReply();

    if (!writeError.isEmpty()) {
        fail(writeError, true, true);
        return;
    }

    if (aborted) {
        closePartFile();
        const int id = m_id;
        m_busy = false;
        m_id = -1;
        m_abortRequested = false;
        emit interrupted(id, m_task);
        return;
    }

    if (nerr == QNetworkReply::NoError && status >= 200 && status < 300) {
        closePartFile();
        finishTransfer();
        return;
    }

    // 服务器给的 302 链有时会被 Qt 的重定向策略挡住（https 链、跳转页正文很小），
    // 这时我们自己跟一跳：保留 .part，下一轮用 Range 接着下。
    if (status >= 300 && status < 400 && !location.isEmpty() && m_redirects < kMaxRedirects) {
        const QUrl target = replyUrl.resolved(QUrl(QString::fromUtf8(location)));
        const QString scheme = target.scheme().toLower();
        if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) {
            ++m_redirects;
            m_task.url = target.toString();
            closePartFile();
            QTimer::singleShot(0, this, [this]() {
                if (m_busy && !m_abortRequested)
                    startAttempt();
            });
            return;
        }
    }

    // 镜像不一定覆盖全部文件（例如 BMCLAPI 就缺日志配置）：任何非 2xx 都先试着
    // 换成官方地址再来一次，官方源拿不到才算真失败。
    const bool httpFailure = (nerr != QNetworkReply::NoError) || status < 200 || status >= 300;
    if (httpFailure && !m_task.fallbackUrl.isEmpty() && m_task.url != m_task.fallbackUrl) {
        m_task.url = m_task.fallbackUrl;
        m_task.fallbackUrl.clear();
        m_attempt = 0;
        m_total = m_task.size;
        m_redirects = 0;
        closePartFile();
        QFile::remove(m_partPath);
        emit logLine(QStringLiteral("[%1] 镜像不可用（HTTP %2 %3），改用官方源：%4")
                         .arg(m_task.label)
                         .arg(status)
                         .arg(errText)
                         .arg(QUrl(m_task.url).host()));
        QTimer::singleShot(0, this, [this]() {
            if (m_busy && !m_abortRequested)
                startAttempt();
        });
        return;
    }

    // 4xx 一般是资源真的不存在，重试没有意义；同时把可能写坏的分片删掉
    bool retryable = true;
    bool discardPartial = status >= 400;
    if (status == 401 || status == 403 || status == 404 || status == 410)
        retryable = false;

    if (status == 429) {
        // 被限流了：退避要长一点，别把限流越拖越久
        qint64 delay = kRateLimitBaseDelayMs * qMax(1, m_attempt);
        if (!retryAfter.isEmpty()) {
            bool numberOk = false;
            const int secs = retryAfter.toInt(&numberOk);
            if (numberOk && secs > 0)
                delay = qMin<qint64>(secs, 60) * 1000;
        }
        m_retryDelayOverride = delay;
        discardPartial = false;
    }

    fail(QStringLiteral("HTTP %1 %2").arg(status).arg(errText), retryable, discardPartial);
}

void DownloadWorker::finishTransfer()
{
    if (m_file) {
        m_file->flush();
        closePartFile();
    }
    if (m_chunked)
        m_written = chunksReceived();

    QString why;
    if (m_total >= 0) {
        const qint64 got = QFileInfo(m_partPath).size();
        if (got != m_total) {
            fail(QStringLiteral("大小不符（收到 %1 / 期望 %2）").arg(got).arg(m_total), true, true);
            return;
        }
    }
    if (!verifyFile(m_partPath, m_task.sha1, m_task.size, &why)) {
        fail(QStringLiteral("校验失败：%1").arg(why), true, true);
        return;
    }
    // .part 已经是正确的完整文件，但改名可能被占用（杀毒/索引器/别的进程正在读目标）
    // 拦住。这时**不要丢掉 .part**：下一次尝试会直接从「已完整的 .part」重新改名，
    // 不必把整个文件再下一遍。fail() 靠 m_partVerified 记住这件事。
    m_partVerified = true;
    if (!promotePartFile(&why)) {
        fail(QStringLiteral("写入目标文件失败：%1").arg(why), !m_destBlocked, false);
        return;
    }
    m_chunks.clear();
    m_chunked = false;
    emitProgress(m_task.size);
    complete(TaskState::Done);
}

void DownloadWorker::fail(const QString &err, bool retryable, bool discardPartial)
{
    closePartFile();
    cleanupReply();
    cleanupChunks();

    // 分段失败后退回单连接重试，避免在同一个坑里反复切段
    if (m_chunked)
        m_chunkingUnsupported = true;
    m_chunked = false;
    m_activeChunks = 0;
    m_chunks.clear();

    // 从第二次失败起丢掉分片、整份重下：连接被中途掐断后，继续在旧分片上追加
    // 有时会反复落在同一个坏区间上，干脆重来更稳。
    // 例外：已经通过 SHA1 的 .part 只剩「改名」这一步，删掉就得整份重下，不能丢。
    if (m_partVerified) {
        emit logLine(QStringLiteral("[%1] 保留已校验的 .part，下次只重试写入目标文件")
                         .arg(m_task.label));
    } else if (discardPartial || m_attempt >= 2) {
        QFile::remove(m_partPath);
    }

    if (!retryable || m_attempt >= kMaxAttempts) {
        complete(TaskState::Failed, err);
        return;
    }

    // 指数退避：1s, 2s, 4s ...（限流时由 m_retryDelayOverride 给出更长的时间）
    const int delay = int(m_retryDelayOverride > 0 ? m_retryDelayOverride
                                                   : (1000 * (1 << (m_attempt - 1))));
    m_retryDelayOverride = 0;
    emit logLine(QStringLiteral("[%1] 第 %2 次失败，%3 ms 后重试：%4")
                     .arg(m_task.label)
                     .arg(m_attempt)
                     .arg(delay)
                     .arg(err));

    QTimer::singleShot(delay, this, [this]() {
        if (!m_busy)
            return;
        if (m_abortRequested) {
            const int id = m_id;
            m_busy = false;
            m_id = -1;
            m_abortRequested = false;
            emit interrupted(id, m_task);
            return;
        }
        startAttempt();
    });
}

void DownloadWorker::abortCurrent()
{
    if (!m_busy)
        return;
    m_abortRequested = true;

    if (m_chunked) {
        if (m_activeChunks > 0) {
            // abort() 会同步触发 finished -> onChunkFinished，最后一段收尾时会
            // m_chunks.clear()。所以不能一边遍历 m_chunks 一边 abort：先把 reply
            // 指针抄出来，再逐个中断，并在中途检查任务是否已经结束。
            QVector<QNetworkReply *> replies;
            replies.reserve(m_chunks.size());
            for (const Chunk &c : m_chunks) {
                if (c.reply)
                    replies.append(c.reply);
            }
            for (QNetworkReply *reply : replies) {
                if (!m_busy)
                    return; // 最后一段的收尾已经上报过 interrupted
                reply->abort();
            }
            return;
        }
        closePartFile();
        QFile::remove(m_partPath);
        m_chunks.clear();
        m_chunked = false;
    } else if (m_reply) {
        m_reply->abort(); // 同步触发 finished -> onReplyFinished 里发 interrupted
        return;
    }

    // 处于重试等待期，没有在途请求：直接中断
    closePartFile();
    const int id = m_id;
    m_busy = false;
    m_id = -1;
    m_abortRequested = false;
    emit interrupted(id, m_task);
}

void DownloadWorker::cleanupReply()
{
    if (!m_reply)
        return;
    m_reply->disconnect(this);
    m_reply->deleteLater();
    m_reply = nullptr;
}

void DownloadWorker::cleanupChunks()
{
    for (Chunk &c : m_chunks) {
        if (!c.reply)
            continue;
        c.reply->disconnect(this);
        c.reply->abort();
        c.reply->deleteLater();
        c.reply = nullptr;
    }
    m_activeChunks = 0;
}

void DownloadWorker::closePartFile()
{
    if (!m_file)
        return;
    if (m_file->isOpen()) {
        m_file->flush();
        m_file->close();
    }
    delete m_file;
    m_file = nullptr;
}

bool DownloadWorker::promotePartFile(QString *err)
{
    // Windows 上新写好的文件常被杀毒/搜索索引/云同步短暂占用，rename 会失败
    // （ERROR_ACCESS_DENIED / ERROR_SHARING_VIOLATION）。这里退避重试约 6 秒，
    // 最后再用「直接写目标文件」兜底 —— 不用 QFile::copy，因为目标已存在时它会失败。
    QString lastError;
    QString blockedReason;
    bool blocked = false;
    m_destBlocked = false;

    for (int i = 0; i < 60; ++i) {
        if (QFile::exists(m_task.dest)) {
            QFile existing(m_task.dest);
            existing.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser
                                    | QFile::WriteUser);
            if (!existing.remove()) {
                lastError = QStringLiteral("目标文件已存在且无法删除：%1")
                                .arg(existing.errorString());
                // 拒绝访问不是「等一会儿就好」的临时占用：文件属于别的账户 SID
                // （从旧系统 / 备份拷来的 .minecraft 里一批一批地出现）或者被系统
                // 当成受保护资源。这种情况下等 6 秒毫无意义，立刻如实上报。
                if (isAccessDenied(existing)) {
                    blocked = true;
                    blockedReason = existing.errorString();
                    break;
                }
                QThread::msleep(100);
                continue;
            }
        }
        QFile part(m_partPath);
        if (part.rename(m_task.dest))
            return true;
        lastError = part.errorString();
        // 改名被拒（目录不许新建文件、目标被占用且不可替换）同样不是临时问题，
        // 既不空转 6 秒，也不该再让上层重试 4 次。
        if (isAccessDenied(part)) {
            blocked = true;
            blockedReason = lastError;
            break;
        }
        QThread::msleep(100);
    }

    // 兜底：把 .part 的内容直接写成目标文件（rename 被拦时这条路通常还能走）。
    // 按 64 KiB 分块拷贝，不能 readAll()：这里动辄是上百 MB 的 jar。
    bool copied = false;
    if (!blocked) {
        QFile src(m_partPath);
        QFile dst(m_task.dest);
        if (!src.open(QIODevice::ReadOnly)) {
            if (isAccessDenied(src)) {
                blocked = true;
                blockedReason = src.errorString();
            }
        } else if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (isAccessDenied(dst)) {
                blocked = true;
                blockedReason = dst.errorString();
            }
        } else {
            bool ok = true;
            char buf[64 * 1024];
            for (;;) {
                const qint64 n = src.read(buf, sizeof(buf));
                if (n < 0) {
                    ok = false;
                    break;
                }
                if (n == 0)
                    break;
                if (dst.write(buf, n) != n) {
                    ok = false;
                    break;
                }
            }
            if (ok && dst.flush()) {
                dst.close();
                const qint64 got = QFileInfo(m_task.dest).size();
                copied = (m_task.size < 0 || got == m_task.size);
            }
            if (!ok && isAccessDenied(dst)) {
                blocked = true;
                blockedReason = dst.errorString();
            }
            if (dst.isOpen())
                dst.close();
            src.close();
        }
        if (copied) {
            QFile::remove(m_partPath);
            return true;
        }
        // 兜底也失败时就先留着：目标文件可能是写了一半的残文件，
        // 下一次 assign() 校验不过会把它删掉重下。
    }

    if (err) {
        if (blocked) {
            *err = QStringLiteral("目标文件 %1 无法替换（%2）。该文件既不能写入也不能删除，"
                                  "通常属于其它账户或受系统保护，"
                                  "需要用管理员权限修复目录的所有权/权限")
                       .arg(QFileInfo(m_task.dest).fileName(), blockedReason);
        } else {
            *err = QStringLiteral("无法把 %1.part 重命名为目标文件（%2；.part %3 字节）")
                       .arg(QFileInfo(m_task.dest).fileName(),
                            lastError.isEmpty() ? QStringLiteral("原因未知") : lastError)
                       .arg(QFileInfo(m_partPath).size());
        }
    }
    m_destBlocked = blocked;
    return false;
}

void DownloadWorker::complete(TaskState state, const QString &err)
{
    closePartFile();
    cleanupReply();
    cleanupChunks();
    m_chunks.clear();
    m_chunked = false;

    TaskResult r;
    r.task = m_task;
    r.state = state;
    r.error = err;
    r.downloaded = m_written;
    r.attempts = m_attempt;

    const int id = m_id;
    m_busy = false;
    m_id = -1;
    m_abortRequested = false;
    emit finished(id, r);
}

} // namespace md
