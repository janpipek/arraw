#include "MainWindow.h"

#include "CopySettingsDialog.h"
#include "CropOverlay.h"
#include "CullingActions.h"
#include "DebugDiagnostics.h"
#include "DebugLog.h"
#include "DebugWindow.h"
#include "DevelopPanel.h"
#include "DisplayImage.h"
#include "ExportDialog.h"
#include "ExportSettings.h"
#include "FilmStrip.h"
#include "PhotoView.h"
#include "RenderDelay.h"
#include "RenderProgressPie.h"
#include "SettingsDialog.h"
#include "ThumbnailCache.h"
#include "ThumbnailWorker.h"
#include "TimingTrace.h"
#include "ViewTransform.h"

#include <ColorEncoding.h>
#include <Develop.h>
#include <Edits.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <Photo.h>
#include <Sidecar.h>
#include <WhiteBalance.h>

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDockWidget>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QStatusBar>
#include <QString>
#include <QStringList>
#include <QStyle>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace arraw::app {

namespace {

/// @brief Reads whether ARRAW_PREVIEW_DEVICE forces the CPU.
///
/// `cpu` forces it, for previews and exports alike; anything else, or nothing,
/// leaves the saved processing preference in effect.
bool cpuForcedByEnvironment() {
    const char* value = std::getenv("ARRAW_PREVIEW_DEVICE");
    return value != nullptr && std::string_view(value) == "cpu";
}

/// @brief Tells whether previews and exports stay on the CPU without looking for a GPU.
///
/// When the environment says so, or on a platform whose GPU is not used by
/// default (Windows for now, ADR 017) unless the settings name an adapter.
bool cpuChosen(const AppSettings& settings) {
    return cpuForcedByEnvironment() || (!gpuUsedByDefault() && !settings.gpu);
}

/// @brief Converts a filesystem path to a Qt string, through UTF-16.
QString toQString(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.u16string());
}

/// @brief Reduces a state to what the crop mode shows (ADR 040, ADR 037).
///
/// The photograph keeps its turns and flips but is neither straightened nor
/// cropped, and the effects, which follow the crop, are left off.
DevelopState uncroppedState(DevelopState state) {
    state.settings.geometry.straighten = 0.0;
    state.settings.geometry.crop = {};
    state.settings.effects = {};
    return state;
}

} // namespace

