#include "MainWindow.h"
#include "AdjustmentPanel.h"
#include "BatchPaste.h"
#include "BatchProgressDialog.h"
#include "CollapsiblePane.h"
#include "ColorManagement.h"
#include "CropGeometry.h"
#include "DevelopGroup.h"
#include "DevelopPreset.h"
#include "DevelopSession.h"
#include "ExifPanel.h"
#include "ExportDialog.h"
#include "FilmStrip.h"
#include "GroupChecklistDialog.h"
#include "ImageViewport.h"
#include "LocalAdjustmentPanel.h"
#include "SpotRemovalPanel.h"
#include "ProofingPanel.h"
#include "RawProcessor.h"
#include "StandardImageLoader.h"
#include "ThumbnailCache.h"
#include "XmpSidecar.h"
#include <algorithm>
#include <cmath>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QColorSpace>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QToolBar>
#include <QToolButton>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrent/QtConcurrent>

namespace {
DevelopSession::SidecarState toSessionSidecarState(SidecarLoadStatus status) {
    switch (status) {
    case SidecarLoadStatus::Missing:
        return DevelopSession::SidecarState::Missing;
    case SidecarLoadStatus::Loaded:
        return DevelopSession::SidecarState::Loaded;
    case SidecarLoadStatus::ParseError:
        return DevelopSession::SidecarState::ParseError;
    }
    return DevelopSession::SidecarState::Unknown;
}
} // namespace

// ---------------------------------------------------------------------------
// Undo command: captures before/after GlobalAdjustment for a single gesture.
// ---------------------------------------------------------------------------
class AdjustmentCommand : public QUndoCommand {
public:
    AdjustmentCommand(
        DevelopSession* session,
        MainWindow* mainWindow,
        const GlobalAdjustment& before,
        const GlobalAdjustment& after)
        : session(session),
          mainWindow(mainWindow),
          before(before),
          after(after) {
        setText("Adjust");
    }

    void undo() override;

    void redo() override;

private:
    DevelopSession* session;
    MainWindow* mainWindow;
    GlobalAdjustment before, after;
};

// ---------------------------------------------------------------------------
// Undo command for local adjustments — add / delete / move-handle / tweak.
// Restores the whole list (which re-renders), mirroring AdjustmentCommand.
// ---------------------------------------------------------------------------
class LocalAdjustmentCommand : public QUndoCommand {
public:
    LocalAdjustmentCommand(
        DevelopSession* session,
        MainWindow* mainWindow,
        std::vector<LocalAdjustment> before,
        std::vector<LocalAdjustment> after)
        : session(session),
          mainWindow(mainWindow),
          before(std::move(before)),
          after(std::move(after)) {
        setText("Adjust Local");
    }

    void undo() override;

    void redo() override;

private:
    DevelopSession* session;
    MainWindow* mainWindow;
    std::vector<LocalAdjustment> before, after;
};

// ---------------------------------------------------------------------------
// Undo command for spots — add / delete / drag committed. Restores the whole
// list so both SpotRemovalPanel and MainWindow::rebuildSpottedBuffers sync up.
// ---------------------------------------------------------------------------
class SpotListCommand : public QUndoCommand {
public:
    SpotListCommand(
        DevelopSession* session,
        MainWindow* mainWindow,
        std::vector<Spot> before,
        std::vector<Spot> after)
        : session(session),
          mainWindow(mainWindow),
          before(std::move(before)),
          after(std::move(after)) {
        setText("Edit Spot");
    }

    void undo() override;

    void redo() override;

private:
    DevelopSession* session;
    MainWindow* mainWindow;
    std::vector<Spot> before, after;
};

// ---------------------------------------------------------------------------
void AdjustmentCommand::undo() {
    session->setParams(before);
    mainWindow->syncSessionToEditors();
}

void AdjustmentCommand::redo() {
    session->setParams(after);
    mainWindow->syncSessionToEditors();
}

// ---------------------------------------------------------------------------
void LocalAdjustmentCommand::undo() {
    session->setLocalAdjustments(before);
    mainWindow->syncSessionToEditors();
}

void LocalAdjustmentCommand::redo() {
    session->setLocalAdjustments(after);
    mainWindow->syncSessionToEditors();
}

void SpotListCommand::undo() {
    session->setSpots(before);
    mainWindow->syncSessionSpotsToEditors(true);
}

void SpotListCommand::redo() {
    session->setSpots(after);
    mainWindow->syncSessionSpotsToEditors(true);
}

