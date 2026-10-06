#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <utility>

namespace arraw {

namespace detail {
class ProgressRoot;
} // namespace detail

/// @brief Named part of a decode or a render, in the order a render runs them.
///
/// Engine names, not sentences: a caller words them, as ::arraw::describe
/// words a ::arraw::Notice.
enum class ProgressStep {
    Decode,    ///< Reading the file and, for a RAW, demosaicing it.
    Denoise,   ///< Noise reduction, the first pass (ADR 039).
    Context,   ///< The log-luminance context Texture, Clarity and Dehaze read (ADR 041).
    Pointwise, ///< The fused pointwise chain: colour, tone and the rest (ADR 011).
    Geometry,  ///< Orientation, rotation, straightening and crop.
    Resize,    ///< The resize to the requested size, and the region's cut.
    Effects,   ///< The effects on the crop frame: vignette and grain (ADR 037).
};

/// @brief Number of ::arraw::ProgressStep values.
///
/// Counted from the last one, which ::arraw::ProgressStep::Effects must stay.
inline constexpr std::size_t progressStepCount =
    static_cast<std::size_t>(ProgressStep::Effects) + 1;

/// @brief How far an operation has got.
struct Progress {
    /// @brief Fraction done, from 0 to 1.
    ///
    /// A render measures it against the whole render from the source to the
    /// effects, so a render that resumes from a checkpoint starts where that
    /// checkpoint's boundary stands and one that stops early ends at its own
    /// boundary: a chain of resumes reads as one render (ADR 042).
    double fraction = 0.0;

    /// @brief Part being worked on.
    ProgressStep step = ProgressStep::Decode;
};

/// @brief What an operation throws when it stopped because its channel was cancelled.
///
/// Distinct from every failure, so that a caller can tell "stopped because I
/// asked" from "could not": nothing is wrong with the inputs, and nothing was
/// produced. A runtime error, so that code which only knows to report errors
/// still does not mistake it for a result.
class Cancelled : public std::runtime_error {
public:
    Cancelled();
};

/// @brief The out-of-band channel of one operation: progress out, cancellation in (ADR 011).
///
/// Passed to a decode or a render rather than held by it, as a
/// ::arraw::DiagnosticLog is; an operation passed none runs exactly as before,
/// at no cost and to the same bits.
///
/// Progress is reported on the thread that runs the operation, never on one of
/// its workers and never concurrently with itself, so the callback needs no
/// locking of its own. Reports are thinned: one when the step changes, and
/// otherwise only once the fraction has grown by about a fifth of a percent;
/// the fraction never decreases, and a finished operation reports its end.
/// A callback that throws stops the operation with what it threw.
///
/// Cancellation is cooperative: ::cancel may be called from any thread, and
/// the operation notices between rows of pixels on the CPU and between renders
/// on the GPU, and throws ::arraw::Cancelled. Nothing partial comes out of a
/// cancelled call; checkpoints the caller already holds stay as valid as they
/// were. Once cancelled a channel stays cancelled, and an operation started
/// with it stops at once, so a chain of calls sharing one stops as a whole.
class ProgressChannel {
public:
    /// @brief Receiver of progress reports.
    using Callback = std::function<void(const Progress&)>;

    /// @brief Makes a channel that only carries cancellation.
    ProgressChannel() = default;

    /// @brief Makes a channel that reports progress to a callback.
    /// @param onProgress Called with each report; may be empty.
    explicit ProgressChannel(Callback onProgress) : onProgress_(std::move(onProgress)) {}

    ProgressChannel(const ProgressChannel&) = delete;
    ProgressChannel& operator=(const ProgressChannel&) = delete;
    ProgressChannel(ProgressChannel&&) = delete;
    ProgressChannel& operator=(ProgressChannel&&) = delete;
    ~ProgressChannel() = default;

    /// @brief Asks the operations using this channel to stop; safe from any thread.
    void cancel() noexcept {
        cancelled_.store(true, std::memory_order_relaxed);
    }

    /// @brief Whether ::cancel has been called.
    [[nodiscard]] bool cancelled() const noexcept {
        return cancelled_.load(std::memory_order_relaxed);
    }

private:
    friend class detail::ProgressRoot;

    /// @brief Hands a report to the callback, if there is one; the engine's, already thinned.
    /// @param progress The report.
    void report(const Progress& progress) const {
        if (onProgress_) {
            onProgress_(progress);
        }
    }

    Callback onProgress_;
    std::atomic<bool> cancelled_{false};
};

} // namespace arraw
