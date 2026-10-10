// The stroke model of the brush prototype (ADR 044, section 6).

#include <BrushStrokes.h>
#include <LocalAdjustments.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <vector>

using namespace arraw;

namespace {

Stroke simple(float u = 0.5F) {
    Stroke stroke;
    stroke.points = {{u, 0.5F}, {u + 0.1F, 0.6F}};
    return stroke;
}

} // namespace

TEST_CASE("Appending shares every earlier stroke", "[brush]") {
    auto list = std::make_shared<const StrokeList>(std::vector<Stroke>{simple(0.1F), simple(0.2F)});
    const std::uint64_t hash = list->contentHash();
    const auto longer = list->appended(simple(0.3F));
    REQUIRE(longer->size() == 3);
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(longer->strokes()[i].get() == list->strokes()[i].get());
    }
    CHECK(list->size() == 2);
    CHECK(list->contentHash() == hash);
    CHECK((*list)[1] == simple(0.2F));
    CHECK(longer->contentHash() != hash);
    CHECK(longer->pointCount() == 6);
}

TEST_CASE("Equal contents compare equal, anything else does not", "[brush]") {
    const StrokeList a(std::vector<Stroke>{simple(0.1F), simple(0.2F)});
    const StrokeList b(std::vector<Stroke>{simple(0.1F), simple(0.2F)});
    CHECK(a == b);
    CHECK(a.contentHash() == b.contentHash());
    CHECK(StrokeList() == StrokeList(std::vector<Stroke>{}));

    CHECK_FALSE(a == StrokeList(std::vector<Stroke>{simple(0.1F), simple(0.2F)}, 2));
    Stroke moved = simple(0.2F);
    moved.points[1].v = std::nextafter(moved.points[1].v, 1.0F);
    CHECK_FALSE(a == StrokeList(std::vector<Stroke>{simple(0.1F), moved}));
    CHECK_FALSE(a == StrokeList(std::vector<Stroke>{simple(0.2F), simple(0.1F)}));
    CHECK(a.contentHash() !=
          StrokeList(std::vector<Stroke>{simple(0.2F), simple(0.1F)}).contentHash());
}

TEST_CASE("The caps on strokes and points are enforced", "[brush]") {
    // Dots cost no swept area or dabs, so only the count of strokes bounds this list.
    Stroke dot = simple();
    dot.points.resize(1);
    std::shared_ptr<const StrokeList> list = std::make_shared<const StrokeList>();
    for (std::size_t i = 0; i < maximumStrokesPerMask; ++i) {
        list = list->appended(dot);
    }
    REQUIRE(list->size() == maximumStrokesPerMask);
    const std::uint64_t hash = list->contentHash();
    CHECK_THROWS_AS(list->appended(dot), std::invalid_argument);
    CHECK(list->size() == maximumStrokesPerMask);
    CHECK(list->contentHash() == hash);
    CHECK_THROWS_AS(StrokeList(std::vector<Stroke>(maximumStrokesPerMask + 1, dot)),
                    std::invalid_argument);

    Stroke long_ = simple();
    long_.points.assign(maximumStrokePoints + 1, SensorPoint{0.5F, 0.5F});
    CHECK_THROWS_AS(validate(long_), std::invalid_argument);
    long_.points.pop_back();
    CHECK_NOTHROW(validate(long_));
}

TEST_CASE("Validation refuses what the contract does not allow", "[brush]") {
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float inf = std::numeric_limits<float>::infinity();
    CHECK_NOTHROW(validate(simple()));
    const auto refuses = [](auto change) {
        Stroke stroke = simple();
        change(stroke);
        CHECK_THROWS_AS(validate(stroke), std::invalid_argument);
    };
    refuses([](Stroke& s) { s.radius = minimumBrushRadius / 2; });
    refuses([](Stroke& s) { s.radius = maximumBrushRadius * 2; });
    refuses([](Stroke& s) { s.radius = nan; });
    refuses([](Stroke& s) { s.hardness = -0.1F; });
    refuses([](Stroke& s) { s.hardness = 1.1F; });
    refuses([](Stroke& s) { s.hardness = nan; });
    refuses([](Stroke& s) { s.flow = -0.1F; });
    refuses([](Stroke& s) { s.flow = 1.1F; });
    refuses([](Stroke& s) { s.flow = inf; });
    refuses([](Stroke& s) { s.points.clear(); });
    refuses([](Stroke& s) { s.points[0].u = nan; });
    refuses([](Stroke& s) { s.points[1].v = -inf; });
    refuses([](Stroke& s) { s.points[0].u = minimumMaskPosition - 0.5F; });
    refuses([](Stroke& s) { s.points[0].v = maximumMaskPosition + 0.5F; });
}

TEST_CASE("Normalising clamps, and refuses what cannot be clamped", "[brush]") {
    Stroke wild = simple();
    wild.radius = 7.0F;
    wild.hardness = -3.0F;
    wild.flow = 2.0F;
    wild.points[0] = {-9.0F, 9.0F};
    const Stroke clamped = normalised(wild);
    CHECK(clamped.radius == maximumBrushRadius);
    CHECK(clamped.hardness == 0.0F);
    CHECK(clamped.flow == 1.0F);
    CHECK(clamped.points[0].u == minimumMaskPosition);
    CHECK(clamped.points[0].v == maximumMaskPosition);
    CHECK_NOTHROW(validate(clamped));
    wild.radius = 0.0F;
    CHECK(normalised(wild).radius == minimumBrushRadius);

    Stroke bad = simple();
    bad.flow = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_AS(normalised(bad), std::invalid_argument);
    bad = simple();
    bad.points[1].u = std::numeric_limits<float>::infinity();
    CHECK_THROWS_AS(normalised(bad), std::invalid_argument);
    bad = simple();
    bad.points.clear();
    CHECK_THROWS_AS(normalised(bad), std::invalid_argument);
    bad.points.assign(maximumStrokePoints + 1, SensorPoint{});
    CHECK_THROWS_AS(normalised(bad), std::invalid_argument);
}

