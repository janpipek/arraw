// The tiled coverage cache of the brush prototype (ADR 044, section 6).

#include "BrushCoverageCache.h"
#include "ProgressScope.h"
#include "support/BrushGenerators.h"

#include <Progress.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <latch>
#include <set>
#include <thread>

using namespace arraw;

namespace {

constexpr std::uint64_t seed = 20261009 + 100;

/// Tiles the dab boxes of the strokes touch, worked out from the box rule alone.
std::set<std::uint32_t> touchedTiles(const StrokeList& list, std::size_t from, ImageSize raster,
                                     std::uint32_t tile) {
    std::set<std::uint32_t> tiles;
    const std::uint32_t columns = (raster.width + tile - 1) / tile;
    for (std::size_t i = from; i < list.size(); ++i) {
        const double radius =
            static_cast<double>(list[i].radius) * std::max(raster.width, raster.height);
        for (const DabCentre& dab : dabCentres(list[i], raster)) {
            const auto x0 = std::max<std::int64_t>(
                0, static_cast<std::int64_t>(std::floor(dab.x - radius)) - 1);
            const auto x1 = std::min<std::int64_t>(
                raster.width - 1, static_cast<std::int64_t>(std::ceil(dab.x + radius)) + 1);
            const auto y0 = std::max<std::int64_t>(
                0, static_cast<std::int64_t>(std::floor(dab.y - radius)) - 1);
            const auto y1 = std::min<std::int64_t>(
                raster.height - 1, static_cast<std::int64_t>(std::ceil(dab.y + radius)) + 1);
            if (x0 > x1 || y0 > y1) {
                continue;
            }
            for (std::int64_t ty = y0 / tile; ty <= y1 / tile; ++ty) {
                for (std::int64_t tx = x0 / tile; tx <= x1 / tile; ++tx) {
                    tiles.insert(static_cast<std::uint32_t>(ty * columns + tx));
                }
            }
        }
    }
    return tiles;
}

struct Shape {
    ImageSize raster;
    std::uint32_t tile;
};

} // namespace

TEST_CASE("Cached coverage is the reference, however it was made", "[brush][cache][slow]") {
    const Shape shape = GENERATE(Shape{{1000, 667}, 64}, Shape{{1000, 667}, 256},
                                 Shape{{1001, 1}, 64}, Shape{{1001, 1}, 256});
    INFO(shape.raster.width << "x" << shape.raster.height << " tile " << shape.tile);
    const double aspect = static_cast<double>(shape.raster.height) / shape.raster.width;
    std::mt19937_64 g(seed);
    BrushCoverageCache cache(std::size_t{512} << 20, shape.tile);

    std::vector<std::shared_ptr<const StrokeList>> lists{std::make_shared<const StrokeList>()};
    lists.push_back(lists.back()->appended(
        test::wanderingStroke(g, test::everydayStyle, std::max(aspect, 0.667))));
    auto first = cache.coverage(lists.back(), shape.raster);
    CHECK(first.lookup == CoverageLookup::Miss);
    CHECK(first.tiles->gathered() == rasteriseBrush(*lists.back(), shape.raster));

    for (int i = 0; i < 12; ++i) {
        lists.push_back(lists.back()->appended(
            test::wanderingStroke(g, test::everydayStyle, std::max(aspect, 0.667))));
        const auto result = cache.coverage(lists.back(), shape.raster);
        REQUIRE(result.lookup == CoverageLookup::Extended);
        REQUIRE(result.tiles->gathered() == rasteriseBrush(*lists.back(), shape.raster));
    }
    CHECK(cache.coverage(lists.back(), shape.raster).lookup == CoverageLookup::Hit);

    SECTION("an undo is a hit") {
        const auto& earlier = lists[lists.size() - 6];
        const auto result = cache.coverage(earlier, shape.raster);
        CHECK(result.lookup == CoverageLookup::Hit);
        CHECK(result.dirty.empty());
        CHECK(result.tiles->gathered() == rasteriseBrush(*earlier, shape.raster));
    }

    SECTION("an equal list behind new pointers is a content hit, and then extends") {
        std::vector<Stroke> copy;
        for (const auto& stroke : lists.back()->strokes()) {
            copy.push_back(*stroke);
        }
        const auto reloaded = std::make_shared<const StrokeList>(std::move(copy));
        const auto same = cache.coverage(reloaded, shape.raster);
        CHECK(same.lookup == CoverageLookup::ContentHit);
        CHECK(same.tiles->gathered() == rasteriseBrush(*reloaded, shape.raster));
        const auto next = reloaded->appended(
            test::wanderingStroke(g, test::everydayStyle, std::max(aspect, 0.667)));
        const auto extended = cache.coverage(next, shape.raster);
        CHECK(extended.lookup == CoverageLookup::Extended);
        CHECK(extended.tiles->gathered() == rasteriseBrush(*next, shape.raster));
    }
}

TEST_CASE("An appended stroke redraws exactly the tiles its dabs touch", "[brush][cache]") {
    constexpr ImageSize raster{1000, 667};
    for (const std::uint32_t tile : {64U, 256U}) {
        INFO("tile " << tile);
        BrushCoverageCache cache(std::size_t{512} << 20, tile);
        // Strokes in the left half of the frame, so that the right stays empty.
        const auto confined = test::paintedMask(seed + 1, 3, test::detailStyle, 0.667,
                                                std::array<double, 4>{0.05, 0.35, 0.1, 0.9});
        const auto before = cache.coverage(confined, raster);
        REQUIRE(before.lookup == CoverageLookup::Miss);
        CHECK(std::set<std::uint32_t>(before.dirty.begin(), before.dirty.end()) ==
              touchedTiles(*confined, 0, raster, tile));

        std::mt19937_64 g(seed + 2);
        const auto next = confined->appended(test::wanderingStroke(
            g, test::detailStyle, 0.667, std::array<double, 4>{0.1, 0.3, 0.3, 0.6}));
        const auto after = cache.coverage(next, raster);
        REQUIRE(after.lookup == CoverageLookup::Extended);
        const std::set<std::uint32_t> expected = touchedTiles(*next, 3, raster, tile);
        CHECK(std::set<std::uint32_t>(after.dirty.begin(), after.dirty.end()) == expected);
        const std::set<std::uint32_t> ever = touchedTiles(*next, 0, raster, tile);
        for (std::uint32_t i = 0; i < after.tiles->columns() * after.tiles->rows(); ++i) {
            if (!expected.contains(i)) {
                CHECK(after.tiles->tile(i) == before.tiles->tile(i));
            }
            if (!ever.contains(i)) {
                CHECK(after.tiles->tile(i) == nullptr);
            }
        }
        CHECK(ever.size() < static_cast<std::size_t>(after.tiles->columns()) * after.tiles->rows());
    }
}

