#include "MainWindow.h"

#include "DebugDiagnostics.h"
#include "DisplayImage.h"

#include <Develop.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <Photo.h>

#include <QAction>
#include <QFileDialog>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QScrollArea>
#include <QStandardPaths>
#include <QString>

#include <exception>
#include <filesystem>
#include <utility>

namespace arraw::app {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildMenu();
    buildImageView();
}

void MainWindow::buildMenu() {
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));

    QAction* openAction = fileMenu->addAction(tr("&Open…"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openFileWithDialog);

    QAction* quitAction = fileMenu->addAction(tr("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, [this] { close(); });
}

void MainWindow::buildImageView() {
    imageView_ = new QLabel(this);
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidget(imageView_);
    setCentralWidget(scrollArea);
}

void MainWindow::openFileWithDialog() {
    const QString filesDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    // TODO: derive the extensions from what loadImage can decode, rather than
    // keeping a second list here.
    const QString filter = tr("All Images (*.cr2 *.arw *.dng *.jpg *.jpeg *.png);;"
                              "RAW Images (*.cr2 *.arw *.dng);;"
                              "JPEG Images (*.jpg *.jpeg);;"
                              "PNG Images (*.png);;"
                              "All Files (*)");
    const QString fileName =
        QFileDialog::getOpenFileName(this, tr("Open Photograph"), filesDir, filter);
    if (fileName.isEmpty()) {
        return; // Cancelled.
    }
    // Through UTF-16, not toStdString(): on Windows a path built from a narrow
    // string is read in the ANSI code page, which mangles non-ASCII names.
    const std::filesystem::path path(fileName.toStdU16String());

    // The one place that can tell the user: an exception must not leave a
    // function Qt's event loop called, which ends in std::terminate.
    try {
        DebugDiagnostics log;
        showPhoto(openPhoto(path, log));
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Cannot Open Photograph"),
                             tr("%1\n\n%2").arg(fileName, QString::fromUtf8(error.what())));
    }
}

void MainWindow::showPhoto(Photo photo) {
    DebugDiagnostics log;

    // Everything that can throw, before anything changes.
    const ImageBuffer decoded = loadImage(photo.path(), log);
    const ImageBuffer developed = develop(decoded, photo.settings());
    const QImage image = toDisplayImage(developed);

    // Commit.
    editSession_.emplace(std::move(photo));
    imageView_->setPixmap(QPixmap::fromImage(image));
    // The scroll area keeps its widget at whatever size it has; the label
    // does not grow to a new pixmap by itself.
    imageView_->adjustSize();
}

} // namespace arraw::app
