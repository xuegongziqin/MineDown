#include "ui/MainWindow.h"

#include "core/DownloadManager.h"
#include "core/Elevation.h"
#include "core/HttpFetcher.h"
#include "core/ManifestParser.h"
#include "core/Mirror.h"
#include "core/Settings.h"
#include "ui/FileTableModel.h"
#include "ui/LoaderDialog.h"
#include "ui/SetupDialog.h"
#include "ui/VersionListModel.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

namespace md {

namespace {

const QString kManifestUrl =
    QStringLiteral("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json");

QString manifestCachePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + QStringLiteral("/version_manifest_v2.json");
}

bool stageIsActive(InstallPipeline::Stage s)
{
    switch (s) {
    case InstallPipeline::Stage::VersionJson:
    case InstallPipeline::Stage::AssetIndex:
    case InstallPipeline::Stage::Client:
    case InstallPipeline::Stage::Libraries:
    case InstallPipeline::Stage::Assets:
        return true;
    default:
        return false;
    }
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    buildUi();
    loadSettings();

    m_manifestFetcher = new HttpFetcher(this);
    connect(m_manifestFetcher, &HttpFetcher::done, this,
            [this](const QByteArray &data, const QUrl &) { onManifestData(data); });
    connect(m_manifestFetcher, &HttpFetcher::error, this, &MainWindow::onManifestError);

    m_dm = new DownloadManager(this);
    m_dm->setConcurrency(m_concurrencySpin->value());

    m_pipeline = new InstallPipeline(m_dm, this);
    connect(m_pipeline, &InstallPipeline::stageChanged, this, &MainWindow::onStageChanged);
    connect(m_pipeline, &InstallPipeline::planReset, this,
            [this]() { m_fileModel->clearRows(); });
    connect(m_pipeline, &InstallPipeline::taskPlanned, this,
            [this](const DownloadTask &t) { m_fileModel->addTask(t); });
    connect(m_pipeline, &InstallPipeline::planned, this,
            [this](qint64 bytes, int files) {
                appendLog(QStringLiteral("计划：%1 个文件，约 %2")
                              .arg(files)
                              .arg(humanBytes(bytes)));
                m_progress->setValue(0);
            });
    connect(m_pipeline, &InstallPipeline::finished, this, &MainWindow::onPipelineFinished);
    connect(m_pipeline, &InstallPipeline::failed, this, [this](const QString &msg) {
        m_busy = false;
        refreshButtons();
        appendLog(QStringLiteral("失败：%1").arg(msg));
        QMessageBox::warning(this, QStringLiteral("安装失败"), msg);
    });
    connect(m_pipeline, &InstallPipeline::logLine, this, &MainWindow::appendLog);

    connect(m_dm, &DownloadManager::statsChanged, this, &MainWindow::onStatsChanged);
    connect(m_dm, &DownloadManager::taskFinished, this, &MainWindow::onTaskFinished);
    connect(m_dm, &DownloadManager::taskProgress, this,
            [this](const QString &dest, qint64 received, qint64 total) {
                m_fileModel->setTaskProgress(dest, received, total);
            });
    connect(m_dm, &DownloadManager::message, this, &MainWindow::appendLog);

    QTimer::singleShot(0, this, &MainWindow::promptForRootDir);
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi()
{
    setWindowTitle(md::isProcessElevated()
                       ? QStringLiteral("MineDown · Minecraft 下载器（管理员）")
                       : QStringLiteral("MineDown · Minecraft 下载器"));
    resize(1180, 780);

    auto *central = new QWidget(this);
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(10, 10, 10, 8);
    outer->setSpacing(8);

    // —— 顶部工具行 ——
    auto *top = new QHBoxLayout;
    top->setSpacing(8);
    top->addWidget(new QLabel(QStringLiteral("游戏目录："), this));
    m_rootEdit = new QLineEdit(this);
    m_rootEdit->setReadOnly(true);
    m_rootEdit->setMinimumWidth(300);
    top->addWidget(m_rootEdit, 1);
    auto *rootBtn = new QPushButton(QStringLiteral("更改…"), this);
    connect(rootBtn, &QPushButton::clicked, this, &MainWindow::chooseRootDir);
    top->addWidget(rootBtn);

    top->addSpacing(10);
    top->addWidget(new QLabel(QStringLiteral("镜像源："), this));
    m_mirrorCombo = new QComboBox(this);
    const QVector<MirrorInfo> mirrors = builtinMirrors();
    for (const MirrorInfo &m : mirrors)
        m_mirrorCombo->addItem(m.name, m.key);
    top->addWidget(m_mirrorCombo);

    top->addSpacing(10);
    top->addWidget(new QLabel(QStringLiteral("并发数："), this));
    m_concurrencySpin = new QSpinBox(this);
    m_concurrencySpin->setRange(1, 32);
    m_concurrencySpin->setToolTip(QStringLiteral("同时下载的文件数，建议 8～16"));
    top->addWidget(m_concurrencySpin);

    top->addSpacing(10);
    m_refreshBtn = new QPushButton(QStringLiteral("刷新版本列表"), this);
    connect(m_refreshBtn, &QPushButton::clicked, this, &MainWindow::reloadManifest);
    top->addWidget(m_refreshBtn);

    m_loaderBtn = new QPushButton(QStringLiteral("安装加载器…"), this);
    m_loaderBtn->setToolTip(QStringLiteral("安装 Fabric / Forge / NeoForge 到独立版本目录"));
    connect(m_loaderBtn, &QPushButton::clicked, this, &MainWindow::openLoaderDialog);
    top->addWidget(m_loaderBtn);
    outer->addLayout(top);

    // —— 左侧：版本列表 ——
    auto *left = new QWidget(central);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(6);

    auto *leftHead = new QHBoxLayout;
    auto *leftTitle = new QLabel(QStringLiteral("版本列表"), left);
    QFont titleFont = leftTitle->font();
    titleFont.setBold(true);
    leftTitle->setFont(titleFont);
    leftHead->addWidget(leftTitle);
    leftHead->addStretch(1);
    m_snapshotCheck = new QCheckBox(QStringLiteral("显示快照"), left);
    leftHead->addWidget(m_snapshotCheck);
    leftLayout->addLayout(leftHead);

    m_searchEdit = new QLineEdit(left);
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索版本号…"));
    m_searchEdit->setClearButtonEnabled(true);
    leftLayout->addWidget(m_searchEdit);

    m_versionView = new QListView(left);
    m_versionModel = new VersionListModel(this);
    m_versionView->setModel(m_versionModel);
    m_versionView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_versionView->setUniformItemSizes(true);
    m_versionView->setAlternatingRowColors(true);
    leftLayout->addWidget(m_versionView, 1);

    // —— 右侧：详情 / 操作 / 进度 / 文件 / 日志 ——
    auto *right = new QWidget(central);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(8);

    m_verTitle = new QLabel(QStringLiteral("请选择版本"), right);
    QFont bigFont = m_verTitle->font();
    bigFont.setPointSizeF(bigFont.pointSizeF() + 6);
    bigFont.setBold(true);
    m_verTitle->setFont(bigFont);
    rightLayout->addWidget(m_verTitle);

    m_verInfo = new QLabel(QStringLiteral("在左侧列表里选择一个版本，然后点击「开始下载」。"), right);
    m_verInfo->setWordWrap(true);
    QPalette pal = m_verInfo->palette();
    pal.setColor(QPalette::WindowText, QColor(0x60, 0x60, 0x60));
    m_verInfo->setPalette(pal);
    rightLayout->addWidget(m_verInfo);

    auto *actRow = new QHBoxLayout;
    actRow->setSpacing(8);
    m_startBtn = new QPushButton(QStringLiteral("开始下载"), right);
    m_pauseBtn = new QPushButton(QStringLiteral("暂停"), right);
    m_resumeBtn = new QPushButton(QStringLiteral("继续"), right);
    m_cancelBtn = new QPushButton(QStringLiteral("取消"), right);
    actRow->addWidget(m_startBtn);
    actRow->addWidget(m_pauseBtn);
    actRow->addWidget(m_resumeBtn);
    actRow->addWidget(m_cancelBtn);
    actRow->addStretch(1);
    rightLayout->addLayout(actRow);

    auto *progHead = new QHBoxLayout;
    m_stageLabel = new QLabel(QStringLiteral("等待开始"), right);
    m_statsLabel = new QLabel(QString(), right);
    progHead->addWidget(m_stageLabel);
    progHead->addStretch(1);
    progHead->addWidget(m_statsLabel);
    rightLayout->addLayout(progHead);

    m_progress = new QProgressBar(right);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);
    rightLayout->addWidget(m_progress);

    m_fileView = new QTableView(right);
    m_fileModel = new FileTableModel(this);
    m_fileView->setModel(m_fileModel);
    m_fileView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_fileView->setAlternatingRowColors(true);
    m_fileView->verticalHeader()->setVisible(false);
    m_fileView->horizontalHeader()->setSectionResizeMode(FileTableModel::ColName,
                                                         QHeaderView::Stretch);
    m_fileView->horizontalHeader()->setSectionResizeMode(FileTableModel::ColSize,
                                                         QHeaderView::ResizeToContents);
    m_fileView->horizontalHeader()->setSectionResizeMode(FileTableModel::ColState,
                                                         QHeaderView::ResizeToContents);
    rightLayout->addWidget(m_fileView, 1);

    m_log = new QPlainTextEdit(right);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(3000);
    m_log->setMinimumHeight(120);
    m_log->setMaximumHeight(150);
    m_log->setPlaceholderText(QStringLiteral("运行日志"));
    rightLayout->addWidget(m_log);

    auto *split = new QSplitter(Qt::Horizontal, central);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({300, 880});
    split->setChildrenCollapsible(false);
    outer->addWidget(split, 1);

    setCentralWidget(central);

    m_attribution = new QLabel(this);
    statusBar()->addPermanentWidget(m_attribution);
    statusBar()->setSizeGripEnabled(true);

    // 响应式行为（下拉框里没有显示 → 按关键字过滤）
    connect(m_versionView, &QListView::doubleClicked, this,
            [this](const QModelIndex &) { startDownload(); });
    connect(m_versionView->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &, const QModelIndex &) {
                updateVersionInfo();
                refreshButtons();
            });
    connect(m_searchEdit, &QLineEdit::textChanged, this,
            [this](const QString &t) { m_versionModel->setFilter(t); });
    connect(m_snapshotCheck, &QCheckBox::toggled, this, [this](bool on) {
        Settings::instance().setShowSnapshots(on);
        m_versionModel->setShowSnapshots(on);
    });
    connect(m_mirrorCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::onMirrorChanged);
    connect(m_concurrencySpin, &QSpinBox::valueChanged, this,
            &MainWindow::onConcurrencyChanged);
    connect(m_startBtn, &QPushButton::clicked, this, &MainWindow::startDownload);
    connect(m_pauseBtn, &QPushButton::clicked, this, [this]() {
        m_pipeline->pause();
        refreshButtons();
    });
    connect(m_resumeBtn, &QPushButton::clicked, this, [this]() {
        m_pipeline->resume();
        refreshButtons();
    });
    connect(m_cancelBtn, &QPushButton::clicked, this, [this]() {
        if (QMessageBox::question(this, QStringLiteral("取消下载"),
                                  QStringLiteral("确定要取消当前下载吗？已下载的临时文件会保留。"))
            != QMessageBox::Yes)
            return;
        m_pipeline->cancel();
        m_fileModel->markCanceled();
        m_busy = false;
        refreshButtons();
    });