TEST_CASE("The budget evicts the least recently used, and nothing is lost by it",
          "[brush][cache][slow]") {
    constexpr ImageSize raster{1000, 667};
    const std::size_t plane =
        static_cast<std::size_t>(raster.width) * raster.height * sizeof(float);
    BrushCoverageCache cache(plane, 64);
    std::mt19937_64 g(seed + 3);
    std::vector<std::shared_ptr<const StrokeList>> lists{std::make_shared<const StrokeList>()};
    // Twelve wash strokes, and one more below, stay within the swept-area budget of a mask.
    for (int i = 0; i < 12; ++i) {
        lists.push_back(lists.back()->appended(test::wanderingStroke(g, test::washStyle, 0.667)));
        REQUIRE(cache.coverage(lists.back(), raster).tiles);
    }
    CHECK(cache.entryCount() < 12);
    CHECK((cache.memoryBytes() <= plane || cache.entryCount() == 1));

    const auto old = cache.coverage(lists[3], raster);
    CHECK((old.lookup == CoverageLookup::Miss || old.lookup == CoverageLookup::Extended));
    CHECK(old.tiles->gathered() == rasteriseBrush(*lists[3], raster));

    const std::size_t entries = cache.entryCount();
    const auto live = lists.back()->appended(test::wanderingStroke(g, test::washStyle, 0.667));
    const auto transient = cache.coverage(live, raster, false);
    CHECK(transient.tiles->gathered() == rasteriseBrush(*live, raster));
    CHECK(cache.entryCount() == entries);
}

TEST_CASE("The same list at two raster sizes is two entries that share nothing", "[brush][cache]") {
    BrushCoverageCache cache(std::size_t{512} << 20, 64);
    const auto list = test::paintedMask(seed + 4, 4, test::detailStyle, 0.667);
    const ImageSize large{600, 400};
    const ImageSize small{300, 200};
    CHECK(cache.coverage(list, large).lookup == CoverageLookup::Miss);
    const auto other = cache.coverage(list, small);
    CHECK(other.lookup == CoverageLookup::Miss);
    CHECK(other.tiles->size().width == 300);
    CHECK(other.tiles->gathered() == rasteriseBrush(*list, small));
    CHECK(cache.entryCount() == 2);
    // A longer list at the new size does not start from the other size's tiles.
    std::mt19937_64 g(seed + 5);
    const auto longer = list->appended(test::wanderingStroke(g, test::detailStyle, 0.667));
    CHECK(cache.coverage(longer, {450, 300}).lookup == CoverageLookup::Miss);
    CHECK(cache.coverage(list, large).lookup == CoverageLookup::Hit);
    CHECK(cache.coverage(list, small).lookup == CoverageLookup::Hit);
}

TEST_CASE("A list of another rasteriser is refused", "[brush][cache]") {
    BrushCoverageCache cache;
    const auto other = std::make_shared<const StrokeList>(std::vector<Stroke>{}, 2);
    CHECK_THROWS_AS(cache.coverage(other, {64, 48}), std::invalid_argument);
    CHECK_THROWS_AS(cache.coverage(nullptr, {64, 48}), std::invalid_argument);
    CHECK_THROWS_AS(cache.coverage(std::make_shared<const StrokeList>(), {0, 48}),
                    std::invalid_argument);
    CHECK(cache.entryCount() == 0);
}

TEST_CASE("An extension starts from the longest list held", "[brush][cache]") {
    constexpr ImageSize raster{1000, 667};
    constexpr std::uint32_t tile = 64;
    BrushCoverageCache cache(std::size_t{512} << 20, tile);
    // Strokes in four different quarters, so that which ones were repainted shows in `dirty`.
    const std::array<std::array<double, 4>, 4> boxes{{{0.05, 0.4, 0.05, 0.4},
                                                      {0.6, 0.95, 0.05, 0.4},
                                                      {0.05, 0.4, 0.6, 0.95},
                                                      {0.6, 0.95, 0.6, 0.95}}};
    std::mt19937_64 g(seed + 6);
    std::vector<std::shared_ptr<const StrokeList>> lists{std::make_shared<const StrokeList>()};
    for (const auto& box : boxes) {
        lists.push_back(
            lists.back()->appended(test::wanderingStroke(g, test::detailStyle, 0.667, box)));
    }
    // Hold lists 1 and 3 only: 3 is built from 1 by painting strokes 1 and 2.
    CHECK(cache.coverage(lists[1], raster).lookup == CoverageLookup::Miss);
    const auto third = cache.coverage(lists[3], raster);
    CHECK(third.lookup == CoverageLookup::Extended);
    CHECK(std::set<std::uint32_t>(third.dirty.begin(), third.dirty.end()) ==
          touchedTiles(*lists[3], 1, raster, tile));
    // List 4 extends 3, not 1: only the last stroke is painted.
    const auto fourth = cache.coverage(lists[4], raster);
    CHECK(fourth.lookup == CoverageLookup::Extended);
    const std::set<std::uint32_t> last = touchedTiles(*lists[4], 3, raster, tile);
    CHECK(std::set<std::uint32_t>(fourth.dirty.begin(), fourth.dirty.end()) == last);
    CHECK(last != touchedTiles(*lists[4], 1, raster, tile));
    CHECK(fourth.tiles->gathered() == rasteriseBrush(*lists[4], raster));
}

