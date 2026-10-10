#include "BrushCoverage.h"

#include "BrushRaster.h"
#include "RowBands.h"

#include <algorithm>
#include <cmath>
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

} // namespace

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
        // Retained sizes go through the cache; larger ones use what it already holds and
        // otherwise draw banded without touching it.
        const std::shared_ptr<const CoverageTiles> tiles =
            retain ? brushCoverageCache().coverage(mask.brush.strokes, raster).tiles
                   : brushCoverageCache().find(mask.brush.strokes, raster);
        if (tiles) {
            quantiseInto(*tiles, mask.brush.slot, mask.brush.phase, packed);
        } else {
            bandedInto(mask, coverageBucketRows, packed);
        }
    }
    return packed;
}

} // namespace arraw::detail