MainWindow::MainWindow(DebugLog& debugLog, QWidget* parent)
    : QMainWindow(parent), debugLog_(debugLog), runningSettings_([] {
          QSettings store;
          return restoreAppSettings(store);
      }()),
      previewRenderer_(
          [this](PreviewResult result) {
              // On the worker thread. Dropped if the window is gone by the time the
              // GUI thread would run it.
              QMetaObject::invokeMethod(
                  this, [this, result = std::move(result)] { showResult(result); },
                  Qt::QueuedConnection);
          },
          cpuChosen(runningSettings_) ? PreviewRenderer::Device::Cpu
                                      : PreviewRenderer::Device::Auto,
          runningSettings_,
          [this](std::uint64_t request, const Progress& progress) {
              // On the worker thread, already thinned to about thirty a second.
              QMetaObject::invokeMethod(
                  this,
                  [this, request, progress] {
                      showRenderProgress(request, progress.fraction, progress.step);
                  },
                  Qt::QueuedConnection);
          }),
      exportQueue_(
          [this](ExportResult result) {
              // On the worker thread, as the preview's.
              QMetaObject::invokeMethod(
                  this, [this, result = std::move(result)] { showExportResult(result); },
                  Qt::QueuedConnection);
          },
          cpuChosen(runningSettings_) ? ExportQueue::Device::Cpu : ExportQueue::Device::Auto,
          runningSettings_),
      photoLoader_(
          ThumbnailCache(ThumbnailCache::defaultRoot()),
          [this](DecodedPhoto result) {
              // On the decode thread, as the preview's.
              QMetaObject::invokeMethod(
                  this, [this, result = std::move(result)] { decodeLanded(result); },
                  Qt::QueuedConnection);
          },
          [this](CameraPreview preview) {
              QMetaObject::invokeMethod(
                  this, [this, preview = std::move(preview)] { cameraPreviewLanded(preview); },
                  Qt::QueuedConnection);
          },
          [this](std::uint64_t request, const Progress& progress) {
              // A decode reports a handful of times: no thinning needed.
              QMetaObject::invokeMethod(
                  this,
                  [this, request, progress] {
                      if (request == decodeRequest_) {
                          renderIndicator_->report(progress.fraction, progress.step);
                      }
                  },
                  Qt::QueuedConnection);
          },
          PhotoLoader::imageDecoder(&debugLog.diagnostics)) {
    filmStrip_ = new FilmStrip(this);
    buildMenu();
    buildStatusBar();
    buildImageView();
    buildDevelopDock();
    buildFilmStripDock();

    // Last in the View menu, after the docks' toggles.
    viewMenu_->addSeparator();
    QAction* debugAction = viewMenu_->addAction(tr("&Debug Log"));
    debugAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
    connect(debugAction, &QAction::triggered, this, &MainWindow::showDebugWindow);
    // Also on the window, so the shortcut outlives a hidden menu bar.
    addAction(debugAction);

    cancelPickShortcut_ = new QShortcut(Qt::Key_Escape, this);
    cancelPickShortcut_->setEnabled(false);
    connect(cancelPickShortcut_, &QShortcut::activated, this, [this] { setPicking(false); });

    // The crop mode's keys wherever the focus is, such as on a button the mouse just used;
    // the overlay claims them itself when it has the focus (ADR 040). Reject gives X up
    // while the mode is on, and the picker Esc, so no two shortcuts share a key.
    CropOverlay* crop = &photoView_->cropOverlay();
    const auto addCropShortcut = [this](QKeyCombination key, const std::function<void()>& act) {
        auto* shortcut = new QShortcut(QKeySequence(key), this);
        shortcut->setEnabled(false);
        connect(shortcut, &QShortcut::activated, this, act);
        cropShortcuts_.push_back(shortcut);
    };
    addCropShortcut(QKeyCombination(Qt::Key_Return), [crop] { crop->accept(); });
    addCropShortcut(QKeyCombination(Qt::KeypadModifier, Qt::Key_Enter), [crop] { crop->accept(); });
    addCropShortcut(QKeyCombination(Qt::Key_Escape), [crop] { crop->dismiss(); });
    addCropShortcut(QKeyCombination(Qt::Key_O), [crop] { crop->cycleGuide(); });
    addCropShortcut(QKeyCombination(Qt::Key_X), [crop] {
        crop->edit([](CropEditing& editing) { editing.swapOrientation(); });
    });

    // Long enough to coalesce the events of a drag, short enough to feel prompt.
    resizeTimer_.setSingleShot(true);
    resizeTimer_.setInterval(100);
    connect(&resizeTimer_, &QTimer::timeout, this, &MainWindow::requestRender);
    interactionTimer_.setSingleShot(true);
    interactionTimer_.setInterval(0);
    connect(&interactionTimer_, &QTimer::timeout, this, &MainWindow::requestRender);
    // A noise reduction drag waits for the hand to rest (ADR 039); the edit itself is not held.
    noiseReductionTimer_.setSingleShot(true);
    noiseReductionTimer_.setInterval(app::noiseReductionRenderDelay);
    connect(&noiseReductionTimer_, &QTimer::timeout, this, &MainWindow::requestRender);

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
    openAction->setObjectName("openAction");
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openFileWithDialog);

    QAction* openFolderAction = fileMenu->addAction(tr("Open &Folder…"));
    openFolderAction->setObjectName("openFolderAction");
    openFolderAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    connect(openFolderAction, &QAction::triggered, this, &MainWindow::openFolderWithDialog);

    saveAction_ = fileMenu->addAction(tr("&Save Adjustments"));
    saveAction_->setObjectName("saveAction");
    saveAction_->setShortcut(QKeySequence::Save);
    saveAction_->setEnabled(false);
    connect(saveAction_, &QAction::triggered, this, [this] { saveAdjustments(); });

    exportAction_ = fileMenu->addAction(tr("&Export…"));
    exportAction_->setObjectName("exportAction");
    exportAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    exportAction_->setEnabled(false);
    connect(exportAction_, &QAction::triggered, this, &MainWindow::exportWithDialog);

    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction(tr("&Quit"));
    quitAction->setObjectName("quitAction");
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, [this] { close(); });

    QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
    undoAction_ = editMenu->addAction(tr("&Undo"));
    undoAction_->setObjectName("undoAction");
    undoAction_->setShortcuts(QKeySequence::Undo);
    undoAction_->setEnabled(false);
    connect(undoAction_, &QAction::triggered, this, [this] {
        guarded([this] {
            developPanel_->finishPendingEdit();
            if (photoView_->isCropMode()) {
                // Back through the session's gestures, never past its start (ADR 040).
                photoView_->cropOverlay().undo();
                return;
            }
            open_->session.undo();
            refreshPanel();
        });
    });
    redoAction_ = editMenu->addAction(tr("&Redo"));
    redoAction_->setObjectName("redoAction");
    // Every binding the platform has: Ctrl+Shift+Z as well as Ctrl+Y where both are usual.
    redoAction_->setShortcuts(QKeySequence::Redo);
    redoAction_->setEnabled(false);
    connect(redoAction_, &QAction::triggered, this, [this] {
        guarded([this] {
            developPanel_->finishPendingEdit();
            if (photoView_->isCropMode()) {
                photoView_->cropOverlay().redo();
                return;
            }
            open_->session.redo();
            refreshPanel();
        });
    });

    editMenu->addSeparator();
    copyAction_ = editMenu->addAction(tr("&Copy Settings…"));
    copyAction_->setObjectName("copySettingsAction");
    copyAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    copyAction_->setEnabled(false);
    connect(copyAction_, &QAction::triggered, this, &MainWindow::copySettings);
    pasteAction_ = editMenu->addAction(tr("&Paste Settings"));
    pasteAction_->setObjectName("pasteSettingsAction");
    pasteAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    pasteAction_->setEnabled(false);
    connect(pasteAction_, &QAction::triggered, this, &MainWindow::pasteSettings);

    editMenu->addSeparator();
    QAction* settingsAction = editMenu->addAction(tr("&Settings…"));
    settingsAction->setObjectName("settingsAction");
    settingsAction->setMenuRole(QAction::PreferencesRole);
    settingsAction->setShortcut(QKeySequence::Preferences);
    connect(settingsAction, &QAction::triggered, this, &MainWindow::showSettings);

    photoMenu_ = menuBar()->addMenu(tr("&Photo"));
    cropAction_ = photoMenu_->addAction(tr("&Crop && Straighten"));
    cropAction_->setObjectName("cropAction");
    cropAction_->setShortcut(QKeySequence(Qt::Key_C));
    cropAction_->setCheckable(true);
    cropAction_->setEnabled(false);
    connect(cropAction_, &QAction::triggered, this, &MainWindow::setCropMode);
    photoMenu_->addSeparator();
    const auto addGeometryAction = [this](const QString& name, const QString& text,
                                          const QKeySequence& shortcut,
                                          std::function<void(CropEditing&)> command) {
        QAction* action = photoMenu_->addAction(text);
        action->setObjectName(name);
        if (!shortcut.isEmpty()) {
            action->setShortcut(shortcut);
        }
        action->setEnabled(false);
        connect(action, &QAction::triggered, this,
                [this, command = std::move(command)] { editGeometry(command); });
        geometryActions_.push_back(action);
    };
    addGeometryAction("rotateLeftAction", tr("Rotate &Left"),
                      QKeySequence(Qt::CTRL | Qt::Key_BracketLeft),
                      [](CropEditing& editing) { editing.turn(false); });
    addGeometryAction("rotateRightAction", tr("Rotate &Right"),
                      QKeySequence(Qt::CTRL | Qt::Key_BracketRight),
                      [](CropEditing& editing) { editing.turn(true); });
    addGeometryAction("flipHorizontalAction", tr("Flip &Horizontal"), {},
                      [](CropEditing& editing) { editing.flip(true); });
    addGeometryAction("flipVerticalAction", tr("Flip &Vertical"), {},
                      [](CropEditing& editing) { editing.flip(false); });

    // Rating, colour labels and stepping follow the geometry in the same menu, as in Lightroom.
    photoMenu_->addSeparator();
    culling_ = new CullingActions(*this, *filmStrip_, *photoMenu_);

    buildZoomControls();
}

MainWindow::~MainWindow() {
    // The panel is deleted by ~QWidget, after the members are gone, and hiding it emits
    // curveHistogramWantedChanged into a lambda that uses previewRenderer_.
    if (developPanel_ != nullptr) {
        developPanel_->disconnect(this);
    }
}

void MainWindow::showSettings() {
    QSettings saved;
    SettingsDialog dialog(restoreAppSettings(saved), this);
    while (dialog.exec() == QDialog::Accepted) {
        QSettings store;
        saveAppSettings(dialog.settings(), store);
        store.sync();
        if (store.status() == QSettings::NoError) {
            statusBar()->showMessage(
                tr("Settings saved. Restart Arraw to apply processing changes."), 6000);
            return;
        }
        QMessageBox::warning(this, tr("Could not save settings"),
                             tr("Check that the application settings location is writable."));
    }
}

