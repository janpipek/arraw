#pragma once

#include "RenderActivity.h"

#include <QWidget>

#include <optional>

namespace arraw::app {

/// @brief Status-bar pie chart that shows whether a render is going and how far it is.
///
/// Always visible. A full green pie means up to date; while the indicator says a render is
/// to be shown, a red pie filled with the fraction done, or an empty red one without a
/// fraction yet; with no photograph open, an empty grey ring. The tooltip names the step
/// (ADR 042). A failed render is an empty red ring, tooltip "Render failed: <error>", until
/// a render is shown (setOpened()), a render goes visibly or the photograph changes. A cancelled
/// render delivers nothing, so the pie goes back to "up to date" for it; the window also reports a
/// failure in a message box.
class RenderProgressPie : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(RenderProgressPie)
public:
    /// @brief Steps of the fraction, so that the pie repaints only when it visibly changes.
    static constexpr int resolution = 1000;

    explicit RenderProgressPie(QWidget* parent = nullptr);

    /// @brief Shows what an indicator announced.
    /// @param display What to show.
    void setDisplay(const RenderActivity::Display& display);

    /// @brief Shows that the newest render failed.
    /// @param error What went wrong, for the tooltip.
    void setFailed(const QString& error);

    /// @brief Tells whether the pie shows a failed render.
    [[nodiscard]] bool failed() const noexcept {
        return failed_.has_value();
    }

    /// @brief Tells the pie whether a photograph is open, which sets what idle looks like.
    /// @param open Whether a photograph is open; a pie that is not told assumes one is.
    void setPhotoOpen(bool open);

    /// @brief Starts showing that a photograph is being opened.
    ///
    /// Until setOpened(), the pie is never "up to date": an empty grey ring before a render is
    /// to be shown, then the red pie of the progress, both with the tooltip "Opening <name>…".
    /// A failure shows its error meanwhile but does not end it, as nothing has been rendered yet;
    /// setPhotoOpen(false) does.
    /// @param name Name of the photograph, for the tooltip.
    void setOpening(const QString& name);

    /// @brief Tells that a render is on screen: ends the opening and clears a failure.
    void setOpened();

    /// @brief Tells whether a photograph is being opened.
    [[nodiscard]] bool opening() const noexcept {
        return opening_.has_value();
    }

    /// @brief Tells whether a photograph is open.
    [[nodiscard]] bool photoOpen() const noexcept {
        return photoOpen_;
    }

    /// @brief Gives what is shown now.
    [[nodiscard]] const RenderActivity::Display& display() const noexcept {
        return display_;
    }

    /// @brief Gives the filled fraction of the pie: 1 when it is up to date, 0 with no photograph.
    [[nodiscard]] double filled() const noexcept;

    /// @brief Tells whether the pie shows a render going.
    [[nodiscard]] bool rendering() const noexcept {
        return display_.visible;
    }

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    /// Brings the tooltip and the picture up to the state.
    void refresh();

    /// Whether the picture says "up to date": a photograph is open and not still opening.
    [[nodiscard]] bool upToDate() const noexcept;

    /// Text for the tooltip and the accessible description.
    [[nodiscard]] QString describe() const;

    RenderActivity::Display display_;
    /// Error of the failed render being shown, if any.
    std::optional<QString> failed_;
    /// Whether a photograph is open.
    bool photoOpen_ = true;
    /// Name of the photograph being opened, until its first render is shown.
    std::optional<QString> opening_;
    /// Fill in steps of `resolution`, to tell when the picture changes.
    int shownSteps_ = resolution;
    /// Whether the pie is drawn as rendering.
    bool shownRendering_ = false;
    /// Whether the pie is drawn as failed.
    bool shownFailed_ = false;
    /// Whether the pie is drawn as up to date when idle.
    bool shownUpToDate_ = true;
};

} // namespace arraw::app
