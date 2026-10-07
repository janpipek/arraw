#include "CurveEditing.h"
#include "ToneCurve.h"

#include <ToneCurveSettings.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <random>
#include <vector>

using namespace arraw;
using namespace arraw::app;
using Catch::Approx;

/// The curve editor's point logic, apart from painting (ADR 036).

namespace {

ToneCurve curveWith(std::vector<CurvePoint> points) {
    ToneCurve curve;
    curve.points = std::move(points);
    REQUIRE(isWellFormed(curve));
    return curve;
}

} // namespace

TEST_CASE("Each channel names its own curve", "[app][curve]") {
    ToneCurveSettings curves;
    curves.red.points = {{0.0F, 0.1F}, {1.0F, 1.0F}};
    CHECK(&curveOf(curves, CurveChannel::Luma) == &curves.luma);
    CHECK(&curveOf(curves, CurveChannel::Red) == &curves.red);
    CHECK(&curveOf(curves, CurveChannel::Green) == &curves.green);
    CHECK(&curveOf(curves, CurveChannel::Blue) == &curves.blue);
    const ToneCurveSettings& shown = curves;
    CHECK(curveOf(shown, CurveChannel::Red).points.front().y == 0.1F);
}

TEST_CASE("A point is added where asked, between its neighbours", "[app][curve]") {
    ToneCurve curve;
    const auto index = insertPoint(curve, {0.25F, 0.4F});
    REQUIRE(index == 1U);
    CHECK(curve.points[1] == CurvePoint{0.25F, 0.4F});
    CHECK(isWellFormed(curve));

    CHECK(insertPoint(curve, {0.75F, 0.8F}) == 2U);
    CHECK(insertPoint(curve, {0.5F, 0.5F}) == 2U);
    CHECK(curve.points.size() == 5);
    CHECK(isWellFormed(curve));
}

TEST_CASE("A point added close to another is nudged clear of it, or refused", "[app][curve]") {
    ToneCurve curve = curveWith({{0.0F, 0.0F}, {0.5F, 0.5F}, {1.0F, 1.0F}});

    SECTION("nudged when the gap has room") {
        const auto index = insertPoint(curve, {0.505F, 0.6F});
        REQUIRE(index == 2U);
        CHECK(curve.points[2].x == Approx(0.5F + minimumCurvePointSpacing));
        CHECK(isWellFormed(curve));
    }
    SECTION("refused when the gap is too narrow for the spacing on both sides") {
        curve = curveWith({{0.0F, 0.0F}, {0.5F, 0.5F}, {0.515F, 0.6F}, {1.0F, 1.0F}});
        const ToneCurve before = curve;
        CHECK_FALSE(insertPoint(curve, {0.507F, 0.55F}));
        CHECK(curve == before);
    }
    SECTION("y is clamped into 0 to 1") {
        const auto index = insertPoint(curve, {0.25F, 1.7F});
        REQUIRE(index);
        CHECK(curve.points[*index].y == 1.0F);
    }
}

TEST_CASE("A point is never added at or beyond an end", "[app][curve]") {
    ToneCurve curve;
    CHECK_FALSE(insertPoint(curve, {0.0F, 0.5F}));
    CHECK_FALSE(insertPoint(curve, {1.0F, 0.5F}));
    CHECK_FALSE(insertPoint(curve, {-0.2F, 0.5F}));
    CHECK_FALSE(insertPoint(curve, {1.2F, 0.5F}));
    CHECK(curve.isIdentity());
}

TEST_CASE("A full curve takes no more points", "[app][curve]") {
    ToneCurve curve;
    for (std::size_t i = 1; curve.points.size() < maximumCurvePoints; ++i) {
        REQUIRE(insertPoint(curve, {static_cast<float>(i) / 16.0F, 0.5F}));
    }
    CHECK(curve.points.size() == maximumCurvePoints);
    CHECK(isWellFormed(curve));
    const ToneCurve full = curve;
    CHECK_FALSE(insertPoint(curve, {0.97F, 0.5F}));
    CHECK(curve == full);
}

TEST_CASE("Only interior points are removed, and never below two", "[app][curve]") {
    ToneCurve curve = curveWith({{0.0F, 0.0F}, {0.5F, 0.3F}, {1.0F, 1.0F}});
    CHECK_FALSE(isRemovable(curve, 0));
    CHECK_FALSE(isRemovable(curve, 2));
    CHECK_FALSE(isRemovable(curve, 7));
    CHECK_FALSE(removePoint(curve, 0));
    CHECK_FALSE(removePoint(curve, 2));
    REQUIRE(removePoint(curve, 1));
    CHECK(curve.isIdentity());
    CHECK_FALSE(removePoint(curve, 0));
    CHECK_FALSE(removePoint(curve, 1));
    CHECK(curve.points.size() == minimumCurvePoints);
}

