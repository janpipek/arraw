#include "develop/LocalAdjustment.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <QPointF>

using Catch::Matchers::WithinAbs;

TEST_CASE("Linear mask weight ramps from 0 at p0 to 1 at p1", "[localadj]") {
    // Vertical gradient on a square frame: weight 0 at the top, 1 at the bottom.
    LinearMask m{.p0 = {0.5, 0.0}, .p1 = {0.5, 1.0}};

    REQUIRE_THAT(maskWeight(m, {0.5, 0.0}, 1.0f), WithinAbs(0.0, 1e-6)); // at p0
    REQUIRE_THAT(maskWeight(m, {0.5, 1.0}, 1.0f), WithinAbs(1.0, 1e-6)); // at p1
    REQUIRE_THAT(maskWeight(m, {0.5, 0.5}, 1.0f), WithinAbs(0.5, 1e-6)); // midpoint
}

TEST_CASE("Linear mask falloff is smoothstep, not linear", "[localadj]") {
    LinearMask m{.p0 = {0.5, 0.0}, .p1 = {0.5, 1.0}};

    // smoothstep(t) = t*t*(3 - 2t): eases in/out, so it diverges from linear
    // everywhere except t = 0, 0.5, 1.
    REQUIRE_THAT(maskWeight(m, {0.5, 0.25}, 1.0f), WithinAbs(0.15625, 1e-6));
    REQUIRE_THAT(maskWeight(m, {0.5, 0.75}, 1.0f), WithinAbs(0.84375, 1e-6));
}

TEST_CASE("Linear mask weight is evaluated in aspect-corrected space", "[localadj]") {
    LinearMask m{.p0 = {0.0, 0.0}, .p1 = {1.0, 1.0}}; // diagonal gradient

    // On a square frame the point (1,0) projects to the midpoint: t = 0.5.
    REQUIRE_THAT(maskWeight(m, {1.0, 0.0}, 1.0f), WithinAbs(0.5, 1e-6));

    // On a 2:1 frame the x-axis is scaled by aspect=2, so the same point
    // projects further along the line: t = 4/5 = 0.8 -> smoothstep(0.8) = 0.896.
    REQUIRE_THAT(maskWeight(m, {1.0, 0.0}, 2.0f), WithinAbs(0.896, 1e-6));
}

TEST_CASE("Radial mask weight is 1 inside, ramps to 0 at the boundary", "[localadj]") {
    // Circle (rx=ry=0.4) centred, feather 0.5 → full weight out to half-radius
    // (inner = 0.5), smoothstep to 0 at the edge.
    RadialMask
        m{.center = {0.5, 0.5},
          .radiusX = 0.4,
          .radiusY = 0.4,
          .angle = 0.0,
          .feather = 0.5,
          .invert = false};

    REQUIRE_THAT(radialMaskWeight(m, {0.5, 0.5}, 1.0f), WithinAbs(1.0, 1e-6)); // centre
    REQUIRE_THAT(radialMaskWeight(m, {0.8, 0.5}, 1.0f), WithinAbs(0.5, 1e-6)); // d=0.75
    REQUIRE_THAT(radialMaskWeight(m, {1.0, 0.5}, 1.0f), WithinAbs(0.0, 1e-6)); // d=1.25, outside
}

TEST_CASE("Radial mask invert flips inside and outside", "[localadj]") {
    RadialMask
        m{.center = {0.5, 0.5},
          .radiusX = 0.4,
          .radiusY = 0.4,
          .angle = 0.0,
          .feather = 0.5,
          .invert = true};

    REQUIRE_THAT(radialMaskWeight(m, {0.5, 0.5}, 1.0f), WithinAbs(0.0, 1e-6)); // centre now 0
    REQUIRE_THAT(radialMaskWeight(m, {0.8, 0.5}, 1.0f), WithinAbs(0.5, 1e-6));
    REQUIRE_THAT(radialMaskWeight(m, {1.0, 0.5}, 1.0f), WithinAbs(1.0, 1e-6)); // outside now 1
}

TEST_CASE("Radial mask honours rotation angle", "[localadj]") {
    // rx=0.4 (long axis), ry=0.2; rotated 90° so the long axis runs vertically.
    RadialMask
        m{.center = {0.5, 0.5},
          .radiusX = 0.4,
          .radiusY = 0.2,
          .angle = 90.0,
          .feather = 0.0,
          .invert = false};

    // Straight up by 0.5 (d=1.25 on the rotated long axis) is outside.
    REQUIRE_THAT(radialMaskWeight(m, {0.5, 1.0}, 1.0f), WithinAbs(0.0, 1e-6));
    // Up by 0.2 is well inside the rotated long axis (d=0.5).
    REQUIRE_THAT(radialMaskWeight(m, {0.5, 0.7}, 1.0f), WithinAbs(1.0, 1e-6));
}

