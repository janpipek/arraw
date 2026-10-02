#include "MainWindow.h"

#include "DebugDiagnostics.h"
#include "DisplayImage.h"

#include <ImageBuffer.h>
#include <ImageImport.h>
#include <Photo.h>

#include <QAction>
#include <QEvent>
#include <QFileDialog>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QScreen>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QString>

#include <exception>
#include <filesystem>
#include <utility>

namespace arraw::app {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildMenu();
    buildImageView();

    // Long enough to coalesce the events of a drag, short enough to feel prompt.
    resizeTimer_.setSingleShot(true);
    resizeTimer_.setInterval(100);
    connect(&resizeTimer_, &QTimer::timeout, this, [this] {
        // The message box runs a nested event loop, in which further resizes
        // can fire the timer again: one box at a time.
        if (reportingFailure_) {
            return;
        }
        // An exception must not leave a function Qt's event loop called.
        try {
            rerender();
        } catch (const std::exception& error) {
            reportingFailure_ = true;
            QMessageBox::warning(this, tr("Cannot Render Photograph"),
                                 QString::fromUtf8(error.what()));
            reportingFailure_ = false;
        }
    });

    // No size to restore yet: two thirds of the screen, so the first photograph
    // is fitted to something worth looking at.
    if (const QScreen* screenOfWindow = screen()) {
        resize(screenOfWindow->availableGeometry().size() * 2 / 3);
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == imageView_ && open_ &&
        (event->type() == QEvent::Resize || event->type() == QEvent::DevicePixelRatioChange)) {
        resizeTimer_.start();
    }
    return QMainWindow::eventFilter(watched, event);
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
    imageView_->setAlignment(Qt::AlignCenter);
    // The view takes the room the window gives it; the pixmap must not decide
    // the window's minimum size, or the photograph could never be fitted smaller.
    imageView_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    imageView_->setMinimumSize(1, 1);
    imageView_->installEventFilter(this);
    setCentralWidget(imageView_);
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

QSize MainWindow::viewportPixels() const {
    return (imageView_->size() * imageView_->devicePixelRatioF()).expandedTo({1, 1});
}

void MainWindow::rerender() {
    if (!open_) {
        return;
    }
    const QImage image = renderForViewport(open_->decoded, open_->session.photo().settings(),
                                           viewportPixels(), imageView_->devicePixelRatioF());
    imageView_->setPixmap(QPixmap::fromImage(image));
}

void MainWindow::showPhoto(Photo photo) {
    DebugDiagnostics log;

    // Everything that can throw, before anything changes.
    ImageBuffer decoded = loadImage(photo.path(), log);
    const QImage image = renderForViewport(decoded, photo.settings(), viewportPixels(),
                                           imageView_->devicePixelRatioF());

    // Commit.
    open_.emplace(OpenPhoto{EditSession(std::move(photo)), std::move(decoded)});
    imageView_->setPixmap(QPixmap::fromImage(image));
    resizeTimer_.stop(); // The pixels are already fitted to the current size.
}

} // namespace arraw::app
