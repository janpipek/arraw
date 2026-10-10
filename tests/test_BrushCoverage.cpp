// Packing a brush's coverage: the quantiser, the banded raster, the cache's lookups and the
// retention bound (ADR 044, sections 6 and 7; brush plan sections 2.3 to 2.5).

#include "BrushCoverage.h"
#include "BrushCoverageCache.h"
#include "BrushRaster.h"
#include "LadderAccess.h"
#include "LocalPlan.h"
#include "ProgressScope.h"
#include "RenderProgress.h"
#include "support/BrushGenerators.h"
#include "support/RowBandLimit.h"

#include <CheckpointLadder.h>
#include <Develop.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <LocalAdjustmentEdits.h>
#include <LocalAdjustments.h>
#include <Progress.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <thread>
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

/// A flat image to render, for the ladder cases.
std::shared_ptr<const ImageBuffer> flatSource(ImageSize size) {
    ImageBuffer image(size, workingFormat, workingEncoding);
    std::ranges::fill(image.samples<float>(), 0.25F);
    return std::make_shared<const ImageBuffer>(std::move(image));
}

/// Tiles of the cache's grid at which two lists differ by serial.
std::uint32_t tilesDiffering(const CoverageTiles& a, const CoverageTiles& b) {
    std::uint32_t differing = 0;
    for (std::uint32_t i = 0; i < a.columns() * a.rows(); ++i) {
        const auto serial = [i](const CoverageTiles& tiles) {
            return tiles.tile(i) ? tiles.tile(i)->serial : std::uint64_t{0};
        };
        differing += serial(a) != serial(b) ? 1U : 0U;
    }
    return differing;
}

