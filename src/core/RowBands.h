#pragma once

#include "ProgressScope.h"

#include <Progress.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace arraw::detail {

/// @brief Fewest pixels a band of rows is worth a thread for.
///
/// Starting a thread costs tens of microseconds; below this a band's work
/// is not much more, so a small image, such as a thumbnail's level, runs on
/// fewer threads, or on the calling one alone.
inline constexpr std::uint64_t minimumPixelsPerBand = std::uint64_t{1} << 16;

/// @brief Most bands any work is split into, or zero for as many as there are hardware threads.
///
/// A knob for tests, which set it to one to get the single-threaded result
/// and compare; nothing else changes it.
inline std::atomic<std::uint32_t> rowBandLimit{0};

/// @brief Gives how many bands of rows an image of some pixels is split into.
/// @param pixels Pixels the work covers.
/// @return From one to the number of hardware threads (one if that is unknown),
/// and at most ::arraw::detail::rowBandLimit when it is set.
[[nodiscard]] inline std::uint32_t rowBandCount(std::uint64_t pixels) noexcept {
    const std::uint32_t limit = rowBandLimit.load(std::memory_order_relaxed);
    const std::uint64_t threads =
        limit > 0 ? limit : std::max(1U, std::thread::hardware_concurrency());
    return static_cast<std::uint32_t>(
        std::clamp<std::uint64_t>(pixels / minimumPixelsPerBand, std::uint64_t{1}, threads));
}

/// @brief Gives how many rows of some width a band runs between looks at its progress.
///
/// About ::arraw::detail::minimumPixelsPerBand of work: a millisecond or a few
/// of the costliest loops, so a cancellation is noticed that soon, and few
/// enough looks that they cost nothing measurable.
[[nodiscard]] inline std::uint32_t rowsPerProgressChunk(std::uint32_t width) noexcept {
    return static_cast<std::uint32_t>(
        std::max<std::uint64_t>(1, minimumPixelsPerBand / std::max<std::uint32_t>(width, 1)));
}

/// @brief Gives the calling thread's scheduling priority, for band threads to take on.
///
/// Linux keeps a thread's nice value across `pthread_create`, so a band thread
/// runs at its caller's priority already; Windows starts every thread at normal
/// priority, and macOS is not relied on to pass the quality-of-service class
/// on. A worker that lowered its own priority, such as the thumbnail worker
/// (ADR 039), would otherwise run its bands at full priority there.
/// @return The priority in the OS's terms on Windows and macOS; zero on Linux.
[[nodiscard]] int callerThreadPriority() noexcept;

/// @brief Gives the calling thread a priority ::arraw::detail::callerThreadPriority gave.
///
/// Does nothing on Linux, or when the priority is already that one. A failure
/// changes nothing that matters and is ignored.
/// @param priority Priority to take on.
void adoptThreadPriority(int priority) noexcept;

