#pragma once

#include "core/LoaderCatalog.h"
#include "core/ManifestParser.h"

#include <QDialog>

class QCloseEvent;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace md {

class LoaderInstaller;

// 安装模组加载器（Fabric / Forge / NeoForge）的对话框。
// 只负责选版本和展示进度；真正的安装在 LoaderInstaller 里，全程写 versions/<id>/ 与 libraries/。
class LoaderDialog : public QDialog
{
    Q_OBJECT

public:
    LoaderDialog(const VersionManifest &manifest, const QString &rootDir, int concurrency,
                 QWidget *parent = nullptr);
    ~LoaderDialog() override;

    // 预选加载器类型（自动化测试用）；会立即去拉版本列表
    void preselectKind(LoaderKind kind);

protected:
    // 安装进行中不许直接关窗口：先确认，再取消安装
    void reject() override;
    void closeEvent(QCloseEvent *event) override;

signals:
    // 安装成功后带出实际生成的版本 id，主窗口据此刷新列表
    void installed(const QString &versionId);

private:
    void buildUi();
    void fillBaseVersions();
    void onKindChanged();
    void onBaseChanged();
    void fetchLoaderVersions();
    void onCatalogReady(const QVector<md::LoaderVersion> &versions);
    void onCatalogFailed(const QString &message);
    void startInstall();
    void onFinished(bool ok, const QString &message, const QString &versionId);
    void updateButtons();
    void appendLog(const QString &line);

    VersionManifest m_manifest;
    QString m_rootDir;
    int m_concurrency = 4;

    LoaderCatalog *m_catalog = nullptr;
    LoaderInstaller *m_installer = nullptr;
    QVector<LoaderVersion> m_versions;

    QComboBox *m_kindCombo = nullptr;
    QComboBox *m_baseCombo = nullptr;
    QComboBox *m_loaderCombo = nullptr;
    QPushButton *m_fetchBtn = nullptr;
    QPushButton *m_installBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
    QLabel *m_hintLabel = nullptr;
    QLabel *m_stageLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QPlainTextEdit *m_log = nullptr;

    bool m_fetching = false;
    LoaderKind m_pendingKind = LoaderKind::Fabric;   // 最近一次发起请求的加载器类型
    bool m_installing = false;
    bool m_closable = true;     // 安装中为 false：关闭要先确认并取消安装
    bool m_closing = false;     // 已经决定关窗：不再为取消弹提示框
};

} // namespace md