// ---------------------------------------------------------------------------
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      presetStore(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/presets") {
    setWindowTitle("arraw");

    viewport = new ImageViewport(this);
    session = new DevelopSession(this);
    undoStack = new QUndoStack(this);
    setCentralWidget(viewport);

    monitorProfilePath = QSettings().value("display/monitorProfile").toString();

    setupDocks();
    setupMenus();
    setupStatusBar();
    setupToolbar();

    connect(proofPanel, &ProofingPanel::proofingChanged, this, &MainWindow::rebuildDisplayLut);
    rebuildDisplayLut();

    connect(&loadWatcher, &QFutureWatcher<LoadResult>::finished, this, &MainWindow::onLoadFinished);

    connect(viewport, &ImageViewport::fullResNeeded, this, &MainWindow::onFullResNeeded);

    connect(viewport, &ImageViewport::zoomChanged, this, &MainWindow::updateZoomStatus);

    connect(
        filmStrip,
        &FilmStrip::marksChanged,
        this,
        [this](const QString& path, const UserMetadata& metadata, bool saved) {
            if (path != session->path())
                return;
            session->setUserMetadata(metadata);
            if (saved) {
                session->markMetadataSaved();
            } else {
                session->markMetadataSaveFailed();
                statusLabel->setText("Could not write " + XmpSidecar::pathFor(session->path()));
            }
        });

    connect(
        viewport, &ImageViewport::cropCommitted, this, [this](const QRectF& rect, bool constrained) {
            GlobalAdjustment before = currentParams();
            GlobalAdjustment after = before;
            after.cropRect = rect;
            after.cropConstrained = constrained;
            if (after != before)
                pushGlobalAdjustmentCommand(before, after);
        });

    connect(viewport, &ImageViewport::rotationCommitted, this, [this](float degrees) {
        GlobalAdjustment before = currentParams();
        GlobalAdjustment after = before;
        after.rotation = degrees;
        if (after != before)
            pushGlobalAdjustmentCommand(before, after);
    });

    connect(viewport, &ImageViewport::whiteBalanceCommitted, this, [this](float kelvin, float tint) {
        GlobalAdjustment before = currentParams();
        GlobalAdjustment after = before;
        after.temperature = kelvin;
        after.tint = tint;
        if (after != before)
            pushGlobalAdjustmentCommand(before, after);
    });

    connect(viewport, &ImageViewport::activeToolChanged, this, [this](ImageViewport::ActiveTool) {
        syncToolActions();
    });

    connect(
        adjPanel,
        &AdjustmentPanel::adjustmentCommitted,
        this,
        [this](const GlobalAdjustment& before, const GlobalAdjustment& after) {
            pushGlobalAdjustmentCommand(before, after);
        });

    connect(
        adjPanel,
        &AdjustmentPanel::paramsChanged,
        this,
        [this](const GlobalAdjustment& params) {
            session->setParams(applyGroups(session->params(), params, allGroups()));
            pushParamsToViewport();
        });

    connect(
        localPanel,
        &LocalAdjustmentPanel::changed,
        this,
        [this](const std::vector<LocalAdjustment>& localAdjustments) {
            session->setLocalAdjustments(localAdjustments);
            pushParamsToViewport();
        });

    // Develop-thumbnail regeneration is debounced: every param change restarts the
    // timer (in pushParamsToViewport), so it fires only once edits settle.
    thumbTimer = new QTimer(this);
    thumbTimer->setSingleShot(true);
    thumbTimer->setInterval(750);
    connect(thumbTimer, &QTimer::timeout, this, &MainWindow::generateDevelopedThumbnail);

    // Panel selection drives which mask the on-image tool edits; on-image drags
    // write the geometry back into the panel.
    connect(
        localPanel,
        &LocalAdjustmentPanel::activeIndexChanged,
        viewport,
        &ImageViewport::setActiveLocalAdjustment);
    connect(
        viewport,
        &ImageViewport::localMaskChanged,
        localPanel,
        &LocalAdjustmentPanel::updateMaskGeometry);

    // Local edits join the shared undo stack: add / delete / tweak / numeric
    // geometry all commit through the panel; an on-image handle drag commits on
    // release.
    connect(
        localPanel,
        &LocalAdjustmentPanel::committed,
        this,
        [this](const std::vector<LocalAdjustment>& before, const std::vector<LocalAdjustment>& after) {
            undoStack->push(new LocalAdjustmentCommand(session, this, before, after));
        });
    connect(
        viewport,
        &ImageViewport::localMaskEditFinished,
        localPanel,
        &LocalAdjustmentPanel::commitMaskEdit);

    // Spot-removal: live drag rebuilds preview only; commit on mouse-release.
    connect(spotPanel, &SpotRemovalPanel::changed, this, [this](const std::vector<Spot>& spots) {
        session->setSpots(spots);
        viewport->setSpots(spots);
        rebuildSpottedBuffers(false);
    });
    connect(
        spotPanel,
        &SpotRemovalPanel::committed,
        this,
        [this](std::vector<Spot> before, std::vector<Spot> after) {
            undoStack->push(new SpotListCommand(session, this, std::move(before), std::move(after)));
        });
    // Viewport signals: click places a new spot; handle drags move dest/source.
    connect(viewport, &ImageViewport::spotRequested, this, [this](QPointF destBufPx) {
        if (!session->preview().valid())
            return;
        const ImageBuffer& preview = session->preview();
        const double offset = 0.1 * std::min(preview.width, preview.height);
        const Spot s{
            .destination = destBufPx,
            .source      = autoSourcePosition(destBufPx, offset, preview.width, preview.height),
            .radius      = offset * 0.5,
            .feather     = 0.5,
        };
        spotPanel->addSpot(s);
    });
    connect(
        viewport,
        &ImageViewport::spotHandleChanged,
        this,
        [this](int idx, ImageViewport::SpotHandle h, QPointF bufPx) {
            if (idx < 0 || idx >= static_cast<int>(spotPanel->spots().size()))
                return;
            auto updated = spotPanel->spots()[idx];
            if (h == ImageViewport::SpotHandle::Destination)
                updated.destination = bufPx;
            else
                updated.source = bufPx;
            // updateSpot emits changed → rebuildSpottedBuffers(false) via connection.
            spotPanel->updateSpot(idx, updated);
        });
    connect(
        viewport,
        &ImageViewport::spotHandleCommitted,
        this,
        [this](int idx, ImageViewport::SpotHandle h, QPointF bufPx) {
            if (idx < 0 || idx >= static_cast<int>(spotPanel->spots().size()))
                return;
            auto updated = spotPanel->spots()[idx];
            if (h == ImageViewport::SpotHandle::Destination)
                updated.destination = bufPx;
            else
                updated.source = bufPx;
            spotPanel->commitSpotEdit(idx, updated);
        });

    connect(
        viewport, &ImageViewport::histogramsReady, adjPanel, &AdjustmentPanel::setHistogramSamples);

    connect(
        adjPanel, &AdjustmentPanel::straightenActive, viewport, &ImageViewport::setStraightenActive);

    // Restore window geometry
    QSettings s;
    restoreGeometry(s.value("geometry").toByteArray());
    restoreState(s.value("windowState").toByteArray());

    // restoreState may have brought the Adjustments dock back collapsed; align
    // the pane's internal state with the widgets it just restored.
    if (adjustmentsDock->isHidden())
        adjustmentsPane->collapse();
    else
        adjustmentsPane->expand();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    QSettings s;
    s.setValue("geometry", saveGeometry());
    s.setValue("windowState", saveState());
    QString lastDir = filmStrip->directory();
    if (!lastDir.isEmpty()) {
        s.setValue("lastDir", lastDir);
    }
    QMainWindow::closeEvent(e);
}

void MainWindow::keyPressEvent(QKeyEvent* e) {
    // Culling marks (0-5, X, r/y/g/b/p) are owned by the Image menu's actions —
    // window-level shortcuts that fire whether the strip or the image has focus.
    if (e->key() == Qt::Key_Left)
        filmStrip->navigateBy(-1);
    else if (e->key() == Qt::Key_Right)
        filmStrip->navigateBy(+1);
    else if (e->key() == Qt::Key_S && e->modifiers() == Qt::NoModifier)
        proofPanel->setProofingEnabled(!proofPanel->proofingEnabled());
    else if (e->key() == Qt::Key_J && e->modifiers() == Qt::NoModifier)
        toggleClipping();
    else
        QMainWindow::keyPressEvent(e);
}

