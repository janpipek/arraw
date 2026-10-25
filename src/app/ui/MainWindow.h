#pragma once

#include "BrushPainting.h"
#include "CopySections.h"
#include "CropEditing.h"
#include "ExportQueue.h"
#include "HistogramCounter.h"
#include "LensSupport.h"
#include "MaskEditing.h"
#include "PhotoLoader.h"
#include "PreviewRenderer.h"
#include "RenderIndicator.h"
#include "SnapshotWriter.h"
#include "ThumbnailWorker.h"

#include <DecodeOptions.h>
#include <EditSession.h>
#include <ExportPresets.h>
#include <ExportSettings.h>
#include <ImageBuffer.h>
#include <NegativeSettings.h>
#include <Photo.h>
#include <PhotoMarks.h>
#include <Progress.h>
#include <SnapshotWrites.h>
#include <Snapshots.h>

#include <QImage>
#include <QMainWindow>
#include <QModelIndex>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

class QAction;
class QActionGroup;
class QCloseEvent;
class QDockWidget;
class QMenu;
class QLabel;
class QListView;
class QToolButton;
class QEvent;
class QShortcut;
class QSplitter;
class QTabWidget;
class QTreeView;
class QObject;

namespace arraw::app {

class ChromeHider;
class CollapsibleSection;
class CullingActions;
struct DebugLog;
class DebugWindow;
class DevelopPanel;
class FilmStrip;
class HistogramPanel;
class HistoryModel;
class PhotoView;
class RenderProgressPie;
class SnapshotModel;
class StatusLine;

/// @brief Top-level window of the desktop application.
class MainWindow : public QMainWindow {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(MainWindow)
public:
    /// @brief Makes the window.
    /// @param debugLog Log the window's diagnostics go to, and the debug
    /// window shows; must outlive the window.
    /// @param parent Parent widget, usually none.
    /// @param decoder Decodes the photographs; empty for ::arraw::app::PhotoLoader::imageDecoder.
    /// A test may give one it counts calls of.
    /// @param lensDatabase Gives the lens database, called once on a thread of its own at
    /// startup, and by the export queue and the thumbnails when they need it; empty for the
    /// process's own (::arraw::LensDatabase::system). The lens settings stay disabled until it
    /// has returned. A test may give one that loads a fixture.
    explicit MainWindow(DebugLog& debugLog, QWidget* parent = nullptr,
                        PhotoLoader::Decoder decoder = {}, LensDatabaseSource lensDatabase = {});

    /// @brief Cuts the panel's signals off before the members they use are gone.
    ~MainWindow() override;

    /// @brief Answer to the question about files that already exist in an export's folder.
    enum class ExistingChoice {
        ReplaceAll,   ///< Write every file, replacing those that exist.
        SkipExisting, ///< Write only the files that do not exist yet.
        Cancel,       ///< Export nothing.
    };

    /// @brief The questions exportShots asks, which a test may answer without a dialog.
    ///
    /// Each member left empty is the real dialog.
    struct ExportPrompts {
        /// Gives the settings to export with, or nothing when cancelled; receives the settings
        /// stored last and a frame size to start the custom size at.
        std::function<std::optional<ExportSettings>(const ExportSettings&, ImageSize)> settings;
        /// Gives the folder to export into, or an empty path when cancelled; receives where
        /// to start.
        std::function<std::filesystem::path(const std::filesystem::path&)> folder;
        /// Answers for the number of files that exist.
        std::function<ExistingChoice(std::size_t)> existing;
    };

    /// @brief Replaces the dialogs of exportShots; for tests.
    /// @param prompts Answers; a member left empty keeps the real dialog.
    void setExportPrompts(ExportPrompts prompts) {
        exportPrompts_ = std::move(prompts);
    }

    /// @brief Opens a startup file or folder, restoring the last one when none is supplied.
    void openInitialPath(const std::optional<std::filesystem::path>& path = std::nullopt);

    /// @brief Shows how far the render of a request has got.
    ///
    /// On the GUI thread, which the renderer's progress is handed over to. Drawn
    /// only when the render outlasts ::arraw::app::renderActivityDelay; a report
    /// of a request that is no longer the newest is ignored.
    /// @param request Identifier PreviewRenderer::request returned.
    /// @param fraction Fraction of the render done, from 0 to 1.
    /// @param step Step being worked on.
    void showRenderProgress(std::uint64_t request, double fraction, ProgressStep step);

    /// @brief Shows a finished render, or reports why there is none.
    ///
    /// On the GUI thread, which the renderer's results are handed over to. Ignores results of a
    /// previous photograph and ones older than what is shown. Keeps the picture it shows if
    /// rendering failed, and reports a failure only for the newest request, one message box at a
    /// time. A provisional result (a coarser stand-in) is shown like any other, but does not end
    /// the busy period, and gives no thumbnail; the render of its own level, of the same request,
    /// is shown after it.
    /// @param result Outcome delivered by the renderer.
    void showResult(const PreviewResult& result);

    /// @brief Gives the number of pictures handed to the histogram counter so far.
    ///
    /// For tests: nothing is submitted while the histogram is not wanted (ADR 055, section 3).
    [[nodiscard]] std::uint64_t histogramSubmitCount() const noexcept {
        return histogramSubmitCount_;
    }

    /// @brief Gives the indicator that decides when a render in progress shows.
    [[nodiscard]] RenderIndicator& renderIndicator() const noexcept {
        return *renderIndicator_;
    }

protected:
    /// @brief Sends status tips to the status line, which a status bar message would hide.
    bool event(QEvent* event) override;

    /// @brief Schedules a new render when the view changes size or pixel ratio.
    ///
    /// Watching the view rather than the window catches whatever changes the
    /// room it has, and a move to a screen of another pixel ratio.
    bool eventFilter(QObject* watched, QEvent* event) override;

