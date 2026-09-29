#include "MainWindow.h"

#include <filesystem>

#include <Photo.h>
#include <QAction>
#include <QFileDialog>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QStandardPaths>

namespace arraw::app {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildMenu();
}

MainWindow::~MainWindow() {}

void MainWindow::buildMenu() {
    QMenuBar* menuBar = this->menuBar();

    QMenu* fileMenu = menuBar->addMenu(tr("&File"));

    QAction* openAction = fileMenu->addAction(tr("&Open file..."));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openFileWithDialog);

    QAction* quitAction = fileMenu->addAction(tr("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, [this] { close(); });
}

void MainWindow::openFileWithDialog() {
    const QString filesDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString filter = tr("RAW Images (*.cr2 *.arw .dng);;All Images (*.cr2 *.arw *.dng *.jpg "
                              "*.jpeg *.png);; JPEG Images (*.jpg *.jpeg);;PNG Images (*.png)");
    const QString fileName =
        QFileDialog::getOpenFileName(this, tr("Open image file"), filesDir, filter);

    if (fileName.isEmpty()) {
        // Cancelled
        return;
    }
    const std::filesystem::path path(fileName.toStdU16String());
    qDebug() << path.native();

    Photo photo = openPhoto(path);
}

} // namespace arraw::app
