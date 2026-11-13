#include "core/Elevation.h"

#include <QCoreApplication>
#include <QDir>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <shellapi.h>

#  include <string>
#endif

namespace md {
namespace {

#ifdef Q_OS_WIN
QString win32ErrorText(DWORD code)
{
    wchar_t *buffer = nullptr;
    const DWORD len = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                           | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, code, 0,
                                       reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    QString text;
    if (len > 0 && buffer)
        text = QString::fromWCharArray(buffer, static_cast<int>(len)).trimmed();
    if (buffer)
        ::LocalFree(buffer);
    if (text.isEmpty())
        text = QStringLiteral("Windows 错误 %1").arg(code);
    return text;
}

// 按 Windows 解析命令行的规则（CommandLineToArgvW 那一套）给单个参数加引号：
// 反斜杠只在引号前成对翻倍，引号前补一个反斜杠。
QString quoteArg(const QString &arg)
{
    const bool needsQuotes = arg.isEmpty() || arg.contains(QLatin1Char(' '))
                             || arg.contains(QLatin1Char('\t')) || arg.contains(QLatin1Char('"'));
    if (!needsQuotes)
        return arg;

    QString out = QStringLiteral("\"");
    int backslashes = 0;
    for (const QChar ch : arg) {
        if (ch == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (ch == QLatin1Char('"')) {
            out += QString(backslashes * 2 + 1, QLatin1Char('\\'));
            out += QLatin1Char('"');
            backslashes = 0;
            continue;
        }
        out += QString(backslashes, QLatin1Char('\\'));
        backslashes = 0;
        out += ch;
    }
    out += QString(backslashes * 2, QLatin1Char('\\'));
    out += QLatin1Char('"');
    return out;
}

// 重启时不带上的参数：提权标记本身，以及截图/自检这类一次性自动化开关
// （否则从失败弹窗里重启出来的进程会去截图然后自己退出）。
bool isDroppedFlag(const QString &arg)
{
    static const QStringList bare = {
        QStringLiteral("--elevate"), QStringLiteral("--elevated"),
        QStringLiteral("--selftest"), QStringLiteral("--install-loader"),
    };
    return bare.contains(arg);
}

// 这些开关后面还跟一个取值，重启时要把取值一起丢掉
bool isDroppedFlagWithValue(const QString &arg)
{
    static const QStringList withValue = {
        QStringLiteral("--shot"),       QStringLiteral("--shot-delay"),
        QStringLiteral("--loader-shot"), QStringLiteral("--loader-kind"),
        QStringLiteral("--log"),        QStringLiteral("--version"),
        QStringLiteral("--loader"),     QStringLiteral("--mc"),
        QStringLiteral("--loader-version"),
    };
    return withValue.contains(arg);
}
#endif // Q_OS_WIN

} // namespace

bool isProcessElevated()
{
#ifdef Q_OS_WIN
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation, size, &size);
    ::CloseHandle(token);
    return ok != FALSE && elevation.TokenIsElevated != 0;
#else
    return false;
#endif
}

bool relaunchElevated(const QStringList &extraArgs, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

#ifdef Q_OS_WIN
    if (isProcessElevated())
        return fail(QStringLiteral("当前进程已经是管理员权限，不需要再提权。"));

    QStringList args = QCoreApplication::arguments();
    if (!args.isEmpty())
        args.removeFirst();   // 丢掉 exe 路径

    QStringList kept;
    bool skipValue = false;
    for (const QString &arg : args) {
        if (skipValue) {
            skipValue = false;
            continue;   // 上一个开关的取值
        }
        if (isDroppedFlag(arg))
            continue;
        if (isDroppedFlagWithValue(arg)) {
            skipValue = true;
            continue;
        }
        kept.append(arg);
    }
    for (const QString &arg : extraArgs) {
        if (!arg.isEmpty())
            kept.append(arg);
    }
    kept.append(QStringLiteral("--elevated"));

    QString params;
    for (int i = 0; i < kept.size(); ++i) {
        if (i > 0)
            params += QLatin1Char(' ');
        params += quoteArg(kept.at(i));
    }

    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const std::wstring exeW = exe.toStdWString();
    const std::wstring paramsW = params.toStdWString();

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = exeW.c_str();
    info.lpParameters = paramsW.empty() ? nullptr : paramsW.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&info)) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_CANCELLED)
            return fail(QStringLiteral("你在 UAC 提示里点了「否」，没有获得管理员权限。"));
        return fail(QStringLiteral("无法以管理员身份启动：%1").arg(win32ErrorText(code)));
    }
    if (info.hProcess)
        ::CloseHandle(info.hProcess);
    return true;
#else
    Q_UNUSED(extraArgs);
    return fail(QStringLiteral("只有 Windows 需要提权。"));
#endif
}

} // namespace md
