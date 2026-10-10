#include "BrushCoverage.h"

#include "BrushRaster.h"
#include "ProgressScope.h"
#include "RenderProgress.h"
#include "RowBands.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <stdexcept>

namespace arraw::detail {

namespace {

/// Makes zeroed planes for a number of brushes.
PackedCoverage emptyPlanes(ImageSize size, std::size_t brushes) {
    PackedCoverage packed;
    packed.size = size;
    const std::size_t planes = (brushes + 3) / 4;
    packed.planes.assign(planes, std::vector<std::uint8_t>(
                                     static_cast<std::size_t>(size.width) * size.height * 4, 0));
    return packed;
}

/// Gives the raster the plan's brushes are for, checking they agree.
ImageSize rasterOf(const LocalPlan& local) {
    ImageSize raster;
    for (const LocalMaskPlan& mask : local.masks) {
        if (mask.kind != LocalMaskKind::Brush) {
            continue;
        }
        if (raster.empty()) {
            raster = mask.brush.raster;
        } else if (!(raster == mask.brush.raster)) {
            throw std::logic_error("the brush masks of a plan are for different rasters");
        }
    }
    return raster;
}

/// Rasterises one brush banded into its channel of the planes.
void bandedInto(const LocalMaskPlan& mask, std::uint32_t bucketRows, PackedCoverage& packed) {
    const ImageSize raster = packed.size;
    const std::vector<PlacedStroke> placed = placedStrokes(*mask.brush.strokes, raster);
    const DabBuckets buckets(placed, raster.height, bucketRows);
    std::vector<std::uint8_t>& plane = packed.planes[mask.brush.slot / 4];
    const std::uint32_t channel = mask.brush.slot % 4;
    const std::uint32_t phase = mask.brush.phase;
    forEachRowBand(
        buckets.count(), bucketRows * raster.width, [&](std::uint32_t first, std::uint32_t last) {
            std::vector<float> scratch;
            for (std::uint32_t b = first; b < last; ++b) {
                const std::uint32_t y0 = b * bucketRows;
                const std::uint32_t rows = std::min(bucketRows, raster.height - y0);
                scratch.assign(static_cast<std::size_t>(rows) * raster.width, 0.0F);
                paintBucket(placed, buckets, b, {0, y0, raster.width, rows}, scratch);
                for (std::uint32_t row = 0; row < rows; ++row) {
                    const float* in = scratch.data() + static_cast<std::size_t>(row) * raster.width;
                    std::uint8_t* out =
                        plane.data() + static_cast<std::size_t>(y0 + row) * raster.width * 4;
                    for (std::uint32_t x = 0; x < raster.width; ++x) {
                        out[static_cast<std::size_t>(x) * 4 + channel] =
                            coverageCode(in[x], x, y0 + row, phase);
                    }
                }
            }
        });
}

/// Quantises the listed tiles of a grid into one slot's channel; a null tile writes zeros.
void quantiseTiles(const CoverageTiles& tiles, std::span<const std::uint32_t> indices,
                   std::uint32_t slot, std::uint32_t phase, PackedCoverage& packed) {
    const ImageSize size = tiles.size();
    const std::uint32_t tileSize = tiles.tileSize();
    const std::uint32_t columns = tiles.columns();
    std::vector<std::uint8_t>& plane = packed.planes[slot / 4];
    const std::uint32_t channel = slot % 4;
    if (indices.empty()) {
        // Nothing changed, but the unit the caller declared for the packing is still counted.
        completeUnit();
    }
    forEachRowBand(
        static_cast<std::uint32_t>(indices.size()), tileSize * tileSize,
        [&](std::uint32_t first, std::uint32_t last) {
            for (std::uint32_t n = first; n < last; ++n) {
                const std::uint32_t index = indices[n];
                const std::uint32_t x0 = index % columns * tileSize;
                const std::uint32_t y0 = index / columns * tileSize;
                const std::uint32_t width = std::min(tileSize, size.width - x0);
                const std::uint32_t height = std::min(tileSize, size.height - y0);
                const auto& tile = tiles.tile(index);
                for (std::uint32_t row = 0; row < height; ++row) {
                    std::uint8_t* out =
                        plane.data() + (static_cast<std::size_t>(y0 + row) * size.width + x0) * 4;
                    const float* in =
                        tile ? tile->values.data() + static_cast<std::size_t>(row) * tile->width
                             : nullptr;
                    for (std::uint32_t i = 0; i < width; ++i) {
                        out[static_cast<std::size_t>(i) * 4 + channel] =
                            in != nullptr ? coverageCode(in[i], x0 + i, y0 + row, phase) : 0;
                    }
                }
            }
        });
}

/// Gives the serial of a tile, zero for an absent one.
std::uint64_t serialOf(const std::shared_ptr<const CoverageTile>& tile) noexcept {
    return tile ? tile->serial : 0;
}

} // namespace

void CoverageResidency::reset() noexcept {
    packed_ = PackedCoverage{};
    for (Record& record : records_) {
        record = Record{};
    }
    pending_.clear();
    tileSize_ = 0;
}

bool CoverageResidency::holds(const BrushCoverageRef& brush) const noexcept {
    if (brush.slot >= records_.size() || !(brush.raster == packed_.size) ||
        packed_.planes.size() <= brush.slot / 4) {
        return false;
    }
    const Record& record = records_[brush.slot];
    return record.identity && *record.identity == brush;
}

std::vector<PixelRect> CoverageResidency::pending(std::uint32_t plane) const {
    std::vector<PixelRect> rects;
    if (plane >= pending_.size() || tileSize_ == 0) {
        return rects;
    }
    const std::uint32_t columns = (packed_.size.width + tileSize_ - 1) / tileSize_;
    for (std::uint32_t index = 0; index < pending_[plane].size(); ++index) {
        if (!pending_[plane][index]) {
            continue;
        }
        const std::uint32_t x = index % columns * tileSize_;
        const std::uint32_t y = index / columns * tileSize_;
        rects.push_back({x, y, std::min(tileSize_, packed_.size.width - x),
                         std::min(tileSize_, packed_.size.height - y)});
    }
    return rects;
}

bool CoverageResidency::hasPending(std::uint32_t plane) const noexcept {
    return plane < pending_.size() &&
           std::ranges::any_of(pending_[plane], [](bool changed) { return changed; });
}

void CoverageResidency::clearPending(std::uint32_t plane) noexcept {
    if (plane < pending_.size()) {
        pending_[plane].assign(pending_[plane].size(), false);
    }
}

const PackedCoverage& CoverageResidency::update(const LocalPlan& local, ImageSize size) {
    return update(local, size, brushCoverageCache());
}

const PackedCoverage& CoverageResidency::update(const LocalPlan& local, ImageSize size,
                                                BrushCoverageCache& cache) {
    if (!(size == packed_.size)) {
        reset();
        packed_.size = size;
    }
    const std::size_t brushes = local.brushCount();
    const std::size_t planeCount = (brushes + 3) / 4;
    const std::size_t planeBytes = static_cast<std::size_t>(size.width) * size.height * 4;
    packed_.planes.resize(planeCount);
    pending_.resize(planeCount);
    for (auto& plane : packed_.planes) {
        if (plane.size() != planeBytes) {
            plane.assign(planeBytes, 0);
        }
    }
    // A slot the plan no longer has holds nothing worth keeping.
    for (std::size_t slot = brushes; slot < records_.size(); ++slot) {
        records_[slot] = Record{};
    }
    for (const LocalMaskPlan& mask : local.masks) {
        if (mask.kind != LocalMaskKind::Brush) {
            continue;
        }
        const BrushCoverageRef& brush = mask.brush;
        if (!(brush.raster == size)) {
            throw std::logic_error("A brush mask is for another raster than the residency's");
        }
        Record& record = records_[brush.slot];
        if (record.identity && *record.identity == brush) {
            continue;
        }
        // One unit of the Coverage step, as ::arraw::detail::coverageUnitWeights counts them,
        // in two: the drawing, then the packing.
        const CoverageWork work = coverageWorkOf(statusOf(brush, nullptr, cache), brush);
        const std::array<double, 2> parts = {work.draw, work.pack};
        const ProgressSpan unit(parts);
        const bool sameDither = record.identity && record.identity->phase == brush.phase;
        std::vector<std::uint64_t> held = std::move(record.serials);
        // Cleared while it is being redone: a cancellation leaves it so, and the next update
        // starts the slot again.
        record = Record{};
        ++cacheCalls_;
        const BrushCoverageCache::Result found = cache.coverage(brush.strokes, size);
        if (found.lookup == CoverageLookup::Hit || found.lookup == CoverageLookup::ContentHit) {
            // Nothing was drawn, whatever was expected: the drawing's unit is done.
            completeUnit();
        }
        const std::shared_ptr<const CoverageTiles> tiles = found.tiles;
        const std::uint32_t count = tiles->columns() * tiles->rows();
        std::vector<std::uint64_t> serials(count);
        std::vector<std::uint32_t> changed;
        for (std::uint32_t index = 0; index < count; ++index) {
            serials[index] = serialOf(tiles->tile(index));
            if (!sameDither || held.size() != count || held[index] != serials[index]) {
                changed.push_back(index);
            }
        }
        quantiseTiles(*tiles, changed, brush.slot, brush.phase, packed_);
        tilesPacked_ += changed.size();
        std::vector<bool>& pending = pending_[brush.slot / 4];
        tileSize_ = tiles->tileSize();
        if (pending.size() != count) {
            // A plane no upload has seen: every tile of it is to be uploaded.
            pending.assign(count, true);
        }
        for (const std::uint32_t index : changed) {
            pending[index] = true;
        }
        record.identity = brush;
        record.serials = std::move(serials);
    }
    return packed_;
}

CoverageStatus statusOf(const BrushCoverageRef& brush, const CoverageResidency* residency,
                        const BrushCoverageCache& cache) {
    if (residency != nullptr && residency->holds(brush)) {
        return {CoverageReadiness::Held, brush.strokes->strokes().size()};
    }
    const BrushCoverageCache::Peek peeked = cache.peek(brush.strokes, brush.raster);
    switch (peeked.lookup) {
    case CoverageLookup::Hit:
    case CoverageLookup::ContentHit:
        return {CoverageReadiness::Cached, peeked.cachedStrokes};
    case CoverageLookup::Extended:
        return {CoverageReadiness::Extended, peeked.cachedStrokes};
    case CoverageLookup::Miss:
        break;
    }
    return {CoverageReadiness::Missing, 0};
}

CoverageStatus statusOf(const BrushCoverageRef& brush, const CoverageResidency* residency) {
    return statusOf(brush, residency, brushCoverageCache());
}

CoverageReadiness readinessOf(const BrushCoverageRef& brush, const CoverageResidency* residency) {
    return statusOf(brush, residency).readiness;
}

BrushCoverageCache& brushCoverageCache() {
    static BrushCoverageCache* const cache = new BrushCoverageCache();
    return *cache;
}

void quantiseInto(const CoverageTiles& tiles, std::uint32_t slot, std::uint32_t phase,
                  PackedCoverage& packed) {
    const ImageSize size = tiles.size();
    const std::uint32_t tileSize = tiles.tileSize();
    const std::uint32_t columns = tiles.columns();
    std::vector<std::uint8_t>& plane = packed.planes[slot / 4];
    const std::uint32_t channel = slot % 4;
    forEachRowBand(size.height, size.width, [&](std::uint32_t first, std::uint32_t last) {
        for (std::uint32_t y = first; y < last; ++y) {
            std::uint8_t* out = plane.data() + static_cast<std::size_t>(y) * size.width * 4;
            for (std::uint32_t column = 0; column < columns; ++column) {
                const auto& tile = tiles.tile((y / tileSize) * columns + column);
                if (!tile) {
                    continue;
                }
                const float* in =
                    tile->values.data() + static_cast<std::size_t>(y % tileSize) * tile->width;
                const std::uint32_t x0 = column * tileSize;
                for (std::uint32_t i = 0; i < tile->width; ++i) {
                    out[static_cast<std::size_t>(x0 + i) * 4 + channel] =
                        coverageCode(in[i], x0 + i, y, phase);
                }
            }
        }
    });
}

PackedCoverage packBanded(const LocalPlan& local, std::uint32_t bucketRows) {
    PackedCoverage packed = emptyPlanes(rasterOf(local), local.brushCount());
    for (const LocalMaskPlan& mask : local.masks) {
        if (mask.kind == LocalMaskKind::Brush) {
            bandedInto(mask, bucketRows, packed);
        }
    }
    return packed;
}

PackedCoverage packCoverage(const LocalPlan& local) {
    PackedCoverage packed = emptyPlanes(rasterOf(local), local.brushCount());
    if (packed.planes.empty()) {
        return packed;
    }
    const ImageSize raster = packed.size;
    const bool retain = raster.pixelCount() <= retainedCoveragePixels;
    for (const LocalMaskPlan& mask : local.masks) {
        if (mask.kind != LocalMaskKind::Brush) {
            continue;
        }
        // One unit of the Coverage step, in two: the drawing, then the packing.
        const CoverageWork work = coverageWorkOf(statusOf(mask.brush, nullptr), mask.brush);
        const std::array<double, 2> parts = {work.draw, work.pack};
        const ProgressSpan unit(parts);
        // Retained sizes go through the cache; larger ones use what it already holds and
        // otherwise draw banded without touching it.
        std::shared_ptr<const CoverageTiles> tiles;
        if (retain) {
            const BrushCoverageCache::Result found =
                brushCoverageCache().coverage(mask.brush.strokes, raster);
            tiles = found.tiles;
            if (found.lookup == CoverageLookup::Hit || found.lookup == CoverageLookup::ContentHit) {
                completeUnit();
            }
        } else {
            tiles = brushCoverageCache().find(mask.brush.strokes, raster);
            if (tiles) {
                completeUnit();
            }
        }
        if (tiles) {
            quantiseInto(*tiles, mask.brush.slot, mask.brush.phase, packed);
        } else {
            bandedInto(mask, coverageBucketRows, packed);
            // Drawing and packing were one loop; the packing's unit is done with it.
            completeUnit();
        }
    }
    return packed;
}

} // namespace arraw::detail
