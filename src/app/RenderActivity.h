#pragma once

#include <Progress.h>

#include <QString>

#include <chrono>
#include <optional>

namespace arraw::app {

/// @brief Wait before a render that has not finished shows itself.
inline constexpr std::chrono::milliseconds renderActivityDelay{250};

/// @brief Least time a render's indicator stays once it showed.
inline constexpr std::chrono::milliseconds renderActivityHold{300};

/// @brief Least time a completed render's full bar stays after it finished.
///
/// So that a slow render visibly ends rather than vanishing as its image lands.
inline constexpr std::chrono::milliseconds renderActivityFilled{150};

/// @brief Period of the indicator's sweep when there is no fraction to show.
inline constexpr std::chrono::milliseconds renderActivitySweep{1200};

/// @brief Decides when a render in progress is shown, and what is shown of it.
///
/// A pure state machine over time points that the caller supplies, so that the
/// timing can be tested without waiting. A render that finishes within
/// ::arraw::app::renderActivityDelay is never shown, which keeps a fast GPU
/// preview from flickering a bar; one that was shown stays for at least
/// ::arraw::app::renderActivityHold, so that a slow one does not blink away,
/// and one that completed shows its full bar for at least
/// ::arraw::app::renderActivityFilled after it finished.
/// Renders that follow one another (an edit supersedes the render in flight)
/// are one busy period: ::begin while busy keeps what is shown until the newer
/// render reports, which with cancellation is within milliseconds.
class RenderActivity {
public:
    /// @brief Clock the timing is read from.
    using Clock = std::chrono::steady_clock;

    /// @brief What there is to show at a moment.
    struct Display {
        /// Whether to show anything: the bar and the step's text.
        bool visible = false;
        /// Fraction done from 0 to 1, or empty when there is none to show: a sweep.
        std::optional<double> fraction;
        /// Step being worked on, or the one last reported.
        ProgressStep step = ProgressStep::Pointwise;
        /// Position of the sweep within its period, from 0 to 1; only for an empty fraction.
        double sweep = 0.0;

        friend bool operator==(const Display&, const Display&) = default;
    };

    /// @brief Starts a busy period, or, within one, a newer render of it.
    ///
    /// A new busy period has no fraction until its render reports, and the
    /// step ::arraw::ProgressStep::Pointwise, the one a render is named after
    /// before it says otherwise. Within one, the last fraction and step stay
    /// until the newer render reports, so the bar neither falls to a sweep nor
    /// flickers between the two during a slider drag.
    /// @param now Current time.
    void begin(Clock::time_point now);

    /// @brief Takes a report of the render in progress.
    ///
    /// Ignored when nothing is in progress: a late report of a finished render.
    /// @param now Current time.
    /// @param fraction Fraction done, from 0 to 1.
    /// @param step Step being worked on.
    void report(Clock::time_point now, double fraction, ProgressStep step);

    /// @brief Ends the busy period: the render was delivered, or dropped.
    /// @param now Current time.
    /// @param completed Whether a render finished, which fills the bar for the
    /// hold and for at least ::arraw::app::renderActivityFilled; false when it
    /// was cancelled or failed.
    void finish(Clock::time_point now, bool completed = true);

    /// @brief Brings the display up to a moment and gives it.
    /// @param now Current time, not before any earlier call's.
    [[nodiscard]] Display poll(Clock::time_point now);

    /// @brief Gives when the display next changes of its own accord, if it will.
    ///
    /// The moment to poll again, apart from the sweep, which moves continuously
    /// while @ref Display::visible and without a fraction.
    [[nodiscard]] std::optional<Clock::time_point> nextChange() const;

    /// @brief Tells whether a render is in progress.
    [[nodiscard]] bool busy() const noexcept {
        return busy_;
    }

private:
    /// @brief Latches the indicator as shown once the delay passed, and as gone once the hold did.
    void advance(Clock::time_point now);

    /// @brief Gives when a finished, shown indicator goes.
    [[nodiscard]] Clock::time_point hiddenAt() const;

    bool busy_ = false;
    bool shown_ = false;
    Clock::time_point busySince_;
    Clock::time_point shownSince_;
    /// When the last render completed, if it did and was shown: its full bar is held from there.
    std::optional<Clock::time_point> completedAt_;
    std::optional<double> fraction_;
    ProgressStep step_ = ProgressStep::Pointwise;
};

/// @brief Words a step for the status bar, such as "Reducing noise…".
[[nodiscard]] QString renderStepText(ProgressStep step);

} // namespace arraw::app