    /// @brief Lets the window close, or asks what to do with exports still running.
    ///
    /// "Wait" keeps the window until the queue is empty and then closes it;
    /// "Cancel Exports" drops the queued exports and closes at once. The export
    /// in progress is never cut off: its file is written atomically, and
    /// destroying the window waits for it to finish.
    /// @param event Close request.
    void closeEvent(QCloseEvent* event) override;

    /// @brief Keeps the Full Screen action and the state to return to in step with the window.
    /// @param event Change notification.
    void changeEvent(QEvent* event) override;

private:
    /// @brief Adds the panel toggles (F7, F8, F9), Full Screen (F11) and Hide Panels (F12)
    /// to the View menu and to the window, so their keys outlive a hidden menu bar.
    void buildPanelActions();

    /// @brief Adds the shortcut-bearing actions of a menu, submenus included, to the window.
    ///
    /// Lights-out hides the menu bar, and Qt does not fire an action whose only widget is a
    /// hidden menu. Run once, at the end of the constructor: re-run it after adding a menu action
    /// that has a shortcut, or that key stops working in lights-out.
    /// @param menu The menu or menu bar to walk.
    void keepMenuShortcutsOnWindow(QWidget* menu);

    /// @brief Hides or restores every panel, the menu bar and the status bar.
    ///
    /// Each comes back as visible as it was; one that was closed stays closed. The dock
    /// toggles (F7, F8, F9) are disabled while the panels are hidden, so a panel cannot
    /// change under the snapshot; only Hide Panels leaves the mode.
    /// @param hidden True to hide, false to restore.
    void setPanelsHidden(bool hidden);

    /// @brief Enters full screen, or returns to the maximised or normal state it left.
    void toggleFullScreen();

    /// @brief Builds the menu bar and the actions it offers.
    void buildMenu();

    /// @brief Shows and saves desktop preferences for the next application session.
    void showSettings();

    /// @brief Builds the view that shows the photograph, zoomed and panned.
    void buildImageView();

    /// @brief Builds the View menu and the status bar's zoom button, from one list of actions.
    ///
    /// The actions are shared: Fit and the presets of ::arraw::app::zoomPresets
    /// are in the View > Zoom menu and in the button's dropdown alike (ADR 0056
    /// of the earlier version).
    void buildZoomControls();

    /// @brief Shows the view's zoom on the button and ticks the matching preset.
    void updateZoomControls();

    /// @brief Gives the presets file for the export dialog, or nothing without one.
    [[nodiscard]] ExportPresetStore* exportPresetStore() {
        return exportPresets_ ? &*exportPresets_ : nullptr;
    }

    /// @brief Asks how to export the photograph and where to, then queues the export.
    ///
    /// Snapshots the photograph's develop state at this moment; editing
    /// carries on while the export runs.
    void exportWithDialog();

    /// @brief Queues the export of the open photograph after asking where to, not how.
    ///
    /// The settings are not stored as the last used ones, nor is a preset remembered.
    /// @param settings What to export with, for instance a preset's.
    void exportPhotoWith(const ExportSettings& settings);

    /// @brief Exports shots as they were saved, into one folder chosen once.
    ///
    /// Asks for the settings once, then for the folder (starting at the last one used), names
    /// the files with ::arraw::app::targetPaths, asks once about files that exist, and queues
    /// one job per file. The jobs read each shot's sidecar on the export thread, so the open
    /// photograph's unsaved edits are not included; the summary says so. The jobs report as one
    /// batch: a summary when the last finishes, not a message per file.
    /// @param primaries Primary files of the shots.
    void exportShots(const QStringList& primaries);

    /// @brief Exports shots like exportShots, but with settings and without asking for them.
    ///
    /// The last used settings are not changed.
    /// @param primaries Primary files of the shots.
    /// @param settings What to export with, for instance a preset's.
    void exportShotsWith(const QStringList& primaries, const ExportSettings& settings);

    /// @brief Shows the export presets as the items of a menu, read now.
    ///
    /// Usable presets are enabled; unusable ones are disabled with the reason. With none, one
    /// disabled "No presets" item.
    /// @param menu Menu to fill; emptied first.
    /// @param chosen Called with the settings of the preset the user picks.
    void fillExportPresetMenu(QMenu& menu, std::function<void(const ExportSettings&)> chosen);

    /// @brief Asks the questions of an export and queues it; shared by the entry points above.
    /// @param given Settings to export with; nothing asks in the dialog.
    void exportPhotoImpl(const std::optional<ExportSettings>& given);

    /// @brief Asks the questions of a batch and queues it; shared by the entry points above.
    /// @param primaries Primary files of the shots.
    /// @param given Settings to export with; nothing asks in the dialog.
    void exportShotsImpl(const QStringList& primaries, const std::optional<ExportSettings>& given);

    /// @brief Reports a finished export, and closes the window if it was waiting for them.
    /// @param result Outcome delivered by the export queue.
    void showExportResult(const ExportResult& result);

    /// @brief Shows what the queue is working on in the status bar.
    void showExportProgress();

    /// @brief Shows the debug window, making it on first use.
    void showDebugWindow();

    /// @brief Asks the user for a photograph and opens it.
    void openFileWithDialog();

    /// @brief Opens a photograph the user chose, and shows its folder in the film strip.
    ///
    /// The file itself is developed, even when it is a companion of a shot (a JPEG beside a
    /// RAW); the strip selects the shot that holds it, once listed: a file below the strip's
    /// folder that its listing under way has yet to bring keeps the strip as it is. Nothing
    /// changes if the file's
    /// description cannot be read or the user cancels leaving the open photograph; its pixels
    /// are decoded afterwards, off the GUI thread (showPhoto()).
    /// @param path File to open.
    void openFile(const std::filesystem::path& path);

