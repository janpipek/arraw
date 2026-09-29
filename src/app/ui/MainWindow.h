#pragma once

#include <QMainWindow>

namespace arraw::app {

class MainWindow : public QMainWindow {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(MainWindow)
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    /// @brief Build the whole menu.
    void buildMenu();
};
} // namespace arraw::app