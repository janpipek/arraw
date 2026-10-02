#include "MainWindow.h"

#include "DebugDiagnostics.h"
#include "DevelopPanel.h"
#include "DisplayImage.h"
#include "PhotoView.h"
#include "ViewTransform.h"

#include <ColorEncoding.h>
#include <Develop.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <Photo.h>
#include <WhiteBalance.h>

#include <QAction>
#include <QActionGroup>
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
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QStandardPaths>
#include <QStatusBar>
#include <QString>
#include <QToolButton>

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
    interactionTimer_.setSingleShot(true);
    interactionTimer_.setInterval(0);
    connect(&interactionTimer_, &QTimer::timeout, this, &MainWindow::requestRender);

    // No size to restore yet: two thirds of the screen, so the first photograph
    // is fitted to something worth looking at.
    if (const QScreen* screenOfWindow = screen()) {
        resize(screenOfWindow->availableGeometry().size() * 2 / 3);
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == photoView_ && open_ &&
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

    buildZoomControls();
}

void MainWindow::buildZoomControls() {
    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    QMenu* zoomMenu = viewMenu->addMenu(tr("&Zoom"));

    zoomInAction_ = zoomMenu->addAction(tr("Zoom &In"));
    zoomInAction_->setShortcut(QKeySequence::ZoomIn);
    connect(zoomInAction_, &QAction::triggered, this, [this] { photoView_->zoomBy(2.0); });
    zoomOutAction_ = zoomMenu->addAction(tr("Zoom &Out"));
    zoomOutAction_->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAction_, &QAction::triggered, this, [this] { photoView_->zoomBy(0.5); });
    zoomMenu->addSeparator();

    // One list of actions, in the menu and in the button's dropdown alike.
    zoomGroup_ = new QActionGroup(this);
    zoomGroup_->setExclusive(true);
    auto* fitAction = new QAction(tr("&Fit"), this);
    fitAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(fitAction, &QAction::triggered, this, [this] { photoView_->zoomToFit(); });
    zoomActions_.push_back(fitAction);
    for (const double preset : zoomPresets) {
        auto* action = new QAction(zoomPercentLabel(preset), this);
        connect(action, &QAction::triggered, this, [this, preset] { photoView_->zoomTo(preset); });
        zoomActions_.push_back(action);
    }
    auto* dropdown = new QMenu(this);
    for (QAction* action : zoomActions_) {
        action->setCheckable(true);
        zoomGroup_->addAction(action);
        zoomMenu->addAction(action);
        dropdown->addAction(action);
    }

    zoomButton_ = new QToolButton(this);
    zoomButton_->setPopupMode(QToolButton::InstantPopup);
    zoomButton_->setAutoRaise(true);
    zoomButton_->setMenu(dropdown);
    zoomButton_->setToolTip(tr("Zoom"));
}

void MainWindow::updateZoomControls() {
    const bool enabled = open_.has_value();
    zoomButton_->setEnabled(enabled);
    zoomInAction_->setEnabled(enabled);
    zoomOutAction_->setEnabled(enabled);
    for (QAction* action : zoomActions_) {
        action->setEnabled(enabled);
    }
    const bool fit = photoView_->isFit();
    const double zoom = photoView_->zoom();
    zoomButton_->setText(enabled ? zoomLabel(zoom, fit) : tr("Fit"));
    // The preset the zoom is, or none: a wheel zoom is between them.
    const int preset = fit ? -1 : matchingZoomPreset(zoom);
    for (std::size_t i = 0; i < zoomActions_.size(); ++i) {
        zoomActions_[i]->setChecked(enabled && (i == 0 ? fit : static_cast<int>(i) - 1 == preset));
    }
    if (!enabled || (!fit && preset < 0)) {
        // Exclusive groups keep one ticked; a zoom between presets ticks none.
        zoomGroup_->setExclusive(false);
        for (QAction* action : zoomActions_) {
            action->setChecked(false);
        }
        zoomGroup_->setExclusive(true);
    }
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
    photoView_->setPicking(picking_);
}

void MainWindow::pickNeutralAt(const QPointF& point) {
    const double x = point.x();
    const double y = point.y();

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
    photoView_ = new PhotoView(this);
    photoView_->installEventFilter(this);
    connect(photoView_, &PhotoView::picked, this, &MainWindow::pickNeutralAt);
    connect(photoView_, &PhotoView::zoomChanged, this, &MainWindow::updateZoomControls);
    // A drag or a wheel burst makes many events; they ask for one render per turn.
    connect(photoView_, &PhotoView::viewChanged, this, [this] {
        if (open_) {
            interactionTimer_.start();
        }
    });
    setCentralWidget(photoView_);
    updateZoomControls();
}

void MainWindow::buildStatusBar() {
    deviceLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(deviceLabel_);
    statusBar()->addPermanentWidget(zoomButton_);
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

void MainWindow::requestRender() {
    if (!open_) {
        return;
    }
    // Any pending resize or interaction is covered by this request.
    resizeTimer_.stop();
    interactionTimer_.stop();
    const DevelopState& state = open_->session.photo().state();
    const qreal ratio = photoView_->devicePixelRatioF();
    PreviewView view{.region = std::nullopt,
                     .outputSize = photoView_->devicePixels(),
                     .devicePixelRatio = ratio};
    try {
        // The crop may have changed the frame; the view keeps its zoom and centre.
        const ImageSize cropped =
            croppedSize(open_->decoded->size(), open_->decoded->orientation(), state);
        photoView_->setFrameSize(
            QSize(static_cast<int>(cropped.width), static_cast<int>(cropped.height)));
        const ViewTransform transform = photoView_->transform();
        view.region = transform.visiblePixels();
        view.outputSize = transform.outputSize();
    } catch (const std::exception&) {
        // Left to the renderer, which reports what is wrong with the state.
    }
    latestRequest_ = previewRenderer_.request(state, std::move(view));
}

void MainWindow::showResult(const PreviewResult& result) {
    if (result.request < firstRequest_ || result.request <= latestShown_) {
        return;
    }
    if (result.image) {
        latestShown_ = result.request;
        photoView_->setImage(*result.image, result.region);
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
    photoView_->resetView();
    updateZoomControls();
    previewRenderer_.setSource(std::move(decoded));
    // Results of the previous photograph are still on their way, or in progress.
    firstRequest_ = latestRequest_ + 1;
    developDock_->setEnabled(true);
    refreshPanel(); // Requests the first render.
}

} // namespace arraw::app
