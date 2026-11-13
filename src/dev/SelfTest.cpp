#include "dev/SelfTest.h"

#include "core/DownloadManager.h"
#include "core/HttpFetcher.h"
#include "core/InstallPipeline.h"
#include "core/ManifestParser.h"
#include "core/Mirror.h"
#include "core/Settings.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

namespace md {

namespace {

const QString kManifestUrl =
    QStringLiteral("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json");

class LogFile
{
public:
    explicit LogFile(const QString &path)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        m_file.setFileName(path);
        if (m_file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            m_stream.setDevice(&m_file);
    }

    void line(const QString &text)
    {
        const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
        const QString s = QStringLiteral("[%1] %2").arg(stamp, text);
        qInfo().noquote() << s;
        if (m_stream.device())
            m_stream << s << '\n';
        if (m_stream.device())
            m_stream.flush();
    }

private:
    QFile m_file;
    QTextStream m_stream;
};

} // namespace

int runSelfTest(const QString &rootDir, const QString &versionId, const QString &logPath)
{
    LogFile log(logPath.isEmpty() ? (rootDir + QStringLiteral("/../minedown-selftest.log"))
                                  : logPath);

    if (rootDir.isEmpty() || versionId.isEmpty()) {
        log.line(QStringLiteral("参数不完整：需要 --root <目录> 与 --version <版本号>"));
        return 2;
    }

    Settings::instance().setRootDir(rootDir);
    log.line(QStringLiteral("自检开始：目录=%1 版本=%2 镜像=%3")
                 .arg(rootDir, versionId, Settings::instance().mirrorBase()));

    DownloadManager dm;
    dm.setConcurrency(Settings::instance().concurrency());
    InstallPipeline pipeline(&dm);
    HttpFetcher fetcher;
    QEventLoop loop;

    int code = 2;
    bool started = false;
    qint64 lastLogMs = 0;

    auto logStats = [&](const DownloadStats &s) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastLogMs < 2000)
            return;
        lastLogMs = now;
        const qint64 planned = pipeline.plannedBytes();
        const int pct = planned > 0 ? static_cast<int>(s.doneBytes * 100 / planned) : 0;
        log.line(QStringLiteral("进度 %1%（%2 / %3）文件 %4/%5 失败 %6 速度 %7 MB/s")
                     .arg(pct)
                     .arg(s.doneBytes)
                     .arg(planned)
                     .arg(s.doneFiles)
                     .arg(pipeline.plannedFiles())
                     .arg(s.failedFiles)
                     .arg(s.speedBytesPerSec() / (1024.0 * 1024.0), 0, 'f', 2));
    };

    QObject::connect(&dm, &DownloadManager::statsChanged, &loop, logStats);
    // 下载线程的日志（重试、分块、回退官方源等）也写进自检日志
    QObject::connect(&dm, &DownloadManager::message, &loop,
                     [&log](const QString &s) { log.line(s); });
    QObject::connect(&dm, &DownloadManager::taskFinished, &loop,
                     [&log](const TaskResult &r) {
                         if (r.state == TaskState::Failed)
                             log.line(QStringLiteral("失败：%1 —— %2")
                                          .arg(r.task.label.isEmpty() ? r.task.dest : r.task.label,
                                               r.error));
                     });

    QObject::connect(&pipeline, &InstallPipeline::logLine, &loop,
                     [&log](const QString &s) { log.line(s); });
    QObject::connect(&pipeline, &InstallPipeline::finished, &loop,
                     [&](bool ok, const QString &summary, bool canceled) {
                         log.line(QStringLiteral("RESULT %1：%2")
                                      .arg(canceled ? QStringLiteral("CANCELED")
                                                    : (ok ? QStringLiteral("OK")
                                                          : QStringLiteral("FAIL")),
                                           summary));
                         code = ok ? 0 : 1;
                         loop.quit();
                     });
    QObject::connect(&pipeline, &InstallPipeline::failed, &loop, [&](const QString &msg) {
        log.line(QStringLiteral("RESULT FAIL：%1").arg(msg));
        code = 1;
        loop.quit();
    });

    QObject::connect(&fetcher, &HttpFetcher::done, &loop,
                     [&](const QByteArray &data, const QUrl &) {
                         VersionManifest manifest;
                         QString err;
                         if (!parseVersionManifest(data, &manifest, &err)) {
                             log.line(QStringLiteral("清单解析失败：%1").arg(err));
                             code = 2;
                             loop.quit();
                             return;
                         }
                         log.line(QStringLiteral("清单加载成功：%1 个版本，最新 %2")
                                      .arg(manifest.versions.size())
                                      .arg(manifest.latestRelease));

                         pipeline.setManifest(manifest);
                         pipeline.setRootDir(rootDir);
                         started = pipeline.start(versionId);
                         if (!started) {
                             log.line(QStringLiteral("启动失败：版本 %1").arg(versionId));
                             code = 2;
                             loop.quit();
                         }
                     });
    QObject::connect(&fetcher, &HttpFetcher::error, &loop, [&](const QString &msg) {
        log.line(QStringLiteral("清单获取失败：%1").arg(msg));
        code = 2;
        loop.quit();
    });

    QTimer::singleShot(60 * 60 * 1000, &loop, [&]() {
        log.line(QStringLiteral("超时（60 分钟）"));
        code = 3;
        loop.quit();
    });

    const QUrl manifestUrl(applyMirror(kManifestUrl, Settings::instance().mirrorBase()));
    fetcher.get(manifestUrl, QString(), QUrl(fallbackForUrl(manifestUrl.toString())));
    loop.exec();

    log.line(QStringLiteral("自检结束，退出码 %1").arg(code));
    return code;
}

} // namespace md
