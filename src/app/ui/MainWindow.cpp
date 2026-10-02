#include "MainWindow.h"

#include "DebugDiagnostics.h"
#include "DevelopPanel.h"
#include "DisplayImage.h"

#include <ColorEncoding.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <Photo.h>
#include <WhiteBalance.h>

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
#include <QMouseEvent>
#include <QPixmap>
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QString>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
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

    cancelPickShortcut_ = new QShortcut(Qt::Key_Escape, this);
    cancelPickShortcut_->setEnabled(false);
    connect(cancelPickShortcut_, &QShortcut::activated, this, [this] { setPicking(false); });

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
    if (watched == imageView_ && picking_ && event->type() == QEvent::MouseButtonPress) {
        const auto* click = static_cast<QMouseEvent*>(event);
        if (click->button() == Qt::LeftButton) {
            pickNeutralAt(click->position());
            return true;
        }
    }
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
    connect(developPanel_, &DevelopPanel::pickToggled, this, &MainWindow::setPicking);
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

void MainWindow::setPicking(bool picking) {
    picking_ = picking && open_.has_value();
    developPanel_->setPicking(picking_);
    cancelPickShortcut_->setEnabled(picking_);
    if (picking_) {
        imageView_->setCursor(Qt::CrossCursor);
    } else {
        imageView_->unsetCursor();
    }
}

void MainWindow::pickNeutralAt(const QPointF& position) {
    // The preview is centred in the view and may be smaller than it, and the
    // click is only meaningful on the photograph itself.
    const QPixmap pixmap = imageView_->pixmap();
    if (pixmap.isNull()) {
        return;
    }
    const QSizeF size = pixmap.deviceIndependentSize();
    const QRectF contents = imageView_->contentsRect();
    const QPointF origin(contents.x() + (contents.width() - size.width()) / 2.0,
                         contents.y() + (contents.height() - size.height()) / 2.0);
    const double x = (position.x() - origin.x()) / size.width();
    const double y = (position.y() - origin.y()) / size.height();
    if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) {
        return;
    }

    setPicking(false);
    guarded([this, x, y] {
        developPanel_->finishPendingEdit();
        // The preview is of a reduced copy, but its frame is the developed
        // frame, so normalised coordinates hold against the full source.
        const DevelopState& current = open_->session.photo().state();
        const ColourTemperature light = neutralTemperatureAt(*open_->decoded, current, x, y);
        DevelopState next = current;
        next.settings.color = {WhiteBalanceMode::Custom, light.kelvin, light.tint};
        open_->session.begin();
        open_->session.update(next);
        open_->session.commit();
        refreshPanel();
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
    PanelContext context{raw, std::nullopt};
    if (const auto* camera = std::get_if<CameraNative>(&photo.metadata().encoding)) {
        try {
            // The light the pixels went through, which is also what development
            // keeps for whichever of Temp and Tint is not named. For a file that
            // recorded its neutral this is the camera's reading; for one that
            // did not, the decode's substitute (ADR 007), and showing that keeps
            // a row from jumping when only the other one moves.
            context.asShot = temperatureForGains(*camera, camera->appliedMultipliers);
        } catch (const std::invalid_argument&) {
            // A calibration that gives no reading: the rows fall back to a fixed one.
        }
    }
    developPanel_->showState(photo.state(), context);
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
    QString text = result.onGpu
                       ? tr("Preview: GPU \u2014 %1").arg(QString::fromStdString(result.deviceName))
                       : tr("Preview: CPU");
    if (result.level > 0) {
        // A reduced copy of the photograph was developed: 1/2^level of its linear size.
        text += tr(" \u00b7 1/%1").arg(1ULL << result.level);
    }
    deviceLabel_->setText(text);
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
    setPicking(false);
    open_.emplace(OpenPhoto{EditSession(std::move(photo)), decoded});
    previewRenderer_.setSource(std::move(decoded));
    // Results of the previous photograph are still on their way, or in progress.
    firstRequest_ = latestRequest_ + 1;
    developDock_->setEnabled(true);
    refreshPanel(); // Requests the first render.
}

} // namespace arraw::app