TEST_CASE("Two threads asking for the same coverage leave one entry", "[brush][cache]") {
    constexpr ImageSize raster{600, 400};
    for (int round = 0; round < 5; ++round) {
        BrushCoverageCache cache(std::size_t{512} << 20, 64);
        const auto list = test::paintedMask(seed + 7 + round, 6, test::detailStyle, 0.667);
        std::latch start(2);
        std::array<BrushCoverageCache::Result, 2> results;
        std::vector<std::jthread> threads;
        for (std::size_t n = 0; n < results.size(); ++n) {
            threads.emplace_back([&, n] {
                start.arrive_and_wait();
                results[n] = cache.coverage(list, raster);
            });
        }
        threads.clear();
        CHECK(cache.entryCount() == 1);
        // The tiles shown, and at most the four an end dab reaches, kept for growing.
        CHECK(cache.memoryBytes() >= results[0].tiles->tileBytes());
        CHECK(cache.memoryBytes() <= results[0].tiles->tileBytes() + 4 * 64 * 64 * sizeof(float));
        const CoveragePlane reference = rasteriseBrush(*list, raster);
        CHECK(results[0].tiles->gathered() == reference);
        CHECK(results[1].tiles->gathered() == reference);
    }
}

// ---------------------------------------------------------------------------------------------
// The tail method (brush GUI plan section 7): a stroke that grows is not redrawn.

namespace {

/// Makes the stroke with its first @p count points.
Stroke partialOf(const Stroke& stroke, std::size_t count) {
    Stroke partial = stroke;
    partial.points.resize(std::min(count, stroke.points.size()));
    return partial;
}

/// Counts the tiles whose serial differs between two grids of one raster.
std::set<std::uint32_t> changedTiles(const CoverageTiles& a, const CoverageTiles& b) {
    std::set<std::uint32_t> changed;
    for (std::uint32_t i = 0; i < a.columns() * a.rows(); ++i) {
        const auto serialOf = [i](const CoverageTiles& t) {
            return t.tile(i) ? t.tile(i)->serial : std::uint64_t{0};
        };
        if (serialOf(a) != serialOf(b)) {
            changed.insert(i);
        }
    }
    return changed;
}

/// Makes a random path of @p count points that steps, repeats, jumps back and leaves the picture.
Stroke difficultPath(std::mt19937_64& g, std::uint32_t count, float radius, float hardness,
                     bool erase) {
    Stroke stroke{radius, hardness, 0.7F, erase, {}};
    double u = 0.5;
    double v = 0.4;
    for (std::uint32_t i = 0; i < count; ++i) {
        stroke.points.push_back({static_cast<float>(u), static_cast<float>(v)});
        const double roll = test::unitDouble(g);
        if (roll < 0.1) {
            continue; // A repeated point.
        }
        if (roll < 0.2 && stroke.points.size() > 3) {
            // A point jumping back to an earlier place.
            const auto& back = stroke.points[test::uniformCount(
                g, 0, static_cast<std::uint32_t>(stroke.points.size()) - 1)];
            u = back.u;
            v = back.v;
            continue;
        }
        const double step = 0.02 + 0.03 * test::unitDouble(g);
        u += step * (test::unitDouble(g) - 0.45);
        v += step * (test::unitDouble(g) - 0.5);
        if (roll > 0.97) {
            // Out of the picture, and back.
            u = -0.2 + 1.4 * test::unitDouble(g);
            v = -0.2 + 1.4 * test::unitDouble(g);
        }
    }
    return stroke;
}

} // namespace

TEST_CASE("A growing stroke gives the reference bits after every step", "[brush][cache][grown]") {
    struct Style {
        float radius;
        float hardness;
        bool erase;
    };
    const Style style =
        GENERATE(Style{0.03F, 0.0F, false}, Style{0.03F, 1.0F, false}, Style{0.02F, 0.5F, true},
                 Style{minimumBrushRadius, 1.0F, false}, Style{0.05F, 0.3F, false});
    const std::uint32_t tile = GENERATE(64U, 256U);
    INFO("radius " << style.radius << " hardness " << style.hardness << " tile " << tile);
    constexpr ImageSize raster{400, 300};
    std::mt19937_64 g(seed + 30);
    BrushCoverageCache cache(std::size_t{512} << 20, tile);
    // Something under the stroke, so that the first update is an extension of a held list.
    const auto before = test::paintedMask(seed + 31, 2, test::everydayStyle, 0.75);
    const Stroke path = difficultPath(g, 200, style.radius, style.hardness, style.erase);
    const bool held = GENERATE(true, false);
    if (held) {
        static_cast<void>(cache.coverage(before, raster));
    }
    std::uint64_t grown = 0;
    for (std::size_t points = 1; points <= path.points.size();
         points += test::uniformCount(g, 1, 50)) {
        const auto list = before->appended(partialOf(path, points));
        const auto result = cache.coverage(list, raster);
        REQUIRE(result.tiles->gathered() == rasteriseBrush(*list, raster));
        if (result.lookup == CoverageLookup::Grown) {
            ++grown;
        } else {
            // Only the first update is anything else.
            REQUIRE(points == 1);
        }
    }
    CHECK(grown > 0);
    CHECK(cache.lookups(CoverageLookup::Grown) == grown);
}

