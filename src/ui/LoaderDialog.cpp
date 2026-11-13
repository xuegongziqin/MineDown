#include "ui/LoaderDialog.h"

#include "core/Elevation.h"
#include "core/LoaderInstaller.h"
#include "core/Settings.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTime>
#include <QVBoxLayout>

namespace md {

LoaderDialog::LoaderDialog(const VersionManifest &manifest, const QString &rootDir,
                           int concurrency, QWidget *parent)
    : QDialog(parent), m_manifest(manifest), m_rootDir(rootDir), m_concurrency(concurrency)
{
    setWindowTitle(QStringLiteral("安装模组加载器"));
    resize(720, 560);

    m_catalog = new LoaderCatalog(this);
    m_installer = new LoaderInstaller(this);
    m_installer->setConcurrency(concurrency > 0 ? qMin(concurrency, 8) : 4);

    connect(m_catalog, &LoaderCatalog::ready, this, &LoaderDialog::onCatalogReady);
    connect(m_catalog, &LoaderCatalog::failed, this, &LoaderDialog::onCatalogFailed);
    connect(m_installer, &LoaderInstaller::stageChanged, this, [this](const QString &t) {
        m_stageLabel->setText(t);
    });
    connect(m_installer, &LoaderInstaller::logLine, this, &LoaderDialog::appendLog);
    connect(m_installer, &LoaderInstaller::progress, this, [this](qint64 done, qint64 total) {
        if (total > 0)
            m_progress->setValue(static_cast<int>(done * 1000 / total));
    });
    connect(m_installer, &LoaderInstaller::finished, this, &LoaderDialog::onFinished);

    buildUi();
    fillBaseVersions();
    onKindChanged();
}

LoaderDialog::~LoaderDialog() = default;

void LoaderDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_kindCombo = new QComboBox(this);
    for (LoaderKind k : {LoaderKind::Fabric, LoaderKind::Forge, LoaderKind::NeoForge})
        m_kindCombo->addItem(loaderKindName(k), static_cast<int>(k));
    form->addRow(QStringLiteral("加载器："), m_kindCombo);

    m_baseCombo = new QComboBox(this);
    m_baseCombo->setMinimumWidth(220);
    form->addRow(QStringLiteral("原版版本："), m_baseCombo);

    auto *loaderRow = new QHBoxLayout;
    m_loaderCombo = new QComboBox(this);
    m_loaderCombo->setMinimumWidth(260);
    m_fetchBtn = new QPushButton(QStringLiteral("获取列表"), this);
    loaderRow->addWidget(m_loaderCombo, 1);
    loaderRow->addWidget(m_fetchBtn);
    form->addRow(QStringLiteral("加载器版本："), loaderRow);

    root->addLayout(form);

    m_hintLabel = new QLabel(this);
    m_hintLabel->setWordWrap(true);
    m_hintLabel->setText(QStringLiteral(
        "安装到 versions/<版本名>/，只用 versions/ 与 libraries/，不会改动原版文件。"
        "Forge 1.13 以后和 NeoForge 需要本机有 Java（会自动查找），安装过程要跑安装器的 processor。"));
    root->addWidget(m_hintLabel);

    m_stageLabel = new QLabel(QStringLiteral("等待开始"), this);
    root->addWidget(m_stageLabel);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    root->addWidget(m_progress);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(4000);
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSize(9);
    m_log->setFont(mono);
    root->addWidget(m_log, 1);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    m_closeBtn = new QPushButton(QStringLiteral("关闭"), this);
    m_installBtn = new QPushButton(QStringLiteral("开始安装"), this);
    m_installBtn->setDefault(true);
    buttons->addWidget(m_closeBtn);
    buttons->addWidget(m_installBtn);
    root->addLayout(buttons);

    connect(m_kindCombo, &QComboBox::currentIndexChanged, this, &LoaderDialog::onKindChanged);
    connect(m_baseCombo, &QComboBox::currentIndexChanged, this, &LoaderDialog::onBaseChanged);
    connect(m_fetchBtn, &QPushButton::clicked, this, &LoaderDialog::fetchLoaderVersions);
    connect(m_installBtn, &QPushButton::clicked, this, &LoaderDialog::startInstall);
    connect(m_closeBtn, &QPushButton::clicked, this, &LoaderDialog::reject);
}

void LoaderDialog::preselectKind(LoaderKind kind)
{
    const int idx = m_kindCombo->findData(static_cast<int>(kind));
    if (idx >= 0)
        m_kindCombo->setCurrentIndex(idx);
}

