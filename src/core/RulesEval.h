#pragma once

#include <QJsonArray>
#include <QString>

namespace md {

// 当前运行环境（用于 libraries[].rules 求值）。本程序只支持 Windows x64。
struct OsInfo {
    QString name;   // windows / linux / osx
    QString arch;   // x86_64 / x86 / arm64
    QString version;

    static OsInfo current();
};

// Minecraft 的规则求值语义：
//   rules 缺失        -> 允许
//   rules 存在但无匹配 -> 拒绝
//   逐条匹配，最后一条匹配的规则决定结果（allow/disallow）
bool rulesAllow(const QJsonArray &rules, const OsInfo &os);

} // namespace md