TEST_CASE("A stroke on an empty brush is a miss once, then grown", "[brush][cache][grown]") {
    BrushCoverageCache cache(std::size_t{512} << 20, 64);
    constexpr ImageSize raster{600, 400};
    std::mt19937_64 g(seed + 32);
    const Stroke path = test::wanderingStroke(g, test::everydayStyle, 0.667);
    const auto empty = std::make_shared<const StrokeList>();
    for (std::size_t points = 10; points <= path.points.size(); points += 10) {
        const auto list = empty->appended(partialOf(path, points));
        const auto result = cache.coverage(list, raster, true);
        CHECK(result.lookup == (points == 10 ? CoverageLookup::Miss : CoverageLookup::Grown));
        CHECK(cache.peek(list, raster).lookup == CoverageLookup::Hit);
        CHECK(result.tiles->gathered() == rasteriseBrush(*list, raster));
    }
    CHECK(cache.lookups(CoverageLookup::Miss) == 1);
    CHECK(cache.entryCount() == 1);
}

TEST_CASE("A stroke on a brush whose list was evicted is a miss once, then grown",
          "[brush][cache][grown]") {
    BrushCoverageCache cache(std::size_t{512} << 20, 64);
    constexpr ImageSize raster{600, 400};
    std::mt19937_64 g(seed + 33);
    const auto before = test::paintedMask(seed + 34, 3, test::everydayStyle, 0.667);
    static_cast<void>(cache.coverage(before, raster));
    cache.clear();
    const std::uint64_t missesBefore = cache.lookups(CoverageLookup::Miss);
    const Stroke path = test::wanderingStroke(g, test::everydayStyle, 0.667);
    for (std::size_t points = 10; points <= path.points.size(); points += 10) {
        const auto list = before->appended(partialOf(path, points));
        const auto result = cache.coverage(list, raster);
        CHECK(result.lookup == (points == 10 ? CoverageLookup::Miss : CoverageLookup::Grown));
        CHECK(result.tiles->gathered() == rasteriseBrush(*list, raster));
    }
    CHECK(cache.lookups(CoverageLookup::Miss) == missesBefore + 1);
}

TEST_CASE("A 200-update stroke leaves one more entry, and the list before it stays a hit",
          "[brush][cache][grown]") {
    BrushCoverageCache cache(std::size_t{512} << 20, 64);
    constexpr ImageSize raster{600, 400};
    std::mt19937_64 g(seed + 35);
    const auto before = test::paintedMask(seed + 36, 4, test::everydayStyle, 0.667);
    static_cast<void>(cache.coverage(before, raster));
    const std::size_t entries = cache.entryCount();
    const std::uint64_t evictionsBefore = cache.evictions();
    Stroke path = test::wanderingStroke(g, test::detailStyle, 0.667);
    path.points.resize(200);
    std::shared_ptr<const StrokeList> last;
    for (std::size_t points = 1; points <= 200; ++points) {
        last = before->appended(partialOf(path, points));
        const auto result = cache.coverage(last, raster);
        CHECK(result.lookup == (points == 1 ? CoverageLookup::Extended : CoverageLookup::Grown));
        REQUIRE(cache.entryCount() == entries + 1);
    }
    CHECK(cache.evictions() == evictionsBefore);
    CHECK(cache.lookups(CoverageLookup::Grown) == 199);
    // Undo: the list before the stroke is held as it was.
    const auto undone = cache.coverage(before, raster);
    CHECK(undone.lookup == CoverageLookup::Hit);
    CHECK(undone.tiles->gathered() == rasteriseBrush(*before, raster));
    CHECK(cache.coverage(last, raster).tiles->gathered() == rasteriseBrush(*last, raster));
}

TEST_CASE("A changed style or a path that is not a prefix extends instead of growing",
          "[brush][cache][grown]") {
    BrushCoverageCache cache(std::size_t{512} << 20, 64);
    constexpr ImageSize raster{600, 400};
    std::mt19937_64 g(seed + 37);
    const auto before = test::paintedMask(seed + 38, 2, test::everydayStyle, 0.667);
    static_cast<void>(cache.coverage(before, raster));
    const Stroke path = test::wanderingStroke(g, test::everydayStyle, 0.667);
    REQUIRE(path.points.size() > 40);
    static_cast<void>(cache.coverage(before->appended(partialOf(path, 20)), raster));
    REQUIRE(cache.coverage(before->appended(partialOf(path, 30)), raster).lookup ==
            CoverageLookup::Grown);

    const auto asked = [&](const Stroke& stroke) {
        const auto list = before->appended(stroke);
        const CoverageLookup peeked = cache.peek(list, raster).lookup;
        const auto result = cache.coverage(list, raster);
        CHECK(result.lookup == peeked);
        CHECK(result.tiles->gathered() == rasteriseBrush(*list, raster));
        return result.lookup;
    };
    Stroke style = partialOf(path, 40);
    style.radius *= 1.5F;
    CHECK(asked(style) == CoverageLookup::Extended);
    style = partialOf(path, 40);
    style.hardness = std::min(1.0F, style.hardness + 0.2F);
    CHECK(asked(style) == CoverageLookup::Extended);
    style = partialOf(path, 40);
    style.flow *= 0.5F;
    CHECK(asked(style) == CoverageLookup::Extended);
    style = partialOf(path, 40);
    style.erase = !style.erase;
    CHECK(asked(style) == CoverageLookup::Extended);
    Stroke moved = partialOf(path, 40);
    moved.points[2].u += 0.01F;
    CHECK(asked(moved) == CoverageLookup::Extended);
    // A shorter path is not a growth either.
    CHECK(asked(partialOf(path, 5)) == CoverageLookup::Extended);
}

