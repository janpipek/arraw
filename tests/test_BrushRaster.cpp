// The brush rasteriser of the prototype: placement, profile and the reference (ADR 044, section 6).

#include "BrushRaster.h"
#include "StrokeCodec.h"
#include "support/BrushGenerators.h"
#include "support/RowBandLimit.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <random>

using namespace arraw;

namespace {

constexpr std::uint64_t seed = 20261009;

float maxDifference(const CoveragePlane& a, const CoveragePlane& b) {
    REQUIRE(a.values.size() == b.values.size());
    float worst = 0.0F;
    for (std::size_t i = 0; i < a.values.size(); ++i) {
        worst = std::max(worst, std::abs(a.values[i] - b.values[i]));
    }
    return worst;
}

/// Averages 2 by 2 blocks.
CoveragePlane halved(const CoveragePlane& plane) {
    CoveragePlane out{{plane.size.width / 2, plane.size.height / 2}, {}};
    out.values.resize(static_cast<std::size_t>(out.size.width) * out.size.height);
    for (std::uint32_t y = 0; y < out.size.height; ++y) {
        for (std::uint32_t x = 0; x < out.size.width; ++x) {
            const auto at = [&](std::uint32_t dx, std::uint32_t dy) {
                return plane
                    .values[static_cast<std::size_t>(2 * y + dy) * plane.size.width + 2 * x + dx];
            };
            out.values[static_cast<std::size_t>(y) * out.size.width + x] =
                (at(0, 0) + at(1, 0) + at(0, 1) + at(1, 1)) / 4.0F;
        }
    }
    return out;
}

double meanDifference(const CoveragePlane& a, const CoveragePlane& b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.values.size(); ++i) {
        sum += std::abs(a.values[i] - b.values[i]);
    }
    return sum / static_cast<double>(a.values.size());
}

test::StrokeStyle softEveryday() {
    test::StrokeStyle style = test::everydayStyle;
    style.hardnessHigh = 0.5F;
    return style;
}

Stroke pathAt(std::initializer_list<float> xs, float radius = 0.0625F) {
    Stroke stroke{radius, 0.5F, 1.0F, false, {}};
    for (const float x : xs) {
        stroke.points.push_back({x, 0.5F});
    }
    return stroke;
}

} // namespace

TEST_CASE("Dabs sit a quarter radius apart along the path", "[brush]") {
    constexpr ImageSize raster{512, 512};
    const std::vector<DabCentre> dabs = dabCentres(pathAt({0.25F, 0.75F}), raster);
    REQUIRE(dabs.size() == 33);
    for (std::size_t k = 0; k < dabs.size(); ++k) {
        CHECK(dabs[k].x == 128.0F + 8.0F * static_cast<float>(k));
        CHECK(dabs[k].y == 256.0F);
    }
    const std::vector<DabCentre> past = dabCentres(pathAt({0.25F, 0.755F}), raster);
    REQUIRE(past.size() == 34);
    CHECK(past.back().x == Catch::Approx(0.755F * 512.0F).margin(1e-3));
    // Without the end dab, what is left is the settled part that a longer path keeps.
    const std::vector<DabCentre> settled = dabCentres(pathAt({0.25F, 0.755F}), raster, false);
    REQUIRE(settled.size() == 33);
    CHECK(std::equal(
        settled.begin(), settled.end(), past.begin(),
        [](const DabCentre& a, const DabCentre& b) { return a.x == b.x && a.y == b.y; }));
    CHECK(dabCentres(pathAt({0.4F}), raster).size() == 1);
    CHECK(dabCentres(pathAt({0.4F, 0.4F}), raster).size() == 1);
}

TEST_CASE("An extra point on a straight segment changes nothing when the split is exact",
          "[brush]") {
    constexpr ImageSize raster{512, 512};
    const auto digest = [&](const Stroke& stroke) {
        return test::planeDigest(rasteriseBrush(StrokeList(std::vector<Stroke>{stroke}), raster));
    };
    const auto plain = digest(pathAt({0.25F, 0.75F}));
    CHECK(digest(pathAt({0.25F, 0.375F, 0.5F, 0.75F})) == plain);
    CHECK(digest(pathAt(
              {0.25F, 0.3125F, 0.375F, 0.4375F, 0.5F, 0.5625F, 0.625F, 0.6875F, 0.75F})) == plain);
}

