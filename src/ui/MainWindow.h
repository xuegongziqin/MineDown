#pragma once

#include "core/InstallPipeline.h"
#include "core/Types.h"

#include <QMainWindow>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableView;

namespace md {

class DownloadManager;
class FileTableModel;
class HttpFetcher;
class VersionListModel;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // 供 --shot 自动化测试时跳过首次目录向导
    void setSkipSetup(bool skip) { m_skipSetup = skip; }

    // 供自动化测试：在列表里选中指定版本并立即开始下载
    void startVersion(const QString &id);

    // 供自动化测试：打开加载器安装对话框，延时截图后退出
    void shotLoaderDialog(const QString &pngPath, int delayMs, const QString &loaderKind = QString());
    // 版本清单是否已加载（自动化测试等它再动手）
    bool manifestReady() const { return !m_manifest.versions.isEmpty(); }

private:
    void buildUi();
    void loadSettings();
    void promptForRootDir();
    void chooseRootDir();
    void reloadManifest();
    void onManifestData(const QByteArray &data);
    void onManifestError(const QString &message);
    void fillVersionModel();
    void updateVersionInfo();
    void startDownload();
    void onStageChanged(md::InstallPipeline::Stage stage, const QString &text);
    void onStatsChanged(const md::DownloadStats &stats);
    void onTaskFinished(const md::TaskResult &result);
    void onPipelineFinished(bool ok, const QString &summary, bool canceled);
    void onMirrorChanged(int index);
    void onConcurrencyChanged(int value);
    void openLoaderDialog();
    void scanLocalVersions();
    void appendLog(const QString &line);
    void refreshButtons();
    void updateAttribution();

    DownloadManager *m_dm = nullptr;
    InstallPipeline *m_pipeline = nullptr;
    HttpFetcher *m_manifestFetcher = nullptr;
    VersionListModel *m_versionModel = nullptr;
    FileTableModel *m_fileModel = nullptr;

    VersionManifest m_manifest;
    QString m_selectedVersion;
    bool m_skipSetup = false;
    bool m_busy = false;

    QLineEdit *m_rootEdit = nullptr;
    QComboBox *m_mirrorCombo = nullptr;
    QSpinBox *m_concurrencySpin = nullptr;
    QCheckBox *m_snapshotCheck = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QPushButton *m_loaderBtn = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QListView *m_versionView = nullptr;

    QLabel *m_verTitle = nullptr;
    QLabel *m_verInfo = nullptr;
    QLabel *m_stageLabel = nullptr;
    QLabel *m_statsLabel = nullptr;
    QPushButton *m_startBtn = nullptr;
    QPushButton *m_pauseBtn = nullptr;
    QPushButton *m_resumeBtn = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    QProgressBar *m_progress = nullptr;
    QTableView *m_fileView = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QLabel *m_attribution = nullptr;
};

} // namespace md
