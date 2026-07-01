#pragma once
#include "BatchPaste.h"
#include "ChromeHider.h"
#include "DecodeCache.h"
#include "MainWindowZoom.h"
#include "core/CropGeometry.h"
#include "develop/GlobalAdjustment.h"
#include "develop/SettingsClipboard.h"
#include "develop/Spot.h"
#include "develop/UserMetadata.h"
#include "io/PresetStore.h"
#include "pipeline/LoadResult.h"
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <QFutureWatcher>
#include <QMainWindow>

class ImageViewport;
class AdjustmentPanel;
class LocalAdjustmentPanel;
class SpotRemovalPanel;
class ProofingPanel;
class InfoPanel;
class HistoryPanel;
class FilmStrip;
class CollapsiblePane;
class QDockWidget;
class QUndoStack;
class QLabel;
class QToolButton;
class QHBoxLayout;
class QToolBar;
class QActionGroup;
class QAction;
class QTabWidget;
class QMenu;
class DevelopSession;

/**
 * Top-level Qt Widgets shell for the editor.
 *
 * MainWindow owns the visible widgets, menus, docks, undo stack, decode jobs,
 * and signal wiring. It is intentionally GUI glue: the current image state
 * lives in DevelopSession, while editor panels and ImageViewport mirror that
 * state and report user intent back through signals.
 */
class MainWindow : public QMainWindow {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(MainWindow)
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override; // out-of-line for unique_ptr<CollapsiblePane>

    // Called after show() with the command-line argument, if any.
    // Accepts a RAW file path or a directory.
    void openPath(const QString& path);

    // Apply all spots to the clean decoded buffers and push spotted textures to
    // the viewport. fullResOnly=false pushes only the preview (live drag);
    // fullResOnly=true also pushes the full-res export buffer (release / undo / load).
    // preserveView keeps the current zoom/pan instead of refitting — for an
    // in-place re-decode of the same image (docs/adr/0036). Public so SpotListCommand
    // can call it on undo/redo.
    void rebuildSpottedBuffers(bool fullResOnly = false, bool preserveView = false);

    // Mirror the canonical session params into editor widgets and the viewport.
    // Public so undo commands can update views after mutating the session.
    void syncSessionToEditors();
    void syncSessionSpotsToEditors(bool fullResOnly = true);