TEST_CASE("Radial mask weight is aspect-corrected", "[localadj]") {
    RadialMask
        m{.center = {0.5, 0.5},
          .radiusX = 0.4,
          .radiusY = 0.4,
          .angle = 0.0,
          .feather = 0.0,
          .invert = false};
    // aspect=2 scales x by 2: a 0.3 step in x reaches d=1.5 (outside).
    REQUIRE_THAT(radialMaskWeight(m, {0.8, 0.5}, 2.0f), WithinAbs(0.0, 1e-6));
    // At aspect=1 the same point is well inside (d=0.75).
    REQUIRE_THAT(radialMaskWeight(m, {0.8, 0.5}, 1.0f), WithinAbs(1.0, 1e-6));
}

TEST_CASE("radial handles sit on the axis ends", "[localadj]") {
    RadialMask
        m{.center = {0.5, 0.5},
          .radiusX = 0.4,
          .radiusY = 0.2,
          .angle = 0.0,
          .feather = 0.5,
          .invert = false};

    const QPointF cx = radialHandlePos(m, RadialHandle::Center, 1.0f);
    CHECK_THAT(cx.x(), WithinAbs(0.5, 1e-6));
    CHECK_THAT(cx.y(), WithinAbs(0.5, 1e-6));
    const QPointF rx = radialHandlePos(m, RadialHandle::RadiusX, 1.0f);
    CHECK_THAT(rx.x(), WithinAbs(0.9, 1e-6)); // +0.4 along x
    CHECK_THAT(rx.y(), WithinAbs(0.5, 1e-6));
    const QPointF ry = radialHandlePos(m, RadialHandle::RadiusY, 1.0f);
    CHECK_THAT(ry.x(), WithinAbs(0.5, 1e-6));
    CHECK_THAT(ry.y(), WithinAbs(0.7, 1e-6)); // +0.2 along y
    // aspect=2 halves the x offset in UV.
    const QPointF rxA = radialHandlePos(m, RadialHandle::RadiusX, 2.0f);
    CHECK_THAT(rxA.x(), WithinAbs(0.7, 1e-6));
}

TEST_CASE("moveRadialHandle moves, resizes, and rotates", "[localadj]") {
    RadialMask
        m{.center = {0.5, 0.5},
          .radiusX = 0.4,
          .radiusY = 0.2,
          .angle = 0.0,
          .feather = 0.5,
          .invert = false};

    SECTION("centre translates") {
        const RadialMask r = moveRadialHandle(m, RadialHandle::Center, {0.6, 0.4}, 1.0f);
        CHECK_THAT(r.center.x(), WithinAbs(0.6, 1e-6));
        CHECK_THAT(r.center.y(), WithinAbs(0.4, 1e-6));
    }
    SECTION("radiusX handle sets radius and angle from the centre vector") {
        const RadialMask r = moveRadialHandle(m, RadialHandle::RadiusX, {0.5, 0.9}, 1.0f);
        CHECK_THAT(r.radiusX, WithinAbs(0.4, 1e-6));
        CHECK_THAT(r.angle, WithinAbs(90.0, 1e-6));
    }
    SECTION("radiusY handle sets radius along the perpendicular") {
        const RadialMask r = moveRadialHandle(m, RadialHandle::RadiusY, {0.5, 0.75}, 1.0f);
        CHECK_THAT(r.radiusY, WithinAbs(0.25, 1e-6));
        CHECK_THAT(r.angle, WithinAbs(0.0, 1e-6)); // unchanged
    }
}

TEST_CASE("nearestHandle picks the closest endpoint or the derived center", "[localadj]") {
    LinearMask m{.p0 = {0.2, 0.5}, .p1 = {0.8, 0.5}}; // center = (0.5, 0.5)
    const double r = 0.05;

    REQUIRE(nearestHandle(m, {0.2, 0.5}, 1.0f, r) == LinearHandle::P0);
    REQUIRE(nearestHandle(m, {0.8, 0.5}, 1.0f, r) == LinearHandle::P1);
    REQUIRE(nearestHandle(m, {0.5, 0.5}, 1.0f, r) == LinearHandle::Center);
    REQUIRE(nearestHandle(m, {0.5, 0.9}, 1.0f, r) == LinearHandle::None);
}