TEST_CASE("A dragged point keeps its distance from its neighbours", "[app][curve]") {
    const ToneCurve curve = curveWith({{0.0F, 0.0F}, {0.3F, 0.2F}, {0.6F, 0.7F}, {1.0F, 1.0F}});

    SECTION("an interior point stops short of either neighbour") {
        CHECK(clampedPosition(curve, 1, {0.9F, 0.5F}).x == Approx(0.6F - minimumCurvePointSpacing));
        CHECK(clampedPosition(curve, 1, {-0.5F, 0.5F}).x == Approx(minimumCurvePointSpacing));
        CHECK(clampedPosition(curve, 2, {0.1F, 0.5F}).x == Approx(0.3F + minimumCurvePointSpacing));
        CHECK(clampedPosition(curve, 1, {0.4F, 0.5F}) == CurvePoint{0.4F, 0.5F});
    }
    SECTION("an end moves only in y") {
        CHECK(clampedPosition(curve, 0, {0.4F, 0.3F}) == CurvePoint{0.0F, 0.3F});
        CHECK(clampedPosition(curve, 3, {0.2F, 0.8F}) == CurvePoint{1.0F, 0.8F});
    }
    SECTION("y is clamped into 0 to 1") {
        CHECK(clampedPosition(curve, 1, {0.3F, -2.0F}).y == 0.0F);
        CHECK(clampedPosition(curve, 3, {1.0F, 3.0F}).y == 1.0F);
    }
}

TEST_CASE("A point dragged out is removed, and brought back is restored", "[app][curve]") {
    const ToneCurve curve = curveWith({{0.0F, 0.0F}, {0.3F, 0.2F}, {0.6F, 0.7F}, {1.0F, 1.0F}});
    const CurvePointDrag drag(curve, 1);

    const ToneCurve out = drag.curveAt({0.4F, 1.5F}, true);
    CHECK(out.points.size() == 3);
    CHECK(out.points[1] == CurvePoint{0.6F, 0.7F});
    CHECK(isWellFormed(out));

    const ToneCurve back = drag.curveAt({0.45F, 0.35F}, false);
    CHECK(back.points.size() == 4);
    CHECK(back.points[1] == CurvePoint{0.45F, 0.35F});
    CHECK(isWellFormed(back));

    SECTION("an end dragged out stays, moved only in y") {
        const CurvePointDrag endDrag(curve, 0);
        const ToneCurve stays = endDrag.curveAt({-0.4F, 0.2F}, true);
        CHECK(stays.points.size() == 4);
        CHECK(stays.points[0] == CurvePoint{0.0F, 0.2F});
    }
    SECTION("the last interior point of a two-point curve cannot exist, so nothing is removed") {
        const CurvePointDrag identity(ToneCurve{}, 1);
        CHECK(identity.curveAt({0.5F, 2.0F}, true).points.size() == 2);
    }
    CHECK_THROWS_AS(CurvePointDrag(curve, 4), std::out_of_range);
}

TEST_CASE("Any sequence of edits leaves a well-formed curve", "[app][curve]") {
    std::mt19937 random(20261004);
    std::uniform_real_distribution<float> coordinate(-0.3F, 1.3F);
    std::uniform_int_distribution<int> operation(0, 2);
    ToneCurve curve;
    for (int step = 0; step < 5000; ++step) {
        const CurvePoint at{coordinate(random), coordinate(random)};
        const std::size_t index =
            std::uniform_int_distribution<std::size_t>(0, curve.points.size() - 1)(random);
        switch (operation(random)) {
        case 0:
            (void)insertPoint(curve, at);
            break;
        case 1:
            curve = CurvePointDrag(curve, index).curveAt(at, coordinate(random) > 1.15F);
            break;
        default:
            (void)removePoint(curve, index);
            break;
        }
        CAPTURE(step);
        REQUIRE(isWellFormed(curve));
    }
}

TEST_CASE("The nearest point within the radius is found", "[app][curve]") {
    const ToneCurve curve = curveWith({{0.0F, 0.0F}, {0.5F, 0.5F}, {0.53F, 0.52F}, {1.0F, 1.0F}});
    CHECK(pointNear(curve, {0.51F, 0.5F}, 0.05F) == 1U);
    CHECK(pointNear(curve, {0.525F, 0.52F}, 0.05F) == 2U);
    CHECK(pointNear(curve, {0.98F, 0.99F}, 0.05F) == 3U);
    CHECK_FALSE(pointNear(curve, {0.25F, 0.75F}, 0.05F));
}

TEST_CASE("A curve is drawn from the table the render reads", "[app][curve]") {
    SECTION("the identity is the diagonal") {
        const std::vector<float> values = sampleCurve(ToneCurve{}, 5);
        CHECK(values == std::vector<float>{0.0F, 0.25F, 0.5F, 0.75F, 1.0F});
    }
    SECTION("any other curve is the engine's evaluation") {
        const ToneCurve curve = curveWith({{0.0F, 0.1F}, {0.4F, 0.6F}, {1.0F, 0.9F}});
        const CurvePlan plan = curvePlanFor(curve);
        const std::vector<float> values = sampleCurve(curve, 101);
        for (std::size_t i = 0; i < values.size(); ++i) {
            CAPTURE(i);
            CHECK(values[i] == evaluateCurve(plan, static_cast<float>(i) / 100.0F));
        }
        CHECK(values.front() == 0.1F);
        CHECK(values.back() == 0.9F);
    }
    CHECK_THROWS_AS(sampleCurve(ToneCurve{}, 1), std::invalid_argument);
    ToneCurve malformed;
    malformed.points = {{0.2F, 0.0F}, {1.0F, 1.0F}};
    CHECK_THROWS_AS(sampleCurve(malformed, 10), std::invalid_argument);
}