    updateAttribution();
    refreshButtons();
}

void MainWindow::loadSettings()
{
    Settings &s = Settings::instance();
    m_rootEdit->setText(QDir::toNativeSeparators(s.rootDir()));

    const int mi = m_mirrorCombo->findData(s.mirrorKey());
    if (mi >= 0) {
        const QSignalBlocker block(m_mirrorCombo);
        m_mirrorCombo->setCurrentIndex(mi);
    }
    m_concurrencySpin->setValue(s.concurrency());
    m_snapshotCheck->setChecked(s.showSnapshots());
    m_versionModel->setShowSnapshots(s.showSnapshots());
    updateAttribution();
}

void MainWindow::promptForRootDir()
{
    Settings &s = Settings::instance();
    if (s.hasRootDir()) {
        m_rootEdit->setText(QDir::toNativeSeparators(s.rootDir()));
        reloadManifest();
        return;
    }
    if (m_skipSetup) {
        appendLog(QStringLiteral("未设置游戏目录（已跳过首次向导）"));
        return;
    }

    SetupDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) {
        QApplication::quit();
        return;
    }
    s.setRootDir(dlg.selectedDir());
    s.sync();
    m_rootEdit->setText(QDir::toNativeSeparators(s.rootDir()));
    appendLog(QStringLiteral("游戏目录：%1").arg(m_rootEdit->text()));
    reloadManifest();
}

