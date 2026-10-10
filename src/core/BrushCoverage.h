#pragma once

#include "BrushCoverageCache.h"
#include "LocalPlan.h"

#include <ImageBuffer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace arraw::detail {

/// Largest raster, in pixels, whose coverage a direct render keeps in the cache (2048 x 2048):
/// the sizes the window renders, its curve histogram, thumbnails and previews. Larger rasters,
/// such as an export, never fill the cache (ADR 044, section 6).
inline constexpr std::uint64_t retainedCoveragePixels = std::uint64_t{2048} * 2048;

/// Rows per bucket of the banded raster.
inline constexpr std::uint32_t coverageBucketRows = 64;

/// The 4 x 4 Bayer matrix, row-major.
inline constexpr std::array<std::uint8_t, 16> bayer4 = {0, 8,  2, 10, 12, 4, 14, 6,
                                                        3, 11, 1, 9,  15, 7, 13, 5};

/// Gives the dither threshold of a pixel, between 0 and 1 exclusive.
/// @param x Column of the raster pixel.
/// @param y Row of the raster pixel.
/// @param phase The mask's index in the state's list, 0 to 15: a distinct shift of the matrix.
[[nodiscard]] inline float ditherThreshold(std::uint32_t x, std::uint32_t y,
                                           std::uint32_t phase) noexcept {
    return (static_cast<float>(bayer4[((y + (phase >> 2)) & 3U) * 4U + ((x + phase) & 3U)]) +
            0.5F) /
           16.0F;
}

/// Quantises one coverage value to its 8-bit code with the pixel's dither threshold.
///
/// `min(255, floor(clamp(m, 0, 1) * 255 + threshold))`: 0 gives 0 and 1 gives 255 exactly. Float
/// arithmetic without contraction.
[[nodiscard]] inline std::uint8_t coverageCode(float m, std::uint32_t x, std::uint32_t y,
                                               std::uint32_t phase) noexcept {
    // Written so that NaN gives 0 rather than an undefined cast.
    const float clamped = !(m > 0.0F) ? 0.0F : (m < 1.0F ? m : 1.0F);
    const float scaled = clamped * 255.0F + ditherThreshold(x, y, phase);
    return static_cast<std::uint8_t>(scaled >= 255.0F ? 255.0F : std::floor(scaled));
}

/// The packed, dithered coverage of a plan's brushes: slot `s` is channel `s % 4` of plane
/// `s / 4`, each plane `width * height * 4` bytes, RGBA, row-major (a texture's upload layout).
struct PackedCoverage {
    ImageSize size;                                ///< The raster's size.
    std::vector<std::vector<std::uint8_t>> planes; ///< `ceil(brushes / 4)` planes.

    /// Reads one slot's code at a pixel.
    [[nodiscard]] std::uint8_t code(std::uint32_t slot, std::uint32_t x,
                                    std::uint32_t y) const noexcept {
        return planes[slot / 4][(static_cast<std::size_t>(y) * size.width + x) * 4 + slot % 4];
    }

    /// Reads the codes of the slots in use at a pixel.
    [[nodiscard]] PixelCoverage at(std::uint32_t x, std::uint32_t y,
                                   std::uint32_t slots) const noexcept {
        PixelCoverage codes;
        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            codes.codes[slot] = code(slot, x, y);
        }
        return codes;
    }
};

/// Gives the process-wide float coverage cache (512 MiB, 128-pixel tiles).
///
/// Allocated once and never destroyed, so a render thread still running during static
/// destruction, such as Python's at interpreter exit, cannot touch a dead cache.
[[nodiscard]] BrushCoverageCache& brushCoverageCache();

/// Packs the coverage of a plan's brushes, outside any ladder (the direct path, ADR 044).
///
/// A raster of at most ::arraw::detail::retainedCoveragePixels goes through the cache and is
/// retained there. A larger one takes the cache's coverage only if it is already held, and is
/// otherwise rasterised banded (::arraw::detail::packBanded) without touching the cache. The
/// bits are `quantise(rasteriseBrush)` whichever way it was served.
/// @param local Plan whose brush masks are packed; the raster is the masks' own.
/// @return The packed planes, none when the plan has no brush.
/// @throws ::arraw::Cancelled if the observed operation is cancelled.
[[nodiscard]] PackedCoverage packCoverage(const LocalPlan& local);

/// Rasterises the brushes of a plan banded, straight into packed planes, outside the cache.
///
/// Strokes are placed once; the dabs are indexed by buckets of rows; each bucket is painted onto
/// float scratch of its rows, then quantised into the plane. Peak memory is the planes and one
/// bucket's floats per thread.
/// @param bucketRows Rows per bucket; the bits do not depend on it.
[[nodiscard]] PackedCoverage packBanded(const LocalPlan& local,
                                        std::uint32_t bucketRows = coverageBucketRows);

/// Quantises float coverage into one slot's channel of packed planes.
/// @param tiles The coverage; null tiles are all zero.
/// @param slot Slot to write.
/// @param phase Dither phase.
/// @param packed Planes to write, of the tiles' size.
void quantiseInto(const CoverageTiles& tiles, std::uint32_t slot, std::uint32_t phase,
                  PackedCoverage& packed);

} // namespace arraw::detail
