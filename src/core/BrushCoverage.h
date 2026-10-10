#pragma once

#include "BrushCoverageCache.h"
#include "LocalPlan.h"

#include <ImageBuffer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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

/// What a render would have to do for one brush's coverage (ADR 044, section 8).
enum class CoverageReadiness {
    Held,     ///< A ladder's residency holds it: nothing to do.
    Cached,   ///< The cache holds it, whole or as an equal list: only packing.
    Extended, ///< The cache holds a prefix of the list: its new strokes are drawn.
    Missing,  ///< Nothing usable is held: it is drawn from nothing.
};

/// Keeps the packed, dithered coverage of one ladder's source, tile by tile (ADR 044, section 8).
///
/// One residency per ladder, made by `LadderAccess::coverage` and dropped when the ladder rebinds
/// or clears. It holds the host planes the CPU pass reads, one record per slot of what that slot's
/// channel holds, and per plane the tiles changed since a GPU upload last completed.
/// Not thread-safe, like the ladder.
class CoverageResidency {
public:
    /// Brings the packed planes up to a plan's brushes and gives them.
    ///
    /// A brush whose reference equals its slot's record is left alone (a delta drag). Otherwise
    /// its coverage comes from the cache and only the tiles whose serial differs from the record's
    /// (all of them when the slot was empty or its phase differs) are quantised and marked pending.
    /// @param local Plan whose brush masks are packed; every raster must be @p size.
    /// @param size Size of the source the ladder renders.
    /// @param cache Cache the coverage comes from; the process-wide one unless a test says so.
    /// @return The planes, of @p size; valid until the next call.
    /// @throws ::arraw::Cancelled if the observed operation is cancelled; the record of the slot
    /// being updated is then cleared, those of the earlier slots stay valid.
    const PackedCoverage& update(const LocalPlan& local, ImageSize size, BrushCoverageCache& cache);

    /// Brings the packed planes up to a plan's brushes, from the process-wide cache.
    const PackedCoverage& update(const LocalPlan& local, ImageSize size);

    /// The planes as the last update left them.
    [[nodiscard]] const PackedCoverage& packed() const noexcept {
        return packed_;
    }

    /// Tells whether a slot's channel holds exactly the coverage a brush reference names.
    [[nodiscard]] bool holds(const BrushCoverageRef& brush) const noexcept;

    /// Gives the rectangles (whole tiles) of a plane changed since its upload last completed.
    [[nodiscard]] std::vector<PixelRect> pending(std::uint32_t plane) const;

    /// Tells whether a plane has changed tiles no upload has taken yet.
    [[nodiscard]] bool hasPending(std::uint32_t plane) const noexcept;

    /// Forgets a plane's pending tiles: an upload of it completed.
    void clearPending(std::uint32_t plane) noexcept;

    /// Number of calls of the cache this residency has made (for tests).
    [[nodiscard]] std::uint64_t cacheCalls() const noexcept {
        return cacheCalls_;
    }

    /// Number of tiles quantised, counting each slot's tile (for tests).
    [[nodiscard]] std::uint64_t tilesPacked() const noexcept {
        return tilesPacked_;
    }

private:
    /// What one slot's channel holds.
    struct Record {
        std::optional<BrushCoverageRef> identity; ///< Empty when the channel holds nothing valid.
        std::vector<std::uint64_t> serials;       ///< Per tile; 0 for an absent, all-zero tile.
    };

    /// Drops the planes, the records and the pending tiles.
    void reset() noexcept;

    PackedCoverage packed_;
    std::array<Record, maximumLocalAdjustments> records_;
    std::vector<std::vector<bool>> pending_; ///< Per plane, per tile: changed since the upload.
    std::uint32_t tileSize_ = 0;             ///< Tile edge of the pending grids; 0 if none.
    std::uint64_t cacheCalls_ = 0;
    std::uint64_t tilesPacked_ = 0;
};

/// What a render would have to do for one brush's coverage, with how much of its list is drawn.
struct CoverageStatus {
    CoverageReadiness readiness = CoverageReadiness::Missing; ///< The work to do.
    /// Strokes of the list already drawn in the cache: the prefix of an `Extended` brush, the
    /// whole list of a `Cached` one, else 0.
    std::size_t cachedStrokes = 0;
};

/// Gives the process-wide float coverage cache (512 MiB, 128-pixel tiles).
///
/// Allocated once and never destroyed, so a render thread still running during static
/// destruction, such as Python's at interpreter exit, cannot touch a dead cache.
[[nodiscard]] BrushCoverageCache& brushCoverageCache();

/// Tells what a render would have to do for a brush's coverage.
/// @param brush The brush's reference.
/// @param residency The ladder's residency, or null for a direct render.
/// @param cache Cache the coverage would come from.
[[nodiscard]] CoverageStatus statusOf(const BrushCoverageRef& brush,
                                      const CoverageResidency* residency,
                                      const BrushCoverageCache& cache);

/// Tells what a render would have to do for a brush's coverage, with the process-wide cache.
[[nodiscard]] CoverageStatus statusOf(const BrushCoverageRef& brush,
                                      const CoverageResidency* residency);

/// Tells what a render would have to do for a brush's coverage.
/// @param brush The brush's reference.
/// @param residency The ladder's residency, or null for a direct render.
[[nodiscard]] CoverageReadiness readinessOf(const BrushCoverageRef& brush,
                                            const CoverageResidency* residency);

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
