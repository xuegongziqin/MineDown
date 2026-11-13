#include "dev/LoaderTest.h"

#include "core/LoaderCatalog.h"
#include "core/LoaderInstaller.h"
#include "core/Settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>

namespace md {
namespace {

class LogFile
{
public:
    explicit LogFile(const QString &path)
    {
        if (path.isEmpty())
            return;
        QDir().mkpath(QFileInfo(path).absolutePath());
        m_file.setFileName(path);
        if (m_file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            m_stream.setDevice(&m_file);
    }

    void line(const QString &text)
    {
        const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
        const QString full = QStringLiteral("[%1] %2").arg(stamp, text);
        if (m_stream.device())
            m_stream << full << '\n' << Qt::flush;
        QTextStream(stdout) << full << '\n' << Qt::flush;
    }

private:
    QFile m_file;
    QTextStream m_stream;
};

} // namespace

int runLoaderTest(const QString &rootDir, const QString &kindKey, const QString &mcVersion,
                  const QString &loaderVersion, const QString &logPath)
{
    if (rootDir.isEmpty() || mcVersion.isEmpty()) {
        QTextStream(stderr) << "用法：--install-loader <fabric|forge|neoforge> --mc <版本> "
                               "--loader <加载器版本> --root <目录> [--log <文件>]\n";
        return 2;
    }

    LoaderKind kind = LoaderKind::Fabric;
    if (!loaderKindFromKey(kindKey, &kind)) {
        QTextStream(stderr) << "未知的加载器：" << kindKey << '\n';
        return 2;
    }

    Settings::instance().setRootDir(QDir::fromNativeSeparators(rootDir));
    Settings::instance().sync();

    LogFile log(logPath);
    log.line(QStringLiteral("加载器 = %1，原版 = %2，目标版本 = %3")
                 .arg(loaderKindName(kind), mcVersion,
                      loaderVersion.isEmpty() ? QStringLiteral("（最新）") : loaderVersion));

    QEventLoop loop;
    int exitCode = 1;

    LoaderCatalog catalog;
    LoaderInstaller installer;
    installer.setConcurrency(4);

    QObject::connect(&catalog, &LoaderCatalog::failed, &loop, [&](const QString &msg) {
        log.line(QStringLiteral("获取加载器列表失败：%1").arg(msg));
        exitCode = 2;
        loop.quit();
    });

    QObject::connect(&catalog, &LoaderCatalog::ready, &loop,
                     [&](const QVector<LoaderVersion> &list) {
                         log.line(QStringLiteral("可用加载器版本：%1 个").arg(list.size()));

                         LoaderVersion chosen;
                         bool found = false;
                         for (const LoaderVersion &v : list) {
                             if (loaderVersion.isEmpty()) {
                                 if (v.stable) {
                                     chosen = v;
                                     found = true;
                                     break;
                                 }
                                 if (!found) {
                                     chosen = v;
                                     found = true;
                                 }
                             } else if (v.loaderVersion == loaderVersion) {
                                 chosen = v;
                                 found = true;
                                 break;
                             }
                         }
                         if (!found) {
                             log.line(QStringLiteral("找不到加载器版本 %1").arg(loaderVersion));
                             exitCode = 2;
                             loop.quit();
                             return;
                         }

                         log.line(QStringLiteral("选用 %1 %2（预计 id=%3）")
                                      .arg(loaderKindName(chosen.kind), chosen.loaderVersion,
                                           chosen.versionId));

                         LoaderInstaller::Request req;
                         req.kind = chosen.kind;
                         req.mcVersion = mcVersion;
                         req.loaderVersion = chosen.loaderVersion;
                         req.versionId = chosen.versionId;
                         req.rootDir = Settings::instance().rootDir();
                         req.fabricEntry = chosen.raw;

                         QObject::connect(&installer, &LoaderInstaller::logLine, &loop,
                                          [&](const QString &t) { log.line(t); });
                         QObject::connect(&installer, &LoaderInstaller::stageChanged, &loop,
                                          [&](const QString &t) {
                                              log.line(QStringLiteral("—— %1").arg(t));
                                          });
                         QObject::connect(&installer, &LoaderInstaller::finished, &loop,
                                          [&](bool ok, const QString &msg, const QString &id) {
                                              log.line(QStringLiteral("结果：%1（id=%2）%3")
                                                           .arg(ok ? QStringLiteral("成功")
                                                                   : QStringLiteral("失败"))
                                                           .arg(id, msg));
                                              exitCode = ok ? 0 : 1;
                                              loop.quit();
                                          });

                         QTimer::singleShot(0, &installer, [&installer, req]() {
                             installer.start(req);
                         });
                     });

    QTimer::singleShot(0, &catalog, [&catalog, kind, mcVersion]() {
        catalog.fetch(kind, mcVersion);
    });

    QTimer::singleShot(45 * 60 * 1000, &loop, [&loop]() { loop.quit(); });
    loop.exec();
    return exitCode;
}

} // namespace md
