#include "core/HttpFetcher.h"

#include "core/Sha1.h"

#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace md {

namespace {
constexpr int kMaxAttempts = 3;
} // namespace

HttpFetcher::HttpFetcher(QObject *parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
    m_nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    m_nam->setTransferTimeout(30000);
}

HttpFetcher::~HttpFetcher()
{
    abort();
}

void HttpFetcher::get(const QUrl &url, const QString &expectSha1, const QUrl &fallbackUrl)
{
    if (m_reply) {
        m_aborted = true;
        m_reply->abort();
    }
    m_url = url;
    m_fallbackUrl = fallbackUrl;
    m_sha1 = expectSha1;
    m_attempt = 0;
    m_aborted = false;
    m_buffer.clear();
    startAttempt();
}

void HttpFetcher::abort()
{
    m_aborted = true;
    if (m_reply) {
        m_reply->abort();
        m_reply = nullptr;
    }
}

void HttpFetcher::startAttempt()
{
    m_attempt += 1;
    m_buffer.clear();

    QNetworkRequest req(m_url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MineDown/0.1.0"));
    req.setRawHeader("Accept-Encoding", "identity");

    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        if (m_reply)
            m_buffer.append(m_reply->readAll());
    });
    connect(m_reply, &QNetworkReply::finished, this, &HttpFetcher::onFinished);
}

void HttpFetcher::onFinished()
{
    if (!m_reply)
        return;

    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError nerr = m_reply->error();
    const QString errText = m_reply->errorString();
    m_buffer.append(m_reply->readAll());

    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();

    if (m_aborted)
        return;

    if (nerr == QNetworkReply::NoError && status >= 200 && status < 300) {
        if (!m_sha1.isEmpty()) {
            const QString actual =
                QString::fromLatin1(QCryptographicHash::hash(m_buffer, QCryptographicHash::Sha1)
                                        .toHex());
            if (actual.compare(m_sha1, Qt::CaseInsensitive) != 0) {
                if (!m_fallbackUrl.isEmpty() && m_fallbackUrl != m_url) {
                    m_url = m_fallbackUrl;
                    m_fallbackUrl = QUrl();
                    m_attempt = 0;
                    QTimer::singleShot(0, this, [this]() {
                        if (!m_aborted)
                            startAttempt();
                    });
                    return;
                }
                if (m_attempt >= kMaxAttempts) {
                    emit error(QStringLiteral("SHA1 校验失败：%1").arg(m_url.toString()));
                    return;
                }
                QTimer::singleShot(500, this, [this]() {
                    if (!m_aborted)
                        startAttempt();
                });
                return;
            }
        }
        emit done(m_buffer, m_url);
        m_buffer.clear();
        return;
    }

    bool retryable = true;
    if (status == 401 || status == 403 || status == 404 || status == 410)
        retryable = false;

    // 镜像拿不到（404、5xx、TLS/连接失败）时回退官方地址再试一轮
    if (!m_fallbackUrl.isEmpty() && m_fallbackUrl != m_url) {
        m_url = m_fallbackUrl;
        m_fallbackUrl = QUrl();
        m_attempt = 0;
        QTimer::singleShot(0, this, [this]() {
            if (!m_aborted)
                startAttempt();
        });
        return;
    }

    if (retryable && m_attempt < kMaxAttempts) {
        const int delay = 500 * (1 << (m_attempt - 1));
        QTimer::singleShot(delay, this, [this]() {
            if (!m_aborted)
                startAttempt();
        });
        return;
    }

    emit error(QStringLiteral("请求失败（HTTP %1）：%2 [%3]")
                   .arg(status)
                   .arg(errText, m_url.toString()));
}

} // namespace md