    /// @brief Asks the user for a folder and opens it.
    void openFolderWithDialog();

    /// @brief Fills the film strip with a folder and develops its first shot.
    ///
    /// Asks about unsaved changes first. The first shot is the first the strip's filter shows;
    /// it is opened when the listing brings it, unless a shot was made active by then, or the
    /// photograph still open was edited in the meantime (leaving it is not asked about twice).
    /// @param folder Folder to open.
    void openFolder(const std::filesystem::path& folder);

    /// @brief Makes the first shot the strip's listing brings open, once it arrives.
    void armFirstListed();

    /// @brief Tells whether the open photograph gained unsaved changes since armFirstListed,
    /// having had none then.
    ///
    /// Finishes an edit in progress first, as leaving the photograph would.
    [[nodiscard]] bool editedSinceAsked();

    /// @brief Develops a shot of the strip, when the user lets go of the open photograph.
    ///
    /// The photograph's description and sidecar are read before anything is asked, so one
    /// that does not open leaves the window as it is; cancelling leaves the strip's active
    /// shot and selection as they were. The pixels follow asynchronously (showPhoto()).
    /// @param primary Primary file of the shot, as the strip names it.
    void activateShot(const QString& primary);

    /// @brief Handles another program changing a shot's sidecar.
    ///
    /// For the open photograph with nothing unsaved the photograph is read again from the
    /// sidecar, the view kept; with unsaved edits the user is told and the edits stay.
    /// @param primary Primary file of the shot whose sidecar changed.
    void sidecarChangedOnDisk(const QString& primary);

    /// @brief Writes the marks of a shot's sidecar, through the session if its photograph is open.
    /// @param primary Primary file of the shot.
    /// @param marks Marks to write.
    /// @throws std::exception if they cannot be written.
    void writeMarks(const std::filesystem::path& primary, const PhotoMarks& marks);

    /// @brief Builds the dock holding the film strip.
    void buildFilmStripDock();

    /// @brief Names the folder the file dialogs start in.
    /// @return The last folder opened, else the pictures folder.
    [[nodiscard]] QString dialogFolder() const;

    /// @brief Remembers a folder for the file dialogs.
    /// @param folder Folder just opened.
    void rememberFolder(const std::filesystem::path& folder);

    /// @brief Writes the develop state of the open photograph to its sidecar.
    /// @return Whether it is saved, or there was nothing to save; a failure is shown to the user.
    bool saveAdjustments();

    /// @brief Settles unsaved changes before the open photograph is left.
    ///
    /// Asks "Save changes to <name>?" when the develop state differs from the
    /// sidecar's. Save writes it, and a failure is shown and stays; Discard
    /// returns the session to the saved state. Everything that replaces or
    /// closes the photograph asks this first.
    /// @return Whether to go on: false on Cancel or a failed save, so nothing is left.
    bool confirmLeavingPhoto();

    /// @brief Sets the marks of the open photograph through its session, which writes them at once.
    ///
    /// Marks are not develop edits, so nothing in the panel or the title changes.
    /// @param marks Marks the photograph carries from now on.
    /// @throws std::exception if they cannot be written; nothing changes.
    void setMarksForCurrent(PhotoMarks marks);

    /// @brief Shows the file name and whether it has unsaved changes in the title.
    void updateTitle();

    /// @brief Builds the dock holding the develop panel.
    void buildDevelopDock();

    /// @brief Runs a slot body that calls into the session, reporting what it throws.
    ///
    /// An exception must not leave a function Qt's event loop called. A failed
    /// edit is cancelled, and the panel shows the state of the session again.
    /// @param action Body to run.
    void guarded(const std::function<void()>& action);

    /// @brief Opens an edit of the open photograph's session, as the panel and the mask overlay
    /// begin theirs.
    ///
    /// In the crop mode it begins a step of the crop session's history instead.
    void beginEdit();

    /// @brief Takes the state an edit has reached, from the panel or the mask overlay.
    ///
    /// Ignored when no edit is open: a failure cancelled it, and the rest of that drag is dropped.
    void updateEdit(const DevelopState& state);

    /// @brief Takes the negative settings a drag in the Negative group has reached.
    ///
    /// For the vignetting amount (ADR 050): the open edit keeps them and the preview shows the
    /// scaled correction, without decoding again. Ignored when no edit is open.
    void updateNegativeEdit(const NegativeSettings& negative);

    /// @brief Closes the edit as one history step.
    /// @param origin Kind of step it is.
    void finishEdit(EditOrigin origin);

    /// @brief Drops the edit that is open, as when a drag is lost, and shows the state before it.
    void cancelEdit();

    /// @brief Enters or leaves the mask mode (M, Photo > Masks).
    ///
    /// Entering finishes a pending panel edit, disarms the picker and leaves the crop mode
    /// keeping the crop. The mode opens no edit of its own: each gesture is a history step
    /// (ADR 044).
    void setMaskMode(bool masking);

    /// @brief Leaves the mask mode, cancelling a gesture under way.
    ///
    /// Keeps the selection, so the panel still shows the mask's rows.
    void leaveMaskMode();

    /// @brief Shows whether the mask mode is on, in its action and in the develop tabs.
    void showMaskMode();

    /// @brief Selects a mask, or none, in the overlay and the panel.
    /// @param id Mask to select; dropped when the state holds none with that id.
    void selectMask(std::optional<LocalAdjustmentId> id);

    /// @brief Arms or disarms a mask tool as the user chose, ending a stroke under way first.
    void chooseMaskTool(MaskTool tool);

    /// @brief Arms or disarms a mask creation tool, entering the mask mode to arm one.
    ///
    /// Never ends a stroke: the program disarms tools itself, as when the list fills.
    void setMaskTool(MaskTool tool);

