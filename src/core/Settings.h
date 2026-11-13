#pragma once

#include <QHash>
#include <QSettings>
#include <QString>
#include <QVariant>

namespace md {

// 全局配置（QSettings 落在注册表 HKCU\Software\MineDown\MineDown）。
class Settings
{
public:
    static Settings &instance();

    // 游戏目录（即 .minecraft 根目录）。首次启动为空，必须先让用户选择。
    QString rootDir() const;
    void setRootDir(const QString &dir);
    bool hasRootDir() const;

    // 镜像
    QString mirrorKey() const;              // official / bmclapi / hkmc / hkmc-eo / custom
    void setMirrorKey(const QString &key);
    QString customMirrorBase() const;
    void setCustomMirrorBase(const QString &base);
    QString mirrorBase() const;              // 实际生效的镜像根地址（官方返回空串）

    // 并发下载数，1..32
    int concurrency() const;
    void setConcurrency(int n);

    // 界面状态
    QString lastVersion() const;             // 上次选择/下载完成的版本，用于列表高亮
    void setLastVersion(const QString &id);
    bool showSnapshots() const;
    void setShowSnapshots(bool on);

    // 其它
    QString javaPath() const;                // 仅用于提示，不参与下载
    void setJavaPath(const QString &path);

    void sync();

    // 命令行/测试覆盖：打开后所有写入只留在本进程内，不落注册表。
    // （`--root` / `--mirror` / `--custom-base` 这类参数属于开发、测试用途，
    //  以前会顺手改掉用户真实的镜像设置，导致下次启动连不上。）
    void setTransientOverrides(bool on);

private:
    Settings() = default;

    QVariant get(const QString &key, const QVariant &fallback) const;
    void put(const QString &key, const QVariant &value);

    QSettings s_;
    bool m_transient = false;
    QHash<QString, QVariant> m_overrides;
};

} // namespace md