void MainWindow::setupMenus() {
    auto* file = menuBar()->addMenu("&File");
    file->addAction("&Open...", QKeySequence::Open, this, &MainWindow::openFile);
    file->addAction(
        "Open &Folder...",
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O),
        filmStrip,
        &FilmStrip::promptForDirectory);
    file->addSeparator();
    file->addAction("&Save Adjustments", QKeySequence::Save, this, &MainWindow::saveAdjustments);
    file->addAction("&Export...", Qt::CTRL | Qt::Key_E, this, &MainWindow::exportFile);
    file->addSeparator();
    file->addAction("&Quit", QKeySequence::Quit, qApp, &QCoreApplication::quit);

    auto* edit = menuBar()->addMenu("&Edit");
    edit->addAction(undoStack->createUndoAction(this));
    edit->addAction(undoStack->createRedoAction(this));
    edit->addSeparator();
    edit->addAction(
        tr("&Copy Settings..."),
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
        this,
        &MainWindow::copySettings);
    edit->addAction(
        tr("&Paste Settings..."),
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V),
        this,
        &MainWindow::pasteSettings);

    presetsMenu = menuBar()->addMenu(tr("&Presets"));
    rebuildPresetsMenu();

    setupImageMenu();

    auto* view = menuBar()->addMenu("&View");
    view->addAction(filmStripDock->toggleViewAction());
    auto* toggleAdjustments = view->addAction("Adjustments Panel", this, [this] {
        adjustmentsPane->toggle();
    });
    toggleAdjustments->setShortcut(Qt::Key_F8);
    view->addSeparator();
    view->addAction("Reset Zoom", Qt::CTRL | Qt::Key_0, viewport, &ImageViewport::resetView);
    view->addSeparator();

    // Clipping overlay (docs/adr/0009). Two independent toggles; J (handled in
    // keyPressEvent) flips both at once. View state, persisted in QSettings.
    QSettings clipSettings;
    clipHighlightsAction = view->addAction("Show &Highlight Clipping");
    clipHighlightsAction->setCheckable(true);
    clipHighlightsAction->setChecked(clipSettings.value("view/clipHighlights", false).toBool());
    connect(clipHighlightsAction, &QAction::toggled, this, &MainWindow::applyClipping);

    clipShadowsAction = view->addAction("Show &Shadow Clipping");
    clipShadowsAction->setCheckable(true);
    clipShadowsAction->setChecked(clipSettings.value("view/clipShadows", false).toBool());
    connect(clipShadowsAction, &QAction::toggled, this, &MainWindow::applyClipping);
    applyClipping(); // push the restored state to the viewport
    view->addSeparator();

    // Monitor profile: how the preview is encoded for this screen.
    // "sRGB" keeps the fast in-shader path; a profile switches to the LUT path.
    auto* monitorMenu = view->addMenu("Monitor Profile");
    auto* monitorGroup = new QActionGroup(this);
    monitorGroup->setExclusive(true);

    auto addMonitorAction = [&](const QString& text, const QString& path) {
        QAction* a = monitorMenu->addAction(text);
        a->setCheckable(true);
        a->setActionGroup(monitorGroup);
        a->setChecked(path == monitorProfilePath);
        connect(a, &QAction::triggered, this, [this, path] {
            monitorProfilePath = path;
            QSettings().setValue("display/monitorProfile", path);
            rebuildDisplayLut();
        });
        return a;
    };

    addMonitorAction("sRGB (assume)", QString())->setChecked(monitorProfilePath.isEmpty());
    for (const IccProfileInfo& info : scanSystemProfiles())
        if (info.isDisplayClass)
            addMonitorAction(info.description, info.path);
}

void MainWindow::setupImageMenu() {
    // Culling marks on the current file. These actions own the single-key
    // shortcuts (window-level, so they fire whether the strip or the image has
    // focus, and beat the file list's type-ahead). The same commands appear in
    // the strip's right-click menu. See docs/adr/0007.
    auto* image = menuBar()->addMenu("&Image");

    auto* rateMenu = image->addMenu("Rating");
    QList<QPair<QAction*, int>> rateActions;
    auto addRate = [&](const QString& text, int n, QKeySequence key) {
        QAction* a = rateMenu->addAction(text, this, [this, n] { setCurrentRating(n); });
        a->setShortcut(key);
        a->setCheckable(true);
        rateActions.append({a, n});
    };
    for (int n = 5; n >= 1; --n)
        addRate(QString(n, QChar(0x2605)), n, QKeySequence(Qt::Key_0 + n)); // ★×n
    rateMenu->addSeparator();
    addRate(tr("Unrated"), 0, QKeySequence(Qt::Key_0));
    addRate(tr("Reject"), -1, QKeySequence(Qt::Key_X));

    auto* labelMenu = image->addMenu("Label");
    QList<QPair<QAction*, ColourLabel>> labelActions;

    struct LabelKey {
        const char* name;
        ColourLabel value;
        Qt::Key key;
    };

    for (auto [name, value, key] :
         {LabelKey{"Red", ColourLabel::Red, Qt::Key_R},
          {"Yellow", ColourLabel::Yellow, Qt::Key_Y},
          {"Green", ColourLabel::Green, Qt::Key_G},
          {"Blue", ColourLabel::Blue, Qt::Key_B},
          {"Purple", ColourLabel::Purple, Qt::Key_P}}) {
        QAction* a = labelMenu->addAction(tr(name), this, [this, value] {
            setCurrentLabel(value);
        });
        a->setShortcut(key);
        a->setCheckable(true);
        labelActions.append({a, value});
    }
    labelMenu->addSeparator();
    // labelCurrent(None) always clears (toggling None off is still None).
    labelMenu->addAction(tr("None"), this, [this] { setCurrentLabel(ColourLabel::None); });

    // Reflect the current file's marks each time the menu opens.
    connect(image, &QMenu::aboutToShow, this, [this, rateActions, labelActions] {
        const UserMetadata m = session->hasImage() ? session->userMetadata() : filmStrip->currentMarks();
        for (const auto& [a, n] : rateActions)
            a->setChecked(m.rating == n);
        for (const auto& [a, value] : labelActions)
            a->setChecked(m.label == value);
    });
}

void MainWindow::setupStatusBar() {
    statusLabel = new QLabel("No image loaded", this);
    statusBar()->addWidget(statusLabel, 1);

    proofLabel = new QLabel(this);
    proofLabel->setVisible(false);
    statusBar()->addPermanentWidget(proofLabel);

    zoomButton = new QToolButton(this);
    zoomButton->setPopupMode(QToolButton::InstantPopup);
    zoomButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    zoomButton->setAutoRaise(true);

    auto* zoomMenu = new QMenu(zoomButton);
    zoomMenu->addAction("Fit", viewport, &ImageViewport::resetView);
    zoomMenu->addSeparator();
    zoomMenu->addAction("50 %", this, [this] { viewport->setPixelZoom(0.5f); });
    zoomMenu->addAction("100 %", this, [this] { viewport->setPixelZoom(1.0f); });
    zoomMenu->addAction("200 %", this, [this] { viewport->setPixelZoom(2.0f); });
    zoomButton->setMenu(zoomMenu);

    statusBar()->addPermanentWidget(zoomButton);
    updateZoomStatus(viewport->pixelZoom());
}

void MainWindow::setupToolbar() {
    auto* tb = new QToolBar("Tools", this);
    tb->setObjectName("ToolsToolBar");
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    addToolBar(Qt::TopToolBarArea, tb);

    // Modal tools (left): mutually exclusive, but all may be off — clicking the
    // active tool again deselects it. The viewport owns the actual tool state.
    toolGroup = new QActionGroup(this);
    toolGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);

    auto addTool = [&](const QString& text, const QKeySequence& sc) {
        QAction* a = tb->addAction(text);
        a->setCheckable(true);
        a->setActionGroup(toolGroup);
        if (!sc.isEmpty())
            a->setShortcut(sc);
        return a;
    };
    cropAction = addTool("Crop", Qt::Key_C);
    straightenAction = addTool("Straighten", {});
    wbAction = addTool("White Bal.", {});
    maskAction = addTool("Masks", Qt::Key_M);
    spotAction = addTool("Spots", Qt::Key_Q);

    connect(toolGroup, &QActionGroup::triggered, this, [this](QAction* a) {
        using T = ImageViewport::ActiveTool;
        T t = T::None;
        if (a->isChecked())
            t = a == cropAction         ? T::Crop
                : a == straightenAction ? T::Straighten
                : a == maskAction       ? T::LocalMask
                : a == spotAction       ? T::SpotTool
                                        : T::WhiteBalance;
        viewport->setActiveTool(t);
    });

    setupAspectMenu(tb);

    // Spacer pushes the action group to the right edge.
    auto* spacer = new QWidget(tb);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);

    // Immediate actions (right): reuse the existing slots; Open stays usable
    // with no image loaded, the rest are image-dependent.
    tb->addAction("Open", this, &MainWindow::openFile);
    saveAction = tb->addAction("Save", this, &MainWindow::saveAdjustments);
    exportAction = tb->addAction("Export", this, &MainWindow::exportFile);

    setToolsEnabled(false);
}