    /// @brief Takes new brush settings from a key: shows, applies and stores them.
    ///
    /// Nothing unless the tool paints.
    void changeBrush(const BrushSettings& settings);

    /// @brief Gives the mask overlay and the panel the session's state and the selection.
    ///
    /// Sets the view's frame size first, so that the overlay maps through the frame the state
    /// has (the crop may have changed it). The selection stays while its mask is in the list.
    void syncMasks();

    /// @brief Arms or disarms the white balance picker.
    ///
    /// Armed, the view shows a cross cursor and a click on the photograph reads
    /// the light there; Esc disarms it.
    /// @param picking Whether the picker is armed.
    void setPicking(bool picking);

    /// @brief Sets the white balance from the neutral under a click on the view.
    /// @param point Click position, in fractions of the developed frame.
    void pickNeutralAt(const QPointF& point);

    /// @brief Enters or leaves the crop mode; leaving this way keeps the crop (C, the Crop button).
    ///
    /// Entering finishes a pending panel edit, disarms the picker and opens the
    /// one edit the whole session is (ADR 022, ADR 040). Only with a photograph open.
    void setCropMode(bool cropping);

    /// @brief Leaves the crop mode, committing or cancelling its edit.
    /// @param accept Whether the crop is kept; otherwise the state before the mode returns.
    void leaveCropMode(bool accept);

    /// @brief Hides the crop overlay and puts the window's controls back as outside the mode.
    ///
    /// Leaves the session's edit alone: that is the caller's.
    void closeCropOverlay();

    /// @brief Gives the overlay something to show at once on entering the mode (ADR 040).
    ///
    /// The last render of the mode for this photograph if nothing it shows has
    /// changed since; else the camera's embedded preview, turned and flipped to
    /// the geometry, under the developed frame as the view showed it, in the frame.
    void seedCropOverlay();

    /// @brief Enables Undo and Redo: through the crop session's gestures in the mode, else
    /// through the photograph's history; Paste follows, as it too edits the history.
    void updateHistoryActions();

    /// @brief Builds the History dock and its toggle in the View menu.
    void buildHistoryDock();

    /// @brief Shows the session's history in the dock, and disables the dock in the crop mode.
    ///
    /// Leaves the list as it is while an edit is open.
    void updateHistoryDock();

    /// @brief Goes to the step a row of the History dock shows, as Undo and Redo do.
    /// @param row Row clicked or activated.
    void goToHistoryRow(const QModelIndex& row);

    /// @brief Builds the snapshot panel that sits above the history list in the same dock.
    ///
    /// A header with the New and menu buttons, the notice about unreadable entries, the banner of
    /// a preview and the tree of snapshots.
    /// @return The panel, to be added to the dock's splitter.
    QWidget* buildSnapshotPanel();

    /// @brief Gives the photograph the view, the panel and the lens status show.
    ///
    /// The previewed snapshot's look while one is previewed, else the session's photograph. Edits
    /// always read the session's.
    /// @pre A photograph is open.
    [[nodiscard]] const Photo& shownPhoto() const {
        return open_->previewPhoto ? *open_->previewPhoto : open_->session.photo();
    }

    /// @brief Tells whether a snapshot is being previewed.
    [[nodiscard]] bool previewing() const noexcept {
        return open_ && open_->previewed.has_value();
    }

    /// @brief Shows the look of a snapshot without changing the session.
    ///
    /// Finishes a pending edit first; on failure, or when the snapshot's look is refused, the
    /// preview does not start and the user is told.
    /// @param id Snapshot to preview.
    void startPreview(SnapshotId id);

    /// @brief Ends the preview, if one is on, and shows the working look again.
    /// @param refresh Whether to refresh the panel and ask for a render; false when the caller
    /// goes on to change the session at once.
    void closePreview(bool refresh = true);

    /// @brief Closes the preview of a snapshot the collection no longer lists.
    void reconcilePreview();

    /// @brief Makes a snapshot's look the working look, as one undoable step.
    /// @param id Snapshot to restore.
    void restoreSnapshot(SnapshotId id);

    /// @brief Asks for a name and creates a snapshot of the working look (Ctrl+N).
    void createSnapshot();

    /// @brief Asks for a new name of the selected snapshot.
    void renameSelectedSnapshot();

    /// @brief Asks to delete the selected snapshot, then does.
    void deleteSelectedSnapshot();

    /// @brief Asks to remove the unreadable snapshot entries of the sidecar, then does.
    void discardUnreadableSnapshots();

    /// @brief Asks for a snapshot name until the answer is storable or the user cancels.
    /// @param title Dialog title.
    /// @param initial Name to start with.
    /// @return The trimmed name, with the default for an empty one; nothing if cancelled.
    [[nodiscard]] std::optional<std::string> askSnapshotName(const QString& title,
                                                             const QString& initial);

    /// @brief Hands a prepared write to the snapshot writer.
    /// @param write Write to carry out.
    /// @param structural Whether it changes the list; at most one is in flight.
    void submitSnapshotWrite(SnapshotWrite write, bool structural);

    /// @brief Takes the result of a snapshot write, on the GUI thread.
    /// @param result Outcome delivered by the writer.
    void snapshotWriteLanded(const SnapshotWriteResult& result);

    /// @brief Takes a thumbnail the film strip's worker made for a snapshot.
    /// @param result Outcome delivered by the worker.
    void snapshotThumbnailLanded(const SnapshotThumbnailResult& result);

    /// @brief Asks the worker for the thumbnails of snapshots not requested yet, or whose stored
    /// one changed.
    void requestSnapshotThumbnails();

    /// @brief Starts a new snapshot generation for the open photograph and asks for its thumbnails.
    void beginSnapshotThumbnails();

    /// @brief Shows the session's snapshots in the tree.
    /// @param force Whether to refresh even if the revision and the bases are the ones shown.
    void refreshSnapshotModel(bool force);

