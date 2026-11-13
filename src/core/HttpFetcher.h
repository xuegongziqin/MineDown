#pragma once

#include <QByteArray>
#include <QObject>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace md {

// 小体积 JSON 的异步抓取器（清单 / 版本 JSON / 资源索引）。
// 批量文件下载不走这里，走 DownloadManager。
class HttpFetcher : public QObject
{
    Q_OBJECT

public:
    explicit HttpFetcher(QObject *parent = nullptr);
    ~HttpFetcher() override;

    void get(const QUrl &url, const QString &expectSha1 = QString(),
             const QUrl &fallbackUrl = QUrl());
    void abort();
    bool busy() const { return m_reply != nullptr; }

signals:
    void done(const QByteArray &data, const QUrl &finalUrl);
    void error(const QString &message);

private:
    void startAttempt();
    void onFinished();

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;
    QUrl m_url;
    QUrl m_fallbackUrl;
    QString m_sha1;
    QByteArray m_buffer;
    int m_attempt = 0;
    bool m_aborted = false;
};

} // namespace md