void LoaderDialog::fillBaseVersions()
{
    // 只列正式版：加载器基本都只跟正式版走。
    for (const VersionEntry &e : m_manifest.versions) {
        if (!e.isRelease())
            continue;
        m_baseCombo->addItem(e.id);
    }
    if (m_baseCombo->count() == 0) {
        for (const VersionEntry &e : m_manifest.versions)
            m_baseCombo->addItem(e.id);
    }

    // 先选本地已经装好的正式版里最新的那个（加载器要继承它，原版没有还得再下），
    // 退而求其次才是上次下载的版本、最新正式版。
    QString pick;
    const QString root = m_rootDir;
    if (!root.isEmpty()) {
        QDir versionsDir(QDir(root).filePath(QStringLiteral("versions")));
        const QFileInfoList dirs = versionsDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &fi : dirs) {
            const QString id = fi.fileName();
            const QString jar =
                QDir(fi.absoluteFilePath()).filePath(id + QStringLiteral(".jar"));
            if (!QFileInfo::exists(jar))
                continue;
            if (m_baseCombo->findText(id) < 0)
                continue;
            if (pick.isEmpty() || compareVersionStrings(id, pick) > 0)
                pick = id;
        }
    }
    if (pick.isEmpty())
        pick = Settings::instance().lastVersion();
    int idx = m_baseCombo->findText(pick);
    if (idx < 0)
        idx = m_baseCombo->findText(m_manifest.latestRelease);
    if (idx < 0 && m_baseCombo->count() > 0)
        idx = 0;
    if (idx >= 0) {
        const QSignalBlocker block(m_baseCombo);
        m_baseCombo->setCurrentIndex(idx);
    }
}

void LoaderDialog::onKindChanged()
{
    const auto kind = static_cast<LoaderKind>(m_kindCombo->currentData().toInt());
    if (kind == LoaderKind::Fabric) {
        m_hintLabel->setText(QStringLiteral(
            "Fabric：直接从 Meta 接口取库列表和启动主类，合成版本 JSON，不需要 Java，速度最快。"));
    } else if (kind == LoaderKind::Forge) {
        m_hintLabel->setText(QStringLiteral(
            "Forge：下载官方安装器，按 install_profile.json 顺序执行 processor（需要 Java 17 或更高）。"
            "1.12.2 及更早的老安装器结构不同，会自动走解压内置库的路线。"));
    } else {
        m_hintLabel->setText(QStringLiteral(
            "NeoForge：安装器结构与 Forge 相同，需要 Java（1.20.6 起要 Java 21，26.x 起要 Java 25）。"
            "版本列表从 maven 元数据读取（官方 API 在本机网络下不可达）。"));
    }
    fetchLoaderVersions();
}

void LoaderDialog::onBaseChanged()
{
    fetchLoaderVersions();
}

void LoaderDialog::fetchLoaderVersions()
{
    if (m_installing)
        return;
    const QString mc = m_baseCombo->currentText();
    if (mc.isEmpty())
        return;

    // 切换加载器/原版时上一次请求可能还在路上：中止它，别让旧结果覆盖新列表。
    if (m_fetching) {
        m_catalog->abort();
        m_fetching = false;
    }

    const auto kind = static_cast<LoaderKind>(m_kindCombo->currentData().toInt());
    m_pendingKind = kind;

    m_fetching = true;
    m_versions.clear();
    m_loaderCombo->clear();
    m_loaderCombo->addItem(QStringLiteral("正在获取…"));
    m_stageLabel->setText(QStringLiteral("正在获取加载器版本列表…"));
    updateButtons();

    appendLog(QStringLiteral("获取 %1（%2）的版本列表…").arg(loaderKindName(kind), mc));
    m_catalog->fetch(kind, mc);
}

void LoaderDialog::onCatalogReady(const QVector<md::LoaderVersion> &versions)
{
    // 丢弃过期的结果（用户已经换成别的加载器了）
    if (!versions.isEmpty() && versions.first().kind != m_pendingKind)
        return;

    m_fetching = false;
    m_versions = versions;
    m_loaderCombo->clear();

    if (versions.isEmpty()) {
        onCatalogFailed(QStringLiteral("这个原版版本没有可用的加载器版本。"));
        return;
    }

    for (const LoaderVersion &v : versions)
        m_loaderCombo->addItem(v.label());

    // 优先选第一个稳定版（列表本身已按版本降序）
    int pick = 0;
    for (int i = 0; i < versions.size(); ++i) {
        if (versions.at(i).stable) {
            pick = i;
            break;
        }
    }
    m_loaderCombo->setCurrentIndex(pick);

    m_stageLabel->setText(QStringLiteral("共 %1 个版本可选").arg(versions.size()));
    appendLog(QStringLiteral("共 %1 个版本，默认选 %2")
                  .arg(versions.size())
                  .arg(versions.at(pick).loaderVersion));
    updateButtons();
}

void LoaderDialog::onCatalogFailed(const QString &message)
{
    m_fetching = false;
    m_versions.clear();
    m_loaderCombo->clear();
    m_loaderCombo->addItem(QStringLiteral("（获取失败）"));
    m_stageLabel->setText(QStringLiteral("加载器版本列表获取失败"));
    appendLog(QStringLiteral("获取失败：%1").arg(message));
    updateButtons();
}