    /// @brief Refreshes everything that follows a change of the snapshots or their bases.
    void refreshSnapshotUi();

    /// @brief Enables the snapshot panel and its actions, and shows the notice and the banner.
    void updateSnapshotPanel();

    /// @brief Gives the snapshot the tree has selected.
    [[nodiscard]] std::optional<SnapshotId> selectedSnapshot() const;

    /// @brief Makes the tree's selection the previewed snapshot, or none.
    void syncSnapshotSelection();

    /// @brief Asks which sections to copy and holds the open photograph's settings for pasting.
    void copySettings();

    /// @brief Carries the held settings onto the open photograph as one history step.
    ///
    /// Tells the status bar what could not be carried.
    void pasteSettings();

    /// @brief Enables Paste: with settings held, a photograph open, and outside the crop mode.
    void updatePasteAction();

    /// @brief Change of the geometry that a menu entry or a panel button asks for, in both forms.
    struct GeometryCommand {
        /// @brief Change as the crop mode makes it, on the overlay's editing.
        std::function<void(CropEditing&)> crop;
        /// @brief Change as the state operations of Edits.h make it, outside the crop mode.
        std::function<DevelopState(const ImageMetadata&, DevelopState)> state;
    };

    /// @brief Applies a geometry command: a quarter-turn, a flip, an aspect, a reset.
    ///
    /// In the crop mode it joins the session's edit; otherwise it is one
    /// history step of its own. The rules are the same either way (CropGeometry.h), so the
    /// crop is carried and kept inside the photograph.
    /// @param command Change to make to the geometry.
    void editGeometry(const GeometryCommand& command);

    /// @brief Arms or disarms the straighten tool, entering the crop mode to arm it.
    void setStraightening(bool straightening);

    /// @brief Gives the geometry a panel edit asks for.
    ///
    /// Outside the crop mode, the geometry as it is: the panel built it with the rules of
    /// Edits.h. In the crop mode, the one the overlay adopts it as.
    /// @param geometry Geometry the panel's edited state carries.
    [[nodiscard]] GeometrySettings reconciledGeometry(const GeometrySettings& geometry);

    /// @brief Shows the open photograph's state in the develop panel, lens rows included.
    /// @pre A photograph is open.
    void showPanel();

    /// @brief Shows the session's state in the panel and updates the actions, then asks for a
    /// render.
    /// @param renderDelay Time to hold the render back; a newer request or edit replaces it.
    void refreshPanel(std::chrono::milliseconds renderDelay = std::chrono::milliseconds{0});

    /// @brief Asks the renderer for the current photograph in its current state.
    ///
    /// The part of the frame in view, at the size the view shows it. Returns at once; the picture
    /// arrives through showResult, and a newer request replaces one not yet started.
    void requestRender();

    /// @brief Makes the open photograph's thumbnail in the strip follow a preview.
    ///
    /// Only for a preview of the whole frame of the shot's primary file; the thumbnail is the
    /// preview reduced to the cache's size, and is not cached, as it may show unsaved edits.
    /// @param result A preview that rendered, and is being shown.
    void followWithThumbnail(const PreviewResult& result);

    /// @brief Creates the status bar with its permanent preview-device label.
    void buildStatusBar();

    /// @brief Shows which device rendered a preview, and why not the GPU if so.
    ///
    /// The fallback reason, when there is one, is the label's tooltip.
    /// @param result Finished render, with its image.
    void showDevice(const PreviewResult& result);

    /// @brief Opens a photograph and makes it the one being edited, decoding it on a worker.
    ///
    /// Returns at once (ADR 043). The session and the panel show the photograph's
    /// state from its sidecar immediately, and the view its thumbnail from the strip
    /// as a stand-in, or nothing; the bar shows the decode's progress. Until the
    /// pixels land (decodeLanded()) nothing that needs them is enabled: the panel,
    /// the crop mode, the geometry commands, the zoom, the picker and the export.
    /// Marks can be set throughout. Opening another photograph meanwhile cancels the
    /// decode, and the window never shows a result of one photograph for another.
    /// @param document Photograph to show, already read with its sidecar and snapshots.
    void showPhoto(OpenDocument document);

    /// @brief Takes the pixels of the photograph being opened, or of a re-decode, or reports why
    /// there are none.
    ///
    /// Ignores a decode the window no longer waits for. On success enables editing and asks
    /// for the first render; on failure closes the photograph, as there is nothing to edit. A
    /// re-decode (syncDecode()) that fails leaves the pixels shown, and says so.
    /// @param result Outcome delivered by the loader.
    void decodeLanded(const DecodedPhoto& result);

    /// @brief Takes the negative settings the user chose in the Negative group, as one step.
    ///
    /// Not in the crop mode, whose history it would join.
    /// The step that turns a lens correction on records the matched lens with it (ADR 050,
    /// section 6), so undo removes both.
    /// @param negative Settings to carry from now on.
    void chooseNegative(const NegativeSettings& negative);

    /// @brief Takes the lens database the loader thread made, and shows what it enables.
    /// @param database The database, valid for the life of the program.
    void lensDatabaseLoaded(const LensDatabase* database);

    /// @brief Gives the open photograph's lens corrections and what the panel says of them.
    ///
    /// Resolved when the corrections (the amount only as 0 or 100), the recorded lens or the
    /// database change, and kept until they do again. Matching a lens takes some milliseconds with
    /// the full database.
    /// @pre A photograph is open.
    [[nodiscard]] const LensView& lensView();

    /// @brief Lets the user choose the lens the corrections are for, from the database.
    void chooseLens();

    /// @brief Forgets the recorded lens and matches the photograph's again.
    void resetLens();

