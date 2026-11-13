#include "core/Elevation.h"
#include "core/Settings.h"
#include "dev/LoaderTest.h"
#include "dev/SelfTest.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QSslSocket>
#include <QTimer>

#include <functional>
#include <memory>

namespace {

QString argValue(const QStringList &args, const QString &key)
{
    const int i = args.indexOf(key);
    if (i >= 0 && i + 1 < args.size())
        return args.at(i + 1);
    return {};
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("MineDown"));
    QApplication::setOrganizationName(QStringLiteral("MineDown"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    // 图标编译在 Qt 资源里（res/app.qrc），窗口/任务栏直接用；exe 文件图标由 res/app.rc 提供
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icon.png")));

    // 优先用 Windows 自带的 SChannel：它会自动补全服务器漏发的中间证书，
    // 部分镜像节点（OpenSSL 会报 "issuer certificate ... could not be found"）只有它能连上。
    if (QSslSocket::availableBackends().contains(QStringLiteral("schannel")))
        QSslSocket::setActiveBackend(QStringLiteral("schannel"));

    const QStringList args = QApplication::arguments();

    // --elevate：用 UAC 以管理员身份重启自己（命令行/快捷方式用）。
    // 重启出来的进程带 --elevated 标记，不会再递归提权。
    if (args.contains(QStringLiteral("--elevate")) && !md::isProcessElevated()) {
        QString error;
        const bool started = md::relaunchElevated(QStringList(), &error);
        if (!started)
            qWarning("%s", qPrintable(error));
        return started ? 0 : 1;
    }

    // 开发/测试用参数：--root <dir> 指定游戏目录，--mirror <key> 指定镜像
    // 这些覆盖只对本次运行生效：不要写进注册表，否则跑一次测试就会把用户
    // 真实的镜像设置改成 mock 服务器地址。
    if (args.contains(QStringLiteral("--root")) || args.contains(QStringLiteral("--mirror"))
        || args.contains(QStringLiteral("--custom-base")))
        md::Settings::instance().setTransientOverrides(true);

    const QString rootArg = argValue(args, QStringLiteral("--root"));
    if (!rootArg.isEmpty()) {
        md::Settings::instance().setRootDir(QDir::fromNativeSeparators(rootArg));
    }
    const QString mirrorArg = argValue(args, QStringLiteral("--mirror"));
    if (!mirrorArg.isEmpty()) {
        md::Settings::instance().setMirrorKey(mirrorArg);
    }
    // 测试用：把自定义镜像根地址指向本地 mock 服务器（等价于在界面里选「自定义」）
    const QString customBaseArg = argValue(args, QStringLiteral("--custom-base"));
    if (!customBaseArg.isEmpty()) {
        md::Settings::instance().setCustomMirrorBase(customBaseArg);
        md::Settings::instance().setMirrorKey(QStringLiteral("custom"));
    }

    const QString shotPath = argValue(args, QStringLiteral("--shot"));
    const bool shotMode = !shotPath.isEmpty();

    // 无界面自检：只跑下载流程，不建窗口
    if (args.contains(QStringLiteral("--selftest"))) {
        return md::runSelfTest(QDir::fromNativeSeparators(argValue(args, QStringLiteral("--root"))),
                               argValue(args, QStringLiteral("--version")),
                               argValue(args, QStringLiteral("--log")));
    }

    // 无界面安装加载器（测试用）
    if (args.contains(QStringLiteral("--install-loader"))) {
        return md::runLoaderTest(QDir::fromNativeSeparators(argValue(args, QStringLiteral("--root"))),
                                 argValue(args, QStringLiteral("--loader")),
                                 argValue(args, QStringLiteral("--mc")),
                                 argValue(args, QStringLiteral("--loader-version")),
                                 argValue(args, QStringLiteral("--log")));
    }

    const QString autoVersion = argValue(args, QStringLiteral("--auto"));

    // 测试用：打开加载器安装对话框并截图（--loader-shot <png> [--shot-delay <ms>]）
    const QString loaderShot = argValue(args, QStringLiteral("--loader-shot"));

    md::MainWindow win;
    win.setSkipSetup(shotMode || !loaderShot.isEmpty());
    win.show();

    if (!autoVersion.isEmpty()) {
        QTimer::singleShot(1500, &win, [&win, autoVersion]() { win.startVersion(autoVersion); });
    }

    if (!loaderShot.isEmpty()) {
        int delay = argValue(args, QStringLiteral("--shot-delay")).toInt();
        if (delay <= 0)
            delay = 8000;
        const QString loaderKind = argValue(args, QStringLiteral("--loader-kind"));
        // 等版本清单加载完成再开对话框（清单没到就直接截图会拿到空列表）
        auto poll = std::make_shared<std::function<void()>>();
        *poll = [&win, loaderShot, loaderKind, delay, poll]() {
            if (win.manifestReady()) {
                win.shotLoaderDialog(loaderShot, delay, loaderKind);
                return;
            }
            QTimer::singleShot(300, &win, [poll]() { (*poll)(); });
        };
        QTimer::singleShot(300, &win, [poll]() { (*poll)(); });
    }

    if (shotMode) {
        int delay = argValue(args, QStringLiteral("--shot-delay")).toInt();
        if (delay <= 0)
            delay = 900;
        QTimer::singleShot(delay, &win, [&win, shotPath]() {
            const bool ok = win.grab().save(shotPath);
            qInfo("shot %s -> %s", ok ? "saved" : "FAILED", qPrintable(shotPath));
            QApplication::quit();
        });
    }

    return app.exec();
}