TEST_CASE("An extra point on a diagonal path changes the coverage by rounding only", "[brush]") {
    constexpr ImageSize raster{640, 427};
    std::mt19937_64 g(seed + 3);
    const SensorPoint corners[] = {{0.1F, 0.1F}, {0.8F, 0.3F}, {0.3F, 0.9F}, {0.9F, 0.8F}};
    const auto path = [&](bool extra) {
        Stroke stroke{0.03F, 0.5F, 0.8F, false, {}};
        for (std::size_t i = 0; i + 1 < std::size(corners); ++i) {
            stroke.points.push_back(corners[i]);
            const std::uint32_t inserted = extra ? test::uniformCount(g, 1, 7) : 0;
            std::vector<double> fractions;
            for (std::uint32_t n = 0; n < inserted; ++n) {
                fractions.push_back(test::unitDouble(g));
            }
            std::ranges::sort(fractions);
            for (const double t : fractions) {
                stroke.points.push_back(
                    {static_cast<float>(corners[i].u + (corners[i + 1].u - corners[i].u) * t),
                     static_cast<float>(corners[i].v + (corners[i + 1].v - corners[i].v) * t)});
            }
        }
        stroke.points.push_back(corners[std::size(corners) - 1]);
        return StrokeList(std::vector<Stroke>{stroke});
    };
    const CoveragePlane plain = rasteriseBrush(path(false), raster);
    const CoveragePlane split = rasteriseBrush(path(true), raster);
    // The inserted points are rounded to floats, so the path moves by up to 3e-8 of an edge.
    INFO("max " << maxDifference(plain, split));
    CHECK(maxDifference(plain, split) <= 1e-5F);
}

TEST_CASE("A coarser raster of the same strokes is the finer one, box-halved", "[brush]") {
    const auto strokes = test::paintedMask(seed + 4, 8, softEveryday(), 0.75);
    const CoveragePlane fine = rasteriseBrush(*strokes, {1024, 768});
    const CoveragePlane coarse = rasteriseBrush(*strokes, {512, 384});
    const CoveragePlane reduced = halved(fine);
    // Measured on the first run: max 0.0091 and mean 7.4e-5 (hardness 1: mean 0.0025); the
    // bounds are about twice that.
    const float worst = maxDifference(reduced, coarse);
    const double mean = meanDifference(reduced, coarse);
    INFO("max " << worst << " mean " << mean);
    CHECK(worst <= 0.02F);
    CHECK(mean <= 0.00015);

    test::StrokeStyle hard = test::everydayStyle;
    hard.hardnessLow = hard.hardnessHigh = 1.0F;
    const auto edges = test::paintedMask(seed + 5, 8, hard, 0.75);
    const double hardMean = meanDifference(halved(rasteriseBrush(*edges, {1024, 768})),
                                           rasteriseBrush(*edges, {512, 384}));
    INFO("hard mean " << hardMean);
    CHECK(hardMean <= 0.005);
}

TEST_CASE("A list read back from either encoding draws the same bits", "[brush]") {
    const auto strokes = test::paintedMask(seed + 6, 6, test::everydayStyle, 0.667);
    const CoveragePlane reference = rasteriseBrush(*strokes, {320, 213});
    for (const bool binary : {false, true}) {
        std::vector<Stroke> again;
        for (const auto& stroke : strokes->strokes()) {
            again.push_back((binary ? strokeFromBase64(strokeBase64(*stroke))
                                    : strokeFromText(strokeText(*stroke)))
                                .stroke);
        }
        const StrokeList reloaded(std::move(again));
        CHECK(reloaded == *strokes);
        CHECK(rasteriseBrush(reloaded, {320, 213}) == reference);
    }
}