    /// @brief Starts, keeps or cancels the re-decode that makes the shown pixels match the
    /// photograph's negative settings (ADR 049).
    ///
    /// The one place that decides: called after every change of the negative (a choice, undo,
    /// redo, a jump in the history, a paste, a discard). Compares the decode options the
    /// photograph needs (decodeOptionsFor(), canonical) with those the shown pixels were decoded
    /// with, so a negative that cannot change the pixels never costs a decode. The old pixels
    /// stay shown, and develop edits keep rendering on them, until the new ones land. A failed
    /// decode is not retried until the photograph has been back on the shown pixels' options.
    void syncDecode();

    /// @brief Keeps a camera preview read for the crop mode, and shows it if the mode still
    /// waits for its first render.
    /// @param preview Outcome delivered by the loader.
    void cameraPreviewLanded(const CameraPreview& preview);

    /// @brief Shows the strip's thumbnail of the photograph being opened, fitted to its frame.
    ///
    /// The frame comes from what the file declares, so the stand-in sits where the render
    /// will. A thumbnail whose shape is not the frame's (a camera preview of a cropped or
    /// turned photograph) is not shown.
    void showStandIn();

    /// @brief Tells whether the histogram is worth counting: its section is open, the develop dock
    /// is shown and a photograph is open.
    [[nodiscard]] bool histogramWanted() const;

    /// @brief Gives the picture the histogram counts, and the row spans of it that count.
    /// @param spans Receives the spans, empty for whole rows.
    /// @return The whole developed frame as shown, or in the crop mode the mode's render with the
    /// crop frame's spans; null if there is none to count yet.
    [[nodiscard]] QImage measuredPicture(std::vector<RowSpan>& spans) const;

    /// @brief Hands the measured picture to the counter, if it is wanted and not already counted.
    void submitMeasured();

    /// @brief Drops what the counter has and the panel shows, for another photograph or none.
    void resetHistogram();

    /// @brief Shows a finished count on the panel, unless it is stale.
    /// @param count Count delivered by the counter, on the GUI thread.
    void showHistogram(const HistogramCount& count);

    /// @brief Reacts to the histogram becoming wanted or not.
    void histogramGateChanged();

    /// @brief Sets which kinds of clipping are painted on the picture, in step with the view,
    /// the panel's triangles and the View menu, and remembers it in QSettings (ADR 055).
    /// @param shadows Whether shadow-clipped pixels are painted.
    /// @param highlights Whether highlight-clipped pixels are painted.
    void setClipping(bool shadows, bool highlights);

    /// @brief Closes the open photograph, leaving the window empty.
    void closePhoto();

    /// @brief Tells whether a photograph is open with its pixels and no snapshot is previewed, so
    /// whether it can be edited.
    [[nodiscard]] bool editable() const noexcept {
        return open_ && open_->decoded && !open_->previewed;
    }

    /// @brief Tells whether a photograph is open with its pixels, so whether it can be viewed,
    /// zoomed, exported and its snapshots used, whether or not a snapshot is previewed.
    [[nodiscard]] bool hasPixels() const noexcept {
        return open_ && open_->decoded;
    }

    /// @brief Enables what needs the pixels of the open photograph, as editable() says.
    void updateEditingActions();

    /// @brief Photograph being edited, with its pixels.
    struct OpenPhoto {
        /// Edit session of the photograph.
        EditSession session;
        /// Pixels, decoded once and developed again for each size; shared with
        /// the renderer, which may still be using them after the window moved on.
        /// Null while the loader decodes them.
        std::shared_ptr<const ImageBuffer> decoded;
        /// Options decoded was decoded with, in canonical form (decodeOptionsFor()).
        DecodeOptions decodedOptions;
        /// Camera's embedded preview, upright; read off the GUI thread on first entering the
        /// crop mode, and null if the file has none.
        std::optional<QImage> cameraPreview;
        /// Last render of the crop mode, and the uncropped state it shows, to show at once
        /// on entering the mode again.
        std::optional<std::pair<DevelopState, QImage>> lastCropImage;
        /// Newest render request made before the pixels last changed; the crop mode's cached
        /// image is made only from renders of later requests.
        std::uint64_t cropImageFloor = 0;
        /// What lensView was resolved for: the corrections and recorded lens, and the database
        /// (null while it loads). Empty before the first resolution.
        std::optional<std::pair<LensSettings, const LensDatabase*>> lensKey = std::nullopt;
        /// The vignetting amount lensView's correction is scaled to; the key holds it as 0 or 100.
        float lensAmount = 0.0F;
        /// The lens corrections and the panel's words about them, as of lensKey.
        LensView lensView = {};
        /// Snapshot being previewed; the session is untouched.
        std::optional<SnapshotId> previewed = std::nullopt;
        /// The photograph with the previewed snapshot's recipe, shown instead of the session's.
        std::optional<Photo> previewPhoto = std::nullopt;
        /// Number of this session among the window's, to drop results of an earlier one.
        std::uint64_t serial = 0;
        /// Snapshot generation of the thumbnails requested for this session.
        std::uint64_t snapshotGeneration = 0;
        /// Ticket of the structural snapshot write in flight, if any.
        std::optional<std::uint64_t> structuralTicket = std::nullopt;
        /// Whether a thumbnail write failed, after which none is asked for or written.
        bool thumbnailWritesFailed = false;
        /// Render key of the stored thumbnail each snapshot had when its thumbnail was last
        /// requested, or of the one written since.
        std::map<std::uint64_t, std::string> requestedKeys = {};
    };

