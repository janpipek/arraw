#pragma once

#include <Develop.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace arraw {

/// @brief Weights of the source pixels that make up one output coordinate.
struct Taps {
    std::int64_t first = 0; ///< Index of the first source pixel; may lie outside the image.
    std::size_t offset = 0; ///< Position of its weight in the shared weight array.
    std::size_t count = 0;  ///< Number of consecutive source pixels.
};

/// @brief Precomputed taps for every output coordinate along one axis.
struct AxisWeights {
    std::vector<Taps> taps;      ///< One entry per output coordinate.
    std::vector<double> weights; ///< Normalised weights of all taps, back to back.
};

/// @brief Alpha below which a resampled pixel counts as fully transparent (2^-16).
inline constexpr float transparentBelow = 1.0F / 65536.0F;

/// @brief Computes the taps and normalised weights for resizing one axis.
///
/// The one place the kernel, the centre mapping, the widening when shrinking
/// and the normalisation are worked out: the CPU resample reads the result
/// directly, and the GPU one uploads it, so the two cannot disagree on a
/// weight. An output pixel centre at (x + 0.5) maps to the source at
/// (x + 0.5) / scale - 0.5 in pixel-centre coordinates. Taps beyond an edge are
/// left as they are: clamping their index to the image is the caller's.
/// @param in Source length along the axis, at least 1.
/// @param out Result length along the axis, at least 1.
/// @param filter Kernel to evaluate.
/// @return One ::arraw::Taps per output coordinate. With @p in equal to
/// @p out each is the single source pixel of the same index with weight 1.
[[nodiscard]] AxisWeights axisWeights(std::uint32_t in, std::uint32_t out, ResizeFilter filter);

} // namespace arraw
