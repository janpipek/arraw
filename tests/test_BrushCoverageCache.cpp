// The tiled coverage cache of the brush prototype (ADR 044, section 6).

#include "BrushCoverageCache.h"
#include "support/BrushGenerators.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
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
        CHECK(cache.memoryBytes() == results[0].tiles->tileBytes());
        const CoveragePlane reference = rasteriseBrush(*list, raster);
        CHECK(results[0].tiles->gathered() == reference);
        CHECK(results[1].tiles->gathered() == reference);
    }
}