TEST_CASE("peek agrees with coverage along a growth", "[brush][cache][grown]") {
    BrushCoverageCache cache(std::size_t{512} << 20, 64);
    constexpr ImageSize raster{500, 300};
    std::mt19937_64 g(seed + 39);
    const auto before = test::paintedMask(seed + 40, 3, test::everydayStyle, 0.6);
    static_cast<void>(cache.coverage(before, raster));
    const Stroke path = test::wanderingStroke(g, test::everydayStyle, 0.6);
    for (std::size_t points = 1; points <= path.points.size(); points += 17) {
        const auto list = before->appended(partialOf(path, points));
        const auto peeked = cache.peek(list, raster);
        const std::size_t entries = cache.entryCount();
        const auto bytes = cache.memoryBytes();
        static_cast<void>(cache.peek(list, raster));
        CHECK(cache.entryCount() == entries);
        CHECK(cache.memoryBytes() == bytes);
        const auto result = cache.coverage(list, raster);
        CHECK(result.lookup == peeked.lookup);
        CHECK(peeked.cachedStrokes == before->size());
    }
}

TEST_CASE("A grown update clones only the tiles the new dabs and the end dab touch",
          "[brush][cache][grown]") {
    constexpr ImageSize raster{1000, 667};
    for (const std::uint32_t tile : {64U, 256U}) {
        INFO("tile " << tile);
        BrushCoverageCache cache(std::size_t{512} << 20, tile);
        std::mt19937_64 g(seed + 41);
        const auto before = test::paintedMask(seed + 42, 3, test::detailStyle, 0.667);
        const Stroke path = test::wanderingStroke(g, test::detailStyle, 0.667);
        std::shared_ptr<const CoverageTiles> shown;
        for (std::size_t points = 5; points <= path.points.size(); points += 10) {
            const auto list = before->appended(partialOf(path, points));
            const auto result = cache.coverage(list, raster);
            if (shown && result.lookup == CoverageLookup::Grown) {
                // The tiles that differ from the last picture are the ones listed as drawn.
                const std::set<std::uint32_t> differ = changedTiles(*shown, *result.tiles);
                const std::set<std::uint32_t> drawn(result.dirty.begin(), result.dirty.end());
                for (const std::uint32_t index : differ) {
                    CHECK(drawn.contains(index));
                }
                // A handful of tiles: the new dabs and the two end dabs, not the stroke.
                CHECK(drawn.size() <= 12);
            }
            shown = result.tiles;
        }
    }
}

TEST_CASE("A grown update lists the tiles the previous end dab touched and this one does not",
          "[brush][cache][grown]") {
    // A path that stops with its end dab across a tile edge, then turns back: the tile only the
    // old end dab reached goes back to its settled contents.
    constexpr ImageSize raster{512, 128};
    std::uint32_t reverted = 0;
    for (int step = 0; step < 80; ++step) {
        const float end = 0.1F + 0.0005F * static_cast<float>(step);
        BrushCoverageCache cache(std::size_t{512} << 20, 64);
        Stroke out;
        out.radius = 0.02F;
        out.hardness = 0.5F;
        out.points = {{0.05F, 0.5F}, {end, 0.5F}};
        Stroke back = out;
        back.points = {{0.05F, 0.5F}, {end, 0.5F}, {0.03F, 0.5F}, {0.02F, 0.5F}};
        const auto empty = std::make_shared<const StrokeList>();
        const auto first = cache.coverage(empty->appended(out), raster);
        const auto list = empty->appended(back);
        const auto result = cache.coverage(list, raster);
        REQUIRE(result.lookup == CoverageLookup::Grown);
        REQUIRE(result.tiles->gathered() == rasteriseBrush(*list, raster));
        const std::set<std::uint32_t> drawn(result.dirty.begin(), result.dirty.end());
        for (const std::uint32_t index : changedTiles(*first.tiles, *result.tiles)) {
            CHECK(drawn.contains(index));
        }
        // A tile of the old picture that no new dab touches, yet is listed.
        const auto touched = touchedTiles(*list, 0, raster, 64);
        for (const std::uint32_t index : drawn) {
            reverted += touched.contains(index) ? 0U : 1U;
        }
    }
    CHECK(reverted > 0);
}

TEST_CASE("A grown update of a wash brush gives the reference bits, with and without retaining",
          "[brush][cache][grown]") {
    constexpr ImageSize raster{600, 400};
    std::mt19937_64 g(seed + 60);
    const bool retain = GENERATE(true, false);
    BrushCoverageCache cache(std::size_t{512} << 20, 64);
    const Stroke path = difficultPath(g, 60, 0.3F, 0.3F, false);
    const auto empty = std::make_shared<const StrokeList>();
    std::uint64_t grown = 0;
    for (std::size_t points = 5; points <= path.points.size(); points += 7) {
        const auto list = empty->appended(partialOf(path, points));
        const auto result = cache.coverage(list, raster, retain);
        REQUIRE(result.tiles->gathered() == rasteriseBrush(*list, raster));
        grown += result.lookup == CoverageLookup::Grown ? 1 : 0;
    }
    CHECK(grown == (retain ? 7 : 0));
    CHECK(cache.entryCount() == (retain ? 1U : 0U));
}

TEST_CASE("A growth that is not retained leaves the entry it grew from", "[brush][cache][grown]") {
    constexpr ImageSize raster{400, 300};
    std::mt19937_64 g(seed + 61);
    BrushCoverageCache cache(std::size_t{512} << 20, 64);
    const Stroke path = difficultPath(g, 80, 0.03F, 0.5F, false);
    const auto empty = std::make_shared<const StrokeList>();
    static_cast<void>(cache.coverage(empty->appended(partialOf(path, 10)), raster));
    const auto bytes = cache.memoryBytes();
    const auto quiet = empty->appended(partialOf(path, 30));
    const auto once = cache.coverage(quiet, raster, false);
    CHECK(once.lookup == CoverageLookup::Grown);
    CHECK(once.tiles->gathered() == rasteriseBrush(*quiet, raster));
    CHECK(cache.entryCount() == 1);
    CHECK(cache.memoryBytes() == bytes);
    const auto kept = empty->appended(partialOf(path, 50));
    const auto next = cache.coverage(kept, raster, true);
    CHECK(next.lookup == CoverageLookup::Grown);
    CHECK(next.tiles->gathered() == rasteriseBrush(*kept, raster));
}