void MainWindow::setupAspectMenu(QToolBar* tb) {
    aspectButton = new QToolButton(tb);
    aspectButton->setText("Aspect");
    aspectButton->setPopupMode(QToolButton::InstantPopup);
    aspectButton->setEnabled(false); // only meaningful while cropping

    auto* menu = new QMenu(aspectButton);
    aspectGroup = new QActionGroup(menu);
    // Optional exclusion: a restored custom ratio (no named preset) leaves every
    // item unchecked while the lock is still active.
    aspectGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);

    struct PresetItem {
        const char* label;
        crop::AspectPreset preset;
    };

    const PresetItem items[] = {
        {"Free", crop::AspectPreset::Free},
        {"Original", crop::AspectPreset::Original},
        {"1:1", crop::AspectPreset::Square},
        {"2:3", crop::AspectPreset::R2x3},
        {"3:4", crop::AspectPreset::R3x4},
        {"4:5", crop::AspectPreset::R4x5},
        {"16:9", crop::AspectPreset::R16x9},
    };
    for (const PresetItem& item : items) {
        QAction* a = menu->addAction(item.label);
        a->setCheckable(true);
        a->setActionGroup(aspectGroup);
        a->setChecked(item.preset == crop::AspectPreset::Free);
        a->setData(int(item.preset)); // looked up by syncToolActions to reflect a restored lock
        const crop::AspectPreset preset = item.preset;
        connect(a, &QAction::triggered, this, [this, preset] {
            aspectPreset = preset;
            applyAspectLock();
        });
    }

    menu->addSeparator();
    orientationAction = menu->addAction("Flip Orientation");
    orientationAction->setShortcut(Qt::Key_X);
    connect(orientationAction, &QAction::triggered, this, [this] {
        aspectLandscape = !aspectLandscape;
        applyAspectLock();
    });

    aspectButton->setMenu(menu);
    tb->addWidget(aspectButton);
}

void MainWindow::applyAspectLock() {
    viewport->setAspectLock(aspectPreset, aspectLandscape);
}

void MainWindow::syncToolActions() {
    const ImageViewport::ActiveTool t = viewport->activeTool();
    // setChecked doesn't emit QActionGroup::triggered, but block toggled too.
    const QSignalBlocker b1(cropAction), b2(straightenAction), b3(wbAction), b4(spotAction);
    const bool cropOn = t == ImageViewport::ActiveTool::Crop;
    cropAction->setChecked(cropOn);
    straightenAction->setChecked(t == ImageViewport::ActiveTool::Straighten);
    wbAction->setChecked(t == ImageViewport::ActiveTool::WhiteBalance);
    maskAction->setChecked(t == ImageViewport::ActiveTool::LocalMask);
    spotAction->setChecked(t == ImageViewport::ActiveTool::SpotTool);

    // Raise the Masks tab while the LocalMask tool is active.
    if (t == ImageViewport::ActiveTool::LocalMask && rightTabs && masksTabIndex >= 0)
        rightTabs->setCurrentIndex(masksTabIndex);

    // The aspect lock only applies while cropping. Reflect whatever the viewport
    // restored from the persisted crop: check the matching preset, or uncheck all
    // for a custom (unnamed) ratio while the lock still holds.
    aspectButton->setEnabled(cropOn);
    if (cropOn) {
        const crop::PresetMatch m = viewport->currentLockMatch();
        aspectPreset = m.preset;
        aspectLandscape = m.landscape;
        for (QAction* a : aspectGroup->actions())
            a->setChecked(m.matched && a->data().toInt() == int(m.preset));
    }
}

void MainWindow::setToolsEnabled(bool on) {
    cropAction->setEnabled(on);
    straightenAction->setEnabled(on);
    wbAction->setEnabled(on);
    maskAction->setEnabled(on);
    spotAction->setEnabled(on);
    saveAction->setEnabled(on);
    exportAction->setEnabled(on);
}

void MainWindow::setupDocks() {
    // Film strip (bottom): a horizontal thumbnail strip under the viewport.
    filmStripDock = new QDockWidget("Film Strip", this);
    // New object name so a window state saved with the old left-side dock
    // doesn't restore the strip to the side.
    filmStripDock->setObjectName("FilmStripDockBottom");
    filmStripDock->setAllowedAreas(Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);
    // Closable so toggleViewAction() (View → Film Strip, F9) is enabled; the
    // custom title bar below replaces the default one, so no close button shows.
    filmStripDock->setFeatures(
        QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable
        | QDockWidget::DockWidgetClosable);

    filmStrip = new FilmStrip(filmStripDock);
    filmStrip->setMinimumHeight(80);
    filmStripDock->setWidget(filmStrip);
    addDockWidget(Qt::BottomDockWidgetArea, filmStripDock);
    resizeDocks({filmStripDock}, {132}, Qt::Vertical); // sensible initial height

    // Custom title bar: folder icon + current path, instead of "Film Strip".
    auto* stripTitle = new QWidget(filmStripDock);
    auto* stripTitleLayout = new QHBoxLayout(stripTitle);
    stripTitleLayout->setContentsMargins(6, 2, 6, 2);
    stripTitleLayout->setSpacing(6);

    auto* folderButton = new QToolButton(stripTitle);
    folderButton->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    folderButton->setAutoRaise(true);
    folderButton->setToolTip("Open folder…");
    connect(folderButton, &QToolButton::clicked, filmStrip, &FilmStrip::promptForDirectory);
    stripTitleLayout->addWidget(folderButton);

    auto* pathLabel = new QLabel("No folder", stripTitle);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    stripTitleLayout->addWidget(pathLabel, 1);
    filmStripDock->setTitleBarWidget(stripTitle);

    connect(filmStrip, &FilmStrip::directoryChanged, this, [pathLabel](const QString& dir) {
        const QString native = QDir::toNativeSeparators(dir);
        pathLabel->setText(native);
        pathLabel->setToolTip(native);
    });

    auto* toggleFilmStrip = filmStripDock->toggleViewAction();
    toggleFilmStrip->setText("Film Strip");
    toggleFilmStrip->setShortcut(Qt::Key_F9);

    connect(filmStrip, &FilmStrip::fileSelected, this, &MainWindow::loadImage);

    // Adjustments + EXIF (right). Collapses to a thin edge strip (ADR 0012).
    auto* rightDock = adjustmentsDock = new QDockWidget("Adjustments", this);
    rightDock->setObjectName("AdjustmentsDock"); // saveState/restoreState key
    rightDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    rightDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    // Custom title bar carrying a collapse chevron (mirrors the film strip's).
    auto* adjTitle = new QWidget(rightDock);
    auto* adjTitleLayout = new QHBoxLayout(adjTitle);
    adjTitleLayout->setContentsMargins(6, 2, 4, 2);
    adjTitleLayout->addWidget(new QLabel("Adjustments", adjTitle), 1);
    auto* collapseBtn = new QToolButton(adjTitle);
    collapseBtn->setAutoRaise(true);
    collapseBtn->setText("›"); // toward the edge: click to collapse
    collapseBtn->setToolTip("Collapse panel (F8)");
    adjTitleLayout->addWidget(collapseBtn);
    rightDock->setTitleBarWidget(adjTitle);

    auto* tabs = new QTabWidget(rightDock);
    tabs->setMinimumWidth(120); // let the panel follow its content; don't pin it wide

    auto* adjScroll = new QScrollArea(tabs);
    adjPanel = new AdjustmentPanel;
    proofPanel = new ProofingPanel;
    auto* adjColumn = new QWidget(adjScroll);
    auto* adjLayout = new QVBoxLayout(adjColumn);
    adjLayout->setContentsMargins(0, 0, 0, 0);
    adjLayout->addWidget(adjPanel);
    adjLayout->addWidget(proofPanel);
    adjScroll->setWidget(adjColumn);
    adjScroll->setWidgetResizable(true);
    adjScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tabs->addTab(adjScroll, "Adjustments");

    localPanel = new LocalAdjustmentPanel(tabs);
    auto* localScroll = new QScrollArea(tabs);
    localScroll->setWidget(localPanel);
    localScroll->setWidgetResizable(true);
    localScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    masksTabIndex = tabs->addTab(localScroll, "Masks");

    spotPanel = new SpotRemovalPanel(tabs);
    auto* spotScroll = new QScrollArea(tabs);
    spotScroll->setWidget(spotPanel);
    spotScroll->setWidgetResizable(true);
    spotScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tabs->addTab(spotScroll, "Spots");

    exifPanel = new ExifPanel(tabs);
    tabs->addTab(exifPanel, "EXIF");

    rightTabs = tabs;
    rightDock->setWidget(tabs);
    addDockWidget(Qt::RightDockWidgetArea, rightDock);

    // Reveal strip: a vertical toolbar pinned to the right edge, visible only
    // while the dock is collapsed; its chevron re-opens the panel (ADR 0012).
    auto* strip = new QToolBar("Adjustments Strip", this);
    strip->setObjectName("AdjustmentsStrip"); // saveState/restoreState key
    strip->setMovable(false);
    strip->setFloatable(false);
    auto* expandAction = strip->addAction("‹"); // toward the centre: click to expand
    expandAction->setToolTip("Expand panel (F8)");
    addToolBar(Qt::RightToolBarArea, strip);

    // CollapsiblePane owns the dock↔strip mutual-exclusion invariant; it starts
    // expanded (dock shown, strip hidden). A restored collapsed state is synced
    // back onto it after restoreState() in the constructor.
    adjustmentsPane = std::make_unique<CollapsiblePane>(rightDock, strip);
    connect(collapseBtn, &QToolButton::clicked, this, [this] { adjustmentsPane->collapse(); });
    connect(expandAction, &QAction::triggered, this, [this] { adjustmentsPane->expand(); });
    // adjPanel → viewport paramsChanged wired in constructor (after both are created)
}

