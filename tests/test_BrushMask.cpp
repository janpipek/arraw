#include "develop/BrushMask.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>

using Catch::Matchers::WithinAbs;

// A blank `size`×`size` mask (all zero).
static BrushRaster blankRaster(int size) {
    return BrushRaster{size, size, std::vector<uint8_t>(static_cast<size_t>(size * size), 0)};
}

// Read one pixel (0..255) from a raster.
static int at(const BrushRaster& r, int x, int y) {
    return r.data[static_cast<size_t>(y * r.width + x)];
}

// ---------------------------------------------------------------------------
// brushDabProfile — coverage falloff of one dab (docs/adr/0047)
// ---------------------------------------------------------------------------

TEST_CASE("brushDabProfile is 1.0 at the dab centre", "[brush]") {
    REQUIRE_THAT(brushDabProfile(0.0, 30.0, 0.5), WithinAbs(1.0, 1e-6));
}

TEST_CASE("brushDabProfile is 0.0 beyond the radius", "[brush]") {
    REQUIRE_THAT(brushDabProfile(40.0, 30.0, 0.5), WithinAbs(0.0, 1e-6));
}

TEST_CASE("brushDabProfile is 0.5 at the middle of the feather band", "[brush]") {
    // feather=1 → inner core at 0, band spans the whole radius; the midpoint of a
    // smoothstep is exactly 0.5.
    REQUIRE_THAT(brushDabProfile(15.0, 30.0, 1.0), WithinAbs(0.5, 1e-6));
}

TEST_CASE("brushDabProfile with feather=0 is a hard edge", "[brush]") {
    // Full coverage right up to the radius, nothing past it.
    REQUIRE_THAT(brushDabProfile(29.9, 30.0, 0.0), WithinAbs(1.0, 1e-6));
    REQUIRE_THAT(brushDabProfile(30.0, 30.0, 0.0), WithinAbs(0.0, 1e-6));
}

// ---------------------------------------------------------------------------
// stampStroke — paint dabs along a path onto a raster (docs/adr/0047)
// ---------------------------------------------------------------------------

TEST_CASE("stampStroke single Add dab reaches flow at centre, zero past radius", "[brush]") {
    const BrushRaster base = blankRaster(100);
    const std::array<QPointF, 1> path{QPointF{50, 50}};
    const BrushDab brush{.radius = 20.0, .feather = 0.5, .flow = 0.8, .erase = false};

    const BrushRaster out = stampStroke(base, path, brush);

    REQUIRE(at(out, 50, 50) == 204); // flow 0.8 → round(0.8 * 255)
    REQUIRE(at(out, 80, 50) == 0);   // 30 px away, past radius 20
}

TEST_CASE("stampStroke overlapping dabs within one stroke combine by max, not sum", "[brush]") {
    const BrushRaster base = blankRaster(100);
    // Two centres 20 px apart; the midpoint is 10 px from each (== inner core for
    // feather 0.5, radius 20), so each dab alone gives full coverage there.
    const std::array<QPointF, 2> path{QPointF{40, 50}, QPointF{60, 50}};
    const BrushDab brush{.radius = 20.0, .feather = 0.5, .flow = 0.5};

    const BrushRaster out = stampStroke(base, path, brush);

    // max → still flow (128); additive would have clamped to 255.
    REQUIRE(at(out, 50, 50) == 128);
}

TEST_CASE("stampStroke across strokes accumulates additively and clamps at 1", "[brush]") {
    const std::array<QPointF, 1> path{QPointF{50, 50}};
    const BrushDab light{.radius = 20.0, .feather = 0.5, .flow = 0.3};

    BrushRaster out = stampStroke(blankRaster(100), path, light);
    out = stampStroke(out, path, light);
    // ~0.6 of full, accumulated through the 8-bit raster (77/255 + 0.3 → 154).
    REQUIRE(at(out, 50, 50) == 154);

    const BrushDab heavy{.radius = 20.0, .feather = 0.5, .flow = 0.8};
    out = stampStroke(out, path, heavy); // 0.6 + 0.8 → clamp 1.0
    REQUIRE(at(out, 50, 50) == 255);
}

TEST_CASE("stampStroke Erase subtracts coverage and clamps at 0", "[brush]") {
    const std::array<QPointF, 1> path{QPointF{50, 50}};
    const BrushDab paint{.radius = 20.0, .feather = 0.5, .flow = 1.0};
    const BrushDab erase{.radius = 20.0, .feather = 0.5, .flow = 0.8, .erase = true};

    BrushRaster out = stampStroke(blankRaster(100), path, paint); // centre 255
    out = stampStroke(out, path, erase);                          // 1.0 - 0.8
    REQUIRE(at(out, 50, 50) == 51);                               // round(0.2 * 255)

    // Erasing an already-empty mask stays at 0 (no underflow).
    const BrushRaster empty = stampStroke(blankRaster(100), path, erase);
    REQUIRE(at(empty, 50, 50) == 0);
}

TEST_CASE("stampStroke fills the interior of a path between far-apart points", "[brush]") {
    const BrushRaster base = blankRaster(100);
    // Endpoints 60 px apart with radius 10 — without interpolation the middle is a
    // gap. Dabs swept along the segment must give full coverage on the centreline.
    const std::array<QPointF, 2> path{QPointF{20, 50}, QPointF{80, 50}};
    const BrushDab brush{.radius = 10.0, .feather = 0.5, .flow = 1.0};

    const BrushRaster out = stampStroke(base, path, brush);

    REQUIRE(at(out, 50, 50) == 255); // midpoint, on the centreline → full coverage
    REQUIRE(at(out, 50, 70) == 0);   // 20 px off the line → outside radius 10
}

// ---------------------------------------------------------------------------
// encodeBrushRaster / decodeBrushRaster — base64 PNG for the sidecar (adr 0047)
// ---------------------------------------------------------------------------

TEST_CASE("brush raster survives a base64-PNG round trip", "[brush]") {
    // A painted raster with a feathered gradient (non-trivial pixel values).
    const std::array<QPointF, 1> path{QPointF{40, 40}};
    const BrushRaster painted
        = stampStroke(blankRaster(80), path, BrushDab{.radius = 30.0, .feather = 0.7, .flow = 0.9});

    const QString encoded = encodeBrushRaster(painted);
    REQUIRE_FALSE(encoded.isEmpty());

    const BrushRaster back = decodeBrushRaster(encoded);
    REQUIRE(back.width == painted.width);
    REQUIRE(back.height == painted.height);
    REQUIRE(back.data == painted.data);
}

TEST_CASE("brush raster codec handles empty and malformed input", "[brush]") {
    REQUIRE(encodeBrushRaster(BrushRaster{}).isEmpty());                // nothing to encode
    REQUIRE(decodeBrushRaster(QStringLiteral("not png")).data.empty()); // garbage in
}