void MainWindow::buildZoomControls() {
    viewMenu_ = menuBar()->addMenu(tr("&View"));
    QMenu* zoomMenu = viewMenu_->addMenu(tr("&Zoom"));

    zoomInAction_ = zoomMenu->addAction(tr("Zoom &In"));
    zoomInAction_->setObjectName("zoomInAction");
    zoomInAction_->setShortcut(QKeySequence::ZoomIn);
    connect(zoomInAction_, &QAction::triggered, this, [this] { photoView_->zoomBy(2.0); });
    zoomOutAction_ = zoomMenu->addAction(tr("Zoom &Out"));
    zoomOutAction_->setObjectName("zoomOutAction");
    zoomOutAction_->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAction_, &QAction::triggered, this, [this] { photoView_->zoomBy(0.5); });
    zoomMenu->addSeparator();

    // One list of actions, in the menu and in the button's dropdown alike.
    zoomGroup_ = new QActionGroup(this);
    zoomGroup_->setExclusive(true);
    auto* fitAction = new QAction(tr("&Fit"), this);
    fitAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(fitAction, &QAction::triggered, this, [this] { photoView_->zoomToFit(); });
    fitAction->setObjectName("zoomFitAction");
    zoomActions_.push_back(fitAction);
    for (const double preset : zoomPresets) {
        auto* action = new QAction(zoomPercentLabel(preset), this);
        connect(action, &QAction::triggered, this, [this, preset] { photoView_->zoomTo(preset); });
        action->setObjectName(QString("zoom%1Action").arg(qRound(preset * 100)));
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
    // The crop mode always fits the whole photograph.
    const bool enabled = editable() && !photoView_->isCropMode();
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
    // A click on a button that takes no focus would otherwise give it to the scroll area,
    // the nearest ancestor that takes it, and the photo view, or the crop mode, would lose
    // its keys (ADR 040). The panel scrolls with the wheel and its scroll bar.
    scroll->setFocusPolicy(Qt::NoFocus);

    // Never narrower than the panel and a vertical scroll bar, so the panel never scrolls
    // sideways; it opens a little wider (DevelopPanel::defaultDockWidth()).
    scroll->setMinimumWidth(developPanel_->minimumDockWidth() + 2 * scroll->frameWidth());

    auto* dock = new QDockWidget(tr("Develop"), this);
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setWidget(scroll);
    dock->setEnabled(false);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    resizeDocks({dock}, {developPanel_->defaultDockWidth()}, Qt::Horizontal);
    developDock_ = dock;

    // In the crop mode the whole session is one edit (ADR 040): a panel edit
    // joins it rather than opening and closing one of its own.
    connect(developPanel_, &DevelopPanel::editStarted, this, [this] {
        guarded([this] {
            geometryEdit_.reset();
            if (photoView_->isCropMode()) {
                // One step of the crop session's history, however many changes it makes.
                photoView_->cropOverlay().beginStep();
            } else {
                open_->session.begin();
            }
        });
    });
    connect(developPanel_, &DevelopPanel::stateEdited, this, [this](const DevelopState& state) {
        guarded([this, &state] {
            // No edit is open after a failure cancelled one: the rest of that
            // drag is dropped, rather than failing again with every move.
            if (!open_->session.editing()) {
                return;
            }
            const DevelopState before = open_->session.photo().state();
            DevelopState next = state;
            next.settings.geometry = reconciledGeometry(state.settings.geometry);
            open_->session.update(next);
            refreshPanel(renderDelayFor(before, open_->session.photo().state(), previewOnGpu_));
        });
    });
    connect(developPanel_, &DevelopPanel::pickToggled, this, &MainWindow::setPicking);

    // The Crop group asks for what the menu and keys ask for; its button and the
    // action stay in step whoever flipped them.
    connect(developPanel_, &DevelopPanel::cropModeToggled, this, [this](bool cropping) {
        setCropMode(cropping);
        developPanel_->setCropMode(photoView_->isCropMode());
    });
    connect(cropAction_, &QAction::toggled, developPanel_, &DevelopPanel::setCropMode);
    connect(developPanel_, &DevelopPanel::straighteningToggled, this, [this](bool straightening) {
        setStraightening(straightening);
        developPanel_->setStraightening(photoView_->cropOverlay().isStraightening());
    });
    connect(&photoView_->cropOverlay(), &CropOverlay::straighteningChanged, developPanel_,
            &DevelopPanel::setStraightening);
    connect(developPanel_, &DevelopPanel::turned, this, [this](bool clockwise) {
        editGeometry([clockwise](CropEditing& editing) { editing.turn(clockwise); });
    });
    connect(developPanel_, &DevelopPanel::flipped, this, [this](bool horizontal) {
        editGeometry([horizontal](CropEditing& editing) { editing.flip(horizontal); });
    });
    connect(developPanel_, &DevelopPanel::orientationSwapped, this,
            [this] { editGeometry([](CropEditing& editing) { editing.swapOrientation(); }); });
    connect(developPanel_, &DevelopPanel::lockToggled, this, [this](bool locked) {
        editGeometry([locked](CropEditing& editing) { editing.setLocked(locked); });
    });
    connect(developPanel_, &DevelopPanel::aspectChosen, this,
            [this](const CropAspect& aspect, bool matchOrientation) {
                editGeometry([aspect, matchOrientation](CropEditing& editing) {
                    CropAspect chosen = aspect;
                    // The menu's ratios are landscape; a portrait crop keeps its orientation.
                    if (const auto* ratio = std::get_if<CropRatio>(&aspect);
                        ratio != nullptr && matchOrientation &&
                        editing.crop().height > editing.crop().width) {
                        chosen = CropRatio{1.0 / ratio->widthOverHeight};
                    }
                    editing.setAspect(chosen);
                });
            });
    // The curve histogram costs a render (ADR 035): counted only while the editor shows.
    connect(developPanel_, &DevelopPanel::curveHistogramWantedChanged, this,
            [this](bool wanted) { previewRenderer_.setCurveHistogramWanted(wanted); });
    previewRenderer_.setCurveHistogramWanted(developPanel_->curveHistogramWanted());
    // Enter or Esc in a spin box ends the typing: the arrow keys are the window's again.
    connect(developPanel_, &DevelopPanel::focusReleased, photoView_,
            qOverload<>(&QWidget::setFocus));
    connect(developPanel_, &DevelopPanel::editFinished, this, [this] {
        guarded([this] {
            geometryEdit_.reset();
            if (photoView_->isCropMode()) {
                photoView_->cropOverlay().endStep();
                return;
            }
            if (!open_->session.editing()) {
                return;
            }
            open_->session.commit();
            refreshPanel();
        });
    });
}

void MainWindow::setPicking(bool picking) {
    picking_ = picking && editable() && !photoView_->isCropMode();
    developPanel_->setPicking(picking_);
    cancelPickShortcut_->setEnabled(picking_);
    photoView_->setPicking(picking_);
}

void MainWindow::setCropMode(bool cropping) {
    if (!cropping) {
        leaveCropMode(true);
        return;
    }
    if (!editable() || photoView_->isCropMode()) {
        cropAction_->setChecked(photoView_->isCropMode());
        return;
    }
    guarded([this] {
        developPanel_->finishPendingEdit();
        setPicking(false);
        CropEditing editing(open_->decoded->size(), open_->decoded->orientation(),
                            open_->session.photo().state().settings.geometry);
        open_->session.begin();
        photoView_->cropOverlay().start(std::move(editing));
        seedCropOverlay();
        photoView_->setCropMode(true);
        cropFirstRequest_ = latestRequest_ + 1;
        lastCropRender_.reset();
        cropRequests_.clear();
        cropAction_->setChecked(true);
        culling_->setCropMode(true);
        developPanel_->setCropMode(true);
        for (QShortcut* shortcut : cropShortcuts_) {
            shortcut->setEnabled(true);
        }
        updateZoomControls();
        refreshPanel();
    });
    cropAction_->setChecked(photoView_->isCropMode());
}

