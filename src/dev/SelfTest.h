#pragma once

#include <QString>

namespace md {

// 无界面自检：拉清单 -> 规划 -> 下载一个版本，把过程写进日志文件，用退出码表示结果。
//   0 = 全部成功；1 = 有文件失败/流程失败；2 = 清单或参数问题；3 = 超时
int runSelfTest(const QString &rootDir, const QString &versionId, const QString &logPath);

} // namespace md
