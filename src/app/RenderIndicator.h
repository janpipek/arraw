#pragma once

#include "RenderActivity.h"

#include <Progress.h>

#include <QObject>
#include <QTimer>

#include <chrono>
#include <functional>

namespace arraw::app {

/// @brief Drives a RenderActivity from real time and announces what to show.
///
/// Owns the activity and a timer that polls it when the display is due to
/// change. The clock is
/// injectable, so a test sets the time itself and calls poll().
class RenderIndicator : public QObject {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(RenderIndicator)
public:
    /// @brief Source of the current time.
    using Clock = std::function<RenderActivity::Clock::time_point()>;

    /// @brief Makes an indicator that reads the steady clock.
    /// @param parent Owner.
    explicit RenderIndicator(QObject* parent = nullptr);

    /// @brief Makes an indicator that reads a given clock, and runs no timer of its own.
    /// @param clock Source of the current time.
    /// @param parent Owner.
    explicit RenderIndicator(Clock clock, QObject* parent = nullptr);

    /// @brief Gives what is shown now.
    [[nodiscard]] const RenderActivity::Display& display() const noexcept {
        return display_;
    }

    /// @brief Tells whether a render is in progress; see RenderActivity::busy.
    [[nodiscard]] bool busy() const noexcept {
        return activity_.busy();
    }

public slots:
    /// @brief Notes that a render was asked for; see RenderActivity::begin.
    void begin();

    /// @brief Notes how far the render in progress has got.
    /// @param fraction Fraction done, from 0 to 1.
    /// @param step Step being worked on.
    void report(double fraction, arraw::ProgressStep step);

    /// @brief Notes that no render is in progress any more.
    /// @param completed Whether it finished, rather than being dropped or failing.
    void finish(bool completed = true);

    /// @brief Brings the display up to the clock's time; emits changed() if it moved.
    void poll();

signals:
    /// Emitted when what is shown changed: it appeared, went, moved or changed step.
    void changed(const arraw::app::RenderActivity::Display& display);

private:
    /// @brief Polls, and arranges the next poll.
    void refresh();

    Clock clock_;
    /// Whether the timer runs; a test with its own clock drives polls by hand.
    bool timed_;
    RenderActivity activity_;
    RenderActivity::Display display_;
    QTimer timer_;
};

} // namespace arraw::app