void MainWindow::seedCropOverlay() {
    CropOverlay& crop = photoView_->cropOverlay();
    const GeometrySettings& geometry = crop.editing().geometry();
    DevelopState uncropped = uncroppedState(open_->session.photo().state());
    if (open_->lastCropImage && open_->lastCropImage->first == uncropped) {
        // Nothing the mode shows changed since it was last left: its render is exact.
        crop.setImage(open_->lastCropImage->second);
        return;
    }
    if (!open_->cameraPreview) {
        // Read off the GUI thread: a full-size JPEG preview takes about half a second
        // (ADR 043). The developed frame stands in alone until it lands.
        crop.setPlaceholder(QImage(), photoView_->wholeFrameImage());
        if (cameraPreviewRequest_ == 0) {
            cameraPreviewRequest_ = photoLoader_.readCameraPreview(open_->session.photo().path());
        }
        return;
    }
    // The camera's preview is upright, with none of the user's turns or flips.
    crop.setPlaceholder(reorientedImage(*open_->cameraPreview, GeometrySettings{}, geometry),
                        photoView_->wholeFrameImage());
}

void MainWindow::cameraPreviewLanded(const CameraPreview& preview) {
    if (preview.request != cameraPreviewRequest_) {
        return;
    }
    cameraPreviewRequest_ = 0;
    if (!open_ || open_->session.photo().path() != preview.path) {
        return;
    }
    open_->cameraPreview = preview.image;
    if (photoView_->isCropMode()) {
        CropOverlay& crop = photoView_->cropOverlay();
        crop.fillPlaceholder(
            reorientedImage(preview.image, GeometrySettings{}, crop.editing().geometry()));
    }
}

void MainWindow::updateHistoryActions() {
    updatePasteAction();
    if (!open_) {
        undoAction_->setEnabled(false);
        redoAction_->setEnabled(false);
        return;
    }
    if (photoView_->isCropMode()) {
        undoAction_->setEnabled(photoView_->cropOverlay().canUndo());
        redoAction_->setEnabled(photoView_->cropOverlay().canRedo());
        return;
    }
    undoAction_->setEnabled(open_->session.canUndo());
    redoAction_->setEnabled(open_->session.canRedo());
}

void MainWindow::updatePasteAction() {
    pasteAction_->setEnabled(clipboard_ && editable() && !photoView_->isCropMode());
}

void MainWindow::copySettings() {
    if (!editable()) {
        return;
    }
    guarded([this] {
        // A drag still on the slider counts.
        developPanel_->finishPendingEdit();
        const Photo& photo = open_->session.photo();
        const Look look = lookOf(photo.metadata(), photo.state().settings);
        QSettings saved;
        CopySettingsDialog dialog(restoreCopySections(saved), look.fromRaw, this);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        QSettings store;
        saveCopySections(dialog.remembered(), store);
        clipboard_ = SettingsClipboard{look, dialog.sections()};
        updatePasteAction();
    });
}

void MainWindow::pasteSettings() {
    if (!clipboard_ || !editable() || photoView_->isCropMode()) {
        return;
    }
    guarded([this] {
        developPanel_->finishPendingEdit();
        const Photo& photo = open_->session.photo();
        const AppliedLook applied =
            withLook(photo.metadata(), photo.state(), clipboard_->look, clipboard_->sections);
        const bool changed = applied.state != photo.state();
        open_->session.setState(applied.state);
        refreshPanel();
        const QString skipped = skippedMessage(applied.skipped);
        if (!skipped.isEmpty()) {
            statusBar()->showMessage(skipped, 8000);
        } else if (!changed) {
            statusBar()->showMessage(tr("Nothing to paste."), 4000);
        }
    });
}

void MainWindow::closeCropOverlay() {
    photoView_->cropOverlay().stop();
    photoView_->setCropMode(false);
    cropAction_->setChecked(false);
    culling_->setCropMode(false);
    developPanel_->setCropMode(false);
    for (QShortcut* shortcut : cropShortcuts_) {
        shortcut->setEnabled(false);
    }
    lastCropRender_.reset();
    cropRequests_.clear();
}

void MainWindow::leaveCropMode(bool accept) {
    if (!photoView_->isCropMode()) {
        return;
    }
    closeCropOverlay();
    // Renders of the crop mode still on their way are not of the cropped frame.
    firstRequest_ = latestRequest_ + 1;
    updateZoomControls();
    guarded([this, accept] {
        if (open_->session.editing()) {
            if (accept) {
                open_->session.commit();
            } else {
                open_->session.cancel();
            }
        }
        refreshPanel();
    });
}

void MainWindow::editGeometry(const std::function<void(CropEditing&)>& command) {
    if (!editable()) {
        return;
    }
    if (photoView_->isCropMode()) {
        photoView_->cropOverlay().edit(command);
        return;
    }
    guarded([this, &command] {
        developPanel_->finishPendingEdit();
        DevelopState next = open_->session.photo().state();
        CropEditing editing(open_->decoded->size(), open_->decoded->orientation(),
                            next.settings.geometry);
        command(editing);
        next.settings.geometry = editing.geometry();
        open_->session.setState(next);
        refreshPanel();
    });
}

void MainWindow::setStraightening(bool straightening) {
    if (straightening && !photoView_->isCropMode()) {
        setCropMode(true);
    }
    if (photoView_->isCropMode()) {
        photoView_->cropOverlay().setStraightening(straightening);
    }
}

GeometrySettings MainWindow::reconciledGeometry(const GeometrySettings& geometry) {
    const GeometrySettings& current = open_->session.photo().state().settings.geometry;
    if (geometry == current || !open_->decoded) {
        return geometry;
    }
    if (photoView_->isCropMode()) {
        photoView_->cropOverlay().adopt(geometry);
        return photoView_->cropOverlay().editing().geometry();
    }
    if (!geometryEdit_) {
        geometryEdit_.emplace(open_->decoded->size(), open_->decoded->orientation(), current);
    }
    geometryEdit_->adopt(geometry);
    return geometryEdit_->geometry();
}

