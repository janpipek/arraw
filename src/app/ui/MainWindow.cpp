#include "MainWindow.h"

#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>

namespace arraw::app {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildMenu();
}

MainWindow::~MainWindow() {}

void MainWindow::buildMenu() {
    QMenuBar* menuBar = this->menuBar();

    QMenu* fileMenu = menuBar->addMenu(tr("&File"));
    QAction* quitAction = fileMenu->addAction(tr("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, [this] { close(); });
}

} // namespace arraw::app
