#pragma once

#include <QDialog>

class AboutDialog : public QDialog {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(AboutDialog)
public:
    explicit AboutDialog(QWidget* parent = nullptr);
};
