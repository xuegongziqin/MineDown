#pragma once

#include "core/Types.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QVector>

namespace md {

QString humanBytes(qint64 bytes);

// 文件表：每个规划出来的下载任务一行，显示名称 / 大小 / 状态。
class FileTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        ColName = 0,
        ColSize,
        ColState,
        ColCount,
    };

    explicit FileTableModel(QObject *parent = nullptr);

    void clearRows();
    void addTask(const md::DownloadTask &task);
    int rowOf(const QString &dest) const;
    int count() const { return m_rows.size(); }

    void setTaskResult(const md::TaskResult &result);
    void setTaskProgress(const QString &dest, qint64 received, qint64 total);
    void markCanceled();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    struct Row {
        DownloadTask task;
        TaskState state = TaskState::Pending;
        qint64 received = 0;
        QString error;
    };

    QString stateText(const Row &row) const;
    QVector<Row> m_rows;
    QHash<QString, int> m_index;
};

} // namespace md
