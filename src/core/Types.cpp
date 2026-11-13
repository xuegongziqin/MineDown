#include "core/Types.h"

#include <QDateTime>

namespace md {

QString taskKindName(TaskKind kind)
{
    switch (kind) {
    case TaskKind::Manifest:    return QStringLiteral("版本清单");
    case TaskKind::VersionJson: return QStringLiteral("版本信息");
    case TaskKind::ClientJar:   return QStringLiteral("客户端");
    case TaskKind::Library:     return QStringLiteral("依赖库");
    case TaskKind::AssetIndex:  return QStringLiteral("资源索引");
    case TaskKind::Asset:       return QStringLiteral("资源文件");
    case TaskKind::LogConfig:   return QStringLiteral("日志配置");
    case TaskKind::Unknown:     break;
    }
    return QStringLiteral("其他");
}

QString taskStateName(TaskState state)
{
    switch (state) {
    case TaskState::Pending:  return QStringLiteral("等待");
    case TaskState::Running:  return QStringLiteral("下载中");
    case TaskState::Done:     return QStringLiteral("完成");
    case TaskState::Skipped:  return QStringLiteral("已存在");
    case TaskState::Failed:   return QStringLiteral("失败");
    case TaskState::Canceled: return QStringLiteral("已取消");
    }
    return QString();
}

double DownloadStats::speedBytesPerSec() const
{
    if (startedMs <= 0)
        return 0.0;
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - startedMs;
    if (elapsed <= 0)
        return 0.0;
    return static_cast<double>(doneBytes) * 1000.0 / static_cast<double>(elapsed);
}

int DownloadStats::percent() const
{
    if (totalBytes <= 0)
        return 0;
    qint64 p = doneBytes * 100 / totalBytes;
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return static_cast<int>(p);
}

} // namespace md
