#pragma once

#include "RenderActivity.h"

#include <QWidget>

namespace arraw::app {

/// @brief Status-bar pie chart that shows whether a render is going and how far it is.
///
/// Always visible. A full green pie means up to date; while the indicator says a render is
/// to be shown, a red pie filled with the fraction done, or an empty red one without a
/// fraction yet. The tooltip names the step (ADR 042).
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

    /// @brief Gives what is shown now.
    [[nodiscard]] const RenderActivity::Display& display() const noexcept {
        return display_;
    }

    /// @brief Gives the filled fraction of the pie, 1 when it is up to date.
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
    /// Text for the tooltip and the accessible description.
    [[nodiscard]] QString describe() const;

    RenderActivity::Display display_;
    /// Fill in steps of `resolution`, to tell when the picture changes.
    int shownSteps_ = resolution;
    /// Whether the pie is drawn as rendering.
    bool shownRendering_ = false;
};

} // namespace arraw::app
