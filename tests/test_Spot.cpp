#include "pipeline/ImagePipeline.h"
#include "io/XmpSidecar.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <QTemporaryDir>

using Catch::Matchers::WithinAbs;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a width×height buffer: pixels with x < splitX are `left`, the rest `right`.
static ImageBuffer makeSplitBuffer(
    int width, int height, int splitX, std::array<float, 3> left, std::array<float, 3> right) {
    ImageBuffer buf;
    buf.width = width;
    buf.height = height;
    buf.data.resize(static_cast<size_t>(width * height * 3));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto& c = (x < splitX) ? left : right;
            const int i = (y * width + x) * 3;
            buf.data[i] = c[0];
            buf.data[i + 1] = c[1];
            buf.data[i + 2] = c[2];
        }
    }
    return buf;
}

// ---------------------------------------------------------------------------
// spotWeight
// ---------------------------------------------------------------------------

TEST_CASE("spotWeight is 1.0 at destination centre", "[spot]") {
    Spot s{.destination = {100.0, 200.0}, .source = {50.0, 50.0}, .radius = 30.0, .feather = 0.5};
    REQUIRE_THAT(spotWeight(s, {100.0, 200.0}), WithinAbs(1.0, 1e-6));
}

TEST_CASE("spotWeight is 0.0 outside radius", "[spot]") {
    Spot s{.destination = {100.0, 100.0}, .source = {50.0, 50.0}, .radius = 30.0, .feather = 0.0};
    // 100px away — well outside radius 30
    REQUIRE_THAT(spotWeight(s, {200.0, 100.0}), WithinAbs(0.0, 1e-6));
}

// ---------------------------------------------------------------------------
// applySpots
// ---------------------------------------------------------------------------

