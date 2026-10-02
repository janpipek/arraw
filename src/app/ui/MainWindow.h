#pragma once

#include <EditSession.h>
#include <ImageBuffer.h>
#include <Photo.h>

#include <QMainWindow>
#include <QTimer>

#include <optional>

class QLabel;
class QEvent;
class QObject;

namespace arraw::app {

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

    /// @brief Builds the view that shows the photograph centred in the window.
    void buildImageView();

    /// @brief Asks the user for a photograph and opens it.
    void openFileWithDialog();

    /// @brief Renders the current photograph again, fitted to the view as it is now.
    ///
    /// Keeps the picture it shows if rendering fails.
    /// @throws std::exception if the photograph cannot be rendered.
    void rerender();

    /// @brief Gives the size of the view in device pixels.
    [[nodiscard]] QSize viewportPixels() const;

    /// @brief Renders a photograph and makes it the one being edited.
    ///
    /// Commits only once rendering has succeeded, so a failure leaves the
    /// window showing the previous photograph, session and pixels together.
    /// @param photo Photograph to show.
    /// @throws std::exception if the photograph cannot be decoded or rendered.
    void showPhoto(Photo photo);

    /// @brief Photograph being edited, with its pixels.
    struct OpenPhoto {
        /// Edit session of the photograph.
        EditSession session;
        /// Pixels, decoded once and developed again for each size.
        ImageBuffer decoded;
    };

    QLabel* imageView_ = nullptr;

    /// Single-shot timer that fires once the view has stopped changing, so that
    /// dragging an edge renders once rather than per pixel.
    QTimer resizeTimer_;

    /// Whether a message about a failed render is on screen.
    bool reportingFailure_ = false;

    /// Photograph being shown; empty until one is opened.
    std::optional<OpenPhoto> open_;
};

} // namespace arraw::app
