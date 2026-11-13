#include "ui/SetupDialog.h"

#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace md {

SetupDialog::SetupDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("MineDown · 首次使用"));
    setModal(true);
    setMinimumWidth(560);

    auto *title = new QLabel(QStringLiteral("请选择游戏目录"), this);
    QFont f = title->font();
    f.setPointSizeF(f.pointSizeF() + 4);
    f.setBold(true);
    title->setFont(f);

    auto *desc = new QLabel(
        QStringLiteral("请选择 Minecraft 的 .minecraft 目录（通常位于 %APPDATA%\\.minecraft）。\n"
                       "程序只会在此目录下创建 versions / libraries / assets 等下载内容，"
                       "不会改动其它启动器的文件。\n"
                       "该目录会记在本机配置中，下次启动直接使用。"),
        this);
    desc->setWordWrap(true);

    m_edit = new QLineEdit(this);
    m_edit->setPlaceholderText(QStringLiteral("例如 C:\\Users\\你\\AppData\\Roaming\\.minecraft"));

    auto *browseBtn = new QPushButton(QStringLiteral("浏览…"), this);
    connect(browseBtn, &QPushButton::clicked, this, &SetupDialog::browse);

    auto *row = new QHBoxLayout;
    row->addWidget(m_edit, 1);
    row->addWidget(browseBtn);

    auto *okBtn = new QPushButton(QStringLiteral("确定"), this);
    okBtn->setDefault(true);
    auto *quitBtn = new QPushButton(QStringLiteral("退出"), this);
    connect(okBtn, &QPushButton::clicked, this, &SetupDialog::accept);
    connect(quitBtn, &QPushButton::clicked, this, &SetupDialog::reject);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(okBtn);
    buttons->addWidget(quitBtn);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);
    layout->addWidget(title);
    layout->addWidget(desc);
    layout->addLayout(row);
    layout->addSpacing(6);
    layout->addLayout(buttons);
}

QString SetupDialog::selectedDir() const
{
    return m_edit->text().trimmed();
}

void SetupDialog::browse()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择 .minecraft 目录"),
        m_edit->text().trimmed());
    if (!dir.isEmpty())
        m_edit->setText(QDir::toNativeSeparators(dir));
}

void SetupDialog::accept()
{
    const QString dir = selectedDir();
    if (dir.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("请选择目录"),
                             QStringLiteral("必须选择一个游戏目录才能继续。"));
        return;
    }
    if (!QDir().mkpath(dir)) {
        QMessageBox::warning(this, QStringLiteral("目录不可用"),
                             QStringLiteral("无法创建或访问该目录：\n%1").arg(dir));
        return;
    }
    QDialog::accept();
}

} // namespace md