TEST_CASE("Extending a stroke changes the coverage only near its end and the new part", "[brush]") {
    constexpr ImageSize raster{400, 300};
    std::mt19937_64 g(seed + 7);
    test::StrokeStyle style = test::everydayStyle;
    style.pointsLow = style.pointsHigh = 120;
    const Stroke base = test::wanderingStroke(g, style, 0.75);
    Stroke longer = base;
    const bool backTracking = GENERATE(as<bool>{}, false, true);
    INFO((backTracking ? "doubling back over the old end" : "a straight extension"));
    const std::size_t added = 40;
    for (std::size_t i = 1; i <= added; ++i) {
        if (backTracking) {
            // Walk back along the old path, past its end dab, and then off to the side.
            const std::size_t from = base.points.size() - 1 - std::min(i, base.points.size() - 1);
            SensorPoint point = base.points[from];
            if (i > 30) {
                point.v = std::min(0.99F, point.v + 0.01F * static_cast<float>(i - 30));
            }
            longer.points.push_back(point);
        } else {
            longer.points.push_back(
                {std::min(0.99F, base.points.back().u + 0.004F * static_cast<float>(i)),
                 base.points.back().v});
        }
    }
    const CoveragePlane before = rasteriseBrush(StrokeList({base}), raster);
    const CoveragePlane after = rasteriseBrush(StrokeList({longer}), raster);
    constexpr double longEdge = 400.0;
    // The radius that is drawn: a stroke thinner than the minimum is drawn wider.
    const double radius = std::max<double>(base.radius, minimumDrawnRadius / longEdge);
    const double s = 0.25 * base.radius;
    constexpr double margin = 1e-6;
    const auto toUnits = [&](const SensorPoint& p) {
        return std::pair{p.u * 400.0 / longEdge, p.v * 300.0 / longEdge};
    };
    const auto distanceToSegment = [](double px, double py, double ax, double ay, double bx,
                                      double by) {
        const double dx = bx - ax;
        const double dy = by - ay;
        const double length2 = dx * dx + dy * dy;
        const double t = length2 == 0.0
                             ? 0.0
                             : std::clamp(((px - ax) * dx + (py - ay) * dy) / length2, 0.0, 1.0);
        return std::sqrt((px - ax - dx * t) * (px - ax - dx * t) +
                         (py - ay - dy * t) * (py - ay - dy * t));
    };
    const auto [endX, endY] = toUnits(base.points.back());
    std::size_t differing = 0;
    for (std::uint32_t y = 0; y < raster.height; ++y) {
        for (std::uint32_t x = 0; x < raster.width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * raster.width + x;
            if (std::bit_cast<std::uint32_t>(before.values[i]) ==
                std::bit_cast<std::uint32_t>(after.values[i])) {
                continue;
            }
            ++differing;
            const double px = (x + 0.5) / longEdge;
            const double py = (y + 0.5) / longEdge;
            const bool nearEnd = std::hypot(px - endX, py - endY) <= radius + s + margin;
            bool nearAdded = false;
            for (std::size_t n = base.points.size() - 1; n + 1 < longer.points.size(); ++n) {
                const auto [ax, ay] = toUnits(longer.points[n]);
                const auto [bx, by] = toUnits(longer.points[n + 1]);
                nearAdded =
                    nearAdded || distanceToSegment(px, py, ax, ay, bx, by) <= radius + margin;
            }
            CHECK((nearEnd || nearAdded));
        }
    }
    CHECK(differing > 0);
}

TEST_CASE("One dab has the profile of the contract", "[brush]") {
    constexpr ImageSize raster{128, 128};
    const auto one = [](float hardness, float flow, bool erase) {
        return Stroke{0.25F, hardness, flow, erase, {{0.5F, 0.5F}}};
    };
    const auto rasterise = [&](std::vector<Stroke> strokes) {
        return rasteriseBrush(StrokeList(std::move(strokes)), raster);
    };
    const auto radiusAt = [](std::uint32_t x, std::uint32_t y) {
        const float dx = (static_cast<float>(x) + 0.5F) - 64.0F;
        const float dy = (static_cast<float>(y) + 0.5F) - 64.0F;
        return std::sqrt(dx * dx + dy * dy);
    };
    const CoveragePlane soft = rasterise({one(0.5F, 0.4F, false)});
    const CoveragePlane hard = rasterise({one(1.0F, 0.4F, false)});
    for (std::uint32_t y = 0; y < 128; ++y) {
        for (std::uint32_t x = 0; x < 128; ++x) {
            const float r = radiusAt(x, y);
            const std::size_t i = static_cast<std::size_t>(y) * 128 + x;
            if (r <= 16.0F) {
                CHECK(soft.values[i] == 0.4F);
            }
            if (r >= 32.0F) {
                CHECK(soft.values[i] == 0.0F);
                CHECK(hard.values[i] == 0.0F);
            }
            if (r <= 31.0F) {
                CHECK(hard.values[i] == 0.4F);
            }
        }
    }
    const std::size_t centre = 64 * 128 + 64;
    CHECK(rasterise({one(0.5F, 0.4F, false), one(0.5F, 0.5F, true)}).values[centre] ==
          0.4F * (1.0F - 0.5F));
    CHECK(rasterise({one(0.5F, 0.5F, true), one(0.5F, 0.4F, false)}).values[centre] == 0.4F);
}

