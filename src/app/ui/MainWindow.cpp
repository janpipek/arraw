#include "MainWindow.h"

#include "DebugDiagnostics.h"
#include "DevelopPanel.h"
#include "DisplayImage.h"

#include <ColorEncoding.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <Photo.h>

#include <QAction>
#include <QDockWidget>
#include <QEvent>
#include <QFileDialog>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPixmap>
#include <QScreen>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QString>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string_view>
#include <utility>
#include <variant>

namespace arraw::app {

namespace {

/// @brief Reads where previews may render from ARRAW_PREVIEW_DEVICE.
///
/// `cpu` forces the CPU; anything else, or nothing, means the GPU when there is one.
PreviewRenderer::Device previewDeviceFromEnvironment() {
    const char* value = std::getenv("ARRAW_PREVIEW_DEVICE");
    return value != nullptr && std::string_view(value) == "cpu" ? PreviewRenderer::Device::Cpu
                                                                : PreviewRenderer::Device::Auto;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      previewRenderer_(
          [this](PreviewResult result) {
              // On the worker thread. Dropped if the window is gone by the time the
              // GUI thread would run it.
              QMetaObject::invokeMethod(
                  this, [this, result = std::move(result)] { showResult(result); },
                  Qt::QueuedConnection);
          },
          previewDeviceFromEnvironment()) {
    buildMenu();
    buildStatusBar();
    buildImageView();
    buildDevelopDock();

    // Long enough to coalesce the events of a drag, short enough to feel prompt.
    resizeTimer_.setSingleShot(true);
    resizeTimer_.setInterval(100);
    connect(&resizeTimer_, &QTimer::timeout, this, &MainWindow::requestRender);

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

    QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
    undoAction_ = editMenu->addAction(tr("&Undo"));
    undoAction_->setShortcut(QKeySequence::Undo);
    undoAction_->setEnabled(false);
    connect(undoAction_, &QAction::triggered, this, [this] {
        guarded([this] {
            developPanel_->finishPendingEdit();
            open_->session.undo();
            refreshPanel();
        });
    });
    redoAction_ = editMenu->addAction(tr("&Redo"));
    redoAction_->setShortcut(QKeySequence::Redo);
    redoAction_->setEnabled(false);
    connect(redoAction_, &QAction::triggered, this, [this] {
        guarded([this] {
            developPanel_->finishPendingEdit();
            open_->session.redo();
            refreshPanel();
        });
    });
}

void MainWindow::buildDevelopDock() {
    developPanel_ = new DevelopPanel;
    auto* scroll = new QScrollArea;
    scroll->setWidget(developPanel_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* dock = new QDockWidget(tr("Develop"), this);
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setWidget(scroll);
    dock->setEnabled(false);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    developDock_ = dock;

    connect(developPanel_, &DevelopPanel::editStarted, this,
            [this] { guarded([this] { open_->session.begin(); }); });
    connect(developPanel_, &DevelopPanel::stateEdited, this, [this](const DevelopState& state) {
        guarded([this, &state] {
            // No edit is open after a failure cancelled one: the rest of that
            // drag is dropped, rather than failing again with every move.
            if (!open_->session.editing()) {
                return;
            }
            open_->session.update(state);
            refreshPanel();
        });
    });
    connect(developPanel_, &DevelopPanel::editFinished, this, [this] {
        guarded([this] {
            if (!open_->session.editing()) {
                return;
            }
            open_->session.commit();
            refreshPanel();
        });
    });
}

void MainWindow::guarded(const std::function<void()>& action) {
    if (!open_) {
        return;
    }
    try {
        action();
    } catch (const std::exception& error) {
        const QString message = QString::fromUtf8(error.what());
        try {
            if (open_->session.editing()) {
                open_->session.cancel();
            }
            refreshPanel();
        } catch (const std::exception&) {
            // Nothing more can be done; the message below still tells the user.
        }
        QMessageBox::warning(this, tr("Cannot Edit Photograph"), message);
    }
}

void MainWindow::refreshPanel() {
    if (!open_) {
        return;
    }
    const Photo& photo = open_->session.photo();
    const bool raw = !std::holds_alternative<NamedEncoding>(photo.metadata().encoding);
    developPanel_->showState(photo.state(), raw);
    undoAction_->setEnabled(open_->session.canUndo());
    redoAction_->setEnabled(open_->session.canRedo());
    requestRender();
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

void MainWindow::buildStatusBar() {
    deviceLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(deviceLabel_);
}

void MainWindow::showDevice(const PreviewResult& result) {
    deviceLabel_->setText(
        result.onGpu ? tr("Preview: GPU \u2014 %1").arg(QString::fromStdString(result.deviceName))
                     : tr("Preview: CPU"));
    deviceLabel_->setToolTip(QString::fromStdString(result.fallbackReason));
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

void MainWindow::requestRender() {
    if (!open_) {
        return;
    }
    // Any pending resize is covered by this request.
    resizeTimer_.stop();
    latestRequest_ = previewRenderer_.request(open_->session.photo().state(), viewportPixels(),
                                              imageView_->devicePixelRatioF());
}

void MainWindow::showResult(const PreviewResult& result) {
    if (result.request < firstRequest_ || result.request <= latestShown_) {
        return;
    }
    if (result.image) {
        latestShown_ = result.request;
        imageView_->setPixmap(QPixmap::fromImage(*result.image));
        showDevice(result);
        return;
    }
    // A newer request is on its way and may well succeed: say nothing yet.
    if (result.request != latestRequest_) {
        return;
    }
    latestShown_ = result.request;
    // The message box runs a nested event loop, in which further results can
    // arrive: one box at a time.
    if (reportingFailure_) {
        return;
    }
    reportingFailure_ = true;
    QMessageBox::warning(this, tr("Cannot Render Photograph"),
                         QString::fromStdString(result.error));
    reportingFailure_ = false;
}

void MainWindow::showPhoto(Photo photo) {
    DebugDiagnostics log;

    // Everything that can throw, before anything changes.
    auto decoded = std::make_shared<const ImageBuffer>(loadImage(photo.path(), log));

    // Commit.
    open_.emplace(OpenPhoto{EditSession(std::move(photo)), decoded});
    previewRenderer_.setSource(std::move(decoded));
    // Results of the previous photograph are still on their way, or in progress.
    firstRequest_ = latestRequest_ + 1;
    developDock_->setEnabled(true);
    refreshPanel(); // Requests the first render.
}

} // namespace arraw::app