void MainWindow::pickNeutralAt(const QPointF& point) {
    if (!editable()) {
        return;
    }
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
        DevelopSettings source;
        source.color = {WhiteBalanceMode::Custom, light.kelvin, light.tint};
        constexpr std::array<std::string_view, 3> keys{"whiteBalance", "temperature", "tint"};
        next = withValues(open_->session.photo().metadata(), std::move(next), keys, source);
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
        if (photoView_->isCropMode()) {
            // The crop on screen can no longer be kept in step with the session.
            closeCropOverlay();
            firstRequest_ = latestRequest_ + 1;
            updateZoomControls();
        }
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

void MainWindow::refreshPanel(std::chrono::milliseconds renderDelay) {
    if (!open_) {
        return;
    }
    const detail::TimingSpan timing("window.panel");
    const Photo& photo = open_->session.photo();
    const bool raw = !std::holds_alternative<NamedEncoding>(photo.metadata().encoding);
    PanelContext context{raw, std::nullopt, defaultStateFor(photo.metadata().encoding).settings,
                         photo.metadata()};
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
    updateHistoryActions();
    saveAction_->setEnabled(open_->session.hasUnsavedChanges());
    updateTitle();
    if (renderDelay.count() > 0) {
        noiseReductionTimer_.start(renderDelay);
    } else {
        requestRender();
    }
}

void MainWindow::updateTitle() {
    if (!open_) {
        setWindowTitle(tr("arraw"));
        return;
    }
    setWindowTitle(
        tr("%1[*] \u2014 arraw").arg(toQString(open_->session.photo().path().filename())));
    setWindowModified(open_->session.hasUnsavedChanges());
}

void MainWindow::showDebugWindow() {
    if (!debugWindow_) {
        debugWindow_ = new DebugWindow(debugLog_, this);
    }
    debugWindow_->show();
    debugWindow_->raise();
    debugWindow_->activateWindow();
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
    CropOverlay& crop = photoView_->cropOverlay();
    connect(&crop, &CropOverlay::geometryEdited, this, [this](const GeometrySettings& geometry) {
        guarded([this, &geometry] {
            if (!open_->session.editing()) {
                return;
            }
            DevelopState next = open_->session.photo().state();
            next.settings.geometry = geometry;
            open_->session.update(next);
            refreshPanel();
        });
    });
    connect(&crop, &CropOverlay::finished, this, [this](bool accepted) {
        // The overlay has stopped itself and is still shown; the window leaves the mode.
        leaveCropMode(accepted);
    });
    connect(&crop, &CropOverlay::historyChanged, this, &MainWindow::updateHistoryActions);
    connect(&crop, &CropOverlay::renderWanted, this, [this] {
        if (open_) {
            interactionTimer_.start();
        }
    });
    setCentralWidget(photoView_);
    renderIndicator_ = new RenderIndicator(this);
    connect(renderIndicator_, &RenderIndicator::changed, renderProgress_,
            &RenderProgressPie::setDisplay);
    updateZoomControls();
}

void MainWindow::showRenderProgress(std::uint64_t request, double fraction, ProgressStep step) {
    if (request == latestRequest_ && request >= firstRequest_) {
        renderIndicator_->report(fraction, step);
    }
}

void MainWindow::buildStatusBar() {
    renderProgress_ = new RenderProgressPie(this);
    // Permanent, so that a status message neither hides it nor is hidden by it.
    statusBar()->addPermanentWidget(renderProgress_);
    deviceLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(deviceLabel_);
    statusBar()->addPermanentWidget(zoomButton_);
}

void MainWindow::showDevice(const PreviewResult& result) {
    previewOnGpu_ = result.onGpu;
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

namespace {

/// @brief Builds the open dialog's name filters from the extensions arraw opens.
///
/// All images first, then one group each for RAW, JPEG, PNG and TIFF, so the
/// dialog cannot offer what ::arraw::loadImage would not decode, nor leave out
/// what it would.
QString openFileFilter() {
    QStringList all;
    QStringList raw;
    for (const std::string_view extension : supportedImageExtensions()) {
        const QString pattern =
            QStringLiteral("*.") +
            QString::fromUtf8(extension.data(), static_cast<qsizetype>(extension.size()));
        all << pattern;
        if (extension != "jpg" && extension != "jpeg" && extension != "png" && extension != "tif" &&
            extension != "tiff") {
            raw << pattern;
        }
    }
    return MainWindow::tr("All Images (%1);;RAW Images (%2);;JPEG Images (*.jpg *.jpeg);;"
                          "PNG Images (*.png);;TIFF Images (*.tif *.tiff);;All Files (*)")
        .arg(all.join(' '), raw.join(' '));
}

} // namespace

void MainWindow::openFileWithDialog() {
    const QString filter = openFileFilter();
    const QString fileName =
        QFileDialog::getOpenFileName(this, tr("Open Photograph"), dialogFolder(), filter);
    if (fileName.isEmpty()) {
        return; // Cancelled.
    }
    // Through UTF-16, not toStdString(): on Windows a path built from a narrow
    // string is read in the ANSI code page, which mangles non-ASCII names.
    openFile(std::filesystem::path(fileName.toStdU16String()));
}

void MainWindow::openInitialPath(const std::optional<std::filesystem::path>& path) {
    QSettings store;
    const auto selected = path ? path : restoreOpenPath(store);
    if (!selected) {
        return;
    }
    const std::filesystem::path absolute(
        QFileInfo(toQString(*selected)).absoluteFilePath().toStdU16String());
    std::error_code error;
    if (std::filesystem::is_directory(absolute, error)) {
        openFolder(absolute);
    } else {
        openFile(absolute);
    }
}

void MainWindow::openFile(const std::filesystem::path& path) {
    // The one place that can tell the user: an exception must not leave a
    // function Qt's event loop called, which ends in std::terminate. The pixels are
    // decoded afterwards, on the loader's thread, and a failure there is reported
    // by decodeLanded.
    const detail::TimingSpan timing("window.openFile");
    try {
        DebugDiagnostics log(debugLog_.diagnostics);
        Photo photo = [&] {
            const detail::TimingSpan metadataTiming("window.open.metadata");
            return openPhoto(path, log);
        }();
        // Asked once the file is known to open, so that cancelling the dialog
        // or choosing a bad file leaves the current photograph and its edits.
        if (!confirmLeavingPhoto()) {
            return;
        }
        const std::filesystem::path folder = path.parent_path();
        if (filmStrip_->folder().lexically_normal() != folder.lexically_normal()) {
            try {
                const detail::TimingSpan folderTiming("window.folder.scan");
                filmStrip_->setFolder(folder);
            } catch (const std::exception& error) {
                // The photograph itself opened; only the strip cannot show its folder.
                statusBar()->showMessage(
                    tr("Cannot list %1: %2")
                        .arg(toQString(folder), QString::fromUtf8(error.what())),
                    8000);
            }
        }
        showPhoto(std::move(photo));
        rememberFolder(folder);
        // The shot that holds the file, as its primary or as a companion; the file the user
        // chose stays the one developed.
        if (const auto shot = filmStrip_->shotContaining(path)) {
            filmStrip_->setActive(*shot);
        } else {
            filmStrip_->clearActive();
        }
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Cannot Open Photograph"),
                             tr("%1\n\n%2").arg(toQString(path), QString::fromUtf8(error.what())));
    }
}

void MainWindow::openFolderWithDialog() {
    const QString folder = QFileDialog::getExistingDirectory(
        this, tr("Open Folder"), dialogFolder(), QFileDialog::ShowDirsOnly);
    if (!folder.isEmpty()) {
        openFolder(std::filesystem::path(folder.toStdU16String()));
    }
}

void MainWindow::openFolder(const std::filesystem::path& folder) {
    if (!confirmLeavingPhoto()) {
        return;
    }
    try {
        const detail::TimingSpan folderTiming("window.folder.scan");
        filmStrip_->setFolder(folder);
    } catch (const std::exception& error) {
        QMessageBox::warning(
            this, tr("Cannot Open Folder"),
            tr("%1\n\n%2").arg(toQString(folder), QString::fromUtf8(error.what())));
        return;
    }
    rememberFolder(folder);
    QSettings().remove("lastFile");
    if (const auto first = filmStrip_->firstVisible()) {
        activateShot(toQString(*first));
    }
}

void MainWindow::activateShot(const QString& primary) {
    const std::filesystem::path path(primary.toStdU16String());
    const detail::TimingSpan timing("window.activateShot");
    try {
        DebugDiagnostics log(debugLog_.diagnostics);
        Photo photo = [&] {
            const detail::TimingSpan metadataTiming("window.open.metadata");
            return openPhoto(path, log);
        }();
        if (!confirmLeavingPhoto()) {
            return; // The strip keeps its active shot and selection.
        }
        showPhoto(std::move(photo));
        filmStrip_->setActive(path);
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Cannot Open Photograph"),
                             tr("%1\n\n%2").arg(toQString(path), QString::fromUtf8(error.what())));
    }
}

QString MainWindow::dialogFolder() const {
    const QString remembered = QSettings().value("lastFolder").toString();
    return remembered.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
                                : remembered;
}