namespace {

/// Places dabs straight from the ADR: cumulative arc length in double, a search per dab, and an
/// end dab when the spacing did not land on the end.
std::vector<std::pair<double, double>> naiveDabs(const Stroke& stroke, ImageSize raster) {
    const double longEdge = std::max(raster.width, raster.height);
    std::vector<std::pair<double, double>> vertices;
    for (const SensorPoint& p : stroke.points) {
        vertices.emplace_back(static_cast<double>(p.u) * raster.width,
                              static_cast<double>(p.v) * raster.height);
    }
    std::vector<double> cumulative{0.0};
    for (std::size_t i = 1; i < vertices.size(); ++i) {
        cumulative.push_back(cumulative.back() +
                             std::hypot(vertices[i].first - vertices[i - 1].first,
                                        vertices[i].second - vertices[i - 1].second));
    }
    const double total = cumulative.back();
    const double spacing = 0.25 * static_cast<double>(stroke.radius) * longEdge;
    std::vector<std::pair<double, double>> dabs{vertices.front()};
    std::uint64_t last = 0;
    for (std::uint64_t k = 1; static_cast<double>(k) * spacing <= total; ++k) {
        const double at = static_cast<double>(k) * spacing;
        std::size_t segment = 0;
        while (segment + 1 < vertices.size() - 1 && cumulative[segment + 1] < at) {
            ++segment;
        }
        const double length = cumulative[segment + 1] - cumulative[segment];
        const double t = length == 0.0 ? 0.0 : (at - cumulative[segment]) / length;
        dabs.emplace_back(vertices[segment].first +
                              (vertices[segment + 1].first - vertices[segment].first) * t,
                          vertices[segment].second +
                              (vertices[segment + 1].second - vertices[segment].second) * t);
        last = k;
    }
    if (static_cast<double>(last) * spacing < total) {
        dabs.push_back(vertices.back());
    }
    return dabs;
}

} // namespace

TEST_CASE("Dabs are placed as the contract says, on a path with joins and repeats", "[brush]") {
    constexpr ImageSize raster{97, 64};
    Stroke stroke{0.04F, 0.5F, 1.0F, false, {}};
    stroke.points = {{0.1F, 0.1F}, {0.1F, 0.1F}, {0.55F, 0.35F}, {0.55F, 0.35F}, {0.31F, 0.92F},
                     {0.9F, 0.7F}, {0.9F, 0.7F}, {0.6F, 0.05F},  {0.62F, 0.07F}, {0.95F, 0.95F}};
    const std::vector<DabCentre> actual = dabCentres(stroke, raster);
    const auto expected = naiveDabs(stroke, raster);
    REQUIRE(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        CHECK(actual[i].x == Catch::Approx(expected[i].first).margin(1e-4));
        CHECK(actual[i].y == Catch::Approx(expected[i].second).margin(1e-4));
    }
}

TEST_CASE("The rasteriser agrees with a naive loop written from the contract", "[brush]") {
    constexpr ImageSize raster{64, 48};
    const auto strokes = test::paintedMask(seed + 8, 6, test::everydayStyle, 0.75);
    const CoveragePlane fast = rasteriseBrush(*strokes, raster);
    std::vector<double> naive(64 * 48, 0.0);
    for (const auto& stroke : strokes->strokes()) {
        // Radius and core in pixels; a brush under the minimum is drawn wider and weaker.
        const double radius = std::max<double>(stroke->radius * 64.0, minimumDrawnRadius);
        const double inner = std::min(stroke->hardness * radius, radius - 1.0);
        const double thin = std::min(1.0, stroke->radius * 64.0 / radius);
        const double flow = stroke->flow * thin * thin;
        for (const auto& [cx, cy] : naiveDabs(*stroke, raster)) {
            for (std::uint32_t y = 0; y < 48; ++y) {
                for (std::uint32_t x = 0; x < 64; ++x) {
                    const double r = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
                    const double t = std::clamp((r - inner) / (radius - inner), 0.0, 1.0);
                    const double d = 1.0 - t * t * (3.0 - 2.0 * t);
                    double& m = naive[static_cast<std::size_t>(y) * 64 + x];
                    m = stroke->erase ? m * (1.0 - flow * d) : m + flow * d * (1.0 - m);
                }
            }
        }
    }
    double worst = 0.0;
    for (std::size_t i = 0; i < naive.size(); ++i) {
        worst = std::max(worst, std::abs(naive[i] - fast.values[i]));
    }
    CHECK(worst <= 1e-4);
}

