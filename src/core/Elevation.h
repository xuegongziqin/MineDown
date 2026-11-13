#pragma once

#include <QString>
#include <QStringList>

namespace md {

// 当前进程是否已经提升（管理员令牌生效）。非 Windows 平台恒为 false。
bool isProcessElevated();

// 用 UAC 以管理员身份重新启动自己。
// extraArgs 追加在原有命令行参数后面；原先的 --elevate/--elevated 以及截图/自检这类
// 自动化参数会被丢掉，重启后就是普通的一次运行（避免重启后又截图退出）。
// 成功返回 true —— 调用方应该结束当前进程；失败返回 false 并在 error 里说明原因
// （最常见的是用户在 UAC 弹窗上点了「否」，此时 error 为 ERROR_CANCELLED 的说明）。
bool relaunchElevated(const QStringList &extraArgs, QString *error);

} // namespace md