void MainWindow::chooseRootDir()
{
    if (m_busy) {
        QMessageBox::information(this, QStringLiteral("请稍候"),
                                 QStringLiteral("下载过程中不能更换游戏目录。"));
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择 .minecraft 目录"), Settings::instance().rootDir());
    if (dir.isEmpty())
        return;
    Settings::instance().setRootDir(dir);
    Settings::instance().sync();
    m_rootEdit->setText(QDir::toNativeSeparators(dir));
    appendLog(QStringLiteral("游戏目录已改为：%1").arg(m_rootEdit->text()));
    reloadManifest();
}

void MainWindow::reloadManifest()
{
    if (Settings::instance().rootDir().isEmpty()) {
        appendLog(QStringLiteral("请先设置游戏目录"));
        return;
    }
    m_refreshBtn->setEnabled(false);
    appendLog(QStringLiteral("正在获取版本清单…"));
    const QUrl manifestUrl(applyMirror(kManifestUrl, Settings::instance().mirrorBase()));
    m_manifestFetcher->get(manifestUrl, QString(), QUrl(fallbackForUrl(manifestUrl.toString())));
}

void MainWindow::onManifestData(const QByteArray &data)
{
    m_refreshBtn->setEnabled(true);

    VersionManifest manifest;
    QString err;
    if (!parseVersionManifest(data, &manifest, &err)) {
        onManifestError(QStringLiteral("解析版本清单失败：%1").arg(err));
        return;
    }

    m_manifest = manifest;

    const QString cache = manifestCachePath();
    QDir().mkpath(QFileInfo(cache).absolutePath());
    QFile f(cache);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(data);

    fillVersionModel();
    appendLog(QStringLiteral("版本清单已加载：%1 个版本（最新正式版 %2）")
                  .arg(m_manifest.versions.size())
                  .arg(m_manifest.latestRelease));
}

void MainWindow::onManifestError(const QString &message)
{
    m_refreshBtn->setEnabled(true);

    const QString cache = manifestCachePath();
    if (QFileInfo::exists(cache)) {
        QFile f(cache);
        if (f.open(QIODevice::ReadOnly)) {
            VersionManifest manifest;
            QString err;
            if (parseVersionManifest(f.readAll(), &manifest, &err)) {
                m_manifest = manifest;
                fillVersionModel();
                appendLog(QStringLiteral("网络获取失败，已改用本地缓存的版本清单。"));
                appendLog(QStringLiteral("原因：%1").arg(message));
                return;
            }
        }
    }

    appendLog(QStringLiteral("获取版本清单失败：%1").arg(message));
    QMessageBox::warning(this, QStringLiteral("无法获取版本清单"),
                         QStringLiteral("%1\n\n可以尝试换一个镜像源，或检查网络连接。")
                             .arg(message));
}

void MainWindow::fillVersionModel()
{
    m_versionModel->setVersions(m_manifest);
    m_versionModel->setShowSnapshots(m_snapshotCheck->isChecked());
    m_versionModel->setFilter(m_searchEdit->text());
    scanLocalVersions();

    int target = -1;
    const QString last = Settings::instance().lastVersion();
    for (int r = 0; r < m_versionModel->rowCount(); ++r) {
        const VersionEntry *e = m_versionModel->entryAt(r);
        if (e && e->id == last) {
            target = r;
            break;
        }
    }
    if (target < 0 && m_versionModel->rowCount() > 0)
        target = 0;
    if (target >= 0)
        m_versionView->setCurrentIndex(m_versionModel->index(target, 0));

    updateVersionInfo();
    refreshButtons();
}

void MainWindow::updateVersionInfo()
{
    const QModelIndex idx = m_versionView->currentIndex();
    const VersionEntry *e = idx.isValid() ? m_versionModel->entryAt(idx.row()) : nullptr;
    if (!e) {
        m_verTitle->setText(QStringLiteral("请选择版本"));
        m_verInfo->setText(QStringLiteral("在左侧列表里选择一个版本，然后点击「开始下载」。"));
        return;
    }

    if (idx.isValid() && m_versionModel->isLocalRow(idx.row())) {
        m_verTitle->setText(e->id);
        m_verInfo->setText(QStringLiteral(
            "本地已安装的版本（清单里没有，通常是加载器装的）。"
            "它继承的原版版本需要单独下载；这里不需要再下载。"));
        return;
    }

    m_verTitle->setText(e->id);

    const QString root = Settings::instance().rootDir();
    const QString jarPath =
        QDir(root).filePath(QStringLiteral("versions/%1/%1.jar").arg(e->id));
    const bool installed = QFileInfo::exists(jarPath);

    m_verInfo->setText(QStringLiteral("类型：%1　　发布时间：%2%3")
                           .arg(e->type, e->releaseTime,
                                installed ? QStringLiteral("　　本地已有该版本")
                                          : QString()));
}

void MainWindow::startDownload()
{
    if (m_busy)
        return;

    const QModelIndex idx = m_versionView->currentIndex();
    const VersionEntry *e = idx.isValid() ? m_versionModel->entryAt(idx.row()) : nullptr;
    if (!e) {
        appendLog(QStringLiteral("请先在左侧选择要下载的版本"));
        return;
    }
    if (m_versionModel->isLocalRow(idx.row())) {
        appendLog(QStringLiteral("%1 是本地已安装的版本，不需要下载。").arg(e->id));
        return;
    }

    const QString root = Settings::instance().rootDir();
    if (root.isEmpty()) {
        promptForRootDir();
        return;
    }

    m_selectedVersion = e->id;
    Settings::instance().setLastVersion(e->id);
    m_fileModel->clearRows();
    m_progress->setValue(0);
    m_statsLabel->clear();
    m_stageLabel->setText(QStringLiteral("准备中…"));

    m_pipeline->setRootDir(root);
    m_pipeline->setManifest(m_manifest);

    m_busy = true;
    refreshButtons();
    if (!m_pipeline->start(e->id)) {
        m_busy = false;
        refreshButtons();
    }
}

void MainWindow::startVersion(const QString &id)
{
    for (int r = 0; r < m_versionModel->rowCount(); ++r) {
        const VersionEntry *e = m_versionModel->entryAt(r);
        if (e && e->id == id) {
            m_versionView->setCurrentIndex(m_versionModel->index(r, 0));
            updateVersionInfo();
            startDownload();
            return;
        }
    }
    appendLog(QStringLiteral("版本列表里找不到 %1").arg(id));
}

void MainWindow::onStageChanged(InstallPipeline::Stage stage, const QString &text)
{
    m_stageLabel->setText(text);
    m_busy = stageIsActive(stage);
    refreshButtons();
}

void MainWindow::onStatsChanged(const DownloadStats &stats)
{
    const qint64 planned = m_pipeline->plannedBytes();
    int pct = 0;
    if (planned > 0)
        pct = static_cast<int>(qMin<qint64>(100, stats.doneBytes * 100 / planned));
    m_progress->setValue(pct);

    m_statsLabel->setText(QStringLiteral("%1 / %2　文件 %3/%4　失败 %5　%6/s")
                              .arg(humanBytes(stats.doneBytes), humanBytes(planned))
                              .arg(stats.doneFiles)
                              .arg(m_pipeline->plannedFiles())
                              .arg(stats.failedFiles)
                              .arg(humanBytes(static_cast<qint64>(stats.speedBytesPerSec())))
                              .replace(QStringLiteral("　"), QStringLiteral("  ")));
}

void MainWindow::onTaskFinished(const TaskResult &result)
{
    m_fileModel->setTaskResult(result);
    if (result.state == TaskState::Failed) {
        appendLog(QStringLiteral("失败：%1 —— %2")
                      .arg(result.task.label.isEmpty() ? result.task.dest : result.task.label,
                           result.error));
    }
}

void MainWindow::onPipelineFinished(bool ok, const QString &summary, bool canceled)
{
    m_busy = false;
    refreshButtons();
    if (canceled) {
        // 取消是用户自己的操作，不用弹窗，日志里 DownloadManager 已经说过一次了
        m_stageLabel->setText(QStringLiteral("已取消"));
        return;
    }
    appendLog(summary);
    if (ok) {
        m_stageLabel->setText(QStringLiteral("下载完成"));
        scanLocalVersions();
        QMessageBox::information(this, QStringLiteral("下载完成"), summary);
        return;
    }

    m_stageLabel->setText(QStringLiteral("下载结束"));
    const int denied = m_dm ? m_dm->permissionDeniedFiles() : 0;
    const bool elevated = md::isProcessElevated();

    QString text = QStringLiteral("%1\n\n失败的文件已标记在列表里，"
                                  "再次点击「开始下载」会重试它们。")
                       .arg(summary);
    if (denied > 0) {
        const QString root = QDir::toNativeSeparators(Settings::instance().rootDir());
        text += QStringLiteral("\n\n其中 %1 个文件是「拒绝访问」：它们属于其它账户（常见于从旧系统或备份"
                               "拷来的 .minecraft）或受系统保护，单纯重试不会好转。")
                    .arg(denied);
        if (elevated) {
            text += QStringLiteral("\n\n当前已经是管理员权限，那这些文件的权限连管理员都被挡住了，"
                                   "用管理员身份执行一次：\n\n"
                                   "takeown /f \"%1\" /r /d y\n"
                                   "icacls \"%1\" /grant *S-1-5-32-545:(OI)(CI)M /t /c")
                        .arg(root);
        } else {
            text += QStringLiteral("\n\n点下面的「以管理员身份重试」，程序会用 UAC 重新启动并自动"
                                   "重试这个版本；如果重启后还失败，再用管理员执行：\n\n"
                                   "takeown /f \"%1\" /r /d y\n"
                                   "icacls \"%1\" /grant *S-1-5-32-545:(OI)(CI)M /t /c")
                        .arg(root);
        }
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("下载未全部成功"));
    box.setText(text);
    QPushButton *elevateButton = nullptr;
    if (denied > 0 && !elevated) {
        elevateButton = box.addButton(QStringLiteral("以管理员身份重试"), QMessageBox::AcceptRole);
        box.setDefaultButton(elevateButton);
    }
    box.addButton(QStringLiteral("关闭"), QMessageBox::RejectRole);
    box.exec();

    if (elevateButton && box.clickedButton() == elevateButton) {
        QStringList extra;
        if (!m_selectedVersion.isEmpty())
            extra << QStringLiteral("--auto") << m_selectedVersion;
        QString error;
        if (md::relaunchElevated(extra, &error)) {
            appendLog(QStringLiteral("已请求管理员权限重新启动；授权后会自动重试 %1")
                          .arg(m_selectedVersion.isEmpty() ? QStringLiteral("刚才的任务")
                                                           : m_selectedVersion));
            QCoreApplication::quit();
        } else {
            QMessageBox::warning(this, QStringLiteral("没有获得管理员权限"), error);
        }
    }
}

void MainWindow::onMirrorChanged(int index)
{
    if (index < 0)
        return;

    const QString key = m_mirrorCombo->itemData(index).toString();
    if (key == QStringLiteral("custom")) {
        bool ok = false;
        const QString base = QInputDialog::getText(
            this, QStringLiteral("自定义镜像"),
            QStringLiteral("镜像根地址（例如 https://example.com，留空取消）："),
            QLineEdit::Normal, Settings::instance().customMirrorBase(), &ok);
        if (!ok || base.trimmed().isEmpty()) {
            const QSignalBlocker block(m_mirrorCombo);
            const int prev = m_mirrorCombo->findData(Settings::instance().mirrorKey());
            if (prev >= 0)
                m_mirrorCombo->setCurrentIndex(prev);
            return;
        }
        Settings::instance().setCustomMirrorBase(base.trimmed());
    }

    Settings::instance().setMirrorKey(key);
    Settings::instance().sync();
    updateAttribution();
    appendLog(QStringLiteral("镜像源切换为：%1").arg(m_mirrorCombo->currentText()));
    if (!m_busy)
        reloadManifest();
}

void MainWindow::onConcurrencyChanged(int value)
{
    Settings::instance().setConcurrency(value);
    if (m_dm)
        m_dm->setConcurrency(value);
    if (m_busy)
        appendLog(QStringLiteral("并发数将在当前下载阶段结束后生效"));
}

void MainWindow::updateAttribution()
{
    const QString key = m_mirrorCombo->currentData().toString();
    if (key == QStringLiteral("bmclapi"))
        m_attribution->setText(QStringLiteral("下载加速由 BMCLAPI 提供"));
    else if (key == QStringLiteral("hkmc"))
        m_attribution->setText(QStringLiteral("下载加速：MC 光速（主线路）"));
    else if (key == QStringLiteral("hkmc-eo"))
        m_attribution->setText(QStringLiteral("下载加速：MC 光速（EO 线路）"));
    else if (key == QStringLiteral("custom"))
        m_attribution->setText(QStringLiteral("使用自定义镜像源"));
    else
        m_attribution->setText(QStringLiteral("直连 Mojang 官方源"));
}

void MainWindow::appendLog(const QString &line)
{
    if (!m_log)
        return;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    m_log->appendPlainText(QStringLiteral("[%1] %2").arg(stamp, line));
}

void MainWindow::openLoaderDialog()
{
    const QString root = Settings::instance().rootDir();
    if (root.isEmpty()) {
        promptForRootDir();
        return;
    }
    if (m_busy) {
        QMessageBox::information(this, QStringLiteral("请稍候"),
                                 QStringLiteral("下载过程中不能安装加载器。"));
        return;
    }
    if (m_manifest.versions.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("版本清单还没加载"),
                                 QStringLiteral("请先等版本清单加载完成，再安装加载器。"));
        return;
    }

    LoaderDialog dlg(m_manifest, root, Settings::instance().concurrency(), this);
    connect(&dlg, &LoaderDialog::installed, this, [this](const QString &versionId) {
        appendLog(QStringLiteral("加载器安装完成：%1").arg(versionId));
        scanLocalVersions();
    });
    dlg.exec();
    scanLocalVersions();
}

