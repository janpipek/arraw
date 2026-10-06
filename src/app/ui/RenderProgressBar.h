#pragma once

#include "RenderActivity.h"

#include <QWidget>

class QPaintEvent;

namespace arraw::app {

/// @brief Thin bar along the top edge of the photo view that shows a render is going.
///
/// Determinate when it has a fraction: a fill in the palette's highlight colour
/// over a faint track. Without one, a short segment sweeps along the track.
/// Takes no mouse events, so it can lie over the view and the crop overlay
/// without taking anything from them. Drawn on whole device pixels, so that it
/// is as crisp at a fractional scale as at 1:1.
class RenderProgressBar : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(RenderProgressBar)
public:
    /// @brief Height of the bar, in logical pixels.
    static constexpr int thickness = 3;

    explicit RenderProgressBar(QWidget* parent = nullptr);

    /// @brief Shows what an indicator announced, or hides the bar.
    /// @param display What to show.
    void setDisplay(const RenderActivity::Display& display);

    /// @brief Gives what is shown now.
    [[nodiscard]] const RenderActivity::Display& display() const noexcept {
        return display_;
    }

    /// @brief Gives the part of the track the bar fills, in whole device pixels, for a width.
    ///
    /// The fill from the left for a fraction, and the segment of the sweep
    /// without one.
    /// @param display What is shown.
    /// @param width Width of the track in device pixels.
    /// @return Left edge and width in device pixels; empty width when nothing is filled.
    [[nodiscard]] static std::pair<int, int> fill(const RenderActivity::Display& display,
                                                  int width);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    RenderActivity::Display display_;
};

} // namespace arraw::app