/// A stroke that touches a corner only, so an extension changes few tiles.
Stroke cornerStroke() {
    return test::straightStroke({0.05F, 0.05F}, {0.12F, 0.1F}, 6, 0.02F, 1.0F, 1.0F);
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

TEST_CASE("A delta drag repacks nothing and calls the cache no more", "[brush][residency]") {
    brushCoverageCache().clear();
    const ImageSize size{300, 200};
    const DevelopState state = brushesOf({paintedMask(seed + 10, 3, everydayStyle, 200.0 / 300.0),
                                          paintedMask(seed + 11, 3, detailStyle, 200.0 / 300.0)});
    CoverageResidency residency;
    residency.update(localPlanFor(state, size), size);
    REQUIRE(residency.cacheCalls() == 2);
    const std::uint64_t packed = residency.tilesPacked();
    REQUIRE(packed > 0);

    // Amount, opacity, invert and the other controls leave every reference equal.
    const LocalAdjustmentId second = state.localAdjustments[1].id;
    DevelopState edited = withLocalDelta(state, state.localAdjustments[0].id, "exposure", 2.0);
    edited = withLocalOpacity(std::move(edited), second, 0.5);
    edited = withLocalAdjustmentInverted(std::move(edited), second, true);
    const LocalPlan local = localPlanFor(edited, size);
    for (const LocalMaskPlan& mask : local.masks) {
        REQUIRE(residency.holds(mask.brush));
    }
    const PackedCoverage& after = residency.update(local, size);
    REQUIRE(residency.cacheCalls() == 2);
    REQUIRE(residency.tilesPacked() == packed);
    REQUIRE(matchesReference(local, after));
}

TEST_CASE("An appended stroke repacks the tiles it touches, and undo those again",
          "[brush][residency]") {
    brushCoverageCache().clear();
    const ImageSize size{400, 300};
    const auto first = paintedMask(seed + 12, 3, everydayStyle, 0.75);
    const auto appended = first->appended(cornerStroke());
    const LocalPlan before = localPlanFor(brushesOf({first}), size);
    const LocalPlan after = localPlanFor(brushesOf({appended}), size);

    CoverageResidency residency;
    residency.update(before, size);
    const std::uint32_t tiles = 4 * 3;
    REQUIRE(residency.tilesPacked() == tiles);
    residency.clearPending(0);

    residency.update(after, size);
    const auto heldFirst = brushCoverageCache().find(first, size);
    const auto heldAppended = brushCoverageCache().find(appended, size);
    REQUIRE(heldFirst != nullptr);
    REQUIRE(heldAppended != nullptr);
    const std::uint32_t touched = tilesDiffering(*heldFirst, *heldAppended);
    REQUIRE(touched > 0);
    REQUIRE(touched < tiles);
    REQUIRE(residency.tilesPacked() == tiles + touched);
    REQUIRE(residency.pending(0).size() == touched);
    REQUIRE(matchesReference(after, residency.packed()));

    // Undo: the cache still holds the earlier list, and only the tiles that differ are repacked.
    residency.update(before, size);
    REQUIRE(residency.tilesPacked() == tiles + 2 * touched);
    REQUIRE(matchesReference(before, residency.packed()));
}

TEST_CASE("A change of size repacks every tile", "[brush][residency]") {
    brushCoverageCache().clear();
    const auto list = paintedMask(seed + 13, 3, everydayStyle, 0.75);
    const DevelopState state = brushesOf({list, list});
    CoverageResidency residency;
    residency.update(localPlanFor(state, {400, 300}), {400, 300});
    REQUIRE(residency.tilesPacked() == 2 * 12);
    const ImageSize smaller{200, 150};
    const LocalPlan local = localPlanFor(state, smaller);
    residency.update(local, smaller);
    REQUIRE(residency.tilesPacked() == 2 * 12 + 2 * 4);
    REQUIRE(residency.packed().size == smaller);
    REQUIRE(matchesReference(local, residency.packed()));
}

TEST_CASE("Disabling an earlier brush moves the later slots without redrawing",
          "[brush][residency]") {
    brushCoverageCache().clear();
    const ImageSize size{300, 200};
    const DevelopState state = brushesOf({paintedMask(seed + 14, 3, everydayStyle, 200.0 / 300.0),
                                          paintedMask(seed + 15, 3, everydayStyle, 200.0 / 300.0)});
    CoverageResidency residency;
    residency.update(localPlanFor(state, size), size);
    const std::size_t entries = brushCoverageCache().entryCount();
    const DevelopState fewer =
        withLocalAdjustmentEnabled(state, state.localAdjustments[0].id, false);
    const LocalPlan local = localPlanFor(fewer, size);
    REQUIRE(local.brushCount() == 1);
    // The moved brush is in the cache, so nothing is drawn: one lookup, and a hit.
    REQUIRE(readinessOf(local.masks[0].brush, &residency) == CoverageReadiness::Cached);
    const std::uint64_t calls = residency.cacheCalls();
    residency.update(local, size);
    REQUIRE(residency.cacheCalls() == calls + 1);
    REQUIRE(brushCoverageCache().entryCount() == entries);
    REQUIRE(matchesReference(local, residency.packed()));
}

TEST_CASE("A cache that keeps almost nothing still gives the direct path's bits",
          "[brush][residency]") {
    // Room for one tile: every request but the last is evicted at once.
    BrushCoverageCache tiny(std::size_t{128} * 128 * sizeof(float), 128);
    const ImageSize size{400, 300};
    const auto a = paintedMask(seed + 16, 3, everydayStyle, 0.75);
    const auto b = paintedMask(seed + 17, 3, everydayStyle, 0.75);
    CoverageResidency residency;
    for (const auto& list : {a, b, a, a->appended(cornerStroke()), b}) {
        const LocalPlan local = localPlanFor(brushesOf({list}), size);
        residency.update(local, size, tiny);
        REQUIRE(matchesReference(local, residency.packed()));
    }
}

TEST_CASE("A cancelled update leaves the slot to be redone and the earlier ones valid",
          "[brush][residency][cancel]") {
    brushCoverageCache().clear();
    const ImageSize size{400, 300};
    const auto a = paintedMask(seed + 18, 3, everydayStyle, 0.75);
    const auto x = paintedMask(seed + 19, 3, everydayStyle, 0.75);
    const auto y = paintedMask(seed + 21, 3, everydayStyle, 0.75);
    const LocalPlan withX = localPlanFor(brushesOf({a, x}), size);
    const LocalPlan withY = localPlanFor(brushesOf({a, y}), size);
    REQUIRE(withY.brushCount() == 2);
    // Y is in the cache, so the update only packs: the cancellation lands inside the packing.
    static_cast<void>(brushCoverageCache().coverage(y, size));

    // One band, so that the calling thread reports after every chunk of tiles.
    const ScopedRowBandLimit oneBand(1);
    bool completed = false;
    int cancelled = 0;
    for (int cancelAt = 1; cancelAt <= 40 && !completed; ++cancelAt) {
        INFO("cancelled at report " << cancelAt);
        // Slot 1 holds X, and an update to Y is cancelled part-way.
        CoverageResidency residency;
        residency.update(withX, size);
        REQUIRE(residency.holds(withX.masks[1].brush));
        int seen = 0;
        ProgressChannel channel([&](const Progress&) {
            if (++seen == cancelAt) {
                channel.cancel();
            }
        });
        try {
            StepWeights weights{};
            weights[static_cast<std::size_t>(ProgressStep::Coverage)] = 1.0;
            ProgressRoot root(&channel, weights, ProgressStep::Coverage);
            const ProgressSpan coverage(ProgressStep::Coverage, 1);
            residency.update(withY, size);
            completed = true;
        } catch (const Cancelled&) {
            ++cancelled;
            REQUIRE_FALSE(residency.holds(withY.masks[1].brush));
        }
        REQUIRE(residency.holds(withY.masks[0].brush));
        // Back to X: whatever the half-written channel held, the bits are the direct path's.
        residency.update(withX, size);
        REQUIRE(residency.holds(withX.masks[1].brush));
        REQUIRE(matchesReference(withX, residency.packed()));
        // And on to Y, whole.
        residency.update(withY, size);
        REQUIRE(matchesReference(withY, residency.packed()));
    }
    REQUIRE(completed);
    REQUIRE(cancelled >= 2);
}

TEST_CASE("Pending tiles stay until an upload says it completed", "[brush][residency][pending]") {
    brushCoverageCache().clear();
    const ImageSize size{400, 300};
    const auto first = paintedMask(seed + 20, 3, everydayStyle, 0.75);
    CoverageResidency residency;
    residency.update(localPlanFor(brushesOf({first}), size), size);
    // Made whole: every tile of the plane, the last column and row clipped to the raster.
    const std::vector<PixelRect> all = residency.pending(0);
    REQUIRE(all.size() == 12);
    REQUIRE(all.back().x == 384);
    REQUIRE(all.back().width == 16);
    REQUIRE(all.back().height == 300 - 256);
    // A second update that changes nothing, as a render on the CPU would, keeps them.
    residency.update(localPlanFor(brushesOf({first}), size), size);
    REQUIRE(residency.pending(0).size() == 12);
    residency.clearPending(0);
    REQUIRE_FALSE(residency.hasPending(0));
    REQUIRE(residency.pending(0).empty());
    // A later change makes only its tiles pending.
    residency.update(localPlanFor(brushesOf({first->appended(cornerStroke())}), size), size);
    REQUIRE(residency.hasPending(0));
    REQUIRE(residency.pending(0).size() < 12);
    REQUIRE(residency.pending(1).empty());
}

TEST_CASE("A copied ladder starts with no packed coverage", "[brush][residency][ladder]") {
    const auto source = flatSource({200, 150});
    const DevelopState state = brushesOf({paintedMask(seed + 21, 3, everydayStyle, 0.75)});
    CheckpointLadder ladder;
    REQUIRE(LadderAccess::coverage(std::as_const(ladder)) == nullptr);
    static_cast<void>(resumeOrDevelop(ladder, source, state));
    REQUIRE(LadderAccess::coverage(std::as_const(ladder)) != nullptr);

    const CheckpointLadder copy(ladder);
    REQUIRE(LadderAccess::coverage(copy) == nullptr);
    REQUIRE(copy.holds(Stage::Pointwise) == ladder.holds(Stage::Pointwise));
    CheckpointLadder assigned;
    static_cast<void>(resumeOrDevelop(assigned, source, state));
    assigned = ladder;
    REQUIRE(LadderAccess::coverage(std::as_const(assigned)) == nullptr);
    // The original keeps its own.
    REQUIRE(LadderAccess::coverage(std::as_const(ladder)) != nullptr);

    // Clearing, or a plan without brushes, lets the planes go.
    ladder.clear();
    REQUIRE(LadderAccess::coverage(std::as_const(ladder)) == nullptr);
    static_cast<void>(resumeOrDevelop(ladder, source, state));
    static_cast<void>(resumeOrDevelop(ladder, source, DevelopState{}));
    REQUIRE(LadderAccess::coverage(std::as_const(ladder)) == nullptr);
}

TEST_CASE("The Coverage step counts only the brushes whose coverage is not ready",
          "[brush][residency][progress]") {
    brushCoverageCache().clear();
    const ImageSize size{300, 200};
    const auto source = flatSource(size);
    const DevelopState state = brushesOf({paintedMask(seed + 22, 3, everydayStyle, 200.0 / 300.0),
                                          paintedMask(seed + 23, 3, detailStyle, 200.0 / 300.0)});
    const LocalPlan local = localPlanFor(state, size);
    const auto coverageOf = [](const std::vector<double>& weights) {
        double sum = 0.0;
        for (const double weight : weights) {
            sum += weight;
        }
        return sum;
    };

    // Nothing made: both brushes are drawn and packed.
    const std::vector<double> fresh = coverageUnitWeights(local, nullptr);
    REQUIRE(fresh.size() == 2);

    // The cache holds them: only the packing is left, so less.
    CheckpointLadder ladder;
    static_cast<void>(resumeOrDevelop(ladder, source, state));
    const std::vector<double> cached = coverageUnitWeights(local, nullptr);
    REQUIRE(cached.size() == 2);
    REQUIRE(coverageOf(cached) < coverageOf(fresh));
    REQUIRE(coverageOf(cached) > 0.0);

    // The ladder holds them: no weight, and no unit.
    const CoverageResidency* residency = LadderAccess::coverage(std::as_const(ladder));
    REQUIRE(residency != nullptr);
    REQUIRE(coverageUnitWeights(local, residency).empty());
    const ProcessingPlan plan = planFor(*source, state, {});
    const StepWeights held = renderStepWeights(plan, {}, residency);
    REQUIRE(held[static_cast<std::size_t>(ProgressStep::Coverage)] == 0.0);
    // A drag of the amount through the same ladder: still none.
    const DevelopState louder =
        withLocalDelta(state, state.localAdjustments[0].id, "exposure", 2.0);
    const StepWeights dragged = renderStepWeights(planFor(*source, louder, {}), {}, residency);
    REQUIRE(dragged[static_cast<std::size_t>(ProgressStep::Coverage)] == 0.0);
    // Without the ladder, the same plan counts its packing.
    const StepWeights direct = renderStepWeights(plan, {});
    REQUIRE(direct[static_cast<std::size_t>(ProgressStep::Coverage)] ==
            Catch::Approx(coverageOf(cached)));
    // After an eviction it counts the drawing again.
    brushCoverageCache().clear();
    REQUIRE(coverageOf(coverageUnitWeights(local, nullptr)) == Catch::Approx(coverageOf(fresh)));
}

TEST_CASE("peek says what coverage then gives, and changes nothing", "[brush][peek]") {
    BrushCoverageCache cache(std::size_t{256} << 20, 64);
    const ImageSize size{200, 150};
    const auto first = paintedMask(seed + 24, 3, everydayStyle, 0.75);
    const auto copy = std::make_shared<const StrokeList>(*first);
    const auto appended = first->appended(cornerStroke());
    const auto other = paintedMask(seed + 25, 3, everydayStyle, 0.75);

    REQUIRE(cache.peek(first, size).lookup == CoverageLookup::Miss);
    REQUIRE(cache.entryCount() == 0);
    REQUIRE(cache.coverage(first, size).lookup == CoverageLookup::Miss);

    const std::size_t entries = cache.entryCount();
    const std::size_t bytes = cache.memoryBytes();
    struct Case {
        std::shared_ptr<const StrokeList> list;
        ImageSize size;
    };
    for (const Case& request :
         {Case{first, size}, Case{copy, size}, Case{appended, size}, Case{other, size},
          Case{first, ImageSize{100, 75}}, Case{StrokeList{}.appended(cornerStroke()), size}}) {
        const CoverageLookup peeked = cache.peek(request.list, request.size).lookup;
        REQUIRE(cache.entryCount() == entries);
        REQUIRE(cache.memoryBytes() == bytes);
        // Asked of a clone, since coverage inserts.
        BrushCoverageCache twin(std::size_t{256} << 20, 64);
        static_cast<void>(twin.coverage(first, size));
        REQUIRE(twin.coverage(request.list, request.size).lookup == peeked);
    }
    REQUIRE(cache.peek(first, size).lookup == CoverageLookup::Hit);
    REQUIRE(cache.peek(copy, size).lookup == CoverageLookup::ContentHit);
    REQUIRE(cache.peek(appended, size).lookup == CoverageLookup::Extended);
    // The strokes already drawn: the prefix an appended list builds on, the whole list otherwise.
    REQUIRE(cache.peek(appended, size).cachedStrokes == first->strokes().size());
    REQUIRE(cache.peek(first, size).cachedStrokes == first->strokes().size());
    REQUIRE(cache.peek(other, size).cachedStrokes == 0);
    REQUIRE(cache.peek(other, size).lookup == CoverageLookup::Miss);
    REQUIRE(cache.peek(nullptr, size).lookup == CoverageLookup::Miss);
}

TEST_CASE("peek does not touch the order entries are evicted in", "[brush][peek]") {
    // Room for two one-tile entries.
    BrushCoverageCache cache(2 * 64 * 64 * sizeof(float), 64);
    const ImageSize size{64, 64};
    const auto make = [](float u) {
        return std::make_shared<const StrokeList>(std::vector<Stroke>{
            test::straightStroke({u, 0.5F}, {u + 0.05F, 0.5F}, 4, 0.1F, 1.0F, 1.0F)});
    };
    const auto x = make(0.2F);
    const auto y = make(0.4F);
    const auto z = make(0.6F);
    static_cast<void>(cache.coverage(x, size));
    static_cast<void>(cache.coverage(y, size));
    REQUIRE(cache.entryCount() == 2);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(cache.peek(x, size).lookup == CoverageLookup::Hit);
    }
    static_cast<void>(cache.coverage(z, size));
    // x is still the oldest by use, whatever peek was asked.
    REQUIRE(cache.peek(x, size).lookup == CoverageLookup::Miss);
    REQUIRE(cache.peek(y, size).lookup == CoverageLookup::Hit);
    REQUIRE(cache.peek(z, size).lookup == CoverageLookup::Hit);
}

