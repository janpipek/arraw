#pragma once

#include <QMainWindow>

class MainWindow : public QMainWindow {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(MainWindow)
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
};