void LoaderDialog::startInstall()
{
    if (m_installing || m_fetching)
        return;
    const int idx = m_loaderCombo->currentIndex();
    if (idx < 0 || idx >= m_versions.size()) {
        appendLog(QStringLiteral("没有可安装的加载器版本，先点「获取列表」。"));
        return;
    }
    if (m_rootDir.isEmpty()) {
        appendLog(QStringLiteral("还没有设置游戏目录。"));
        return;
    }

    const LoaderVersion v = m_versions.at(idx);
    LoaderInstaller::Request req;
    req.kind = v.kind;
    req.mcVersion = m_baseCombo->currentText();
    req.loaderVersion = v.loaderVersion;
    req.versionId = v.versionId;
    req.rootDir = m_rootDir;
    req.fabricEntry = v.raw;

    m_installing = true;
    m_progress->setValue(0);
    m_stageLabel->setText(QStringLiteral("准备中…"));
    updateButtons();

    m_installer->start(req);
}

void LoaderDialog::onFinished(bool ok, const QString &message, const QString &versionId)
{
    m_installing = false;
    updateButtons();

    if (m_closing) {
        // 用户已经决定关窗口并取消安装，不用再弹「安装失败」
        appendLog((ok ? QStringLiteral("结果：成功 —— ") : QStringLiteral("结果：已取消 —— "))
                  + message);
        return;
    }

    m_progress->setValue(ok ? 1000 : m_progress->value());
    m_stageLabel->setText(ok ? QStringLiteral("安装完成") : QStringLiteral("安装失败"));
    appendLog((ok ? QStringLiteral("结果：成功 —— ") : QStringLiteral("结果：失败 —— ")) + message);

    if (ok) {
        emit installed(versionId);
        QMessageBox::information(this, QStringLiteral("安装完成"),
                                 QStringLiteral("%1\n\n版本目录：versions/%2\n"
                                                "如果这个原版版本还没下载，请先在主界面下载原版。")
                                     .arg(message, versionId));
    } else {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QStringLiteral("安装失败"));
        box.setText(message);

        // 写不进 libraries/ versions/ 时，多半是目录权限问题：给个直接提权的入口
        const bool looksDenied = message.contains(QStringLiteral("拒绝访问"))
                                 || message.contains(QStringLiteral("无法替换"))
                                 || message.contains(QStringLiteral("无法删除"))
                                 || message.contains(QStringLiteral("无法写入"))
                                 || message.contains(QStringLiteral("Access is denied"),
                                                     Qt::CaseInsensitive);
        QPushButton *elevateButton = nullptr;
        if (looksDenied && !md::isProcessElevated()) {
            box.setInformativeText(QStringLiteral("装加载器要往 libraries/ 和 versions/ 里写文件，"
                                                  "目录权限不够就会失败。重启成管理员后请再点一次"
                                                  "「开始安装」（已经下载好的文件会跳过）。"));
            elevateButton = box.addButton(QStringLiteral("以管理员身份重启程序"),
                                          QMessageBox::AcceptRole);
            box.setDefaultButton(elevateButton);
        }
        box.addButton(QStringLiteral("关闭"), QMessageBox::RejectRole);
        box.exec();

        if (elevateButton && box.clickedButton() == elevateButton) {
            QString error;
            if (md::relaunchElevated(QStringList(), &error)) {
                m_closing = true;   // 本窗口跟着程序一起退出，交给提权后的新进程
                QDialog::reject();
                QCoreApplication::quit();
            } else {
                QMessageBox::warning(this, QStringLiteral("没有获得管理员权限"), error);
            }
        }
    }
}

void LoaderDialog::updateButtons()
{
    const bool idle = !m_fetching && !m_installing;
    m_closable = !m_installing;
    m_kindCombo->setEnabled(idle);
    m_baseCombo->setEnabled(idle);
    m_loaderCombo->setEnabled(idle);
    m_fetchBtn->setEnabled(idle);
    m_installBtn->setEnabled(idle && !m_versions.isEmpty());
    // 安装中「关闭」仍然可用，但会先问一句、再中断安装
    m_closeBtn->setText(m_installing ? QStringLiteral("取消安装并关闭") : QStringLiteral("关闭"));
}

void LoaderDialog::reject()
{
    if (!m_closable) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("安装正在进行"),
            QStringLiteral("安装还没结束。现在关闭会中断安装（已下载的文件会保留）。\n确定要取消吗？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
        m_closing = true;
        m_installing = false;
        if (m_installer)
            m_installer->cancel();
    }
    QDialog::reject();
}

void LoaderDialog::closeEvent(QCloseEvent *event)
{
    if (!m_closable) {
        // 交给 reject() 统一走「确认 + 取消安装」，别让窗口按钮绕过它
        event->ignore();
        reject();
        return;
    }
    QDialog::closeEvent(event);
}

void LoaderDialog::appendLog(const QString &line)
{
    m_log->appendPlainText(QStringLiteral("[%1] %2")
                               .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), line));
}

} // namespace md