TEST_CASE("drawsBrushCoverage is true only for coverage drawn from nothing",
          "[brush][peek][residency]") {
    brushCoverageCache().clear();
    const ImageSize size{300, 200};
    const auto source = flatSource(size);
    const auto list = paintedMask(seed + 26, 3, everydayStyle, 200.0 / 300.0);
    const DevelopState state = brushesOf({list});
    CheckpointLadder ladder;

    SECTION("a state without a brush") {
        REQUIRE_FALSE(drawsBrushCoverage(ladder, *source, DevelopState{}));
    }
    SECTION("a new list at a size not cached") {
        REQUIRE(drawsBrushCoverage(ladder, *source, state));
        // Another size of the same list is also from nothing.
        static_cast<void>(resumeOrDevelop(ladder, source, state));
        REQUIRE_FALSE(drawsBrushCoverage(ladder, *source, state));
        const auto bigger = flatSource({600, 400});
        CheckpointLadder other;
        REQUIRE(drawsBrushCoverage(other, *bigger, state));
    }
    SECTION("after a ladder render of the same state, and after a delta-only edit") {
        static_cast<void>(resumeOrDevelop(ladder, source, state));
        REQUIRE_FALSE(drawsBrushCoverage(ladder, *source, state));
        const DevelopState louder =
            withLocalDelta(state, state.localAdjustments[0].id, "exposure", 3.0);
        REQUIRE_FALSE(drawsBrushCoverage(ladder, *source, louder));
        // The ladder holds it even when the cache lost it.
        brushCoverageCache().clear();
        REQUIRE_FALSE(drawsBrushCoverage(ladder, *source, louder));
        // A ladder that holds nothing, with the cache empty, would draw: an eviction.
        CheckpointLadder fresh;
        REQUIRE(drawsBrushCoverage(fresh, *source, louder));
    }
    SECTION("an appended stroke extends what is held") {
        static_cast<void>(resumeOrDevelop(ladder, source, state));
        const DevelopState more =
            withStrokeAppended(state, state.localAdjustments[0].id, cornerStroke());
        REQUIRE_FALSE(drawsBrushCoverage(ladder, *source, more));
        CheckpointLadder fresh;
        REQUIRE_FALSE(drawsBrushCoverage(fresh, *source, more));
    }
    SECTION("the cache holds the list behind another pointer") {
        static_cast<void>(resumeOrDevelop(ladder, source, state));
        const DevelopState again = brushesOf({std::make_shared<const StrokeList>(*list)});
        CheckpointLadder fresh;
        REQUIRE_FALSE(drawsBrushCoverage(fresh, *source, again));
    }
    SECTION("a minimum of modelled drawing time") {
        REQUIRE(drawsBrushCoverage(ladder, *source, state, {}, 0.0));
        // The modelled time of a few strokes at 0.06 MP is milliseconds.
        REQUIRE_FALSE(drawsBrushCoverage(ladder, *source, state, {}, 10.0));
    }
    SECTION("a ladder that could resume past the pointwise pass draws nothing") {
        static_cast<void>(resumeOrDevelop(ladder, source, state));
        // A copy keeps the rungs and loses the packed coverage; the cache lost the list too.
        const CheckpointLadder copy = ladder;
        brushCoverageCache().clear();
        REQUIRE(LadderAccess::coverage(copy) == nullptr);
        REQUIRE_FALSE(drawsBrushCoverage(copy, *source, state));
        // A render that has to start the pointwise pass again does draw it.
        const DevelopState louder =
            withLocalDelta(state, state.localAdjustments[0].id, "exposure", 3.0);
        REQUIRE(drawsBrushCoverage(copy, *source, louder));
    }
    SECTION("it draws, packs and inserts nothing") {
        const std::size_t entries = brushCoverageCache().entryCount();
        REQUIRE(drawsBrushCoverage(ladder, *source, state));
        REQUIRE(brushCoverageCache().entryCount() == entries);
        REQUIRE(ladder.empty());
        REQUIRE(LadderAccess::coverage(std::as_const(ladder)) == nullptr);
    }
}

