#pragma once

#include "CropEditing.h"
#include "ExportQueue.h"
#include "PreviewRenderer.h"

#include <EditSession.h>
#include <ImageBuffer.h>
#include <Photo.h>
#include <PhotoMarks.h>

#include <QImage>
#include <QMainWindow>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

class QAction;
class QActionGroup;
class QCloseEvent;
class QDockWidget;
class QMenu;
class QLabel;
class QToolButton;
class QEvent;
class QShortcut;
class QObject;

namespace arraw::app {

class CullingActions;
class DevelopPanel;
class FilmStrip;
class PhotoView;

/// @brief Top-level window of the desktop application.
class MainWindow : public QMainWindow {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(MainWindow)
public:
    explicit MainWindow(QWidget* parent = nullptr);

    /// @brief Opens a startup file or folder, restoring the last one when none is supplied.
    void openInitialPath(const std::optional<std::filesystem::path>& path = std::nullopt);

protected:
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

private:
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

    /// @brief Asks how to export the photograph and where to, then queues the export.
    ///
    /// Snapshots the photograph's develop state at this moment; editing
    /// carries on while the export runs.
    void exportWithDialog();

    /// @brief Reports a finished export, and closes the window if it was waiting for them.
    /// @param result Outcome delivered by the export queue.
    void showExportResult(const ExportResult& result);

    /// @brief Shows what the queue is working on in the status bar.
    void showExportProgress();

    /// @brief Asks the user for a photograph and opens it.
    void openFileWithDialog();

    /// @brief Opens a photograph the user chose, and shows its folder in the film strip.
    ///
    /// The file itself is developed, even when it is a companion of a shot (a JPEG beside a
    /// RAW); the strip selects the shot that holds it. Nothing changes if the file does not
    /// open or the user cancels leaving the open photograph.
    /// @param path File to open.
    void openFile(const std::filesystem::path& path);

    /// @brief Asks the user for a folder and opens it.
    void openFolderWithDialog();

    /// @brief Fills the film strip with a folder and develops its first shot.
    ///
    /// Asks about unsaved changes first. The first shot is the first the strip's filter shows.
    /// @param folder Folder to open.
    void openFolder(const std::filesystem::path& folder);

    /// @brief Develops a shot of the strip, when the user lets go of the open photograph.
    ///
    /// The photograph is read before anything is asked, so one that does not open leaves
    /// the window as it is; cancelling leaves the strip's active shot and selection as they were.
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

    /// @brief Arms or disarms the white balance picker.
    ///
    /// Armed, the view shows a cross cursor and a click on the photograph reads
    /// the light there; Esc disarms it.
    /// @param picking Whether the picker is armed.
    void setPicking(bool picking);

    /// @brief Sets the white balance from the neutral under a click on the view.
    /// @param point Click position, in fractions of the developed frame.
    void pickNeutralAt(const QPointF& point);

    /// @brief Enters or leaves the crop mode; leaving this way keeps the crop (R, the Crop button).
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
    /// through the photograph's history.
    void updateHistoryActions();

    /// @brief Applies a geometry command: a quarter-turn, a flip, an aspect, a reset.
    ///
    /// In the crop mode it joins the session's edit; otherwise it is one
    /// history step of its own. The rules are CropEditing's either way, so the
    /// crop is carried and kept inside the photograph.
    /// @param command Change to make to the geometry.
    void editGeometry(const std::function<void(CropEditing&)>& command);

    /// @brief Arms or disarms the straighten tool, entering the crop mode to arm it.
    void setStraightening(bool straightening);

    /// @brief Gives the geometry a panel edit asks for, through the crop rules.
    ///
    /// A straighten from a slider shrinks the crop as a rotation in the crop
    /// mode does, from where the edit began rather than step by step.
    /// @param geometry Geometry the panel's edited state carries.
    [[nodiscard]] GeometrySettings reconciledGeometry(const GeometrySettings& geometry);

    /// @brief Shows the session's state in the panel and updates the actions, then asks for a
    /// render.
    /// @param renderDelay Time to hold the render back; a newer request or edit replaces it.
    void refreshPanel(std::chrono::milliseconds renderDelay = std::chrono::milliseconds{0});

