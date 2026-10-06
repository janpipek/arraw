#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace arraw {

/// @brief Where a source pixel's centre falls on a box-reduced grid along one axis.
///
/// The bilinear read of a grid that the Denoise pass's colour blur
/// (ADR 039) and the Presence context (ADR 041) share, and that their shaders
/// mirror as `gridTap`.
struct GridTap {
    std::uint32_t first = 0;  ///< Cell at or before the position.
    std::uint32_t second = 0; ///< The next cell, or the same one at the edge.
    float fraction = 0.0F;    ///< Weight of the second cell.
};

/// @brief Places a source coordinate on a grid: `u = (x + 0.5) / reduction - 0.5`, clamped.
///
/// A cell covers `reduction` source pixels a side, so its centre lies at
/// `(cell + 0.5) * reduction` and the source pixel's centre at `x + 0.5`. Exact
/// in float while the reduction is a power of two.
/// @param coordinate Column or row of the source pixel.
/// @param reduction Source pixels per cell side; a power of two.
/// @param cells Cells along the axis, at least one.
[[nodiscard]] inline GridTap gridTap(std::uint32_t coordinate, std::uint32_t reduction,
                                     std::uint32_t cells) {
    const float position =
        (static_cast<float>(coordinate) + 0.5F) / static_cast<float>(reduction) - 0.5F;
    const float clamped = std::clamp(position, 0.0F, static_cast<float>(cells - 1));
    const float below = std::floor(clamped);
    GridTap tap;
    tap.first = static_cast<std::uint32_t>(below);
    tap.second = std::min(tap.first + 1, cells - 1);
    tap.fraction = clamped - below;
    return tap;
}

/// @brief Gives the number of cells a box reduction of a length makes: the last one may be partial.
[[nodiscard]] constexpr std::uint32_t gridCells(std::uint32_t length,
                                                std::uint32_t reduction) noexcept {
    return (length + reduction - 1) / reduction;
}

} // namespace arraw