TEST_CASE("A ladder render and a direct render of the same strokes at once give their bits",
          "[brush][residency][threads]") {
    brushCoverageCache().clear();
    const ImageSize size{256, 192};
    const auto source = flatSource(size);
    const DevelopState state = brushesOf({paintedMask(seed + 27, 4, everydayStyle, 0.75),
                                          paintedMask(seed + 28, 4, detailStyle, 0.75)});
    const ImageBuffer reference = develop(*source, state);
    brushCoverageCache().clear();

    const auto same = [&](const ImageBuffer& image) {
        return std::ranges::equal(image.samples<float>(), reference.samples<float>());
    };
    std::atomic<int> wrong{0};
    for (int round = 0; round < 3; ++round) {
        brushCoverageCache().clear();
        std::vector<std::jthread> threads;
        threads.emplace_back([&] {
            CheckpointLadder ladder;
            if (!same(resumeOrDevelop(ladder, source, state).checkpoint.readBack())) {
                ++wrong;
            }
        });
        threads.emplace_back([&] {
            if (!same(develop(*source, state))) {
                ++wrong;
            }
        });
        threads.emplace_back([&] {
            CheckpointLadder ladder;
            if (!same(resumeOrDevelop(ladder, source, state).checkpoint.readBack())) {
                ++wrong;
            }
        });
    }
    REQUIRE(wrong.load() == 0);
}