/// @brief Splits rows into bands and runs each on a thread of its own.
///
/// The machinery under ::arraw::detail::forEachRowBand: calls
/// `band(first, last, onCaller)` for each band, the calling thread taking the
/// first (`onCaller` true), and rethrows the first exception once every band
/// has finished.
template <typename Band> void splitRowBands(std::uint32_t rows, std::uint32_t width, Band&& band) {
    const std::uint32_t bands =
        std::min(rowBandCount(static_cast<std::uint64_t>(rows) * width), rows);
    if (bands == 1) {
        band(std::uint32_t{0}, rows, true);
        return;
    }
    const auto edge = [rows, bands](std::uint32_t index) {
        return static_cast<std::uint32_t>(static_cast<std::uint64_t>(rows) * index / bands);
    };
    std::exception_ptr failure;
    std::mutex failureMutex;
    const auto run = [&](std::uint32_t index) noexcept {
        try {
            band(edge(index), edge(index + 1), index == 0);
        } catch (...) {
            const std::scoped_lock lock(failureMutex);
            if (!failure) {
                failure = std::current_exception();
            }
        }
    };
    {
        const int priority = callerThreadPriority();
        std::vector<std::jthread> workers;
        workers.reserve(bands - 1);
        for (std::uint32_t index = 1; index < bands; ++index) {
            workers.emplace_back(
                [&run, priority](std::uint32_t band) {
                    adoptThreadPriority(priority);
                    run(band);
                },
                index);
        }
        run(0);
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

/// @brief Runs one band's rows in chunks, counting them and looking for a cancellation between.
///
/// What an observed loop does in each band (ADR 042): the body sees the same
/// rows, only in more calls, which by the contract of
/// ::arraw::detail::forEachRowBand gives the same bits. Only the calling thread
/// reports; every band counts.
/// @param stop Raised when another band failed, so this one stops too.
template <typename Body>
void runObservedBand(ProgressSpan& observed, const ProgressSlot& slot, std::uint32_t rows,
                     std::uint32_t chunk, std::atomic<std::uint64_t>& done, std::atomic<bool>& stop,
                     std::uint32_t first, std::uint32_t last, bool onCaller, Body& body) {
    try {
        std::uint32_t row = first;
        while (row < last) {
            if (stop.load(std::memory_order_relaxed)) {
                return;
            }
            if (observed.cancelled()) {
                throw Cancelled();
            }
            const std::uint32_t next = last - row > chunk ? row + chunk : last;
            {
                // Unobserved inside, on this thread as on the workers.
                const HiddenProgress hidden;
                body(row, next);
            }
            const std::uint64_t total =
                done.fetch_add(next - row, std::memory_order_relaxed) + (next - row);
            if (onCaller) {
                observed.report(slot, static_cast<double>(total) / rows);
            }
            row = next;
        }
    } catch (...) {
        stop.store(true, std::memory_order_relaxed);
        throw;
    }
}

/// @brief Runs a function over bands of rows, one thread per band.
///
/// Splits the rows `[0, rows)` into contiguous bands of nearly equal height
/// (see ::arraw::detail::rowBandCount) and calls `body(first, last)` for each,
/// the calling thread taking the first band. Every row is in exactly one band,
/// so a body that writes only its own rows and reads only what no band writes
/// gives the same bits however the rows are split: the single-threaded result,
/// which is what keeps the CPU a reference (ADR 039).
///
/// When the operation is observed (ADR 042) the loop is one unit of the
/// current ::arraw::detail::ProgressSpan: each band runs its rows in chunks of
/// about ::arraw::detail::minimumPixelsPerBand pixels, between which it counts
/// them, the calling thread reports, and every band looks for a cancellation.
/// Unobserved, it is exactly the loop it was.
///
/// The first exception a body throws is rethrown here once every band has
/// finished; observed, the other bands stop at their next chunk.
/// @param rows Rows to cover.
/// @param width Pixels per row, which with @p rows decides how many bands are worth it.
/// @param body Callable `(std::uint32_t first, std::uint32_t last)`, `last` exclusive.
/// @throws ::arraw::Cancelled if the observed operation is cancelled.
template <typename Body> void forEachRowBand(std::uint32_t rows, std::uint32_t width, Body&& body) {
    if (rows == 0) {
        return;
    }
    ProgressSpan* observed = currentProgress();
    if (observed == nullptr) {
        splitRowBands(rows, width,
                      [&body](std::uint32_t first, std::uint32_t last, bool /*onCaller*/) {
                          body(first, last);
                      });
        return;
    }
    const ProgressSlot slot = observed->beginUnit();
    const std::uint32_t chunk = rowsPerProgressChunk(width);
    std::atomic<std::uint64_t> done{0};
    std::atomic<bool> stop{false};
    splitRowBands(rows, width, [&](std::uint32_t first, std::uint32_t last, bool onCaller) {
        runObservedBand(*observed, slot, rows, chunk, done, stop, first, last, onCaller, body);
    });
    observed->endUnit();
}

/// @brief Runs a function over the rows of an image on the calling thread.
///
/// For the loops that are not banded across threads: unobserved it is one call
/// of `body(0, rows)`; observed it is one unit of the current
/// ::arraw::detail::ProgressSpan, run in chunks as a band of
/// ::arraw::detail::forEachRowBand is. The body must give the same bits
/// however its rows are split, as there.
/// @param rows Rows to cover.
/// @param width Pixels of work per row, which sizes the chunks.
/// @param body Callable `(std::uint32_t first, std::uint32_t last)`, `last` exclusive.
/// @throws ::arraw::Cancelled if the observed operation is cancelled.
template <typename Body>
void forEachRowInTurn(std::uint32_t rows, std::uint32_t width, Body&& body) {
    if (rows == 0) {
        return;
    }
    ProgressSpan* observed = currentProgress();
    if (observed == nullptr) {
        body(std::uint32_t{0}, rows);
        return;
    }
    const ProgressSlot slot = observed->beginUnit();
    std::atomic<std::uint64_t> done{0};
    std::atomic<bool> stop{false};
    runObservedBand(*observed, slot, rows, rowsPerProgressChunk(width), done, stop, 0, rows, true,
                    body);
    observed->endUnit();
}

} // namespace arraw::detail
