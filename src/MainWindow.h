#pragma once
#include "CropGeometry.h"
#include "DecodeCache.h"
#include "ImagePipeline.h"
#include "PresetStore.h"
#include "SettingsClipboard.h"
#include <atomic>
#include <memory>
#include <optional>
#include <QFutureWatcher>
#include <QMainWindow>

class ImageViewport;
class AdjustmentPanel;
class LocalAdjustmentPanel;
class ProofingPanel;
class ExifPanel;
class FilmStrip;
class CollapsiblePane;
class QDockWidget;
class QUndoStack;
class QLabel;
class QToolButton;
class QToolBar;
class QActionGroup;
class QAction;
class QTabWidget;
class QTimer;
class QMenu;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override; // out-of-line for unique_ptr<CollapsiblePane>

    // Called after show() with the command-line argument, if any.
    // Accepts a RAW file path or a directory.
    void openPath(const QString& path);

protected:
    void closeEvent(QCloseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private slots:
    void openFile();
    void saveAdjustments();
    void exportFile();
    void onLoadFinished();
    void onFullResNeeded();

    // Settings Propagation (Milestone 8). Copy/Paste use the session-only
    // settingsClipboard; presets persist via presetStore.
    void copySettings();
    void pasteSettings();
    void saveCurrentAsPreset();
    void managePresets();

private:
    void setupMenus();
    void setupImageMenu();
    void setupDocks();
    void setupStatusBar();
    void setupToolbar();
    void syncToolActions();        // reflect viewport->activeTool() in the buttons
    void setToolsEnabled(bool on); // image-dependent toolbar items

    // Crop aspect-ratio menu (enabled only while the crop tool is active). The
    // chosen preset/orientation is pushed to the viewport as a transient lock.
    void setupAspectMenu(QToolBar* tb);
    void applyAspectLock(); // forward aspectPreset/aspectLandscape to the viewport
    void loadImage(const QString& path);
    // Single place a finished decode (or a cache hit) becomes the displayed
    // image: stores buffers, re-reads the sidecar, restores exif. Used by both
    // onLoadFinished and the decode-cache hit path in loadImage.
    void applyLoadResult(const QString& path, const LoadResult& result);
    // Push the pending image's resolved params to the panels + viewport, so the
    // first paint of a new image wears its own edits (not the previous image's).
    void applyPendingParams();
    void setLoadingState(bool loading);
    void updateZoomStatus(float zoom);

    // Apply a develop change to the global params as one undo step (the source
    // of truth for copy/paste and preset apply). No-op if nothing changed.
    void applyDevelopChange(const GlobalAdjustment& after);
    void applyPreset(const DevelopPreset& preset);
    void rebuildPresetsMenu(); // re-list saved presets after save/delete

    // The full develop params = global edits (adjPanel) + local adjustments
    // (localPanel) merged into one GlobalAdjustment for render, save, export.
    GlobalAdjustment currentParams() const;
    // Feed currentParams() to the viewport (after a global or local change).
    void pushParamsToViewport();

    // Re-render the current image's filmstrip thumbnail through the develop
    // pipeline (debounced after edits), so the strip reflects the edits.
    void generateDevelopedThumbnail();

    // Rebuild the viewport's display LUT from the current soft-proofing
    // settings and monitor profile (no LUT when both are off).
    void rebuildDisplayLut();

    // Push the clipping-overlay actions' state to the viewport and persist it
    // (docs/adr/0009); toggleClipping flips both at once for the J key.
    void applyClipping();
    void toggleClipping();

    ImageViewport* viewport;
    AdjustmentPanel* adjPanel;
    LocalAdjustmentPanel* localPanel;
    ProofingPanel* proofPanel;
    ExifPanel* exifPanel;
    FilmStrip* filmStrip;
    QDockWidget* filmStripDock;
    QDockWidget* adjustmentsDock;                     // right; collapses to a strip
    std::unique_ptr<CollapsiblePane> adjustmentsPane; // adjustmentsDock ↔ edge strip
    QUndoStack* undoStack;
    QLabel* statusLabel;
    QLabel* proofLabel;
    QToolButton* zoomButton;

    // Toolbar: modal tools (left) + immediate actions (right).
    QActionGroup* toolGroup;
    QAction* cropAction;
    QAction* straightenAction;
    QAction* wbAction;
    QAction* maskAction;             // LocalMask tool toggle
    QTabWidget* rightTabs = nullptr; // Adjustments / Masks / EXIF
    int masksTabIndex = -1;

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

    // Settings Propagation state (Milestone 8).
    std::optional<SettingsClipboard> settingsClipboard; // session-only, never the OS clipboard
    GroupSelection lastCopySelection = allGroups();     // remembers the last checklist
    PresetStore presetStore;
    QMenu* presetsMenu = nullptr;

    QString monitorProfilePath; // empty = assume sRGB

    ImageBuffer fullRes;
    ImageBuffer preview;
    QString currentPath;

    // Params of the image currently being loaded, resolved up front from its
    // sidecar and applied atomically with the first paint of that image.
    GlobalAdjustment pendingParams;

    // Session decode cache: skips the multi-second demosaic when re-opening a
    // recently viewed image. ~1.5 GiB of decoded buffers, LRU, current pinned.
    static constexpr size_t kDecodeCacheBudget = size_t(1536) * 1024 * 1024;
    DecodeCache decodeCache{kDecodeCacheBudget};

    std::shared_ptr<std::atomic<bool>> loadCancel;
    QFutureWatcher<LoadResult> loadWatcher;

    // Debounces develop-thumbnail regeneration after edits settle.
    QTimer* thumbTimer = nullptr;
};