TEST_CASE("The cache's float coverage at level 0 and level 1 agrees after box-halving",
          "[brush][resolution]") {
    // An even-sized source, so that level 1 is an exact half (ADR 044, section 6).
    BrushCoverageCache cache;
    const ImageSize fine{1024, 768};
    const ImageSize coarse{512, 384};
    const auto soft = [] {
        StrokeStyle style = everydayStyle;
        style.hardnessHigh = 0.5F;
        return style;
    }();
    const auto halvedMean = [&](const std::shared_ptr<const StrokeList>& list, float& worst) {
        const CoveragePlane finer = cache.coverage(list, fine).tiles->gathered();
        const CoveragePlane coarser = cache.coverage(list, coarse).tiles->gathered();
        double sum = 0.0;
        worst = 0.0F;
        for (std::uint32_t y = 0; y < coarse.height; ++y) {
            for (std::uint32_t x = 0; x < coarse.width; ++x) {
                const auto at = [&](std::uint32_t dx, std::uint32_t dy) {
                    return finer
                        .values[static_cast<std::size_t>(2 * y + dy) * fine.width + 2 * x + dx];
                };
                const float box = (at(0, 0) + at(1, 0) + at(0, 1) + at(1, 1)) / 4.0F;
                const float difference =
                    std::abs(box - coarser.values[static_cast<std::size_t>(y) * coarse.width + x]);
                sum += difference;
                worst = std::max(worst, difference);
            }
        }
        return sum / (static_cast<double>(coarse.width) * coarse.height);
    };
    float worst = 0.0F;
    const double mean = halvedMean(paintedMask(seed + 3, 8, soft, 0.75), worst);
    INFO("max " << worst << " mean " << mean);
    // The prototype measured a max of 0.02 on its seeds; another seed gave 0.0201, so the bound
    // is 0.021, with the mean (0.00015) as the tighter guard of the agreement.
    CHECK(worst <= 0.021F);
    CHECK(mean <= 0.00015);

    StrokeStyle hard = everydayStyle;
    hard.hardnessLow = hard.hardnessHigh = 1.0F;
    const double hardMean = halvedMean(paintedMask(seed + 4, 8, hard, 0.75), worst);
    INFO("hard mean " << hardMean);
    CHECK(hardMean <= 0.005);
}