MainWindow::~MainWindow() = default;

void MainWindow::openPath(const QString& path) {
    QFileInfo fi(path);
    if (!fi.exists())
        return;

    if (fi.isDir()) {
        filmStrip->setDirectory(fi.absoluteFilePath());
        filmStrip->selectFirst();
    } else {
        loadImage(fi.absoluteFilePath());
    }
}

void MainWindow::openFile() {
    QSettings s;
    const QString startDir = s.value("lastDir", QDir::homePath()).toString();
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Open Image",
        startDir,
        "All Images (*.cr2 *.cr3 *.nef *.arw *.dng *.raf *.orf *.rw2 *.pef *.srw "
        "*.jpg *.jpeg *.png *.tiff *.tif *.webp *.bmp);;"
        "RAW Images (*.cr2 *.cr3 *.nef *.arw *.dng *.raf *.orf *.rw2 *.pef *.srw);;"
        "Standard Images (*.jpg *.jpeg *.png *.tiff *.tif *.webp *.bmp);;"
        "All Files (*)");
    if (!path.isEmpty())
        loadImage(path);
}

// Decode-cache key: path + size + mtime, so an externally modified file misses
// and re-decodes (mirrors ThumbnailCache's keying).
static QString decodeKey(const QString& path) {
    const QFileInfo fi(path);
    return fi.canonicalFilePath() + '|' + QString::number(fi.size()) + '|'
           + QString::number(fi.lastModified().toMSecsSinceEpoch());
}

void MainWindow::loadImage(const QString& path) {
    // Cancel any in-progress load so it stops before dcraw_process.
    if (loadCancel)
        loadCancel->store(true);
    loadCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = loadCancel;

    session->beginLoading(path);
    exifPanel->clear();
    viewport->cancelActiveTool(); // discard any in-progress tool from the last image
    viewport->setOriginalImageSize(0, 0);
    setLoadingState(true);

    // Populate the strip from the file's directory
    filmStrip->setDirectory(QFileInfo(path).absolutePath());
    filmStrip->setCurrentFile(path);

    // Resolve the new image's develop params up front, so its first paint wears
    // its own edits, not the previous image's. Crop is a placeholder (full frame)
    // until the demosaic yields the real DefaultCrop for never-edited RAWs.
    pendingParams = XmpSidecar::resolveAdjustments(path, QRectF(0.0, 0.0, 1.0, 1.0));

    // Re-opening a recently viewed image: a cached decode skips the background
    // task entirely — instant, and correct (straight to the demosaiced image).
    const QString key = decodeKey(path);
    if (const LoadResult* hit = decodeCache.get(key)) {
        decodeCache.pin(key);
        applyLoadResult(path, *hit);
        return;
    }

    // Cache miss: keep the previous image on screen until the new image's embedded
    // preview (or full demosaic) is ready, then swap pixels + params atomically.
    loadWatcher.setFuture(QtConcurrent::run([this, path, cancel]() -> LoadResult {
        auto onPreview = [this, path, cancel](ImageBuffer buf) {
            if (cancel->load())
                return;
            QMetaObject::invokeMethod(
                this,
                [this, path, buf = std::move(buf)]() mutable {
                    if (session->path() == path) {
                        applyPendingParams();    // new image's params, before the paint
                        viewport->setImage(buf); // embedded preview (camera look, base off)
                    }
                },
                Qt::QueuedConnection);
        };
        if (StandardImageLoader::canLoad(path))
            return StandardImageLoader::load(path, cancel);
        return RawProcessor::load(path, std::move(onPreview), cancel);
    }));
}

void MainWindow::onLoadFinished() {
    LoadResult result = loadWatcher.result();

    // Empty result means the task was cancelled — another load is already running.
    if (!result.fullRes.valid() && result.error.isEmpty())
        return;

    if (!result.error.isEmpty()) {
        setLoadingState(false);
        QMessageBox::critical(this, "Load Error", result.error);
        statusLabel->setText("Load failed.");
        exifPanel->clear();
        return;
    }

    // Cache the decode (authoritative copy) and pin it as the current image, then
    // display it through the same path a cache hit takes.
    const QString key = decodeKey(session->path());
    decodeCache.insert(key, std::move(result));
    decodeCache.pin(key);
    if (const LoadResult* cached = decodeCache.get(key))
        applyLoadResult(session->path(), *cached);
}