    DebugLog& debugLog_;
    DebugWindow* debugWindow_ = nullptr;
    PhotoView* photoView_ = nullptr;
    FilmStrip* filmStrip_ = nullptr;
    /// Whether the first shot the strip's listing brings is to be opened (a folder was opened,
    /// or the subfolders that held the open photograph were dropped).
    bool activateFirstListed_ = false;
    /// Whether the open photograph had no unsaved changes when activateFirstListed_ was set: the
    /// question of leaving it was answered then, or not needed.
    bool cleanWhenArmed_ = true;
    QDockWidget* stripDock_ = nullptr;
    QMenu* viewMenu_ = nullptr;
    CullingActions* culling_ = nullptr;
    QToolButton* zoomButton_ = nullptr;
    QActionGroup* zoomGroup_ = nullptr;
    /// Fit, then one per preset, in the order of ::arraw::app::zoomPresets.
    std::vector<QAction*> zoomActions_;
    QAction* zoomInAction_ = nullptr;
    QAction* zoomOutAction_ = nullptr;
    /// Device label, owned by statusLine_.
    QLabel* deviceLabel_ = nullptr;
    /// Pie in the status bar that shows whether a render is going and how far it is; owned by
    /// statusLine_.
    RenderProgressPie* renderProgress_ = nullptr;
    /// Status-bar line that owns the pie, the message, the device label and the zoom button.
    StatusLine* statusLine_ = nullptr;
    /// Decides when a render in progress shows, and what of it.
    RenderIndicator* renderIndicator_ = nullptr;
    DevelopPanel* developPanel_ = nullptr;
    /// Tabs of the develop dock: Adjustments and Masks, the tab following the mask mode.
    QTabWidget* developTabs_ = nullptr;
    QDockWidget* developDock_ = nullptr;
    /// Collapsible section at the top of the develop dock that holds the histogram panel.
    CollapsibleSection* histogramSection_ = nullptr;
    /// Histogram, clipping triangles and pixel readout.
    HistogramPanel* histogramPanel_ = nullptr;
    /// Picture and spans last handed to the counter; empty while nothing is counted.
    std::optional<std::pair<qint64, std::vector<RowSpan>>> lastMeasured_;
    /// Serial of the newest count shown.
    std::uint64_t histogramShownSerial_ = 0;
    /// Number of pictures handed to the counter.
    std::uint64_t histogramSubmitCount_ = 0;
    /// Whether the window was maximised when it last entered full screen.
    bool maximizedBeforeFullScreen_ = false;
    /// Full Screen toggle (F11), checked while the window is full screen.
    QAction* fullScreenAction_ = nullptr;
    /// View menu's "Refresh Folder", on at once when the strip shows a folder.
    QAction* refreshFolderAction_ = nullptr;
    /// View > Show Clipping (J), checked while any kind of clipping is painted.
    QAction* showClippingAction_ = nullptr;
    /// Hide Panels toggle (F12), checked while the panels are hidden.
    QAction* hidePanelsAction_ = nullptr;
    /// The dock toggles of F7, F8 and F9.
    std::vector<QAction*> panelToggles_;
    /// Hides the panels and the bars together for Hide Panels.
    std::unique_ptr<ChromeHider> panelHider_;
    /// Steps of the open photograph's session, newest on top.
    HistoryModel* historyModel_ = nullptr;
    QListView* historyView_ = nullptr;
    QDockWidget* historyDock_ = nullptr;
    /// Snapshots of the open photograph's session, as a tree.
    SnapshotModel* snapshotModel_ = nullptr;
    QTreeView* snapshotTree_ = nullptr;
    /// Section of the History dock above the history list.
    QWidget* snapshotPanel_ = nullptr;
    /// Notice that the sidecar holds snapshot entries that cannot be read, and its text.
    QWidget* snapshotDamageNotice_ = nullptr;
    QLabel* snapshotDamageLabel_ = nullptr;
    /// Banner shown while a snapshot is previewed, and its text.
    QWidget* snapshotPreviewBanner_ = nullptr;
    QLabel* snapshotPreviewLabel_ = nullptr;
    QAction* createSnapshotAction_ = nullptr;
    QAction* renameSnapshotAction_ = nullptr;
    QAction* deleteSnapshotAction_ = nullptr;
    QAction* restoreSnapshotAction_ = nullptr;
    QAction* discardUnreadableAction_ = nullptr;
    /// Esc while a snapshot is previewed.
    QShortcut* closePreviewShortcut_ = nullptr;
    /// Whether the tree's selection is being set by the window, which is no request to preview.
    bool syncingSnapshotSelection_ = false;
    /// What the tree last showed, so that an edit that changes none of it costs nothing.
    struct ShownSnapshots {
        std::uint64_t serial = 0;
        std::uint64_t revision = 0;
        std::size_t count = 0;
        std::optional<SnapshotId> base;
        std::optional<SnapshotId> savedBase;
    };
    std::optional<ShownSnapshots> shownSnapshots_;
    /// Serial given to the newest session.
    std::uint64_t sessionSerial_ = 0;
    QAction* saveAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* copyAction_ = nullptr;
    QAction* pasteAction_ = nullptr;
    /// Settings copied from a photograph; lasts across photographs, which is its use.
    std::optional<SettingsClipboard> clipboard_;
    QAction* exportAction_ = nullptr;
    QMenu* exportPresetMenu_ = nullptr; ///< File > Export with Preset.
    QShortcut* cancelPickShortcut_ = nullptr;
    /// Enter, Esc, X and O of the crop mode wherever the focus is; enabled only in the mode.
    std::vector<QShortcut*> cropShortcuts_;
    /// Esc, O, Delete, Backspace and the brush keys of the mask mode wherever the focus is; enabled
    /// only in it.
    std::vector<QShortcut*> maskShortcuts_;
    /// Settings of the brush tool, as the panel shows them and the next stroke is made with.
    BrushSettings brushSettings_;
    /// Whether the edit being committed is a brush stroke's.
    bool committingStroke_ = false;
    QAction* maskAction_ = nullptr;
    /// Mask whose handles are shown; view state of the window (ADR 044).
    std::optional<LocalAdjustmentId> maskSelection_;
    QMenu* photoMenu_ = nullptr;
    QAction* cropAction_ = nullptr;