TEST_CASE("The cost of a brush's coverage weighs only the strokes not yet drawn",
          "[brush][peek][progress]") {
    brushCoverageCache().clear();
    const ImageSize size{300, 200};
    const auto list = paintedMask(seed + 27, 8, everydayStyle, 200.0 / 300.0);
    const auto more = list->appended(cornerStroke());
    const auto brushOfList = [&](const std::shared_ptr<const StrokeList>& strokes) {
        return localPlanFor(brushesOf({strokes}), size).masks[0].brush;
    };
    const auto workOf = [&](const std::shared_ptr<const StrokeList>& strokes) {
        const BrushCoverageRef brush = brushOfList(strokes);
        return coverageWorkOf(statusOf(brush, nullptr), brush);
    };

    const CoverageWork fresh = workOf(more);
    REQUIRE(fresh.draw > 0.0);
    REQUIRE(fresh.pack > 0.0);
    static_cast<void>(brushCoverageCache().coverage(list, size));
    // One stroke appended to eight drawn: only that stroke is drawn.
    const CoverageWork extended = workOf(more);
    REQUIRE(extended.pack == fresh.pack);
    REQUIRE(extended.draw > 0.0);
    REQUIRE(extended.draw < fresh.draw / 2.0);
    // Held in the cache whole: packing only.
    const CoverageWork cached = workOf(list);
    REQUIRE(cached.draw == 0.0);
    REQUIRE(cached.pack == fresh.pack);
    // Held by a residency: nothing.
    CoverageResidency residency;
    const LocalPlan plan = localPlanFor(brushesOf({list}), size);
    residency.update(plan, size);
    const CoverageWork held =
        coverageWorkOf(statusOf(plan.masks[0].brush, &residency), plan.masks[0].brush);
    REQUIRE(held.draw == 0.0);
    REQUIRE(held.pack == 0.0);
}

