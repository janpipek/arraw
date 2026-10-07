#pragma once

#include "RenderActivity.h"

#include <QWidget>

class QLabel;
class QProgressBar;

namespace arraw::app {

/// @brief Status-bar widget that shows a render is going: the step's name and its progress.
///
/// A label with the step ("Reducing noise…") beside a progress bar with the percentage, or a
/// busy bar when there is no fraction yet. Both are hidden while the indicator says nothing
/// is to be shown (ADR 042).
class RenderProgressBar : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(RenderProgressBar)
public:
    /// @brief Steps of the bar from empty to full.
    static constexpr int resolution = 1000;

    explicit RenderProgressBar(QWidget* parent = nullptr);

    /// @brief Shows what an indicator announced, or hides the widget.
    /// @param display What to show.
    void setDisplay(const RenderActivity::Display& display);

    /// @brief Gives what is shown now.
    [[nodiscard]] const RenderActivity::Display& display() const noexcept {
        return display_;
    }

    /// @brief Gives the label that names the step.
    [[nodiscard]] QLabel& stepLabel() const noexcept {
        return *step_;
    }

    /// @brief Gives the bar that shows the fraction done.
    [[nodiscard]] QProgressBar& bar() const noexcept {
        return *bar_;
    }

private:
    RenderActivity::Display display_;
    /// Name of the step being worked on.
    QLabel* step_ = nullptr;
    /// Fraction done; busy without one.
    QProgressBar* bar_ = nullptr;
};

} // namespace arraw::app
