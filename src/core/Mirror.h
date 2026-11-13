#pragma once

#include <QString>
#include <QVector>

namespace md {

// 镜像站本质是「地址替换层」：把官方地址换成镜像地址，再由我们落到同一个本地路径。
// 但不同镜像对路径的改写规则不一样（实测结果，见 README「镜像地址规则」）：
//   官方   libraries.minecraft.net/at/yawk/...        → 路径不变
//   官方   resources.download.minecraft.net/b6/xxx     → 路径不变
//   BMCLAPI  .../libraries/at/yawk/...                → 库地址前插 /libraries/
//   BMCLAPI  .../assets/b6/xxx                        → 资源地址前插 /assets/
//   BMCLAPI  .../v1/objects/<hash>/client.jar         → piston-data 对象保留原路径
//   MC 光速  .../maven/at/yawk/...                    → 库地址前插 /maven/
//   MC 光速  .../assets/b6/xxx                        → 资源地址前插 /assets/
// 所以不能只做域名替换，必须按「资源类型」重建路径。
enum class MirrorId {
    Official = 0,
    Bmclapi,
    HkmcMain,
    HkmcEo,
    Custom,
};

enum class MirrorLayout {
    HostOnly, // 只换域名，路径原样（自定义镜像按此处理）
    Bmclapi,  // 库前插 libraries/，资源前插 assets/
    Hkmc,     // 库前插 maven/，资源前插 assets/
};

struct MirrorInfo {
    MirrorId id = MirrorId::Official;
    QString key;    // 持久化标识
    QString name;   // 界面显示名
    QString base;   // 根地址（无结尾斜杠）；Custom 时为空，由用户填写
    MirrorLayout layout = MirrorLayout::HostOnly;
};

// 内置镜像列表（顺序即界面下拉框顺序）。
QVector<MirrorInfo> builtinMirrors();

// 按 key 找内置镜像，找不到返回 nullptr。
const MirrorInfo *findMirror(const QString &key);

// 根据根地址推断镜像的路径改写规则（未知地址按「只换域名」处理）。
MirrorLayout layoutForBase(const QString &base);

// 把任意一条官方/镜像地址改写成指定镜像下的地址。base 为空时返回官方地址（归一化后）。
// 对已经是目标镜像的地址是幂等的；自定义 Maven 仓库等未知地址原样返回。
QString applyMirror(const QString &url, const QString &base);

// 把任意一条地址还原成官方地址（用于镜像缺文件时回退官方源）。
QString officialUrlFor(const QString &url);

// 回退地址：已经是官方地址时返回空串（表示无需回退）。
QString fallbackForUrl(const QString &url);

// 判断 URL 是否属于需要做镜像替换的官方域名。
bool isOfficialHost(const QString &url);

// 加载器（Forge / NeoForge / Fabric）的官方仓库地址，供安装器拼地址用。
QString forgeMavenBase();
QString neoForgeMavenBase();
QString fabricMavenBase();
QString fabricMetaBase();

} // namespace md
