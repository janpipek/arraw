#pragma once

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

/// @brief Runs a function over bands of rows, one thread per band.
///
/// Splits the rows `[0, rows)` into contiguous bands of nearly equal height
/// (see ::arraw::detail::rowBandCount) and calls `body(first, last)` for each,
/// the calling thread taking the first band. Every row is in exactly one band,
/// so a body that writes only its own rows and reads only what no band writes
/// gives the same bits however the rows are split: the single-threaded result,
/// which is what keeps the CPU a reference (ADR 039).
///
/// The first exception a body throws is rethrown here once every band has
/// finished.
/// @param rows Rows to cover.
/// @param width Pixels per row, which with @p rows decides how many bands are worth it.
/// @param body Callable `(std::uint32_t first, std::uint32_t last)`, `last` exclusive.
template <typename Body> void forEachRowBand(std::uint32_t rows, std::uint32_t width, Body&& body) {
    if (rows == 0) {
        return;
    }
    const std::uint32_t bands =
        std::min(rowBandCount(static_cast<std::uint64_t>(rows) * width), rows);
    if (bands == 1) {
        body(std::uint32_t{0}, rows);
        return;
    }
    const auto edge = [rows, bands](std::uint32_t band) {
        return static_cast<std::uint32_t>(static_cast<std::uint64_t>(rows) * band / bands);
    };
    std::exception_ptr failure;
    std::mutex failureMutex;
    const auto run = [&](std::uint32_t band) noexcept {
        try {
            body(edge(band), edge(band + 1));
        } catch (...) {
            const std::scoped_lock lock(failureMutex);
            if (!failure) {
                failure = std::current_exception();
            }
        }
    };
    {
        std::vector<std::jthread> workers;
        workers.reserve(bands - 1);
        for (std::uint32_t band = 1; band < bands; ++band) {
            workers.emplace_back(run, band);
        }
        run(0);
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

} // namespace arraw::detail
