#pragma once

#include "PreviewRenderer.h"

#include <EditSession.h>
#include <ImageBuffer.h>
#include <Photo.h>

#include <QMainWindow>
#include <QPointF>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QAction;
class QActionGroup;
class QLabel;
class QToolButton;
class QEvent;
class QShortcut;
class QObject;

namespace arraw::app {

class DevelopPanel;
class PhotoView;

/// @brief Top-level window of the desktop application.
class MainWindow : public QMainWindow {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(MainWindow)
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    /// @brief Schedules a new render when the view changes size or pixel ratio.
    ///
    /// Watching the view rather than the window catches whatever changes the
    /// room it has, and a move to a screen of another pixel ratio.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// @brief Builds the menu bar and the actions it offers.
    void buildMenu();

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

    /// @brief Asks the user for a photograph and opens it.
    void openFileWithDialog();

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

    /// @brief Shows the session's state in the panel and updates the actions.
    void refreshPanel();

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
    };

    PhotoView* photoView_ = nullptr;
    QToolButton* zoomButton_ = nullptr;
    QActionGroup* zoomGroup_ = nullptr;
    /// Fit, then one per preset, in the order of ::arraw::app::zoomPresets.
    std::vector<QAction*> zoomActions_;
    QAction* zoomInAction_ = nullptr;
    QAction* zoomOutAction_ = nullptr;
    QLabel* deviceLabel_ = nullptr;
    DevelopPanel* developPanel_ = nullptr;
    QWidget* developDock_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QShortcut* cancelPickShortcut_ = nullptr;

    /// Whether the next click on the photograph picks a neutral.
    bool picking_ = false;

    /// Single-shot timer that fires once the view has stopped changing, so that
    /// dragging an edge renders once rather than per pixel.
    QTimer resizeTimer_;

    /// Single-shot timer of no delay, so that a drag or a wheel burst asks for
    /// one render per turn of the event loop, however many events it has.
    QTimer interactionTimer_;

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

    /// Worker that renders the preview.
    ///
    /// Declared last so that it is destroyed first: its destructor joins the
    /// thread, after which no callback can run, so nothing it touches has
    /// been destroyed yet.
    PreviewRenderer previewRenderer_;
};

} // namespace arraw::app