TEST_CASE("A growth builds on an entry inserted by a content hit", "[brush][cache][grown]") {
    constexpr ImageSize raster{400, 300};
    std::mt19937_64 g(seed + 62);
    BrushCoverageCache cache(std::size_t{512} << 20, 64);
    const Stroke path = difficultPath(g, 80, 0.03F, 0.5F, false);
    const auto empty = std::make_shared<const StrokeList>();
    static_cast<void>(cache.coverage(empty->appended(partialOf(path, 20)), raster));
    // The same list behind new pointers: a content hit that inserts its own entry.
    const auto reloaded =
        std::make_shared<const StrokeList>(std::vector<Stroke>{partialOf(path, 20)});
    CHECK(cache.coverage(reloaded, raster).lookup == CoverageLookup::ContentHit);
    Stroke longer = partialOf(path, 45);
    const auto list = std::make_shared<const StrokeList>(std::vector<Stroke>{longer});
    const auto result = cache.coverage(list, raster);
    CHECK(result.lookup == CoverageLookup::Grown);
    CHECK(result.tiles->gathered() == rasteriseBrush(*list, raster));
}

TEST_CASE("Two threads growing the same chain give the reference bits", "[brush][cache][grown]") {
    constexpr ImageSize raster{600, 400};
    for (int round = 0; round < 5; ++round) {
        BrushCoverageCache cache(std::size_t{512} << 20, 64);
        std::mt19937_64 g(seed + 50 + round);
        const auto before = test::paintedMask(seed + 60 + round, 3, test::everydayStyle, 0.667);
        const Stroke path = test::wanderingStroke(g, test::everydayStyle, 0.667);
        std::vector<std::shared_ptr<const StrokeList>> lists;
        std::vector<CoveragePlane> references;
        for (std::size_t points = 5; points <= path.points.size(); points += 5) {
            lists.push_back(before->appended(partialOf(path, points)));
            references.push_back(rasteriseBrush(*lists.back(), raster));
        }
        std::latch start(2);
        std::array<std::vector<bool>, 2> equal;
        {
            std::vector<std::jthread> threads;
            for (std::size_t n = 0; n < equal.size(); ++n) {
                threads.emplace_back([&, n] {
                    start.arrive_and_wait();
                    for (std::size_t i = 0; i < lists.size(); ++i) {
                        const auto result = cache.coverage(lists[i], raster);
                        equal[n].push_back(result.tiles->gathered() == references[i]);
                    }
                });
            }
        }
        for (const auto& thread : equal) {
            CHECK(std::ranges::all_of(thread, [](bool same) { return same; }));
        }
        // Whatever the interleaving, the cache holds a few entries, not one per update.
        CHECK(cache.entryCount() <= 4);
    }
}

TEST_CASE("An insert whose parent is gone adds a plain entry", "[brush][cache][grown]") {
    constexpr ImageSize raster{600, 400};
    BrushCoverageCache cache(std::size_t{512} << 20, 64);
    std::mt19937_64 g(seed + 70);
    const auto before = test::paintedMask(seed + 71, 2, test::everydayStyle, 0.667);
    const Stroke path = test::wanderingStroke(g, test::everydayStyle, 0.667);
    REQUIRE(path.points.size() > 60);
    const auto first = before->appended(partialOf(path, 20));
    const auto second = before->appended(partialOf(path, 40));
    const auto third = before->appended(partialOf(path, 60));
    static_cast<void>(cache.coverage(before, raster));
    static_cast<void>(cache.coverage(first, raster));
    const std::size_t entries = cache.entryCount();

    // The first thread grows `second` from `first` and stops while it draws.
    std::atomic<bool> drawing{false};
    std::atomic<bool> blocked{false};
    std::latch entered(1);
    std::latch release(1);
    ProgressChannel channel([&](const Progress&) {
        if (drawing.load() && !blocked.exchange(true)) {
            entered.count_down();
            release.wait();
        }
    });
    BrushCoverageCache::Result slow;
    std::thread worker([&] {
        detail::StepWeights weights{};
        weights[static_cast<std::size_t>(ProgressStep::Coverage)] = 1.0;
        detail::ProgressRoot root(&channel, weights, ProgressStep::Coverage);
        const detail::ProgressSpan span(ProgressStep::Coverage, 1);
        drawing = true;
        slow = cache.coverage(second, raster);
    });
    entered.wait();
    // Meanwhile the second thread grows `third` from `first`, replacing it.
    const auto quick = cache.coverage(third, raster);
    CHECK(quick.lookup == CoverageLookup::Grown);
    CHECK(cache.entryCount() == entries);
    release.count_down();
    worker.join();

    CHECK(slow.lookup == CoverageLookup::Grown);
    CHECK(slow.tiles->gathered() == rasteriseBrush(*second, raster));
    CHECK(quick.tiles->gathered() == rasteriseBrush(*third, raster));
    // `first` was gone when `second` arrived: it is inserted plainly, beside `third`.
    CHECK(cache.entryCount() == entries + 1);
    CHECK(cache.coverage(second, raster).lookup == CoverageLookup::Hit);
    CHECK(cache.coverage(third, raster).lookup == CoverageLookup::Hit);
}

// ---------------------------------------------------------------------------------------------
// Superseded entries: the base of an extension is kept for undo, and evicted first.

