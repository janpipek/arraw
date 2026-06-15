#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "LocalAdjustment.h"

#include <QPointF>

using Catch::Matchers::WithinAbs;

TEST_CASE("Linear mask weight ramps from 0 at p0 to 1 at p1", "[localadj]") {
    // Vertical gradient on a square frame: weight 0 at the top, 1 at the bottom.
    LinearMask m{ .p0 = {0.5, 0.0}, .p1 = {0.5, 1.0} };

    REQUIRE_THAT(maskWeight(m, {0.5, 0.0}, 1.0f), WithinAbs(0.0, 1e-6));  // at p0
    REQUIRE_THAT(maskWeight(m, {0.5, 1.0}, 1.0f), WithinAbs(1.0, 1e-6));  // at p1
    REQUIRE_THAT(maskWeight(m, {0.5, 0.5}, 1.0f), WithinAbs(0.5, 1e-6));  // midpoint
}

TEST_CASE("Linear mask falloff is smoothstep, not linear", "[localadj]") {
    LinearMask m{ .p0 = {0.5, 0.0}, .p1 = {0.5, 1.0} };

    // smoothstep(t) = t*t*(3 - 2t): eases in/out, so it diverges from linear
    // everywhere except t = 0, 0.5, 1.
    REQUIRE_THAT(maskWeight(m, {0.5, 0.25}, 1.0f), WithinAbs(0.15625, 1e-6));
    REQUIRE_THAT(maskWeight(m, {0.5, 0.75}, 1.0f), WithinAbs(0.84375, 1e-6));
}

TEST_CASE("Linear mask weight is evaluated in aspect-corrected space", "[localadj]") {
    LinearMask m{ .p0 = {0.0, 0.0}, .p1 = {1.0, 1.0} };  // diagonal gradient

    // On a square frame the point (1,0) projects to the midpoint: t = 0.5.
    REQUIRE_THAT(maskWeight(m, {1.0, 0.0}, 1.0f), WithinAbs(0.5, 1e-6));

    // On a 2:1 frame the x-axis is scaled by aspect=2, so the same point
    // projects further along the line: t = 4/5 = 0.8 -> smoothstep(0.8) = 0.896.
    REQUIRE_THAT(maskWeight(m, {1.0, 0.0}, 2.0f), WithinAbs(0.896, 1e-6));
}

TEST_CASE("nearestHandle picks the closest endpoint or the derived center",
          "[localadj]") {
    LinearMask m{ .p0 = {0.2, 0.5}, .p1 = {0.8, 0.5} };  // center = (0.5, 0.5)
    const double r = 0.05;

    REQUIRE(nearestHandle(m, {0.2, 0.5}, 1.0f, r) == LinearHandle::P0);
    REQUIRE(nearestHandle(m, {0.8, 0.5}, 1.0f, r) == LinearHandle::P1);
    REQUIRE(nearestHandle(m, {0.5, 0.5}, 1.0f, r) == LinearHandle::Center);
    REQUIRE(nearestHandle(m, {0.5, 0.9}, 1.0f, r) == LinearHandle::None);
}

TEST_CASE("nearestHandle: an endpoint wins a tie against the center",
          "[localadj]") {
    // Short mask: p0=0.50, center=0.52, p1=0.54. Cursor at 0.51 is equidistant
    // (0.01) from p0 and the center; the endpoint must win.
    LinearMask m{ .p0 = {0.50, 0.5}, .p1 = {0.54, 0.5} };
    REQUIRE(nearestHandle(m, {0.51, 0.5}, 1.0f, 0.05) == LinearHandle::P0);
}

TEST_CASE("moveHandle repositions endpoints and translates via center",
          "[localadj]") {
    LinearMask m{ .p0 = {0.2, 0.5}, .p1 = {0.8, 0.5} };  // center = (0.5, 0.5)

    SECTION("P0 moves, p1 fixed") {
        const LinearMask r = moveHandle(m, LinearHandle::P0, {0.3, 0.4});
        REQUIRE_THAT(r.p0.x(), WithinAbs(0.3, 1e-9));
        REQUIRE_THAT(r.p0.y(), WithinAbs(0.4, 1e-9));
        REQUIRE(r.p1 == m.p1);
    }
    SECTION("P1 moves, p0 fixed") {
        const LinearMask r = moveHandle(m, LinearHandle::P1, {0.9, 0.6});
        REQUIRE(r.p0 == m.p0);
        REQUIRE_THAT(r.p1.x(), WithinAbs(0.9, 1e-9));
        REQUIRE_THAT(r.p1.y(), WithinAbs(0.6, 1e-9));
    }
    SECTION("Center translates both, preserving spread") {
        // old center (0.5,0.5) -> (0.6,0.5): delta (0.1, 0)
        const LinearMask r = moveHandle(m, LinearHandle::Center, {0.6, 0.5});
        REQUIRE_THAT(r.p0.x(), WithinAbs(0.3, 1e-9));
        REQUIRE_THAT(r.p1.x(), WithinAbs(0.9, 1e-9));
        REQUIRE_THAT(r.p0.y(), WithinAbs(0.5, 1e-9));
        REQUIRE_THAT(r.p1.y(), WithinAbs(0.5, 1e-9));
    }
}