TEST_CASE("applySpots with empty list returns identical buffer", "[spot]") {
    ImageBuffer buf = makeSplitBuffer(10, 10, 5, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
    auto result = applySpots(buf, {});
    REQUIRE(result.data == buf.data);
}

TEST_CASE("applySpots replaces destination centre with source pixel (feather=0)", "[spot]") {
    // Left half red, right half green. Spot: destination in red zone, source in green zone.
    ImageBuffer buf = makeSplitBuffer(200, 100, 100, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
    Spot s{.destination = {50.0, 50.0}, .source = {150.0, 50.0}, .radius = 10.0, .feather = 0.0};
    auto result = applySpots(buf, {s});

    // Centre pixel should be fully replaced by green
    const int idx = (50 * 200 + 50) * 3;
    REQUIRE_THAT(result.data[idx], WithinAbs(0.f, 1e-5));     // R=0
    REQUIRE_THAT(result.data[idx + 1], WithinAbs(1.f, 1e-5)); // G=1
    REQUIRE_THAT(result.data[idx + 2], WithinAbs(0.f, 1e-5)); // B=0
}

TEST_CASE("applySpots leaves pixels outside radius unchanged", "[spot]") {
    ImageBuffer buf = makeSplitBuffer(200, 100, 100, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
    Spot s{.destination = {50.0, 50.0}, .source = {150.0, 50.0}, .radius = 10.0, .feather = 0.0};
    auto result = applySpots(buf, {s});

    // Pixel far outside the spot should stay red
    const int idx = (50 * 200 + 5) * 3;
    REQUIRE_THAT(result.data[idx], WithinAbs(1.f, 1e-5));     // R=1
    REQUIRE_THAT(result.data[idx + 1], WithinAbs(0.f, 1e-5)); // G=0
}

TEST_CASE("applySpots blends in the feather band", "[spot]") {
    // Left half red, right half green.
    // Destination (50,50), source (150,50), radius=40, feather=0.5.
    // Pixel at (80,50): d=30 from destination → weight=0.5.
    // Source sample at (150+(80-50), 50) = (180,50) → green.
    // Expected blend: 0.5*red + 0.5*green = (0.5, 0.5, 0).
    ImageBuffer buf = makeSplitBuffer(200, 100, 100, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
    Spot s{.destination = {50.0, 50.0}, .source = {150.0, 50.0}, .radius = 40.0, .feather = 0.5};
    auto result = applySpots(buf, {s});

    const int idx = (50 * 200 + 80) * 3;
    REQUIRE_THAT(result.data[idx], WithinAbs(0.5f, 1e-5));
    REQUIRE_THAT(result.data[idx + 1], WithinAbs(0.5f, 1e-5));
    REQUIRE_THAT(result.data[idx + 2], WithinAbs(0.0f, 1e-5));
}

TEST_CASE("applySpots applies two spots independently", "[spot]") {
    // All blue. Two spots sample from known positions.
    ImageBuffer buf;
    buf.width = 100;
    buf.height = 100;
    buf.data.assign(static_cast<size_t>(100 * 100 * 3), 0.f);
    // Patch red at (10,10) and green at (90,10)
    buf.data[(10 * 100 + 10) * 3] = 1.f;     // R at (10,10)
    buf.data[(10 * 100 + 90) * 3 + 1] = 1.f; // G at (90,10)

    Spot s1{.destination = {50.0, 50.0}, .source = {10.0, 10.0}, .radius = 1.0, .feather = 0.0};
    Spot s2{.destination = {70.0, 50.0}, .source = {90.0, 10.0}, .radius = 1.0, .feather = 0.0};
    auto result = applySpots(buf, {s1, s2});

    // Destination of s1 should be red
    REQUIRE_THAT(result.data[(50 * 100 + 50) * 3], WithinAbs(1.f, 1e-5));
    REQUIRE_THAT(result.data[(50 * 100 + 50) * 3 + 1], WithinAbs(0.f, 1e-5));
    // Destination of s2 should be green
    REQUIRE_THAT(result.data[(50 * 100 + 70) * 3], WithinAbs(0.f, 1e-5));
    REQUIRE_THAT(result.data[(50 * 100 + 70) * 3 + 1], WithinAbs(1.f, 1e-5));
}

// ---------------------------------------------------------------------------
// autoSourcePosition
// ---------------------------------------------------------------------------

TEST_CASE("autoSourcePosition offsets right by the given offset", "[spot]") {
    const QPointF src = autoSourcePosition({50.0, 50.0}, 10.0, 200, 100);
    REQUIRE_THAT(src.x(), WithinAbs(60.0, 1e-6));
    REQUIRE_THAT(src.y(), WithinAbs(50.0, 1e-6));
}

TEST_CASE("autoSourcePosition clamps inside buffer", "[spot]") {
    // Destination near the right edge: offset would push source outside
    const QPointF src = autoSourcePosition({195.0, 50.0}, 10.0, 200, 100);
    REQUIRE(src.x() <= 199.0);
    REQUIRE(src.x() >= 0.0);
}

// ---------------------------------------------------------------------------
// XMP round-trip
// ---------------------------------------------------------------------------

TEST_CASE("spots survive a save/load XMP round-trip", "[spot]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString raw = dir.filePath("test.CR3");

    GlobalAdjustment params;
    params.spots = {
        Spot{.destination = {123.5, 456.0}, .source = {789.0, 12.0}, .radius = 25.0, .feather = 0.3},
        Spot{.destination = {50.0, 50.0}, .source = {150.0, 50.0}, .radius = 10.0, .feather = 0.0},
    };

    REQUIRE(XmpSidecar::saveAdjustments(raw, params));
    const GlobalAdjustment loaded = XmpSidecar::loadAdjustments(raw);

    REQUIRE(loaded.spots.size() == 2);
    constexpr double tol = 1e-4;
    CHECK_THAT(loaded.spots[0].destination.x(), WithinAbs(123.5, tol));
    CHECK_THAT(loaded.spots[0].destination.y(), WithinAbs(456.0, tol));
    CHECK_THAT(loaded.spots[0].source.x(), WithinAbs(789.0, tol));
    CHECK_THAT(loaded.spots[0].source.y(), WithinAbs(12.0, tol));
    CHECK_THAT(loaded.spots[0].radius, WithinAbs(25.0, tol));
    CHECK_THAT(loaded.spots[0].feather, WithinAbs(0.3, tol));
    CHECK_THAT(loaded.spots[1].destination.x(), WithinAbs(50.0, tol));
    CHECK_THAT(loaded.spots[1].feather, WithinAbs(0.0, tol));
}

TEST_CASE("empty spot list round-trips cleanly", "[spot]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString raw = dir.filePath("test.CR3");
    GlobalAdjustment params; // spots empty by default
    REQUIRE(XmpSidecar::saveAdjustments(raw, params));
    const GlobalAdjustment loaded = XmpSidecar::loadAdjustments(raw);
    REQUIRE(loaded.spots.empty());
}

TEST_CASE("spotWeight blends smoothly in the feather band", "[spot]") {
    // radius=40, feather=0.5 → inner=20px, outer=40px
    // At d=30: nd=0.75, inner_norm=0.5, t=0.5, s=smoothstep(0.5)=0.5, w=0.5
    Spot s{.destination = {0.0, 0.0}, .source = {100.0, 0.0}, .radius = 40.0, .feather = 0.5};
    REQUIRE_THAT(spotWeight(s, {30.0, 0.0}), WithinAbs(0.5, 1e-6));
}