    /// @brief Asks the renderer for the current photograph in its current state.
    ///
    /// The part of the frame in view, at the size the view shows it. Returns at once; the picture
    /// arrives through showResult, and a newer request replaces one not yet started.
    void requestRender();

    /// @brief Shows a finished render, or reports why there is none.
    ///
    /// Ignores results of a previous photograph and ones older than what is
    /// shown. Keeps the picture it shows if rendering failed, and reports a
    /// failure only for the newest request, one message box at a time.
    /// @param result Outcome delivered by the renderer.
    void showResult(const PreviewResult& result);

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

    /// @brief Opens a photograph and makes it the one being edited.
    ///
    /// Decodes synchronously, before anything changes, so a failure leaves the
    /// window showing the previous photograph, session and pixels together.
    /// Rendering is asynchronous: the previous picture stays until the first
    /// render of this photograph arrives, and a failure of that render is
    /// reported through showResult.
    /// @param photo Photograph to show.
    /// @throws std::exception if the photograph cannot be decoded.
    void showPhoto(Photo photo);

    /// @brief Photograph being edited, with its pixels.
    struct OpenPhoto {
        /// Edit session of the photograph.
        EditSession session;
        /// Pixels, decoded once and developed again for each size; shared with
        /// the renderer, which may still be using them after the window moved on.
        std::shared_ptr<const ImageBuffer> decoded;
        /// Camera's embedded preview, upright; read on first entering the crop mode, and
        /// null if the file has none.
        std::optional<QImage> cameraPreview;
        /// Last render of the crop mode, and the uncropped state it shows, to show at once
        /// on entering the mode again.
        std::optional<std::pair<DevelopState, QImage>> lastCropImage;
    };

    PhotoView* photoView_ = nullptr;
    FilmStrip* filmStrip_ = nullptr;
    QDockWidget* stripDock_ = nullptr;
    QMenu* viewMenu_ = nullptr;
    CullingActions* culling_ = nullptr;
    QToolButton* zoomButton_ = nullptr;
    QActionGroup* zoomGroup_ = nullptr;
    /// Fit, then one per preset, in the order of ::arraw::app::zoomPresets.
    std::vector<QAction*> zoomActions_;
    QAction* zoomInAction_ = nullptr;
    QAction* zoomOutAction_ = nullptr;
    QLabel* deviceLabel_ = nullptr;
    DevelopPanel* developPanel_ = nullptr;
    QWidget* developDock_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    QShortcut* cancelPickShortcut_ = nullptr;
    /// Enter, Esc, X and O of the crop mode wherever the focus is; enabled only in the mode.
    std::vector<QShortcut*> cropShortcuts_;
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

    /// Identifier of the first request made for the photograph being edited;
    /// results below it belong to a previous photograph.
    std::uint64_t firstRequest_ = 0;

    /// Identifier of the first render requested in the crop mode; later results are the overlay's.
    std::uint64_t cropFirstRequest_ = 0;

    /// What the crop mode last asked to render, so that a crop edit asks for nothing new.
    std::optional<std::pair<DevelopState, QSize>> lastCropRender_;

    /// Renders of the crop mode on their way, with the state each shows, oldest first.
    std::deque<std::pair<std::uint64_t, DevelopState>> cropRequests_;

    /// Crop rules for a panel edit of the geometry outside the crop mode, from its start.
    std::optional<CropEditing> geometryEdit_;

    /// File names of the exports that have not reported yet, oldest first.
    std::deque<QString> exportNames_;

    /// Whether the user chose to wait for the exports before closing.
    bool closeWhenIdle_ = false;

    /// Desktop preferences used by this application session.
    AppSettings runningSettings_;

    /// Worker that renders the preview.
    ///
    /// Declared last so that it is destroyed first: its destructor joins the
    /// thread, after which no callback can run, so nothing it touches has
    /// been destroyed yet.
    PreviewRenderer previewRenderer_;

    /// Worker that develops and writes exports.
    ///
    /// Declared after everything it reports to, so that it is destroyed
    /// before any of it, and no callback reaches a destroyed window.
    ExportQueue exportQueue_;
};

} // namespace arraw::app
