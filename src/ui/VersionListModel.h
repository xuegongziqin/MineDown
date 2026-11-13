#pragma once

#include "core/ManifestParser.h"

#include <QAbstractListModel>
#include <QStringList>
#include <QVector>

namespace md {

// 版本列表：清单里 versions[] 的视图，外加「本地已安装、但不属于清单」的版本
// （加载器装出来的 fabric-loader-x、1.20.1-forge-x 就属于这一类）。
// 支持快照开关与关键字过滤。
class VersionListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        TypeRole,
        UrlRole,
        Sha1Role,
        SizeRole,
        ReleaseTimeRole,
        LocalRole,          // true = 本地已装的加载器版本（清单里没有，不能重新下载）
    };

    explicit VersionListModel(QObject *parent = nullptr);

    void setVersions(const VersionManifest &manifest);
    // 本地 versions/ 下已安装、但清单里没有的版本 id（按名称排序显示在最前）
    void setLocalVersions(const QStringList &ids);

    void setFilter(const QString &text);
    void setShowSnapshots(bool on);

    // 注意：返回的指针指向内部缓存的副本，rebuild/重新设置后失效。
    const VersionEntry *entryAt(int row) const;
    bool isLocalRow(int row) const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

private:
    void rebuild();

    VersionManifest m_manifest;
    QStringList m_local;
    QVector<VersionEntry> m_entries;   // 可见行的最终内容（本地版本在前）
    QVector<bool> m_localFlags;        // 与 m_entries 一一对应
    QString m_filter;
    bool m_showSnapshots = true;
};

} // namespace md
