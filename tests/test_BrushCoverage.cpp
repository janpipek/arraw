// Packing a brush's coverage: the quantiser, the banded raster, the cache's lookups and the
// retention bound (ADR 044, sections 6 and 7; brush plan sections 2.3 to 2.5).

#include "BrushCoverage.h"
#include "BrushCoverageCache.h"
#include "BrushRaster.h"
#include "LocalPlan.h"
#include "ProgressScope.h"
#include "support/BrushGenerators.h"
#include "support/RowBandLimit.h"

#include <DevelopState.h>
#include <LocalAdjustmentEdits.h>
#include <LocalAdjustments.h>
#include <Progress.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <utility>
#include <vector>

using namespace arraw;
using namespace arraw::detail;
using namespace arraw::test;

namespace {

constexpr std::uint64_t seed = 20261010;

/// A state of brushes, each with an Exposure delta so that the plan keeps it.
DevelopState brushesOf(const std::vector<std::shared_ptr<const StrokeList>>& lists) {
    DevelopState state;
    for (const auto& list : lists) {
        LocalAdjustment adjustment;
        adjustment.shape = BrushMask{list};
        adjustment.deltas.exposure = 1.0F;
        state = withLocalAdjustmentAdded(std::move(state), adjustment);
    }
    return state;
}

/// Quantises float coverage as the packed planes should hold it, by the formula alone.
std::uint8_t expectedCode(float m, std::uint32_t x, std::uint32_t y, std::uint32_t phase) {
    const float threshold =
        (static_cast<float>(bayer4[((y + (phase >> 2)) & 3U) * 4U + ((x + phase) & 3U)]) + 0.5F) /
        16.0F;
    const float scaled = std::clamp(m, 0.0F, 1.0F) * 255.0F + threshold;
    return static_cast<std::uint8_t>(std::min(255.0F, std::floor(scaled)));
}

/// Whether every packed code of a plan's brushes is the quantised reference rasteriser's.
bool matchesReference(const LocalPlan& local, const PackedCoverage& packed) {
    for (const LocalMaskPlan& mask : local.masks) {
        if (mask.kind != LocalMaskKind::Brush) {
            continue;
        }
        const CoveragePlane plane = rasteriseBrush(*mask.brush.strokes, mask.brush.raster);
        for (std::uint32_t y = 0; y < plane.size.height; ++y) {
            for (std::uint32_t x = 0; x < plane.size.width; ++x) {
                const float m = plane.values[static_cast<std::size_t>(y) * plane.size.width + x];
                if (packed.code(mask.brush.slot, x, y) != expectedCode(m, x, y, mask.brush.phase)) {
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace

TEST_CASE("The quantiser's golden codes", "[brush][quantiser]") {
    // m = 0.5 is 127.5: the code is 127 where the threshold is below one half, else 128. The
    // first row of the matrix is 0, 8, 2, 10: thresholds 1/32, 17/32, 5/32, 21/32.
    const std::array<std::uint8_t, 4> phaseZero{127, 128, 127, 128};
    for (std::uint32_t x = 0; x < 4; ++x) {
        CHECK(coverageCode(0.5F, x, 0, 0) == phaseZero[x]);
    }
    // Phase 1 shifts the matrix one column: x = 0 reads the 8.
    const std::array<std::uint8_t, 4> phaseOne{128, 127, 128, 127};
    for (std::uint32_t x = 0; x < 4; ++x) {
        CHECK(coverageCode(0.5F, x, 0, 1) == phaseOne[x]);
    }
    // Phase 4 shifts the matrix one row (and no column): row 0 reads the matrix's row 1.
    const std::array<std::uint8_t, 4> phaseFour{128, 127, 128, 127};
    for (std::uint32_t x = 0; x < 4; ++x) {
        CHECK(coverageCode(0.5F, x, 0, 4) == phaseFour[x]);
    }
    // Further values, against the formula, over all phases and a 4 x 4 block of pixels.
    const std::array<float, 9> values{0.0F,  1.0F / 255.0F, 0.1F, 0.25F,     0.5F,
                                      0.75F, 0.99F,         1.0F, 0.3333333F};
    for (std::uint32_t phase = 0; phase < 16; ++phase) {
        for (const float m : values) {
            for (std::uint32_t y = 0; y < 4; ++y) {
                for (std::uint32_t x = 0; x < 4; ++x) {
                    INFO("m " << m << " phase " << phase << " at " << x << ", " << y);
                    CHECK(coverageCode(m, x, y, phase) == expectedCode(m, x, y, phase));
                }
            }
        }
    }
}

TEST_CASE("Zero is code zero and one is code 255 at every pixel and phase", "[brush][quantiser]") {
    for (std::uint32_t phase = 0; phase < 16; ++phase) {
        for (std::uint32_t y = 0; y < 8; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                REQUIRE(coverageCode(0.0F, x, y, phase) == 0);
                REQUIRE(coverageCode(1.0F, x, y, phase) == 255);
                // Out-of-range values clamp.
                REQUIRE(coverageCode(-0.5F, x, y, phase) == 0);
                REQUIRE(coverageCode(1.5F, x, y, phase) == 255);
            }
        }
    }
    REQUIRE(coverageWeight(0) == 0.0F);
    REQUIRE(coverageWeight(255) == 1.0F);
}

TEST_CASE("The sixteen phases are sixteen distinct shifts of the matrix", "[brush][quantiser]") {
    std::set<std::array<float, 16>> matrices;
    for (std::uint32_t phase = 0; phase < 16; ++phase) {
        std::array<float, 16> m{};
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                m[y * 4 + x] = ditherThreshold(x, y, phase);
            }
        }
        // Each is the matrix's thresholds, every value once.
        std::array<float, 16> sorted = m;
        std::ranges::sort(sorted);
        for (std::size_t i = 0; i < 16; ++i) {
            REQUIRE(sorted[i] == (static_cast<float>(i) + 0.5F) / 16.0F);
        }
        matrices.insert(m);
    }
    REQUIRE(matrices.size() == 16);
}

TEST_CASE("The banded raster is the quantised reference at any thread count and bucket height",
          "[brush][banded]") {
    const ImageSize size{800, 700};
    const auto list = paintedMask(seed, 6, everydayStyle, 700.0 / 800.0);
    const auto second = paintedMask(seed + 1, 3, detailStyle, 700.0 / 800.0);
    // Five brushes, so that the planes hold more than four slots; two are the interesting ones.
    const DevelopState state = brushesOf({list, second, emptyStrokeList(), list, second});
    const LocalPlan local = localPlanFor(state, size);
    REQUIRE(local.brushCount() == 5);

    const std::uint32_t threads = GENERATE(1U, 2U, 8U);
    const std::uint32_t bucketRows = GENERATE(1U, 64U, 1000U);
    const bool observed = GENERATE(false, true);
    CAPTURE(threads, bucketRows, observed);
    const ScopedRowBandLimit limit(threads);
    ProgressChannel channel;
    std::optional<ProgressRoot> root;
    if (observed) {
        StepWeights weights{};
        weights[static_cast<std::size_t>(ProgressStep::Pointwise)] = 1.0;
        root.emplace(&channel, weights, ProgressStep::Pointwise);
    }
    const PackedCoverage packed = packBanded(local, bucketRows);
    REQUIRE(packed.planes.size() == 2);
    REQUIRE(matchesReference(local, packed));
}

TEST_CASE("A banded raster of an empty or a distant brush is zero", "[brush][banded]") {
    const ImageSize size{64, 48};
    const auto distant = std::make_shared<const StrokeList>(
        std::vector<Stroke>{straightStroke({2.5F, 2.5F}, {2.9F, 2.9F}, 3, 0.01F, 1.0F, 1.0F)});
    const LocalPlan local = localPlanFor(brushesOf({emptyStrokeList(), distant}), size);
    const PackedCoverage packed = packBanded(local);
    REQUIRE(packed.planes.size() == 1);
    REQUIRE(std::ranges::all_of(packed.planes[0], [](std::uint8_t code) { return code == 0; }));
}

TEST_CASE("A plan without a brush packs nothing", "[brush][banded]") {
    const LocalPlan none = localPlanFor(DevelopState{}, ImageSize{10, 10});
    REQUIRE(packCoverage(none).planes.empty());
    REQUIRE(packBanded(none).planes.empty());
}

TEST_CASE("The cache's find returns hits only and never inserts", "[brush][cache]") {
    BrushCoverageCache cache(std::size_t{64} << 20, 64);
    const ImageSize size{300, 200};
    const auto list = paintedMask(seed + 2, 3, everydayStyle, 200.0 / 300.0);
    REQUIRE(cache.find(list, size) == nullptr);
    REQUIRE(cache.entryCount() == 0);

    const auto made = cache.coverage(list, size);
    REQUIRE(cache.entryCount() == 1);
    REQUIRE(cache.find(list, size) == made.tiles);
    // Another size is not a hit, and neither is a list that only extends the held one.
    REQUIRE(cache.find(list, {301, 200}) == nullptr);
    const auto longer =
        list->appended(straightStroke({0.2F, 0.2F}, {0.4F, 0.4F}, 4, 0.02F, 0.5F, 1.0F));
    REQUIRE(cache.find(longer, size) == nullptr);
    // An equal list behind other pointers is a hit.
    const auto copy =
        std::make_shared<const StrokeList>(std::vector<Stroke>{(*list)[0], (*list)[1], (*list)[2]});
    REQUIRE(copy != list);
    REQUIRE(cache.find(copy, size) == made.tiles);
    REQUIRE(cache.entryCount() == 1);
}

TEST_CASE("Tile serials are unique, and an extension keeps those of the tiles it leaves",
          "[brush][cache]") {
    BrushCoverageCache cache(std::size_t{64} << 20, 64);
    const ImageSize size{512, 256};
    // Two strokes in the left half, then one in the right.
    const auto first = std::make_shared<const StrokeList>(
        std::vector<Stroke>{straightStroke({0.05F, 0.1F}, {0.3F, 0.1F}, 5, 0.01F, 1.0F, 1.0F),
                            straightStroke({0.05F, 0.2F}, {0.3F, 0.3F}, 5, 0.01F, 1.0F, 1.0F)});
    const auto base = cache.coverage(first, size);
    std::set<std::uint64_t> serials;
    std::uint32_t made = 0;
    for (std::uint32_t i = 0; i < base.tiles->columns() * base.tiles->rows(); ++i) {
        if (const auto& tile = base.tiles->tile(i)) {
            REQUIRE(tile->serial != 0);
            serials.insert(tile->serial);
            ++made;
        }
    }
    REQUIRE(made > 0);
    REQUIRE(serials.size() == made);

    const auto extended =
        first->appended(straightStroke({0.8F, 0.6F}, {0.95F, 0.9F}, 4, 0.01F, 1.0F, 1.0F));
    const auto next = cache.coverage(extended, size);
    REQUIRE(next.lookup == CoverageLookup::Extended);
    std::uint32_t kept = 0;
    std::uint32_t added = 0;
    for (std::uint32_t i = 0; i < next.tiles->columns() * next.tiles->rows(); ++i) {
        const auto& tile = next.tiles->tile(i);
        if (!tile) {
            continue;
        }
        if (serials.contains(tile->serial)) {
            // A tile the new stroke did not touch is the same tile.
            REQUIRE(tile == base.tiles->tile(i));
            ++kept;
        } else {
            REQUIRE(tile->serial != 0);
            ++added;
        }
    }
    REQUIRE(kept == made);
    REQUIRE(added > 0);
    // Serials do not come back: a redrawn tile after clearing is new.
    cache.clear();
    const auto again = cache.coverage(first, size);
    for (std::uint32_t i = 0; i < again.tiles->columns() * again.tiles->rows(); ++i) {
        if (const auto& tile = again.tiles->tile(i)) {
            REQUIRE_FALSE(serials.contains(tile->serial));
        }
    }
}

TEST_CASE("Tiles drawn from the row-bucket index are the reference's bits", "[brush][cache]") {
    for (const std::uint32_t tile : {32U, 128U}) {
        BrushCoverageCache cache(std::size_t{256} << 20, tile);
        const ImageSize size{517, 301};
        const auto list = paintedMask(seed + 3, 8, everydayStyle, 301.0 / 517.0);
        const auto result = cache.coverage(list, size);
        REQUIRE(result.tiles->gathered() == rasteriseBrush(*list, size));
        // An extension of it too.
        const auto longer =
            list->appended(straightStroke({0.1F, 0.1F}, {0.9F, 0.9F}, 40, 0.03F, 0.4F, 0.8F, true));
        REQUIRE(cache.coverage(longer, size).tiles->gathered() == rasteriseBrush(*longer, size));
    }
}

TEST_CASE("The dab index paints a region as every dab does", "[brush][banded]") {
    const ImageSize size{257, 130};
    const auto list = paintedMask(seed + 4, 5, everydayStyle, 130.0 / 257.0);
    const std::vector<PlacedStroke> placed = placedStrokes(*list, size);
    for (const std::uint32_t bucketRows : {1U, 7U, 64U, 1000U}) {
        const DabBuckets buckets(placed, size.height, bucketRows);
        std::vector<float> all(static_cast<std::size_t>(size.width) * size.height);
        paintRegion(placed, {0, 0, size.width, size.height}, all);
        std::vector<float> bucketed(all.size());
        for (std::uint32_t b = 0; b < buckets.count(); ++b) {
            const std::uint32_t y0 = b * bucketRows;
            const std::uint32_t rows = std::min(bucketRows, size.height - y0);
            paintBucket(
                placed, buckets, b, {0, y0, size.width, rows},
                std::span<float>(bucketed).subspan(static_cast<std::size_t>(y0) * size.width,
                                                   static_cast<std::size_t>(rows) * size.width));
        }
        REQUIRE(bucketed == all);
    }
}

TEST_CASE("A direct render up to the retention bound leaves an entry, and a second makes no raster",
          "[brush][retention]") {
    brushCoverageCache().clear();
    const ImageSize size{640, 480};
    REQUIRE(size.pixelCount() <= retainedCoveragePixels);
    const auto list = paintedMask(seed + 5, 4, everydayStyle, 0.75);
    const LocalPlan local = localPlanFor(brushesOf({list}), size);

    const PackedCoverage first = packCoverage(local);
    REQUIRE(brushCoverageCache().entryCount() == 1);
    const auto held = brushCoverageCache().find(list, size);
    REQUIRE(held != nullptr);
    // The second render takes the same tiles: nothing was drawn (the tiles keep their serials).
    const PackedCoverage second = packCoverage(local);
    REQUIRE(brushCoverageCache().entryCount() == 1);
    REQUIRE(brushCoverageCache().find(list, size) == held);
    REQUIRE(first.planes == second.planes);
    REQUIRE(matchesReference(local, first));
}

TEST_CASE("A direct render above the bound leaves the cache as it was, with the same bits",
          "[brush][retention][slow]") {
    brushCoverageCache().clear();
    const ImageSize size{2400, 1800};
    REQUIRE(size.pixelCount() > retainedCoveragePixels);
    const auto list = paintedMask(seed + 6, 4, everydayStyle, 0.75);
    const LocalPlan local = localPlanFor(brushesOf({list}), size);

    const std::size_t entries = brushCoverageCache().entryCount();
    const std::size_t bytes = brushCoverageCache().memoryBytes();
    const PackedCoverage banded = packCoverage(local);
    REQUIRE(brushCoverageCache().entryCount() == entries);
    REQUIRE(brushCoverageCache().memoryBytes() == bytes);
    REQUIRE(matchesReference(local, banded));

    // Held in the cache already (by a retained call elsewhere): the same bits from the tiles.
    const auto tiles = brushCoverageCache().coverage(list, size).tiles;
    const PackedCoverage fromCache = packCoverage(local);
    REQUIRE(fromCache.planes == banded.planes);
    REQUIRE(brushCoverageCache().find(list, size) == tiles);
}
