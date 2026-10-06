#pragma once

#include <Progress.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace arraw::detail {

/// @brief Relative cost of each step of a whole operation, indexed by ::arraw::ProgressStep.
///
/// Zero for a step the operation does not run. Only ratios matter.
using StepWeights = std::array<double, progressStepCount>;

/// @brief Part of an operation's overall fraction that one piece of work fills.
struct ProgressSlot {
    double start = 0.0; ///< Fraction when the work starts.
    double width = 0.0; ///< Fraction the work adds when it is done.

    /// @brief Gives the fraction when the work is done.
    [[nodiscard]] double end() const noexcept {
        return start + width;
    }
};

class ProgressRoot;

/// @brief A part of an observed operation, open on the thread that runs it (ADR 042).
///
/// Spans nest: a step's span is the step's share of the whole operation, and a
/// span opened inside another fills that one's next *unit*. A span divided
/// into units counts each loop over rows (::arraw::detail::forEachRowBand), each
/// GPU render and each span opened inside it as one unit, in order, so a pass
/// that knows its loops says how many there are, or how much each costs, and
/// they share the span accordingly; it must then run exactly those, which a
/// debug build asserts. A span with no units is filled by whatever runs in it,
/// which should then be one loop or one render. A loop or a render outside
/// every step's span, under the operation's own top span, adds nothing to the
/// fraction, though it still notices a cancellation.
///
/// When nothing observes the operation every span is inactive: constructing one
/// reads a thread-local pointer and nothing else.
class ProgressSpan {
public:
    /// @brief Opens a step's span: its share of the operation.
    /// @param step Step this span is; also what its reports name.
    /// @param units Equal units the span is divided into, or zero for none.
    /// @throws ::arraw::Cancelled if the operation's channel is cancelled.
    explicit ProgressSpan(ProgressStep step, std::uint32_t units = 0);

    /// @brief Opens a step's span divided into units of given costs.
    /// @param step Step this span is.
    /// @param unitWeights Relative cost of each unit, in the order they run; must
    /// outlive the span.
    /// @throws ::arraw::Cancelled if the operation's channel is cancelled.
    ProgressSpan(ProgressStep step, std::span<const double> unitWeights);

    /// @brief Opens a span in the next unit of the current one.
    /// @param unitWeights Relative cost of each unit, in the order they run; must
    /// outlive the span.
    /// @throws ::arraw::Cancelled if the operation's channel is cancelled.
    explicit ProgressSpan(std::span<const double> unitWeights);

    ProgressSpan(const ProgressSpan&) = delete;
    ProgressSpan& operator=(const ProgressSpan&) = delete;
    ProgressSpan(ProgressSpan&&) = delete;
    ProgressSpan& operator=(ProgressSpan&&) = delete;

    /// @brief Closes the span: reports its end, unless an exception is leaving it.
    ~ProgressSpan();

    /// @brief Whether an operation is being observed through this span.
    [[nodiscard]] bool active() const noexcept {
        return root_ != nullptr;
    }

    /// @brief Starts the next unit and gives the part of the operation it fills.
    /// @throws ::arraw::Cancelled if the operation's channel is cancelled.
    [[nodiscard]] ProgressSlot beginUnit();

    /// @brief Ends the unit begun last, and reports that.
    void endUnit();

    /// @brief Reports how far a unit has got.
    ///
    /// On the thread that runs the operation only; workers count, and that
    /// thread reports.
    /// @param slot What ::beginUnit gave for the unit.
    /// @param fraction Fraction of the unit done, from 0 to 1.
    void report(const ProgressSlot& slot, double fraction) const;

    /// @brief Whether the operation's channel is cancelled; safe from any thread.
    [[nodiscard]] bool cancelled() const noexcept;

private:
    friend class ProgressRoot;

    /// @brief Opens the top span of a root, covering all of the operation.
    explicit ProgressSpan(ProgressRoot* root) noexcept;

    /// @brief Makes this span the current one, under the one that was.
    void enter();

    ProgressRoot* root_ = nullptr;
    ProgressSpan* parent_ = nullptr;
    ProgressSlot slot_;
    ProgressStep step_ = ProgressStep::Decode;
    std::span<const double> weights_;
    double totalWeight_ = 0.0;
    std::uint32_t units_ = 0;
    std::uint32_t done_ = 0;
    /// Whether this span is a unit of its parent, so ends one when it closes.
    bool isUnit_ = false;
    int exceptions_ = 0;
};

/// @brief Gives the innermost open span on this thread, or null when nothing observes.
[[nodiscard]] ProgressSpan* currentProgress() noexcept;

/// @brief Hiding of the current span from what runs during its lifetime, on this thread.
///
/// What an observed loop wraps each call of its body in, so that a body which
/// itself runs a loop or opens a span runs it unobserved on the calling thread,
/// as it does on the workers, rather than spending the loop's own span's units
/// from one band only.
class HiddenProgress {
public:
    /// @brief Hides the current span.
    HiddenProgress() noexcept;

    HiddenProgress(const HiddenProgress&) = delete;
    HiddenProgress& operator=(const HiddenProgress&) = delete;
    HiddenProgress(HiddenProgress&&) = delete;
    HiddenProgress& operator=(HiddenProgress&&) = delete;

    /// @brief Restores the span hidden.
    ~HiddenProgress();

private:
    ProgressSpan* hidden_;
};

/// @brief Throws ::arraw::Cancelled if the operation on this thread is observed and cancelled.
void throwIfCancelled();

/// @brief Counts one unit of the current span as begun and done: a GPU render.
void completeUnit();

/// @brief The observation of one operation, on the thread that runs it.
///
/// Installed by a public entry point for its whole call: with a channel it
/// opens the top span, and without one it hides any outer operation's, so that
/// nothing an unobserved call runs reports into another.
class ProgressRoot {
public:
    /// @brief Starts observing an operation, or makes sure nothing is.
    /// @param channel The caller's channel, or null.
    /// @param weights Relative cost of each step of the whole operation.
    /// @param first First step this call runs: its start is the first report.
    /// @throws ::arraw::Cancelled if @p channel is already cancelled.
    ProgressRoot(ProgressChannel* channel, const StepWeights& weights, ProgressStep first);

    ProgressRoot(const ProgressRoot&) = delete;
    ProgressRoot& operator=(const ProgressRoot&) = delete;
    ProgressRoot(ProgressRoot&&) = delete;
    ProgressRoot& operator=(ProgressRoot&&) = delete;

    /// @brief Ends the observation, restoring whatever was observed before.
    ~ProgressRoot();

    /// @brief Reports that the operation finished, at the end of the last step it ran.
    /// @param last Last step this call ran.
    void finish(ProgressStep last);

private:
    friend class ProgressSpan;

    /// @brief Gives a step's share of the operation.
    [[nodiscard]] ProgressSlot slotOf(ProgressStep step) const noexcept;

    /// @brief Hands a report to the channel, unless it is too close to the last.
    void publish(double fraction, ProgressStep step, bool force = false);

    ProgressChannel* channel_;
    std::array<ProgressSlot, progressStepCount> slots_{};
    double reported_ = 0.0;
    std::optional<ProgressStep> reportedStep_;
    ProgressSpan* outer_;
    ProgressSpan top_;
};

} // namespace arraw::detail