void MainWindow::rememberFolder(const std::filesystem::path& folder) {
    QSettings().setValue("lastFolder", QFileInfo(toQString(folder)).absoluteFilePath());
}

void MainWindow::buildFilmStripDock() {
    stripDock_ = new QDockWidget(tr("Film Strip"), this);
    stripDock_->setObjectName("FilmStripDock");
    stripDock_->setAllowedAreas(Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);
    // Closable for the View menu's toggle; the strip's own title bar has no close button.
    stripDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                            QDockWidget::DockWidgetClosable);
    filmStrip_->setMinimumHeight(80);
    stripDock_->setWidget(filmStrip_);
    stripDock_->setTitleBarWidget(filmStrip_->titleBar());
    addDockWidget(Qt::BottomDockWidgetArea, stripDock_);
    resizeDocks({stripDock_}, {132}, Qt::Vertical);

    QAction* toggle = stripDock_->toggleViewAction();
    toggle->setObjectName("filmStripAction");
    toggle->setText(tr("&Film Strip"));
    toggle->setShortcut(Qt::Key_F9);
    viewMenu_->addAction(toggle);

    filmStrip_->setMarksWriter([this](const std::filesystem::path& primary,
                                      const PhotoMarks& marks) { writeMarks(primary, marks); });
    connect(filmStrip_, &FilmStrip::activationRequested, this, &MainWindow::activateShot);
    connect(filmStrip_, &FilmStrip::folderRequested, this, &MainWindow::openFolderWithDialog);
    connect(filmStrip_, &FilmStrip::exportRequested, this, &MainWindow::exportWithDialog);
    connect(filmStrip_, &FilmStrip::sidecarChangedExternally, this,
            &MainWindow::sidecarChangedOnDisk);
}

void MainWindow::writeMarks(const std::filesystem::path& primary, const PhotoMarks& marks) {
    // The marks of a shot live in its primary's sidecar. The session writes them for the
    // photograph it holds; a companion open in the develop view has a sidecar of its own, and
    // the shot's marks are not in it.
    if (open_ && open_->session.photo().path() == primary) {
        setMarksForCurrent(marks);
    } else {
        writeSidecarMarks(primary, marks);
    }
}

void MainWindow::sidecarChangedOnDisk(const QString& primary) {
    const std::filesystem::path path(primary.toStdU16String());
    if (!open_ || open_->session.photo().path() != path) {
        return; // The strip has refreshed the marks; nothing else is open on it.
    }
    const QString name = toQString(path.filename());
    leaveCropMode(true);
    try {
        developPanel_->finishPendingEdit();
    } catch (const std::exception&) {
        // The edit stays open, and counts as unsaved below.
    }
    if (open_->session.hasUnsavedChanges()) {
        statusBar()->showMessage(
            tr("The sidecar of %1 changed on disk; your unsaved edits are kept.").arg(name), 10000);
        return;
    }
    try {
        DebugDiagnostics log(debugLog_.diagnostics);
        Photo photo = openPhoto(path, log);
        const Photo& saved = open_->session.saved();
        if (photo.state() == saved.state() && photo.marks() == saved.marks()) {
            return; // Touched, not changed: keep the history.
        }
        // The pixels are the same file's; only the settings and marks are read again, and the
        // view stays where it is.
        auto decoded = open_->decoded;
        auto cameraPreview = std::move(open_->cameraPreview);
        auto lastCropImage = std::move(open_->lastCropImage);
        open_.emplace(OpenPhoto{EditSession(std::move(photo)), std::move(decoded),
                                std::move(cameraPreview), std::move(lastCropImage)});
        refreshPanel();
        statusBar()->showMessage(tr("Reloaded %1: its sidecar changed on disk.").arg(name), 5000);
    } catch (const std::exception& error) {
        statusBar()->showMessage(
            tr("Cannot reload the sidecar of %1: %2").arg(name, QString::fromUtf8(error.what())),
            10000);
    }
}

namespace {

/// @brief Describes a format for the save dialog's filter.
QString nameFilterFor(ImageFileFormat format) {
    switch (format) {
    case ImageFileFormat::Jpeg:
        return QObject::tr("JPEG Images (*.jpg *.jpeg)");
    case ImageFileFormat::Png:
        return QObject::tr("PNG Images (*.png)");
    case ImageFileFormat::Tiff:
        return QObject::tr("TIFF Images (*.tif *.tiff)");
    }
    return {};
}

} // namespace

void MainWindow::exportWithDialog() {
    if (!open_) {
        return;
    }
    if (!open_->decoded) {
        statusBar()->showMessage(tr("%1 is still being decoded.")
                                     .arg(toQString(open_->session.photo().path().filename())),
                                 5000);
        return;
    }
    // The end of a pending edit may change what is exported.
    developPanel_->finishPendingEdit();
    try {
        const Photo& photo = open_->session.photo();
        const ImageSize frame =
            croppedSize(open_->decoded->size(), open_->decoded->orientation(), photo.state());

        QSettings store;
        ExportDialog dialog(restoreSettings(store), frame, this);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        const ExportSettings settings = dialog.settings();
        saveSettings(settings, store);

        const std::filesystem::path suggested = suggestedPath(photo.path(), settings.format);
        const QString chosen = QFileDialog::getSaveFileName(
            this, tr("Export Image"), toQString(suggested), nameFilterFor(settings.format));
        if (chosen.isEmpty()) {
            return; // Cancelled.
        }
        const std::filesystem::path typed(chosen.toStdU16String());
        const std::filesystem::path path = withSuffix(typed, settings.format);
        if (isSameFile(path, photo.path())) {
            QMessageBox::warning(this, tr("Cannot Export Photograph"),
                                 tr("%1\n\nThis is the photograph itself; the export would "
                                    "overwrite it. Choose another name.")
                                     .arg(toQString(path)));
            return;
        }
        std::error_code ignored;
        if (path != typed && std::filesystem::exists(path, ignored) &&
            QMessageBox::question(this, tr("Export Image"),
                                  tr("%1 already exists. Replace it?").arg(toQString(path))) !=
                QMessageBox::Yes) {
            return; // The file dialog only asked about the name as typed.
        }

        const std::uint64_t id =
            exportQueue_.enqueue({.state = photo.state(),
                                  .source = open_->decoded,
                                  .request = requestOf(settings),
                                  .options = optionsOf(settings),
                                  .path = path,
                                  .metadata = ExportMetadata{.source = photo.path(),
                                                             .marks = photo.marks(),
                                                             .selection = selectionOf(settings)}});
        exportNames_.emplace(id, toQString(path.filename()));
        showExportProgress();
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Cannot Export Photograph"), QString::fromUtf8(error.what()));
    }
}

void MainWindow::showExportProgress() {
    if (exportNames_.empty()) {
        return;
    }
    QString text = tr("Exporting %1…").arg(exportNames_.begin()->second);
    if (exportNames_.size() > 1) {
        text += tr(" (%1 more)").arg(exportNames_.size() - 1);
    }
    statusBar()->showMessage(text);
}

