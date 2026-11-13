#include "ui/VersionListModel.h"

namespace md {

VersionListModel::VersionListModel(QObject *parent) : QAbstractListModel(parent) {}

void VersionListModel::setVersions(const VersionManifest &manifest)
{
    m_manifest = manifest;
    rebuild();
}

void VersionListModel::setLocalVersions(const QStringList &ids)
{
    if (ids == m_local)
        return;
    m_local = ids;
    rebuild();
}

void VersionListModel::setFilter(const QString &text)
{
    const QString t = text.trimmed().toLower();
    if (t == m_filter)
        return;
    m_filter = t;
    rebuild();
}

void VersionListModel::setShowSnapshots(bool on)
{
    if (on == m_showSnapshots)
        return;
    m_showSnapshots = on;
    rebuild();
}

void VersionListModel::rebuild()
{
    beginResetModel();
    m_entries.clear();
    m_localFlags.clear();

    // 本地加载器版本：清单里没有的那些，放在最前面方便一眼看到自己装了什么。
    for (const QString &id : m_local) {
        if (id.isEmpty() || m_manifest.find(id))
            continue;
        if (!m_filter.isEmpty() && !id.toLower().contains(m_filter))
            continue;
        VersionEntry e;
        e.id = id;
        e.type = QStringLiteral("local");
        m_entries.append(e);
        m_localFlags.append(true);
    }

    for (const VersionEntry &e : m_manifest.versions) {
        if (!m_showSnapshots && !e.isRelease())
            continue;
        if (!m_filter.isEmpty() && !e.id.toLower().contains(m_filter))
            continue;
        m_entries.append(e);
        m_localFlags.append(false);
    }

    endResetModel();
}

const VersionEntry *VersionListModel::entryAt(int row) const
{
    if (row < 0 || row >= m_entries.size())
        return nullptr;
    return &m_entries.at(row);
}

bool VersionListModel::isLocalRow(int row) const
{
    if (row < 0 || row >= m_localFlags.size())
        return false;
    return m_localFlags.at(row);
}

int VersionListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant VersionListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};

    const VersionEntry *e = entryAt(index.row());
    if (!e)
        return {};
    const bool local = isLocalRow(index.row());

    switch (role) {
    case Qt::DisplayRole:
        if (local)
            return QStringLiteral("%1  ·  已安装").arg(e->id);
        if (e->isRelease())
            return e->id;
        return QStringLiteral("%1  ·  快照").arg(e->id);
    case Qt::ToolTipRole:
        if (local) {
            return QStringLiteral("本地已安装的版本：%1\n（加载器版本由安装器生成，不需要重新下载）")
                .arg(e->id);
        }
        return QStringLiteral("版本：%1\n类型：%2\n发布时间：%3")
            .arg(e->id, e->type, e->releaseTime);
    case IdRole:
        return e->id;
    case TypeRole:
        return e->type;
    case UrlRole:
        return e->url;
    case Sha1Role:
        return e->sha1;
    case SizeRole:
        return e->size;
    case ReleaseTimeRole:
        return e->releaseTime;
    case LocalRole:
        return local;
    default:
        return {};
    }
}

} // namespace md
