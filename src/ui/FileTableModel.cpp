#include "ui/FileTableModel.h"

#include <QColor>
#include <QFont>

namespace md {

QString humanBytes(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("—");
    const double kb = 1024.0;
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / kb, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / (kb * kb), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / (kb * kb * kb), 0, 'f', 2);
}

FileTableModel::FileTableModel(QObject *parent) : QAbstractTableModel(parent) {}

void FileTableModel::clearRows()
{
    beginResetModel();
    m_rows.clear();
    m_index.clear();
    endResetModel();
}

void FileTableModel::addTask(const DownloadTask &task)
{
    const int row = m_rows.size();
    beginInsertRows(QModelIndex(), row, row);
    Row r;
    r.task = task;
    m_rows.append(r);
    m_index.insert(task.dest, row);
    endInsertRows();
}

int FileTableModel::rowOf(const QString &dest) const
{
    return m_index.value(dest, -1);
}

void FileTableModel::setTaskResult(const TaskResult &result)
{
    int row = rowOf(result.task.dest);
    if (row < 0 && !result.task.url.isEmpty()) {
        for (int i = 0; i < m_rows.size(); ++i) {
            if (m_rows.at(i).task.url == result.task.url) {
                row = i;
                break;
            }
        }
    }
    if (row < 0 || row >= m_rows.size())
        return;

    Row &r = m_rows[row];
    r.state = result.state;
    r.received = result.downloaded > 0 ? result.downloaded : r.received;
    r.error = result.error;
    emit dataChanged(index(row, ColName), index(row, ColState));
}

void FileTableModel::setTaskProgress(const QString &dest, qint64 received, qint64 total)
{
    Q_UNUSED(total)
    const int row = rowOf(dest);
    if (row < 0 || row >= m_rows.size())
        return;

    Row &r = m_rows[row];
    if (r.state == TaskState::Pending)
        r.state = TaskState::Running;
    if (r.state != TaskState::Running)
        return;

    r.received = received;
    emit dataChanged(index(row, ColSize), index(row, ColSize));
    emit dataChanged(index(row, ColState), index(row, ColState));
}

void FileTableModel::markCanceled()
{
    bool any = false;
    for (int i = 0; i < m_rows.size(); ++i) {
        Row &r = m_rows[i];
        if (r.state == TaskState::Pending || r.state == TaskState::Running) {
            r.state = TaskState::Canceled;
            any = true;
        }
    }
    if (any)
        emit dataChanged(index(0, ColName), index(m_rows.size() - 1, ColState));
}

QString FileTableModel::stateText(const Row &row) const
{
    switch (row.state) {
    case TaskState::Pending:
        return QStringLiteral("等待");
    case TaskState::Running: {
        const qint64 total = row.task.size;
        if (total > 0)
            return QStringLiteral("下载中 %1%").arg(
                static_cast<int>(row.received * 100 / total));
        return QStringLiteral("下载中");
    }
    case TaskState::Done:
        return QStringLiteral("完成");
    case TaskState::Skipped:
        return QStringLiteral("已存在");
    case TaskState::Failed:
        return row.error.isEmpty() ? QStringLiteral("失败")
                                   : QStringLiteral("失败：%1").arg(row.error);
    case TaskState::Canceled:
        return QStringLiteral("已取消");
    }
    return {};
}

int FileTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

int FileTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColCount;
}

QVariant FileTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};

    const Row &r = m_rows.at(index.row());

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColName:
            return r.task.label.isEmpty() ? r.task.dest : r.task.label;
        case ColSize:
            if (r.received > 0 && r.task.size > 0 && r.state == TaskState::Running)
                return QStringLiteral("%1 / %2").arg(humanBytes(r.received),
                                                     humanBytes(r.task.size));
            return humanBytes(r.task.size > 0 ? r.task.size : r.received);
        case ColState:
            return stateText(r);
        default:
            return {};
        }
    }

    if (role == Qt::ToolTipRole) {
        switch (index.column()) {
        case ColName:
            return r.task.dest;
        case ColState:
            return r.error.isEmpty() ? QStringLiteral("目标：%1").arg(r.task.dest) : r.error;
        default:
            return r.task.url;
        }
    }

    if (role == Qt::ForegroundRole) {
        switch (r.state) {
        case TaskState::Done:
            return QColor(0x1a, 0x7f, 0x37);
        case TaskState::Skipped:
            return QColor(0x8a, 0x8a, 0x8a);
        case TaskState::Failed:
            return QColor(0xc6, 0x28, 0x28);
        case TaskState::Canceled:
            return QColor(0x9a, 0x9a, 0x9a);
        default:
            return {};
        }
    }

    if (role == Qt::FontRole && index.column() == ColName) {
        QFont f;
        if (r.task.kind == TaskKind::ClientJar)
            f.setBold(true);
        return f;
    }

    return {};
}

QVariant FileTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
        return {};

    if (orientation == Qt::Horizontal) {
        switch (section) {
        case ColName:
            return QStringLiteral("文件");
        case ColSize:
            return QStringLiteral("大小");
        case ColState:
            return QStringLiteral("状态");
        default:
            return {};
        }
    }
    return QString::number(section + 1);
}

} // namespace md