namespace {

constexpr ImageSize supersedeRaster{300, 200};
constexpr std::uint32_t supersedeTile = 32;

/// Makes @p count wash strokes on top of an empty list, keeping every prefix.
std::vector<std::shared_ptr<const StrokeList>> washChain(std::uint64_t chainSeed,
                                                         std::size_t count) {
    std::mt19937_64 g(chainSeed);
    std::vector<std::shared_ptr<const StrokeList>> lists;
    auto list = std::make_shared<const StrokeList>();
    for (std::size_t i = 0; i < count; ++i) {
        list = list->appended(test::wanderingStroke(g, test::washStyle, 0.667));
        lists.push_back(list);
    }
    return lists;
}

/// Makes a list of one small stroke, which adds a few tiles.
std::shared_ptr<const StrokeList> smallList(std::uint64_t listSeed) {
    return test::paintedMask(listSeed, 1, test::detailStyle, 0.667, {{0.1, 0.2, 0.1, 0.2}});
}

/// Bytes of the distinct tiles of some grids.
std::size_t distinctTileBytes(std::initializer_list<const CoverageTiles*> grids) {
    std::set<const CoverageTile*> seen;
    std::size_t total = 0;
    for (const CoverageTiles* grid : grids) {
        for (std::uint32_t i = 0; i < grid->columns() * grid->rows(); ++i) {
            const auto& tile = grid->tile(i);
            if (tile && seen.insert(tile.get()).second) {
                total += tile->values.size() * sizeof(float);
            }
        }
    }
    return total;
}

/// Bytes the chain of the first @p count lists takes in a cache with room to spare.
std::size_t chainBytes(const std::vector<std::shared_ptr<const StrokeList>>& chain,
                       std::size_t count) {
    BrushCoverageCache probe(std::size_t{512} << 20, supersedeTile);
    for (std::size_t i = 0; i < count; ++i) {
        static_cast<void>(probe.coverage(chain[i], supersedeRaster));
    }
    return probe.memoryBytes();
}

} // namespace

TEST_CASE("A superseded parent is evicted before an older live entry", "[brush][cache]") {
    const auto a = washChain(seed + 60, 1)[0];
    const auto b = washChain(seed + 61, 2);
    const auto pressure = smallList(seed + 62);
    BrushCoverageCache full(std::size_t{512} << 20, supersedeTile);
    static_cast<void>(full.coverage(a, supersedeRaster));
    static_cast<void>(full.coverage(b[0], supersedeRaster));
    static_cast<void>(full.coverage(b[1], supersedeRaster));
    BrushCoverageCache cache(full.memoryBytes(), supersedeTile);
    static_cast<void>(cache.coverage(a, supersedeRaster));
    static_cast<void>(cache.coverage(b[0], supersedeRaster));
    CHECK(cache.coverage(b[1], supersedeRaster).lookup == CoverageLookup::Extended);
    REQUIRE(cache.peek(b[0], supersedeRaster).lookup == CoverageLookup::Hit);

    static_cast<void>(cache.coverage(pressure, supersedeRaster));
    CHECK(cache.peek(a, supersedeRaster).lookup == CoverageLookup::Hit);
    const auto parent = cache.peek(b[0], supersedeRaster).lookup;
    CHECK(parent != CoverageLookup::Hit);
    CHECK(parent != CoverageLookup::ContentHit);
    CHECK(cache.peek(b[1], supersedeRaster).lookup == CoverageLookup::Hit);
    CHECK(cache.evictions() == 1);
}

TEST_CASE("A hit on a superseded entry makes it live again", "[brush][cache]") {
    const auto a = washChain(seed + 63, 1)[0];
    const auto b = washChain(seed + 64, 2);
    const auto pressure = smallList(seed + 65);
    BrushCoverageCache full(std::size_t{512} << 20, supersedeTile);
    static_cast<void>(full.coverage(a, supersedeRaster));
    static_cast<void>(full.coverage(b[0], supersedeRaster));
    static_cast<void>(full.coverage(b[1], supersedeRaster));
    BrushCoverageCache cache(full.memoryBytes(), supersedeTile);
    static_cast<void>(cache.coverage(a, supersedeRaster));
    static_cast<void>(cache.coverage(b[0], supersedeRaster));
    static_cast<void>(cache.coverage(b[1], supersedeRaster));
    // An undo: A and the newest list are used, then the parent, which is live again. A is now the
    // oldest of the three.
    static_cast<void>(cache.coverage(a, supersedeRaster));
    static_cast<void>(cache.coverage(b[1], supersedeRaster));
    CHECK(cache.coverage(b[0], supersedeRaster).lookup == CoverageLookup::Hit);

    static_cast<void>(cache.coverage(pressure, supersedeRaster));
    CHECK(cache.peek(a, supersedeRaster).lookup != CoverageLookup::Hit);
    CHECK(cache.peek(b[0], supersedeRaster).lookup == CoverageLookup::Hit);
    CHECK(cache.peek(b[1], supersedeRaster).lookup == CoverageLookup::Hit);
}

TEST_CASE("Superseded prefixes go in LRU order", "[brush][cache]") {
    const auto chain = washChain(seed + 66, 4);
    BrushCoverageCache cache(chainBytes(chain, 4), supersedeTile);
    for (const auto& list : chain) {
        static_cast<void>(cache.coverage(list, supersedeRaster));
    }
    // Pressure: small lists one by one, noting the step at which each list of the chain goes.
    constexpr int never = 99;
    std::array<int, 3> gone{never, never, never};
    for (int step = 1; step <= 30; ++step) {
        static_cast<void>(cache.coverage(smallList(seed + 70 + static_cast<std::uint64_t>(step)),
                                         supersedeRaster));
        for (std::size_t i = 0; i < gone.size(); ++i) {
            if (gone[i] == never &&
                cache.peek(chain[i], supersedeRaster).lookup != CoverageLookup::Hit) {
                gone[i] = step;
            }
        }
    }
    CHECK(gone[0] < never);
    CHECK(gone[0] <= gone[1]);
    CHECK(gone[1] <= gone[2]);
    CHECK(cache.peek(chain[3], supersedeRaster).lookup == CoverageLookup::Hit);
}

