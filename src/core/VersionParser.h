#pragma once

#include "core/RulesEval.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace md {

// 一个需要落盘的文件。relPath 是相对游戏目录的路径（用 '/' 分隔）。
struct DownloadSpec {
    QString url;
    QString sha1;
    qint64 size = -1;
    QString relPath;
};

struct LibrarySpec {
    QString name;      // Maven 坐标，日志用
    QString relPath;   // libraries/...
    QString url;
    QString sha1;
    qint64 size = -1;
    bool isNative = false;
};

struct VersionMeta {
    QString id;
    QString type;
    QString mainClass;

    DownloadSpec client;       // versions/<id>/<id>.jar
    DownloadSpec versionJson;  // versions/<id>/<id>.json（由 pipeline 回填）
    DownloadSpec assetIndex;   // assets/indexes/<indexId>.json
    DownloadSpec logConfig;    // assets/log_configs/<logId>

    QString assetIndexId;      // 例如 "17" / "legacy"
    qint64 assetTotalSize = -1; // 资源索引里声明的总大小，用于进度预估

    int javaMajor = -1;
    QString javaComponent;

    QVector<LibrarySpec> libraries;
};

struct ParseOptions {
    QString mirrorBase;        // 空 = 官方源
    OsInfo os = OsInfo::current();
    bool includeNatives = true;
};

bool parseVersionJson(const QByteArray &json, const ParseOptions &opt, VersionMeta *out,
                      QString *err);

// 把 group:artifact:version[:classifier] 转成 Maven 相对路径
// （libraries/net/minecraft/launchwrapper/1.12/launchwrapper-1.12.jar 这样的形式）。
QString mavenRelPath(const QString &coordinate, const QString &classifier = QString());

} // namespace md