TEST_CASE("A brush unit reports its drawing and its packing, never falling",
          "[brush][residency][progress]") {
    brushCoverageCache().clear();
    const ImageSize size{400, 300};
    const LocalPlan plan =
        localPlanFor(brushesOf({paintedMask(seed + 28, 5, everydayStyle, 0.75)}), size);
    const ScopedRowBandLimit oneBand(1);
    for (const bool cached : {false, true}) {
        INFO((cached ? "coverage in the cache" : "coverage drawn"));
        if (cached) {
            static_cast<void>(brushCoverageCache().coverage(plan.masks[0].brush.strokes, size));
        }
        std::vector<double> fractions;
        ProgressChannel channel(
            [&fractions](const Progress& progress) { fractions.push_back(progress.fraction); });
        StepWeights weights{};
        weights[static_cast<std::size_t>(ProgressStep::Coverage)] = 1.0;
        {
            ProgressRoot root(&channel, weights, ProgressStep::Coverage);
            const ProgressSpan coverage(ProgressStep::Coverage, 1);
            CoverageResidency residency;
            residency.update(plan, size);
        }
        REQUIRE(std::ranges::is_sorted(fractions));
        REQUIRE(fractions.back() == 1.0);
        // The packing moves the pie after the drawing: some report is strictly inside the end.
        REQUIRE(std::ranges::any_of(fractions, [](double f) { return f > 0.0 && f < 1.0; }));
    }
}