void MainWindow::applyPendingParams() {
    session->setParams(pendingParams);
    syncSessionToEditors();
    {
        QSignalBlocker block(spotPanel);
        spotPanel->setSpots(session->params().spots);
    }
    viewport->setSpots(session->params().spots);
}

void MainWindow::applyLoadResult(const QString& path, const LoadResult& result) {
    setLoadingState(false);
    viewport->setOriginalImageSize(result.fullRes.width, result.fullRes.height);

    // Re-read the sidecar every time (params are never cached) so edits made in
    // another app — or a prior session — are always reflected.
    const SidecarLoadResult sidecar = XmpSidecar::resolveForImage(path, result.defaultCrop);
    session->setLoadedImage(
        path,
        result,
        sidecar.data.adjustments,
        toSessionSidecarState(sidecar.status),
        sidecar.data.metadata);
    session->setBaseLook(true);
    syncSessionToEditors();
    syncSessionSpotsToEditors(true);
    filmStrip->setMarks(path, session->userMetadata());

    exifPanel->setMetadata(result.metadata);
    undoStack->clear();

    QString status = QString("%1  —  %2 × %3")
                         .arg(QFileInfo(path).fileName())
                         .arg(session->fullRes().width)
                         .arg(session->fullRes().height);
    if (session->sidecarState() == DevelopSession::SidecarState::ParseError)
        status += "  —  Sidecar unreadable; defaults applied";
    statusLabel->setText(status);

    setToolsEnabled(true);
}

void MainWindow::rebuildSpottedBuffers(bool fullResOnly) {
    if (session->previewForDisplay().valid())
        viewport->setImage(session->previewForDisplay(), session->baseLook());

    if (fullResOnly && session->fullResForExport().valid())
        viewport->setFullResImage(session->fullResForExport());
}

void MainWindow::onFullResNeeded() {
    if (!session->fullResForExport().valid())
        return;
    viewport->setFullResImage(session->fullResForExport());
}

void MainWindow::updateZoomStatus(float zoom) {
    zoomButton->setVisible(zoom > 0.0f);
    if (zoom <= 0.0f)
        return;
    zoomButton->setText(QString("%1 %").arg(qRound(zoom * 100.0f)));
}

void MainWindow::setLoadingState(bool loading) {
    menuBar()->setEnabled(!loading);
    adjPanel->setEnabled(!loading);
    exifPanel->setEnabled(!loading);
    if (loading)
        setToolsEnabled(false); // re-enabled in onLoadFinished on success
    statusLabel->setText(
        loading ? QString("Loading %1...").arg(QFileInfo(session->path()).fileName()) : QString());
}

void MainWindow::applyClipping() {
    const bool hi = clipHighlightsAction->isChecked();
    const bool sh = clipShadowsAction->isChecked();
    viewport->setClipWarnings(hi, sh);
    QSettings s;
    s.setValue("view/clipHighlights", hi);
    s.setValue("view/clipShadows", sh);
}

void MainWindow::toggleClipping() {
    // J: if either overlay is on, turn both off; otherwise turn both on.
    const bool anyOn = clipHighlightsAction->isChecked() || clipShadowsAction->isChecked();
    clipHighlightsAction->setChecked(!anyOn);
    clipShadowsAction->setChecked(!anyOn); // toggled() drives applyClipping()
}

void MainWindow::rebuildDisplayLut() {
    const bool proofing = proofPanel->proofingEnabled();
    if (!proofing && monitorProfilePath.isEmpty()) {
        viewport->clearDisplayLut();
        viewport->setGamutWarning(false);
        proofLabel->setVisible(false);
        return;
    }

    const DisplayLut lut = buildDisplayLut(
        proofing ? proofPanel->profilePath() : QString(),
        proofPanel->intent(),
        proofPanel->blackPointCompensation(),
        monitorProfilePath);
    if (!lut.valid()) {
        QMessageBox::warning(
            this,
            "Color Management",
            "Could not build the display transform — check the selected ICC profiles.");
        viewport->clearDisplayLut();
        viewport->setGamutWarning(false);
        proofLabel->setVisible(false);
        return;
    }

    viewport->setDisplayLut(lut);
    viewport->setGamutWarning(proofing && proofPanel->gamutWarning());
    proofLabel->setText(proofing ? "Proofing: " + proofPanel->profileName() : QString());
    proofLabel->setVisible(proofing);
}

void MainWindow::applyDevelopChange(const GlobalAdjustment& after) {
    const GlobalAdjustment before = currentParams();
    if (after != before)
        pushGlobalAdjustmentCommand(before, after);
}

void MainWindow::applyCurrentUserMetadata(const UserMetadata& metadata) {
    if (session->path().isEmpty())
        return;
    session->setUserMetadata(metadata);
    filmStrip->setMarks(session->path(), metadata);
    if (XmpSidecar::saveMetadata(session->path(), metadata)) {
        session->markMetadataSaved();
    } else {
        session->markMetadataSaveFailed();
        statusLabel->setText("Could not write " + XmpSidecar::pathFor(session->path()));
    }
}

void MainWindow::setCurrentRating(int rating) {
    UserMetadata metadata = session->userMetadata();
    metadata.rating = rating;
    applyCurrentUserMetadata(metadata);
}

void MainWindow::setCurrentLabel(ColourLabel label) {
    UserMetadata metadata = session->userMetadata();
    metadata.label
        = (label == ColourLabel::None || metadata.label != label) ? label : ColourLabel::None;
    applyCurrentUserMetadata(metadata);
}

