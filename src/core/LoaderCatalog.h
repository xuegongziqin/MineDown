#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

namespace md {

class HttpFetcher;

enum class LoaderKind {
    Fabric = 0,
    Forge,
    NeoForge,
};

QString loaderKindKey(LoaderKind kind);
QString loaderKindName(LoaderKind kind);
bool loaderKindFromKey(const QString &key, LoaderKind *out);

// 某原版版本（mcVersion）可用的一个加载器版本。
struct LoaderVersion {
    LoaderKind kind = LoaderKind::Fabric;
    QString mcVersion;
    QString loaderVersion;   // Forge: 47.4.29；NeoForge: 21.5.75；Fabric: 0.16.9
    QString versionId;       // 预计的版本目录名（安装时以安装器里的 version 字段为准）
    bool stable = true;
    QJsonObject raw;         // Fabric 用：Meta API 里的原始条目（合成版本 JSON 要靠它）

    // 界面显示：Forge/NeoForge 显示版本号本身，Fabric 额外标出 stable。
    QString label() const;
};

// 版本列表抓取器：Forge / NeoForge 读 maven-metadata.xml，Fabric 读 Meta API。
// 三家都只有「针对某个原版版本」的列表，所以 fetch 必须带 mcVersion。
class LoaderCatalog : public QObject
{
    Q_OBJECT

public:
    explicit LoaderCatalog(QObject *parent = nullptr);

    void fetch(LoaderKind kind, const QString &mcVersion);
    void abort();

signals:
    void ready(const QVector<md::LoaderVersion> &versions);
    void failed(const QString &message);

private:
    void onData(const QByteArray &data);
    void onError(const QString &message);
    void parseForgeXml(const QByteArray &data, bool neoForgeLegacy);
    QString mirrorBase() const;

    HttpFetcher *m_fetcher = nullptr;
    LoaderKind m_kind = LoaderKind::Fabric;
    QString m_mc;
    // NeoForge 有两套坐标（1.20.2 起是 neoforge，更早是 forge），需要记着当前请求的是哪套。
    bool m_neoTriedLegacy = false;
    bool m_neoTriedNew = false;
    QString m_pendingError;
};

// 版本号比较（按 . 与 - 切段，数字段按数值比较）；返回 <0 / 0 / >0。
int compareVersionStrings(const QString &a, const QString &b);

} // namespace md