void MainWindow::showExportResult(const ExportResult& result) {
    statusBar()->setToolTip({});
    // A result names its job; one whose job was cancelled has no name left.
    if (const auto found = exportNames_.find(result.id); found != exportNames_.end()) {
        const QString name = found->second;
        exportNames_.erase(found);
        if (result.error.empty()) {
            if (result.warnings.empty()) {
                statusBar()->showMessage(tr("Exported %1").arg(name), 5000);
            } else {
                QStringList details;
                for (const auto& warning : result.warnings) {
                    details << QString::fromStdString(warning);
                }
                statusBar()->setToolTip(details.join(QLatin1Char('\n')));
                statusBar()->showMessage(tr("Exported %1 without some metadata").arg(name), 8000);
            }
        } else {
            statusBar()->clearMessage();
        }
    }
    if (!result.error.empty()) {
        QMessageBox::warning(
            this, tr("Cannot Export Photograph"),
            tr("%1\n\n%2").arg(toQString(result.path), QString::fromStdString(result.error)));
    }
    showExportProgress();
    if (exportNames_.empty() && closeWhenIdle_) {
        close();
    }
}

bool MainWindow::saveAdjustments() {
    if (!open_) {
        return true;
    }
    const detail::TimingSpan timing("window.save");
    try {
        leaveCropMode(true);
        developPanel_->finishPendingEdit();
        open_->session.save();
        filmStrip_->noteOwnWrite(open_->session.photo().path());
        // The cache gets the thumbnail of what was saved, rendered by the strip's worker: the
        // preview may show a zoomed part of the frame, or a size the cache does not want.
        filmStrip_->noteSettingsSaved(open_->session.photo().path());
        refreshPanel();
        return true;
    } catch (const std::exception& error) {
        // Nothing changed: the edits are still there, unsaved.
        QMessageBox::warning(
            this, tr("Cannot Save Adjustments"),
            tr("%1\n\n%2")
                .arg(toQString(open_->session.photo().path()), QString::fromUtf8(error.what())));
        return false;
    }
}