TEST_CASE("A thinnest stroke is drawn at every raster size and sub-pixel position", "[brush]") {
    for (const std::uint32_t edge : {400U, 750U, 1500U}) {
        const ImageSize raster{edge, edge * 2 / 3};
        const double radiusPx = static_cast<double>(minimumBrushRadius) * edge;
        double lightest = 1e9;
        double heaviest = 0.0;
        for (int step = 0; step < 8; ++step) {
            // The line runs across rows 100 to 101 and moves by eighths of a pixel.
            const double y = 100.0 + step / 8.0;
            const Stroke stroke{minimumBrushRadius,
                                1.0F,
                                1.0F,
                                false,
                                {{0.2F, static_cast<float>(y / raster.height)},
                                 {0.8F, static_cast<float>(y / raster.height)}}};
            const CoveragePlane plane = rasteriseBrush(StrokeList({stroke}), raster);
            // Coverage per pixel of length, away from the ends.
            const std::uint32_t from = edge * 2 / 5;
            const std::uint32_t to = edge * 3 / 5;
            double mass = 0.0;
            for (std::uint32_t row = 0; row < raster.height; ++row) {
                for (std::uint32_t x = from; x < to; ++x) {
                    mass += plane.values[static_cast<std::size_t>(row) * edge + x];
                }
            }
            mass /= to - from;
            lightest = std::min(lightest, mass);
            heaviest = std::max(heaviest, mass);
        }
        INFO("long edge " << edge << " radius " << radiusPx << " px: " << lightest << " to "
                          << heaviest << " per pixel of length");
        // Never lost between pixel centres, never thinner than the line it stands for, never
        // much heavier, and the same weight wherever it falls. Measured: 1.02 to 1.14.
        CHECK(lightest >= 2.0 * radiusPx);
        CHECK(heaviest <= 6.0 * radiusPx);
        CHECK(heaviest / lightest <= 1.2);
    }
}

TEST_CASE("Threads and strips give the same bits", "[brush][slow]") {
    constexpr ImageSize raster{1000, 667};
    const auto strokes = test::paintedMask(seed + 9, 6, test::everydayStyle, 0.667);
    const CoveragePlane threaded = rasteriseBrush(*strokes, raster);
    {
        const test::ScopedRowBandLimit one(1);
        CHECK(rasteriseBrush(*strokes, raster) == threaded);
    }
    const auto placed = placedStrokes(*strokes, raster);
    CoveragePlane strips{raster, std::vector<float>(static_cast<std::size_t>(1000) * 667)};
    for (std::uint32_t y = 0; y < raster.height; y += 37) {
        const std::uint32_t height = std::min<std::uint32_t>(37, raster.height - y);
        paintRegion(placed, {0, y, raster.width, height},
                    std::span<float>(strips.values.data() + static_cast<std::size_t>(y) * 1000,
                                     static_cast<std::size_t>(height) * 1000));
    }
    CHECK(strips == threaded);
}

TEST_CASE("Rasteriser 1 draws what it always drew", "[brush]") {
    std::vector<Stroke> strokes;
    const auto base = test::paintedMask(seed, 12, test::everydayStyle, 213.0 / 320.0);
    for (const auto& stroke : base->strokes()) {
        strokes.push_back(*stroke);
    }
    for (const std::size_t at : {std::size_t{3}, std::size_t{8}}) {
        strokes[at].erase = true;
    }
    const CoveragePlane plane = rasteriseBrush(StrokeList(std::move(strokes)), {320, 213});
    // Pinned from the first run: the rules of version 1 must not change, on any platform.
    INFO(std::hex << test::planeDigest(plane));
    CHECK(test::planeDigest(plane) == 0x7f55489a6ffcc311ULL);

    // Strokes of a pixel or two, some under the minimum drawn radius, so that the rule for
    // thin strokes is pinned too.
    const auto thin = test::paintedMask(seed + 1, 10, test::detailStyle, 213.0 / 320.0);
    const CoveragePlane thinPlane = rasteriseBrush(*thin, {320, 213});
    INFO(std::hex << test::planeDigest(thinPlane));
    CHECK(test::planeDigest(thinPlane) == 0xcee6eb41d335752dULL);
}

TEST_CASE("An empty list draws nothing and another rasteriser is refused", "[brush]") {
    const CoveragePlane none = rasteriseBrush(StrokeList(), {40, 30});
    CHECK(std::ranges::all_of(none.values, [](float v) { return v == 0.0F; }));
    CHECK_THROWS_AS(rasteriseBrush(StrokeList({}, 2), {40, 30}), std::invalid_argument);
    CHECK_THROWS_AS(rasteriseBrush(StrokeList(), {0, 30}), std::invalid_argument);
}