void MainWindow::copySettings() {
    if (session->path().isEmpty())
        return;
    viewport->commitActiveTool(); // fold any pending crop into the params first

    GroupChecklistDialog dlg(tr("Copy Settings"), allGroups(), lastCopySelection, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const GroupSelection chosen = dlg.selectedGroups();
    if (chosen.none())
        return;

    lastCopySelection = chosen;
    settingsClipboard = SettingsClipboard{currentParams(), chosen};
}

void MainWindow::pasteSettings() {
    if (session->path().isEmpty() || !settingsClipboard)
        return;

    // Paste's checklist is bounded by what was copied (narrow only).
    GroupChecklistDialog
        dlg(tr("Paste Settings"), settingsClipboard->groups, settingsClipboard->groups, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const GroupSelection chosen = dlg.selectedGroups();
    const QStringList targets = filmStrip->selectedPaths();

    if (targets.size() <= 1) {
        // Single file: apply to the active image in memory only.
        applyDevelopChange(applyGroups(currentParams(), settingsClipboard->snapshot, chosen));
        return;
    }

    // Multi-file: read before-state from each file's sidecar (or from memory
    // for the active file, which may have unsaved edits), then push one macro
    // undo step that auto-saves XMP for all targets (ADR 0018).
    QVector<BatchPasteRecord> records;
    records.reserve(targets.size());
    for (const QString& path : targets) {
        const GlobalAdjustment before
            = (path == session->path()) ? currentParams() : XmpSidecar::loadAdjustments(path);
        records.append({path, before, applyGroups(before, settingsClipboard->snapshot, chosen)});
    }
    undoStack->push(new BatchAdjustmentCommand(
        session->path(),
        records,
        [this](const GlobalAdjustment& params) {
            session->setParams(params);
            syncSessionToEditors();
            syncSessionSpotsToEditors(true);
        }));
}

void MainWindow::saveCurrentAsPreset() {
    if (session->path().isEmpty())
        return;
    viewport->commitActiveTool();

    GroupChecklistDialog dlg(tr("Save Preset"), allGroups(), lastCopySelection, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const GroupSelection chosen = dlg.selectedGroups();
    if (chosen.none())
        return;
    lastCopySelection = chosen;

    bool ok = false;
    const QString name
        = QInputDialog::getText(
              this, tr("Save Preset"), tr("Preset name:"), QLineEdit::Normal, QString(), &ok)
              .trimmed();
    if (!ok || name.isEmpty())
        return;

    DevelopPreset preset;
    preset.name = name;
    preset.groups = chosen;
    preset.values = currentParams();
    if (!presetStore.save(preset)) {
        QMessageBox::warning(this, tr("Save Preset"), tr("Could not write the preset file."));
        return;
    }
    rebuildPresetsMenu();
}

void MainWindow::applyPreset(const DevelopPreset& preset) {
    if (session->path().isEmpty())
        return;
    applyDevelopChange(applyGroups(currentParams(), preset.values, preset.groups));
}

void MainWindow::managePresets() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Manage Presets"));
    auto* layout = new QVBoxLayout(&dlg);

    auto* list = new QListWidget(&dlg);
    for (const DevelopPreset& p : presetStore.loadAll())
        list->addItem(p.name);
    layout->addWidget(list);

    auto* buttons = new QDialogButtonBox(&dlg);
    auto* deleteBtn = buttons->addButton(tr("Delete"), QDialogButtonBox::DestructiveRole);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    connect(deleteBtn, &QPushButton::clicked, &dlg, [this, list] {
        QListWidgetItem* item = list->currentItem();
        if (!item)
            return;
        presetStore.remove(item->text());
        delete list->takeItem(list->row(item));
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    dlg.exec();
    rebuildPresetsMenu();
}

void MainWindow::rebuildPresetsMenu() {
    presetsMenu->clear();
    presetsMenu->addAction(
        tr("&Save Current Settings as Preset..."), this, &MainWindow::saveCurrentAsPreset);
    QAction* manage
        = presetsMenu->addAction(tr("&Manage Presets..."), this, &MainWindow::managePresets);
    presetsMenu->addSeparator();

    const std::vector<DevelopPreset> presets = presetStore.loadAll();
    manage->setEnabled(!presets.empty());
    if (presets.empty()) {
        QAction* none = presetsMenu->addAction(tr("(No presets saved)"));
        none->setEnabled(false);
        return;
    }
    for (const DevelopPreset& p : presets)
        presetsMenu->addAction(p.name, this, [this, p] { applyPreset(p); });
}

GlobalAdjustment MainWindow::currentParams() const {
    return session->params();
}

void MainWindow::syncSessionToEditors() {
    {
        QSignalBlocker block(adjPanel);
        adjPanel->setParams(session->params());
    }
    {
        QSignalBlocker block(localPanel);
        localPanel->setLocalAdjustments(session->params().localAdjustments);
    }
    viewport->setAdjustments(session->params());
    if (thumbTimer)
        thumbTimer->start();
}

void MainWindow::syncSessionSpotsToEditors(bool fullResOnly) {
    {
        QSignalBlocker block(spotPanel);
        spotPanel->setSpots(session->params().spots);
    }
    viewport->setSpots(session->params().spots);
    rebuildSpottedBuffers(fullResOnly);
}

void MainWindow::pushGlobalAdjustmentCommand(
    const GlobalAdjustment& before,
    const GlobalAdjustment& after) {
    const GlobalAdjustment current = session->params();
    const GlobalAdjustment beforeSnapshot = applyGroups(current, before, allGroups());
    const GlobalAdjustment afterSnapshot = applyGroups(current, after, allGroups());
    if (afterSnapshot != beforeSnapshot)
        undoStack->push(new AdjustmentCommand(session, this, beforeSnapshot, afterSnapshot));
}

void MainWindow::pushParamsToViewport() {
    viewport->setAdjustments(session->params());
    if (thumbTimer)
        thumbTimer->start(); // refresh the develop thumbnail once edits settle
}

void MainWindow::generateDevelopedThumbnail() {
    // Don't snapshot a half-loaded image: the buffers/params may be mid-swap.
    if (loadWatcher.isRunning())
        return;
    if (session->path().isEmpty() || !session->previewForDisplay().valid()
        || !viewport->rendererReady())
        return;

    const GlobalAdjustment p = currentParams();
    const ImageBuffer& preview = session->previewForDisplay();
    const QSize sz = developedThumbSize(preview.width, preview.height, p.cropRect, 512);

    // Same pipeline as export: linear working-space render → output transform.
    QImage lin = viewport->renderToImage(preview, p, sz.width(), sz.height());
    if (lin.isNull())
        return;
    const QImage srgb = toOutputImage(lin, OutputProfile::SRgb, /*sixteenBit=*/false);

    ThumbnailCache::store(session->path(), srgb); // persist (overwrites the embedded one)
    filmStrip->setThumbnail(session->path(), srgb); // live strip update
}

void MainWindow::saveAdjustments() {
    if (session->path().isEmpty())
        return;
    viewport->commitActiveTool(); // fold any pending crop into the params first
    if (XmpSidecar::saveAdjustments(session->path(), currentParams())) {
        session->markDevelopSaved();
        statusLabel->setText("Saved: " + XmpSidecar::pathFor(session->path()));
    } else {
        session->markDevelopSaveFailed();
        QMessageBox::warning(
            this, "Save Error", "Could not write " + XmpSidecar::pathFor(session->path()));
    }
}

// Simple unsharp mask: radius ≈ 1% of image width, amount 0..100.
// The blur is approximated by a smooth downscale + upscale round-trip,
// which is much cheaper than a real Gaussian at these radii.
// Runs in encoded (output-profile) space — Format_RGB888 or Format_RGBA64.
static QImage applyUnsharpMask(QImage img, int amount) {
    if (amount == 0)
        return img;
    const float a = amount / 100.0f;
    int r = std::max(1, img.width() / 100);
    QImage blurred
        = img.scaled(
                 img.width() / (r + 1),
                 img.height() / (r + 1),
                 Qt::IgnoreAspectRatio,
                 Qt::SmoothTransformation)
              .scaled(img.width(), img.height(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (img.format() == QImage::Format_RGBA64) {
        for (int y = 0; y < img.height(); ++y) {
            const auto* src = reinterpret_cast<const quint16*>(img.constScanLine(y));
            const auto* blr = reinterpret_cast<const quint16*>(blurred.constScanLine(y));
            auto* dst = reinterpret_cast<quint16*>(img.scanLine(y));
            for (int x = 0; x < img.width() * 4; ++x) {
                if ((x & 3) == 3)
                    continue; // leave alpha alone
                int v = int(src[x]) + int(a * (int(src[x]) - int(blr[x])));
                dst[x] = quint16(std::clamp(v, 0, 65535));
            }
        }
        return img;
    }
    for (int y = 0; y < img.height(); ++y) {
        const uchar* src = img.constScanLine(y);
        const uchar* blr = blurred.constScanLine(y);
        uchar* dst = img.scanLine(y);
        for (int x = 0; x < img.width() * 3; ++x) {
            int v = int(src[x]) + int(a * (int(src[x]) - int(blr[x])));
            dst[x] = uchar(std::clamp(v, 0, 255));
        }
    }
    return img;
}

void MainWindow::exportFile() {
    if (!session->fullRes().valid()) {
        QMessageBox::information(this, "Export", "No image loaded.");
        return;
    }

    const QStringList targets = filmStrip->selectedPaths();
    if (targets.size() > 1) {
        exportBatch(targets);
        return;
    }

    viewport->commitActiveTool(); // fold any pending crop into the params first
    const GlobalAdjustment p = currentParams();

    // Natural output size = full-res pixels inside the crop rect (shared with
    // the crop overlay's live readout so the two can never disagree).
    const QSize natural
        = crop::cropPixelSize(session->fullRes().width, session->fullRes().height, p.cropRect);

    ExportDialog optDlg(natural.width(), natural.height(), this);
    if (optDlg.exec() != QDialog::Accepted)
        return;

    const ExportOptions opts = optDlg.options();

    QString suffix, filter;
    switch (opts.format) {
    case ExportOptions::Format::JPEG:
        suffix = "jpg";
        filter = "JPEG (*.jpg *.jpeg)";
        break;
    case ExportOptions::Format::PNG:
        suffix = "png";
        filter = "PNG (*.png)";
        break;
    case ExportOptions::Format::TIFF:
        suffix = "tif";
        filter = "TIFF (*.tif *.tiff)";
        break;
    }

    QFileDialog fileDlg(this, "Export Image", QFileInfo(session->path()).absolutePath());
    fileDlg.setAcceptMode(QFileDialog::AcceptSave);
    fileDlg.setNameFilter(filter);
    fileDlg.setDefaultSuffix(suffix);
    if (fileDlg.exec() != QDialog::Accepted)
        return;

    QString path = fileDlg.selectedFiles().constFirst();
    if (QFileInfo(path).suffix().isEmpty())
        path += "." + suffix;

    statusLabel->setText("Exporting…");
    QApplication::processEvents(); // repaint the status bar before the render blocks the UI

    // Linear working-space render → output profile (lcms2) → sharpen → save.
    QImage out = viewport->renderToImage(session->fullResForExport(), p, opts.width, opts.height);
    out = toOutputImage(out, opts.profile, opts.bitDepth == 16);
    out = applyUnsharpMask(std::move(out), opts.sharpening);

    bool ok;
    if (opts.format == ExportOptions::Format::JPEG)
        ok = out.save(path, "JPEG", opts.quality);
    else
        ok = out.save(path);

    if (!ok)
        QMessageBox::critical(this, "Export Error", "Failed to save " + path);
    else
        statusLabel->setText("Exported: " + QFileInfo(path).fileName());
}

void MainWindow::exportBatch(const QStringList& paths) {
    viewport->commitActiveTool();
    const GlobalAdjustment activeParams = currentParams();
    const QSize natural = crop::cropPixelSize(
        session->fullRes().width, session->fullRes().height, activeParams.cropRect);

    ExportDialog optDlg(natural.width(), natural.height(), this);
    if (optDlg.exec() != QDialog::Accepted)
        return;
    const ExportOptions opts = optDlg.options();

    QString suffix;
    switch (opts.format) {
    case ExportOptions::Format::JPEG: suffix = "jpg"; break;
    case ExportOptions::Format::PNG:  suffix = "png"; break;
    case ExportOptions::Format::TIFF: suffix = "tif"; break;
    }

    const QString outputDir = QFileDialog::getExistingDirectory(
        this, tr("Export to Folder"), QFileInfo(session->path()).absolutePath());
    if (outputDir.isEmpty())
        return;

    BatchProgressDialog progress(paths.size(), this);
    progress.show();
    // A freshly shown top-level window is not painted within a single
    // processEvents() pass: the map/expose handshake with the window manager is
    // asynchronous. Spin the loop until the window is actually on screen so it is
    // visible before any work begins (the per-file decode waits in its own nested
    // event loop below, which also paints, but the active image renders straight
    // away with no such wait). Bounded so a missing compositor can't hang us.
    if (QWindow* handle = progress.windowHandle()) {
        QElapsedTimer t;
        t.start();
        while (!handle->isExposed() && t.elapsed() < 1000)
            QApplication::processEvents(
                QEventLoop::WaitForMoreEvents | QEventLoop::ExcludeUserInputEvents, 20);
    }

    int exported = 0;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    for (int i = 0; i < paths.size(); ++i) {
        if (progress.wasCancelled())
            break;

        const QString& rawPath = paths[i];
        progress.setCurrentFile(QFileInfo(rawPath).fileName());
        progress.setValue(i);
        QApplication::processEvents();

        // Decode the buffer. The active image is already in memory; everything
        // else is decoded on a worker thread while a nested event loop keeps the
        // window servicing events — so it stays responsive (no "not responding"
        // from the window manager) while the modal dialog blocks user input.
        // The GPU render + encode below are short and must stay on the GUI thread
        // (renderToImage needs the RHI context).
        LoadResult loaded;
        if (rawPath == session->path()) {
            loaded.fullRes = session->fullRes();
        } else {
            QFutureWatcher<LoadResult> watcher;
            QEventLoop wait;
            connect(&watcher, &QFutureWatcher<LoadResult>::finished, &wait, &QEventLoop::quit);
            connect(&progress, &BatchProgressDialog::cancelRequested, &wait, [&] {
                cancel->store(true); // abort the in-flight decode
                wait.quit();
            });
            watcher.setFuture(QtConcurrent::run([rawPath, cancel]() -> LoadResult {
                return StandardImageLoader::canLoad(rawPath)
                    ? StandardImageLoader::load(rawPath, cancel)
                    : RawProcessor::load(rawPath, nullptr, cancel);
            }));
            wait.exec();
            if (progress.wasCancelled())
                break; // leave the future to finish and be discarded (cancel is set)
            loaded = watcher.result();
            if (!loaded.error.isEmpty())
                continue;
        }

        const GlobalAdjustment p = (rawPath == session->path())
            ? activeParams
            : XmpSidecar::resolveAdjustments(rawPath, QRectF(0, 0, 1, 1));

        // Apply spots stored in the adjustment (ADR 0017).
        const ImageBuffer renderBuf = p.spots.empty()
            ? loaded.fullRes
            : applySpots(loaded.fullRes, p.spots);

        // Compute output size: use per-file natural size when opts is zero.
        const QSize perFileNatural = crop::cropPixelSize(
            loaded.fullRes.width, loaded.fullRes.height, p.cropRect);
        const int outW = opts.width  > 0 ? opts.width  : perFileNatural.width();
        const int outH = opts.height > 0 ? opts.height : perFileNatural.height();

        QImage out = viewport->renderToImage(renderBuf, p, outW, outH);
        out = toOutputImage(out, opts.profile, opts.bitDepth == 16);
        out = applyUnsharpMask(std::move(out), opts.sharpening);

        const QString outPath = outputDir + "/"
            + QFileInfo(rawPath).completeBaseName() + "." + suffix;
        const bool ok = opts.format == ExportOptions::Format::JPEG
            ? out.save(outPath, "JPEG", opts.quality)
            : out.save(outPath);

        if (ok)
            ++exported;
    }

    progress.close();
    statusLabel->setText(
        tr("Exported %1 of %2 file(s)").arg(exported).arg(paths.size()));
}
