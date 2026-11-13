#pragma once

#include <QDialog>

class QLineEdit;

namespace md {

// 首次启动强制选择游戏目录。
class SetupDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SetupDialog(QWidget *parent = nullptr);

    QString selectedDir() const;

private:
    void browse();
    void accept() override;

    QLineEdit *m_edit = nullptr;
};

} // namespace md