namespace {

/// A straight stroke of the given radius from (0, 0.5) to (x, 0.5).
Stroke straight(float radius, float length, std::size_t points = 2) {
    Stroke stroke;
    stroke.radius = radius;
    for (std::size_t i = 0; i < points; ++i) {
        stroke.points.push_back(
            {length * static_cast<float>(i) / static_cast<float>(points - 1), 0.5F});
    }
    return stroke;
}

} // namespace

TEST_CASE("A stroke's budget is its length times its radius and over a quarter radius", "[brush]") {
    const StrokeBudget budget = budgetOf(straight(0.02F, 1.0F, 5));
    CHECK(budget.points == 5);
    CHECK(budget.sweptArea == Catch::Approx(0.02).epsilon(1e-6));
    CHECK(budget.dabs == Catch::Approx(200.0).epsilon(1e-6));
    CHECK(budgetOf(simple()).sweptArea > 0.0);
    Stroke dot = simple();
    dot.points.resize(1);
    CHECK(budgetOf(dot).sweptArea == 0.0);
    CHECK(budgetOf(dot).dabs == 0.0);
}

TEST_CASE("A mask holds at most 100 000 points", "[brush]") {
    Stroke dense = straight(0.01F, 0.001F, maximumStrokePoints);
    std::shared_ptr<const StrokeList> list = std::make_shared<const StrokeList>();
    for (std::size_t i = 0; i < maximumPointsPerMask / maximumStrokePoints; ++i) {
        list = list->appended(dense);
    }
    REQUIRE(list->pointCount() == maximumPointsPerMask);
    CHECK_FALSE(list->accepts(simple()));
    const std::uint64_t hash = list->contentHash();
    CHECK_THROWS_AS(list->appended(simple()), std::invalid_argument);
    CHECK(list->size() == maximumPointsPerMask / maximumStrokePoints);
    CHECK(list->contentHash() == hash);
    CHECK_THROWS_AS(StrokeList(std::vector<Stroke>(11, dense)), std::invalid_argument);
    CHECK_NOTHROW(StrokeList(std::vector<Stroke>(10, dense)));
}

TEST_CASE("A mask's swept area is at most 4", "[brush]") {
    // Each stroke: length 1 at radius 0.5 sweeps 0.5.
    const Stroke wide = straight(0.5F, 1.0F);
    std::shared_ptr<const StrokeList> list = std::make_shared<const StrokeList>();
    for (int i = 0; i < 8; ++i) {
        REQUIRE(list->accepts(wide));
        list = list->appended(wide);
    }
    CHECK(list->budget().sweptArea == Catch::Approx(4.0));
    CHECK_FALSE(list->accepts(wide));
    const std::uint64_t hash = list->contentHash();
    CHECK_THROWS_AS(list->appended(wide), std::invalid_argument);
    CHECK(list->size() == 8);
    CHECK(list->contentHash() == hash);
    Stroke dot = wide;
    dot.points.resize(1);
    CHECK(list->accepts(dot));
    CHECK_FALSE(list->accepts(straight(0.0005F, 1.0F)));

    CHECK_NOTHROW(StrokeList(std::vector<Stroke>(8, wide)));
    CHECK_THROWS_AS(StrokeList(std::vector<Stroke>(9, wide)), std::invalid_argument);
}

TEST_CASE("A mask's dabs are at most 2 000 000", "[brush]") {
    // Length 1 at radius 0.0005: 8 000 dabs, area 0.0005. A zigzag lengthens the path.
    Stroke fine = simple();
    fine.radius = 0.0005F;
    fine.points.clear();
    for (std::size_t i = 0; i < 5'000; ++i) {
        fine.points.push_back({i % 2 == 0 ? 0.0F : 1.0F, 0.5F});
    }
    // 4 999 passes of length 1: 39.99 million dabs by itself, past the budget.
    CHECK_THROWS_AS(validate(fine), std::invalid_argument);
    CHECK_THROWS_AS(normalised(fine), std::invalid_argument);
    CHECK_THROWS_AS(StrokeList(std::vector<Stroke>{fine}), std::invalid_argument);

    // 250 passes: 2 000 000 dabs minus 8 000: fits alone, and two do not fit together.
    Stroke half = fine;
    half.points.resize(250);
    REQUIRE(budgetOf(half).dabs < maximumDabsPerMask);
    REQUIRE(budgetOf(half).dabs > maximumDabsPerMask / 2);
    CHECK_NOTHROW(validate(half));
    const auto one = std::make_shared<const StrokeList>()->appended(half);
    CHECK_FALSE(one->accepts(half));
    CHECK_THROWS_AS(one->appended(half), std::invalid_argument);
    CHECK(one->size() == 1);
    CHECK_THROWS_AS(StrokeList(std::vector<Stroke>{half, half}), std::invalid_argument);
}

TEST_CASE("The budgets count the long-edge metric at most, whatever the aspect", "[brush]") {
    // A path along v only: normalised length 1 bounds the long-edge length of any aspect.
    Stroke vertical = simple();
    vertical.radius = 1.0F;
    vertical.points = {{0.5F, 0.0F}, {0.5F, 1.0F}};
    CHECK(budgetOf(vertical).sweptArea == Catch::Approx(1.0));
    CHECK(budgetOf(vertical).dabs == Catch::Approx(4.0));
}
