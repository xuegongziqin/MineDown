#pragma once

#include <QString>

namespace md {

// 无界面安装加载器（只给测试用）：先用清单确认版本存在，再跑完整安装流程。
// 返回码：0 成功 / 1 安装失败 / 2 参数或清单问题。
int runLoaderTest(const QString &rootDir, const QString &kindKey, const QString &mcVersion,
                  const QString &loaderVersion, const QString &logPath);

} // namespace md