void MainWindow::shotLoaderDialog(const QString &pngPath, int delayMs, const QString &loaderKind)
{
    const QString root = Settings::instance().rootDir();
    if (root.isEmpty() || m_manifest.versions.isEmpty()) {
        appendLog(QStringLiteral("截图失败：没有游戏目录或版本清单"));
        return;
    }

    auto *dlg = new LoaderDialog(m_manifest, root, Settings::instance().concurrency(), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    if (!loaderKind.isEmpty()) {
        LoaderKind kind = LoaderKind::Fabric;
        if (loaderKindFromKey(loaderKind, &kind))
            dlg->preselectKind(kind);
    }
    dlg->show();

    QTimer::singleShot(delayMs, this, [dlg, pngPath]() {
        dlg->grab().save(pngPath);
        QApplication::quit();
    });
}

void MainWindow::scanLocalVersions()
{
    const QString root = Settings::instance().rootDir();
    if (root.isEmpty() || !m_versionModel)
        return;

    QStringList ids;
    QDir versionsDir(QDir(root).filePath(QStringLiteral("versions")));
    const QFileInfoList dirs = versionsDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &fi : dirs) {
        // 只有「目录里有同名 json」的才算一个可用版本
        const QString json = QDir(fi.absoluteFilePath()).filePath(fi.fileName() + QStringLiteral(".json"));
        if (QFileInfo::exists(json))
            ids.append(fi.fileName());
    }
    ids.sort(Qt::CaseInsensitive);
    m_versionModel->setLocalVersions(ids);
}

void MainWindow::refreshButtons()
{
    const QModelIndex idx = m_versionView ? m_versionView->currentIndex() : QModelIndex();
    const bool hasSelection = idx.isValid();
    const bool localOnly = hasSelection && m_versionModel->isLocalRow(idx.row());
    m_startBtn->setEnabled(!m_busy && hasSelection && !localOnly);
    m_startBtn->setText(localOnly ? QStringLiteral("已安装") : QStringLiteral("开始下载"));
    m_pauseBtn->setEnabled(m_busy && !m_pipeline->paused());
    m_resumeBtn->setEnabled(m_busy && m_pipeline->paused());
    m_cancelBtn->setEnabled(m_busy);
    m_refreshBtn->setEnabled(!m_busy);
    m_loaderBtn->setEnabled(!m_busy);
    m_mirrorCombo->setEnabled(!m_busy);
    m_concurrencySpin->setEnabled(true);
}

} // namespace md