    /// Photo menu's rotations and flips, enabled with a photograph open.
    std::vector<QAction*> geometryActions_;

    /// Whether the next click on the photograph picks a neutral.
    bool picking_ = false;

    /// Single-shot timer that fires once the view has stopped changing, so that
    /// dragging an edge renders once rather than per pixel.
    QTimer resizeTimer_;

    /// Single-shot timer of no delay, so that a drag or a wheel burst asks for
    /// one render per turn of the event loop, however many events it has.
    QTimer interactionTimer_;

    /// Single-shot timer that holds a render back while a noise reduction row is dragged.
    QTimer noiseReductionTimer_;

    /// Whether a message about a failed render is on screen.
    bool reportingFailure_ = false;

    /// Photograph being shown; empty until one is opened.
    std::optional<OpenPhoto> open_;

    /// Identifier of the newest render requested.
    std::uint64_t latestRequest_ = 0;

    /// Identifier of the newest result shown or reported.
    std::uint64_t latestShown_ = 0;
    /// Whether what latestShown_ shows is a coarser stand-in, so that the render of its own
    /// level, of the same request, is still to be shown.
    bool latestShownProvisional_ = false;

    /// Identifier of the first request made for the photograph being edited;
    /// results below it belong to a previous photograph.
    std::uint64_t firstRequest_ = 0;

    /// Identifier of the decode the window waits for; 0 when none.
    std::uint64_t decodeRequest_ = 0;

    /// Identifier of the re-decode for another negative the window waits for; 0 when none.
    std::uint64_t redecodeRequest_ = 0;

    /// Options of the re-decode in flight; meaningful while redecodeRequest_ is set.
    DecodeOptions redecodeOptions_;

    /// Options whose re-decode failed, which syncDecode() does not try again until the
    /// photograph has been back on the shown pixels' options.
    std::optional<DecodeOptions> failedOptions_;

    /// Whether the last render shown came from the GPU, where a noise reduction edit is not
    /// held back (renderDelayFor()).
    bool previewOnGpu_ = false;

    /// Identifier of the camera preview read the crop mode waits for; 0 when none.
    std::uint64_t cameraPreviewRequest_ = 0;

    /// Identifier of the first render requested in the crop mode; later results are the overlay's.
    std::uint64_t cropFirstRequest_ = 0;

    /// What the crop mode last asked to render, so that a crop edit asks for nothing new.
    std::optional<std::pair<DevelopState, QSize>> lastCropRender_;

    /// Renders of the crop mode on their way, with the state each shows, oldest first.
    std::deque<std::pair<std::uint64_t, DevelopState>> cropRequests_;

    /// @brief What the jobs of one exportShots call have done so far.
    struct ExportBatch {
        std::size_t total = 0;   ///< Jobs queued.
        std::size_t written = 0; ///< Files written.
        std::size_t failed = 0;  ///< Jobs that failed.
        std::size_t skipped = 0; ///< Shots left out before queueing.
        QStringList details;     ///< Failures, then skips and notices, for the tooltip.
        QString note;            ///< Sentence the summary ends with, or empty.
    };

    /// Batch of each queued job of exportShots, by the identifier the queue gave.
    std::map<std::uint64_t, std::shared_ptr<ExportBatch>> exportBatches_;

    /// Answers that replace the export questions; empty members ask.
    ExportPrompts exportPrompts_;

    /// The export presets file; empty when there is no configuration directory.
    std::optional<ExportPresetStore> exportPresets_;

    /// File names of the exports that have not reported yet, by the identifier the queue gave.
    std::map<std::uint64_t, QString> exportNames_;

    /// Whether the user chose to wait for the exports before closing.
    bool closeWhenIdle_ = false;

    /// Desktop preferences used by this application session.
    AppSettings runningSettings_;

    /// Gives the lens database; called on the loader thread, the export thread and the
    /// thumbnail thread, never on the interface thread.
    LensDatabaseSource lensSource_;

    /// The lens database once the loader thread returned it; null while it loads.
    const LensDatabase* lensDatabase_ = nullptr;

    /// Worker that renders the preview.
    ///
    /// The three workers are the last members, in the order previewRenderer_,
    /// exportQueue_, photoLoader_, so they are destroyed in the reverse order,
    /// before everything declared above them. Each destructor joins its thread
    /// (the export queue finishes its job in progress), after which no callback
    /// can run, so nothing a callback touches has been destroyed yet. The
    /// workers do not use one another, so their order among themselves is free.
    PreviewRenderer previewRenderer_;

    /// Thread that counts the histogram of the picture shown (ADR 055).
    ///
    /// Declared after previewRenderer_ and before the other workers, with the same reasoning.
    HistogramCounter histogramCounter_;

    /// Worker that develops and writes exports.
    ///
    /// Declared after everything it reports to; see previewRenderer_.
    ExportQueue exportQueue_;

    /// Workers that decode photographs and read camera previews (ADR 043).
    ///
    /// Declared last, so it is destroyed first: it cancels the decode in
    /// progress and waits for its threads; see previewRenderer_.
    PhotoLoader photoLoader_;

    /// Thread that writes snapshot changes to sidecars (ADR 056, Gate 4).
    ///
    /// Declared after every worker its results touch, so it is destroyed, and joined, early:
    /// queued structural writes are carried out, and nothing is delivered once destruction began.
    SnapshotWriter snapshotWriter_;

    /// Thread that loads the lens database at startup (ADR 050, step 7).
    ///
    /// Declared after every worker, so it is destroyed, and joined, first: the load cannot be
    /// interrupted, so closing the window while it runs waits for it.
    std::jthread lensLoader_;
};

} // namespace arraw::app