TEST_CASE("nearestHandle: an endpoint wins a tie against the center", "[localadj]") {
    // Short mask: p0=0.50, center=0.52, p1=0.54. Cursor at 0.51 is equidistant
    // (0.01) from p0 and the center; the endpoint must win.
    LinearMask m{.p0 = {0.50, 0.5}, .p1 = {0.54, 0.5}};
    REQUIRE(nearestHandle(m, {0.51, 0.5}, 1.0f, 0.05) == LinearHandle::P0);
}

TEST_CASE("moveHandle repositions endpoints and translates via center", "[localadj]") {
    LinearMask m{.p0 = {0.2, 0.5}, .p1 = {0.8, 0.5}}; // center = (0.5, 0.5)

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

// --- History labelling: each local edit reads distinctly (docs/adr/0038) ------

TEST_CASE("localChangeLabel names an added mask by its kind", "[localadj]") {
    std::vector<LocalAdjustment> before;
    std::vector<LocalAdjustment> after(1); // one default (Linear) mask
    CHECK(localChangeLabel(before, after) == "Add Linear Mask");

    LocalAdjustment radial;
    radial.mask = RadialMask{};
    CHECK(localChangeLabel(before, {radial}) == "Add Radial Mask");
}

TEST_CASE("localChangeLabel names a removed mask by its kind", "[localadj]") {
    LocalAdjustment radial;
    radial.mask = RadialMask{};
    CHECK(localChangeLabel({radial}, {}) == "Delete Radial Mask");
}

TEST_CASE("localChangeLabel names a single delta change with its value", "[localadj]") {
    std::vector<LocalAdjustment> before(1);
    std::vector<LocalAdjustment> after(1);
    after[0].exposure = 0.5f; // EV, signed, two decimals
    CHECK(
        localChangeLabel(before, after)
        == QString::fromUtf8("Linear 1 \xe2\x80\x94 Exposure +0.50 EV"));

    after[0] = LocalAdjustment{};
    after[0].contrast = -20.0f;
    CHECK(
        localChangeLabel(before, after) == QString::fromUtf8("Linear 1 \xe2\x80\x94 Contrast -20"));
}

TEST_CASE("localChangeLabel names a geometry move", "[localadj]") {
    std::vector<LocalAdjustment> before(1);
    std::vector<LocalAdjustment> after(1);
    after[0].mask = LinearMask{{0.1, 0.1}, {0.9, 0.9}};
    CHECK(localChangeLabel(before, after) == "Linear 1 Geometry");
}

TEST_CASE("localChangeLabel names a brush mask by its kind", "[localadj]") {
    LocalAdjustment brush;
    brush.mask = BrushMask{};
    CHECK(localChangeLabel({}, {brush}) == "Add Brush Mask");
    CHECK(localChangeLabel({brush}, {}) == "Delete Brush Mask");
}

TEST_CASE("localChangeLabel calls a brush raster change a Stroke, not Geometry", "[localadj]") {
    std::vector<LocalAdjustment> before(1);
    before[0].mask = BrushMask{}; // empty raster
    auto after = before;
    after[0].mask = BrushMask{std::make_shared<const BrushRaster>(BrushRaster{4, 4, {}})};
    CHECK(localChangeLabel(before, after) == QString::fromUtf8("Brush 1 \xe2\x80\x94 Stroke"));
}

TEST_CASE("maskDisplayName names brush masks with their own ordinal", "[localadj]") {
    std::vector<LocalAdjustment> list(2);
    list[0].mask = BrushMask{};
    list[1].mask = BrushMask{};
    CHECK(maskDisplayName(list, 0) == "Brush 1");
    CHECK(maskDisplayName(list, 1) == "Brush 2");
}

TEST_CASE("localChangeLabel uses the panel ordinal for the changed mask", "[localadj]") {
    // Two Linear masks then a Radial; editing the second Linear reads "Linear 2".
    std::vector<LocalAdjustment> before(3);
    before[2].mask = RadialMask{};
    auto after = before;
    after[1].shadows = 30.0f;
    CHECK(localChangeLabel(before, after) == QString::fromUtf8("Linear 2 \xe2\x80\x94 Shadows +30"));
}

TEST_CASE("localChangeLabel falls back to the generic verb when ambiguous", "[localadj]") {
    std::vector<LocalAdjustment> before(2);
    auto after = before;
    after[0].exposure = 0.5f;
    after[1].exposure = 0.5f; // two masks changed at once
    CHECK(localChangeLabel(before, after) == "Adjust Local");
    CHECK(localChangeLabel(before, before) == "Adjust Local"); // nothing changed
}