bool MainWindow::confirmLeavingPhoto() {
    if (!open_) {
        return true;
    }
    leaveCropMode(true);
    try {
        developPanel_->finishPendingEdit();
    } catch (const std::exception&) {
        // The edit stays open, and counts as unsaved below.
    }
    if (!open_->session.hasUnsavedChanges()) {
        return true;
    }
    QMessageBox box(
        QMessageBox::Question, tr("Unsaved Changes"),
        tr("Save changes to %1?").arg(toQString(open_->session.photo().path().filename())),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.setInformativeText(tr("Your adjustments will be lost if you do not save them."));
    box.setDefaultButton(QMessageBox::Save);
    box.setEscapeButton(QMessageBox::Cancel);
    switch (box.exec()) {
    case QMessageBox::Save:
        return saveAdjustments();
    case QMessageBox::Discard:
        open_->session.discardChanges();
        // The window may stay open (a cancelled close), and then shows the saved state.
        refreshPanel();
        return true;
    default:
        return false;
    }
}

void MainWindow::setMarksForCurrent(PhotoMarks marks) {
    if (!open_) {
        return;
    }
    // Marks are no develop edit and not part of what the panel shows, so no refresh.
    open_->session.setMarks(marks);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    closeWhenIdle_ = false;
    // Unsaved changes first: they are the user's work, whereas running exports
    // are asked about only once the window is certain to go.
    if (!confirmLeavingPhoto()) {
        event->ignore();
        return;
    }
    if (exportNames_.empty()) {
        QMainWindow::closeEvent(event);
        return;
    }
    QMessageBox box(QMessageBox::Question, tr("Exports Running"), tr("Exports are still running."),
                    QMessageBox::NoButton, this);
    box.setInformativeText(tr("Wait for them to finish, or cancel the ones that have not started? "
                              "The one in progress finishes either way."));
    QPushButton* wait = box.addButton(tr("Wait"), QMessageBox::AcceptRole);
    QPushButton* cancel = box.addButton(tr("Cancel Exports"), QMessageBox::DestructiveRole);
    box.setDefaultButton(wait);
    box.setEscapeButton(wait);
    box.exec();
    if (box.clickedButton() == cancel) {
        // The queue's oldest job is the one in progress, or about to start;
        // the others are dropped and will not report.
        const std::size_t dropped = exportQueue_.cancelQueued();
        // Identifiers increase, so the dropped jobs are the newest names.
        for (std::size_t i = 0; i < dropped && !exportNames_.empty(); ++i) {
            exportNames_.erase(std::prev(exportNames_.end()));
        }
        QMainWindow::closeEvent(event);
        return;
    }
    closeWhenIdle_ = true;
    event->ignore();
}

void MainWindow::requestRender() {
    // Nothing to render until the pixels land; decodeLanded asks then.
    if (!editable()) {
        return;
    }
    // Any pending resize or interaction is covered by this request.
    resizeTimer_.stop();
    interactionTimer_.stop();
    noiseReductionTimer_.stop();
    const DevelopState& state = open_->session.photo().state();
    const qreal ratio = photoView_->devicePixelRatioF();
    if (photoView_->isCropMode()) {
        // The photograph turned and flipped, neither straightened nor cropped:
        // the overlay straightens it on screen (ADR 040). The effects follow the
        // crop (ADR 037), so they wait for it. One geometry for the whole
        // session, so the renderer's checkpoints serve every crop edit, and an
        // edit that changes nothing here asks for nothing.
        DevelopState uncropped = uncroppedState(state);
        const QSize size = photoView_->cropOverlay().renderSize().expandedTo({1, 1});
        if (lastCropRender_ && lastCropRender_->first == uncropped &&
            lastCropRender_->second == size) {
            return;
        }
        lastCropRender_.emplace(uncropped, size);
        renderIndicator_->begin();
        latestRequest_ = previewRenderer_.request(uncropped, PreviewView::wholeFrame(size, ratio));
        cropRequests_.emplace_back(latestRequest_, std::move(uncropped));
        return;
    }
    PreviewView view{.region = std::nullopt,
                     .outputSize = photoView_->devicePixels(),
                     .devicePixelRatio = ratio,
                     .thumbnailEdge = ThumbnailCache::maxEdge};
    try {
        // The crop may have changed the frame; the view keeps its zoom and centre.
        const ImageSize cropped =
            croppedSize(open_->decoded->size(), open_->decoded->orientation(), state);
        photoView_->setFrameSize(
            QSize(static_cast<int>(cropped.width), static_cast<int>(cropped.height)));
        const ViewTransform transform = photoView_->transform();
        const QRect visible = transform.visiblePixels();
        // A quarter-view margin on each side covers short pans immediately.
        const int marginX = (visible.width() + 3) / 4;
        const int marginY = (visible.height() + 3) / 4;
        const QRect region = visible.adjusted(-marginX, -marginY, marginX, marginY)
                                 .intersected(QRect(QPoint(0, 0), transform.frame().toSize()));
        view.region = region;
        view.outputSize =
            (QSizeF(region.size()) * std::min(transform.zoom(), 1.0)).toSize().expandedTo({1, 1});
    } catch (const std::exception&) {
        // Left to the renderer, which reports what is wrong with the state.
    }
    renderIndicator_->begin();
    latestRequest_ = previewRenderer_.request(state, std::move(view));
}

void MainWindow::showResult(const PreviewResult& result) {
    const detail::TimingSpan timing("window.result", result.request);
    // The newest request's render, delivered or failed, ends the busy period; a recount of
    // the histogram or a refreshed fallback follows a render, and is not one. Before the
    // filter below, so that a render no longer wanted (a photograph was opened, the crop
    // mode left) still ends it when no request follows.
    // While a decode is awaited the busy period is the decode's.
    if (decodeRequest_ == 0 && result.request >= latestRequest_ &&
        (result.image || !result.error.empty())) {
        renderIndicator_->finish(result.image.has_value() && result.request >= firstRequest_);
    }
    if (result.request < firstRequest_) {
        return;
    }
    if (photoView_->isCropMode() && result.request >= cropFirstRequest_) {
        // The state the render shows, and the requests older than it, which will not be shown.
        std::optional<DevelopState> renderedFor;
        while (!cropRequests_.empty() && cropRequests_.front().first <= result.request) {
            if (cropRequests_.front().first == result.request) {
                renderedFor = cropRequests_.front().second;
            }
            cropRequests_.pop_front();
        }
        if (result.image && result.request > latestShown_ && renderedFor) {
            latestShown_ = result.request;
            // Turned and flipped to the geometry now, should a turn have happened since.
            photoView_->cropOverlay().setImage(*result.image, renderedFor->settings.geometry);
            open_->lastCropImage.emplace(*renderedFor, *result.image);
            showDevice(result);
            return;
        }
        if (result.image || result.background || result.curveHistogram) {
            return; // Of the uncropped frame: no thumbnail, background or histogram of the photo.
        }
    }
    // A recounted curve histogram comes once requests pause, for the newest
    // state rendered, and on a result of its own.
    if (result.curveHistogram) {
        developPanel_->showCurveHistogram(*result.curveHistogram);
        return;
    }
    // A refreshed fallback follows the render it belongs to, so it is never
    // older than what is shown.
    if (!result.image && result.background) {
        photoView_->setBackground(*result.background);
        return;
    }
    if (result.request <= latestShown_) {
        return;
    }
    if (result.image) {
        latestShown_ = result.request;
        photoView_->setImage(*result.image, result.region, result.background.value_or(QImage{}));
        showDevice(result);
        followWithThumbnail(result);
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

void MainWindow::followWithThumbnail(const PreviewResult& result) {
    if (!open_) {
        return;
    }
    // Reduced on the worker (ADR 043), and only for a render of the whole frame: a zoomed
    // view shows a part of it, which is no thumbnail of the photograph.
    if (!result.thumbnail) {
        return;
    }
    // The strip's cell is the photograph's primary; a companion has a sidecar, and a thumbnail,
    // of its own that the strip does not show.
    const std::filesystem::path path = open_->session.photo().path();
    if (filmStrip_->shotContaining(path) != path) {
        return;
    }
    filmStrip_->setLiveThumbnail(path, *result.thumbnail);
}

void MainWindow::showPhoto(Photo photo) {
    const detail::TimingSpan timing("window.showPhoto");
    if (photoView_->isCropMode()) {
        // Only when nothing asked first; the edit belongs to the session being replaced.
        closeCropOverlay();
    }
    geometryEdit_.reset();
    setPicking(false);
    // The shot just left shows its saved settings again, not the edits that were abandoned.
    filmStrip_->releaseLiveThumbnail();
    const std::filesystem::path path = photo.path();
    open_.emplace(OpenPhoto{EditSession(std::move(photo)), nullptr, std::nullopt, std::nullopt});
    // The previous photograph's pixels go, and with them whatever the renderer was doing.
    previewRenderer_.setSource(nullptr);
    // Results of the previous photograph are still on their way, or in progress.
    firstRequest_ = latestRequest_ + 1;
    // The previous photograph's histogram is no histogram of this one.
    developPanel_->clearCurveHistogram();
    cameraPreviewRequest_ = 0;
    showStandIn();
    // Cancels the decode of a photograph opened before this one (ADR 042).
    decodeRequest_ = photoLoader_.decode(path);
    renderIndicator_->begin();
    // A render of the previous photograph may still hold the bar: its step is not this one's.
    renderIndicator_->report(0.0, ProgressStep::Decode);
    updateEditingActions();
    refreshPanel(); // The sidecar's state at once; the render waits for the pixels.
    rememberFolder(path.parent_path());
    QSettings().setValue("lastFile", QFileInfo(toQString(path)).absoluteFilePath());
}

void MainWindow::showStandIn() {
    const Photo& photo = open_->session.photo();
    QSize frame;
    try {
        const ImageSize cropped =
            croppedSize(photo.metadata().size, photo.metadata().orientation, photo.state());
        frame = QSize(static_cast<int>(cropped.width), static_cast<int>(cropped.height));
    } catch (const std::exception&) {
        // A state the renderer will report on; no frame to fit until then.
    }
    QImage standIn;
    if (filmStrip_->shotContaining(photo.path()) == photo.path()) {
        standIn = filmStrip_->thumbnail(photo.path());
    }
    // A thumbnail of another shape is of another frame: a camera preview of a turned or
    // cropped photograph. Within the rounding of a small thumbnail it is this frame.
    constexpr double tolerance = 0.03;
    if (!standIn.isNull() && !frame.isEmpty()) {
        const double frameAspect = static_cast<double>(frame.width()) / frame.height();
        const double standInAspect = static_cast<double>(standIn.width()) / standIn.height();
        if (std::abs(standInAspect / frameAspect - 1.0) > tolerance) {
            standIn = {};
        }
    }
    photoView_->setFrameSize(frame);
    photoView_->resetView();
    photoView_->setStandIn(standIn);
}

void MainWindow::decodeLanded(const DecodedPhoto& result) {
    if (result.request != decodeRequest_) {
        return; // Of a photograph left since.
    }
    decodeRequest_ = 0;
    if (!open_) {
        return;
    }
    if (!result.decoded) {
        renderIndicator_->finish(false);
        const std::filesystem::path path = open_->session.photo().path();
        closePhoto();
        QMessageBox::warning(
            this, tr("Cannot Open Photograph"),
            tr("%1\n\n%2").arg(toQString(path), QString::fromStdString(result.error)));
        return;
    }
    const detail::TimingSpan timing("window.decodeLanded");
    open_->decoded = result.decoded;
    previewRenderer_.setSource(result.decoded);
    updateEditingActions();
    refreshPanel(); // Requests the first render.
}

void MainWindow::closePhoto() {
    if (photoView_->isCropMode()) {
        closeCropOverlay();
    }
    geometryEdit_.reset();
    setPicking(false);
    filmStrip_->releaseLiveThumbnail();
    filmStrip_->clearActive();
    open_.reset();
    photoLoader_.cancelDecode();
    decodeRequest_ = 0;
    cameraPreviewRequest_ = 0;
    previewRenderer_.setSource(nullptr);
    firstRequest_ = latestRequest_ + 1;
    developPanel_->clearCurveHistogram();
    photoView_->setFrameSize({});
    photoView_->resetView();
    updateEditingActions();
    updateHistoryActions();
    saveAction_->setEnabled(false);
    updateTitle();
    QSettings().remove("lastFile");
}

void MainWindow::updateEditingActions() {
    const bool ready = editable();
    developDock_->setEnabled(ready);
    exportAction_->setEnabled(ready);
    cropAction_->setEnabled(ready);
    copyAction_->setEnabled(ready);
    updatePasteAction();
    for (QAction* action : geometryActions_) {
        action->setEnabled(ready);
    }
    updateZoomControls();
}

} // namespace arraw::app