    // Re-decode the current image with the session's current demosaic algorithm,
    // swapping the decoded buffers in place while keeping the develop edit on
    // screen (docs/adr/0036). A cached algorithm (re-pick / undo / redo) is
    // instant; a never-tried one decodes asynchronously. Public so the develop
    // undo command can trigger it on undo/redo of a demosaic change.
    void redecodeForDemosaicChange();

protected:
    void closeEvent(QCloseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void changeEvent(QEvent* e) override; // tracks WM-driven fullscreen state (docs/adr/0028)

private slots:
    void openFile();
    void saveAdjustments();
    void exportFile();
    void exportBatch(const QStringList& paths);
    void onLoadFinished();
    void onRedecodeFinished();
    void onFullResNeeded();

    // Settings Propagation (Milestone 8). Copy/Paste use the session-only
    // settingsClipboard; presets persist via presetStore.
    void copySettings();
    void pasteSettings();
    void saveCurrentAsPreset();
    void managePresets();
    void showAboutDialog();

private:
    void setupMenus();
    void setupImageMenu();
    // Appends "Fit" then one action per kZoomPresets entry to `menu`, wired to
    // the viewport. If `group` is given, the preset actions (not Fit) are made
    // checkable and exclusive, for the caller to drive from
    // matchingZoomPresetIndex(). Shared by the View > Zoom submenu and the
    // status bar's zoom dropdown so the two can't diverge
    // (docs/superpowers/specs/2026-07-01-zoom-menu-design.md).
    struct ZoomMenuActions {
        QAction* fit;
        std::array<QAction*, kZoomPresets.size()> presets;
    };
    ZoomMenuActions addZoomPresetActions(QMenu* menu, QActionGroup* group = nullptr);
    // Enables/disables the Zoom submenu (no image => nothing to zoom, mirrors
    // zoomButton's visibility) and checks whichever preset matches `zoom`.
    void updateZoomMenuState(float zoom);
    void setupDocks();
    // Builds the rating/colour filter controls and appends them to the film-strip
    // title bar (ADR 0042). Wires each control to FilmStrip::setFilter.
    void addFilmStripFilterControls(QHBoxLayout* into);
    void setupStatusBar();
    void setupToolbar();
    void syncToolActions();       // reflect viewport->activeTool() in the buttons
    void syncAdjustmentTabTool(); // reflect the selected Masks/Spots tab in the viewport
    void selectAdjustmentTab(int index);
    void setToolsEnabled(bool on); // image-dependent toolbar items

    // Crop aspect-ratio menu (enabled only while the crop tool is active). The
    // chosen preset/orientation is pushed to the viewport as a transient lock.
    void setupAspectMenu(QToolBar* tb);
    void applyAspectLock(); // forward aspectPreset/aspectLandscape to the viewport
    void loadImage(const QString& path);
    bool confirmLeavingCurrentImage();
    bool saveDirtySidecar(bool forceDevelopSave = false);
    // Single place a finished decode (or a cache hit) becomes the displayed
    // image: stores buffers, re-reads the sidecar, restores exif. Used by both
    // onLoadFinished and the decode-cache hit path in loadImage.
    void applyLoadResult(const QString& path, const LoadResult& result);
    // Push the pending preview's resolved params to the panels + viewport, so the
    // first paint of a new image wears its own edits (not the previous image's).
    void applyPendingPreviewParams();
    void setLoadingState(bool loading);
    void updateZoomStatus(float zoom);

    // Apply a develop change to the global params as one undo step (the source
    // of truth for copy/paste and preset apply). No-op if nothing changed. An
    // explicit label names the History step (e.g. "Apply Preset Punchy"); when
    // empty the step is auto-named from what changed (developChangeLabel).
    void pushGlobalAdjustmentCommand(
        const GlobalAdjustment& before, const GlobalAdjustment& after, const QString& label = {});
    void applyDevelopChange(const GlobalAdjustment& after, const QString& label = {});
    void populateFilmStripContextMenu(const QString& path, const QStringList& targets, QMenu* menu);
    void copySettingsFromPath(const QString& path);
    void pasteSettingsToPaths(QStringList targets);
    void applyPresetToPaths(const DevelopPreset& preset, QStringList targets);
    // Commit a multi-file batch (paste / preset). Pushes one undo step onto the
    // session History only when the batch touches the open image; otherwise it
    // auto-saves the off-image XMP directly, keeping that per-image History clean.
    void commitBatchAdjustment(QVector<BatchPasteRecord> records, const QString& text);
    void exportPaths(const QStringList& paths);
    void applyPreset(const DevelopPreset& preset);
    void rebuildPresetsMenu(); // re-list saved presets after save/delete
    void applyCurrentUserMetadata(
        const UserMetadata& metadata, const UserMetadataPresence& changedFields = {});
    void setCurrentRating(int rating);
    void setCurrentLabel(ColourLabel label);
    GlobalAdjustment paramsForPath(const QString& path) const;
    bool isDefaultDevelopSettings(const QString& path, const GlobalAdjustment& params) const;

    // The full develop params for save/export. DevelopSession is canonical;
    // panels mirror/edit this state but do not answer "what is current?"
    GlobalAdjustment currentParams() const;
    // Feed the session params to the viewport after an edit.
    void pushParamsToViewport();

    // Re-render the current image's filmstrip thumbnail through the develop
    // pipeline. Called only for saved/resolved sidecar state.
    void generateDevelopedThumbnail();

    // Rebuild the viewport's display LUT from the current soft-proofing
    // settings and monitor profile (no LUT when both are off).
    void rebuildDisplayLut();

    // Push the clipping-overlay actions' state to the viewport and persist it
    // (docs/adr/0009); toggleClipping flips both at once for the J key.
    void applyClipping();
    void toggleClipping();
    void applySensorClipping();
    void toggleFullScreen();
    void exitFullScreen(); // leave fullscreen, restoring the prior maximized/normal state
    void toggleChrome();
    void restoreFocusModes();

    // Snapshot management (docs/adr/0038). addCurrentAsSnapshot prompts for a
    // name and captures the current develop state; restoreSnapshot replays one as
    // an undoable develop step; rename/delete edit the persisted list. Each saves
    // the sidecar immediately and refreshes the History panel.
    void addCurrentAsSnapshot();
    void restoreSnapshot(int index);
    void renameSnapshot(int index, const QString& name);
    void deleteSnapshot(int index);
    void saveSnapshotsNow();

    ImageViewport* viewport;
    AdjustmentPanel* adjPanel;
    LocalAdjustmentPanel* localPanel;
    SpotRemovalPanel* spotPanel;
    ProofingPanel* proofPanel;
    InfoPanel* infoPanel;
    HistoryPanel* historyPanel;
    FilmStrip* filmStrip;
    QDockWidget* filmStripDock;
    QDockWidget* historyDock;     // left; History list + Snapshots (docs/adr/0038)
    QDockWidget* adjustmentsDock; // right; Adjustments/Masks/Spots/Info
    std::unique_ptr<CollapsiblePane> historyPane;     // historyDock ↔ edge strip
    std::unique_ptr<CollapsiblePane> adjustmentsPane; // adjustmentsDock ↔ edge strip
    QToolBar* mainToolBar = nullptr;
    QUndoStack* undoStack;
    QLabel* statusLabel;
    QLabel* proofLabel;
    QToolButton* zoomButton;
    QMenu* zoomMenu = nullptr;                                     // View → Zoom submenu
    QActionGroup* zoomPresetGroup = nullptr;                       // exclusive preset actions in zoomMenu
    std::array<QAction*, kZoomPresets.size()> zoomPresetActions{}; // kZoomPresets order

    // Toolbar: modal tools (left) + immediate actions (right).
    QActionGroup* toolGroup;
    QAction* cropAction;
    QAction* straightenAction;
    QAction* wbAction;
    QAction* masksTabShortcut;
    QAction* spotsTabShortcut;
    QTabWidget* rightTabs = nullptr; // Adjustments / Masks / Spots / Info
    int masksTabIndex = -1;
    int spotsTabIndex = -1;
    bool toolsEnabled = false;

    // Crop aspect-ratio lock UI + its transient state (mirrors the viewport's).
    QToolButton* aspectButton = nullptr;
    QActionGroup* aspectGroup = nullptr; // exclusive preset actions; first is Free
    QAction* orientationAction = nullptr;
    crop::AspectPreset aspectPreset = crop::AspectPreset::Free;
    bool aspectLandscape = true;
    QAction* saveAction;
    QAction* exportAction;
    QAction* clipHighlightsAction; // View → Show Highlight Clipping
    QAction* clipShadowsAction;    // View → Show Shadow Clipping
    QAction* sensorClipAction;     // View → Show Sensor Clipping
    QAction* fullScreenAction = nullptr;
    QAction* lightsOutAction = nullptr;
    std::optional<ChromeHider> chromeHider;
    bool wasMaximized = false;

    // Settings Propagation state (Milestone 8).
    std::optional<SettingsClipboard> settingsClipboard; // session-only, never the OS clipboard
    GroupSelection lastCopySelection = defaultCopySelection(); // Geometry off by default
    PresetStore presetStore;
    QMenu* presetsMenu = nullptr;

    QString monitorProfilePath; // empty = assume sRGB

    DevelopSession* session;

    // Params of the image currently being loaded, resolved up front from its
    // sidecar and applied atomically with the first paint of that image.
    GlobalAdjustment pendingPreviewParams;

    // Session decode cache: skips the multi-second demosaic when re-opening a
    // recently viewed image. ~1.5 GiB of decoded buffers, LRU, current pinned.
    static constexpr size_t kDecodeCacheBudget = size_t(1536) * 1024 * 1024;
    DecodeCache decodeCache{kDecodeCacheBudget};

    std::shared_ptr<std::atomic<bool>> loadCancel;
    QFutureWatcher<LoadResult> loadWatcher;
    // A demosaic re-decode runs on its own watcher (its finish swaps buffers in
    // place rather than going through applyLoadResult, which would reset the edit
    // and the undo stack). Cache key it is decoding, for the finish handler.
    QFutureWatcher<LoadResult> redecodeWatcher;
    std::shared_ptr<std::atomic<bool>> redecodeCancel;
    QString redecodeKey;
    bool leaveConfirmationSatisfied = false;
};