TEST_CASE("Extending releases the settled grid of the base", "[brush][cache][grown]") {
    std::mt19937_64 g(seed + 90);
    const auto before = test::paintedMask(seed + 91, 1, test::everydayStyle, 0.667);
    const Stroke path = test::wanderingStroke(g, test::everydayStyle, 0.667);
    const auto base = before->appended(partialOf(path, 20));
    BrushCoverageCache cache(std::size_t{512} << 20, supersedeTile);
    const auto first = cache.coverage(base, supersedeRaster);
    const auto extended = base->appended(test::wanderingStroke(g, test::everydayStyle, 0.667));
    const auto second = cache.coverage(extended, supersedeRaster);
    REQUIRE(second.lookup == CoverageLookup::Extended);
    // The new entry keeps its own settled grid; the base keeps tiles only.
    BrushCoverageCache alone(std::size_t{512} << 20, supersedeTile);
    static_cast<void>(alone.coverage(extended, supersedeRaster));
    const std::size_t settledOfNew = alone.memoryBytes() - distinctTileBytes({second.tiles.get()});
    CHECK(cache.memoryBytes() ==
          distinctTileBytes({first.tiles.get(), second.tiles.get()}) + settledOfNew);

    // The same stroke, longer: its base is superseded and has no settled grid, so this cannot
    // be a Grown, but it is right.
    const auto longer = before->appended(partialOf(path, 40));
    const auto grown = cache.coverage(longer, supersedeRaster);
    CHECK(grown.lookup != CoverageLookup::Grown);
    CHECK(grown.tiles->gathered() == rasteriseBrush(*longer, supersedeRaster));
}

TEST_CASE("An extension does not touch a live prefix it passes", "[brush][cache]") {
    const auto chain = washChain(seed + 92, 3);
    const auto other = washChain(seed + 93, 1)[0];
    const auto pressure = smallList(seed + 94);
    BrushCoverageCache full(std::size_t{512} << 20, supersedeTile);
    static_cast<void>(full.coverage(chain[0], supersedeRaster));
    static_cast<void>(full.coverage(chain[1], supersedeRaster));
    static_cast<void>(full.coverage(other, supersedeRaster));
    BrushCoverageCache cache(full.memoryBytes(), supersedeTile);
    static_cast<void>(cache.coverage(chain[0], supersedeRaster));
    static_cast<void>(cache.coverage(chain[1], supersedeRaster));
    // An undo: the first list is live again, and used before the unrelated one.
    CHECK(cache.coverage(chain[0], supersedeRaster).lookup == CoverageLookup::Hit);
    static_cast<void>(cache.coverage(other, supersedeRaster));
    // A longer list builds on chain[1] and passes chain[0], which it must not touch.
    CHECK(cache.coverage(chain[2], supersedeRaster, false).lookup == CoverageLookup::Extended);

    static_cast<void>(cache.coverage(pressure, supersedeRaster));
    CHECK(cache.peek(chain[0], supersedeRaster).lookup != CoverageLookup::Hit);
    CHECK(cache.peek(other, supersedeRaster).lookup == CoverageLookup::Hit);
    CHECK(cache.peek(chain[1], supersedeRaster).lookup == CoverageLookup::Hit);
}

namespace {

/// Checks that @p revive, run on the superseded parent of an extension, makes it live again: it
/// then outlives an older entry under pressure.
void checkRevived(
    std::uint64_t listSeed,
    const std::function<void(BrushCoverageCache&, const std::shared_ptr<const StrokeList>&,
                             const std::shared_ptr<const StrokeList>&)>& revive) {
    const auto a = washChain(listSeed, 1)[0];
    const auto b = washChain(listSeed + 1, 2);
    // The same content behind other pointers.
    const auto equal = washChain(listSeed + 1, 2);
    const auto pressure = smallList(listSeed + 2);
    BrushCoverageCache full(std::size_t{512} << 20, supersedeTile);
    static_cast<void>(full.coverage(a, supersedeRaster));
    static_cast<void>(full.coverage(b[0], supersedeRaster));
    static_cast<void>(full.coverage(b[1], supersedeRaster));
    BrushCoverageCache cache(full.memoryBytes(), supersedeTile);
    static_cast<void>(cache.coverage(a, supersedeRaster));
    static_cast<void>(cache.coverage(b[0], supersedeRaster));
    static_cast<void>(cache.coverage(b[1], supersedeRaster));
    static_cast<void>(cache.coverage(a, supersedeRaster));
    static_cast<void>(cache.coverage(b[1], supersedeRaster));
    revive(cache, b[0], equal[0]);

    static_cast<void>(cache.coverage(pressure, supersedeRaster));
    CHECK(cache.peek(a, supersedeRaster).lookup != CoverageLookup::Hit);
    CHECK(cache.peek(b[0], supersedeRaster).lookup == CoverageLookup::Hit);
    CHECK(cache.peek(b[1], supersedeRaster).lookup == CoverageLookup::Hit);
}

} // namespace

TEST_CASE("An undo through find or a content hit makes a superseded entry live", "[brush][cache]") {
    SECTION("find, same pointer") {
        checkRevived(seed + 100, [](BrushCoverageCache& c, const auto& same, const auto&) {
            CHECK(c.find(same, supersedeRaster) != nullptr);
        });
    }
    SECTION("find, equal list") {
        checkRevived(seed + 110, [](BrushCoverageCache& c, const auto&, const auto& equal) {
            CHECK(c.find(equal, supersedeRaster) != nullptr);
        });
    }
    SECTION("coverage, equal list") {
        checkRevived(seed + 120, [](BrushCoverageCache& c, const auto&, const auto& equal) {
            CHECK(c.coverage(equal, supersedeRaster, false).lookup == CoverageLookup::ContentHit);
        });
    }
}
