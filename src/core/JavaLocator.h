#pragma once

#include <QString>
#include <QVector>

namespace md {

struct JavaInfo {
    QString path;      // java.exe 的绝对路径
    int major = -1;    // 主版本：8 / 17 / 21 …
    QString version;   // 原始版本串：21.0.7 / 1.8.0_402
    bool isValid() const { return !path.isEmpty() && major > 0; }
};

// 扫描「配置里指定的 + PATH 里的 + 常见安装目录里的」java.exe，逐个跑 `java -version` 判定主版本。
// 结果按主版本从高到低排序。扫描一次后走缓存（refresh 强制重扫）。
QVector<JavaInfo> scanJavaRuntimes(bool refresh = false);

// 在候选里挑一个主版本 >= minMajor 的（同版本优先挑路径最短/最像正式安装的）；
// 都不满足就返回最高版本的那个（调用方自行决定是否报错）。
JavaInfo findJava(int minMajor);

// 给日志/界面用的版本描述，例如「Java 21（21.0.7）」。
QString javaVersionText(const JavaInfo &info);

} // namespace md
