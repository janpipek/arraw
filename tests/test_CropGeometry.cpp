#include "core/CropGeometry.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QRectF>
#include <QSize>

using Catch::Approx;

namespace {
void requireRect(const QRectF& got, double x, double y, double w, double h) {
    REQUIRE(got.x() == Approx(x).margin(1e-6));
    REQUIRE(got.y() == Approx(y).margin(1e-6));
    REQUIRE(got.width() == Approx(w).margin(1e-6));
    REQUIRE(got.height() == Approx(h).margin(1e-6));
}
} // namespace

TEST_CASE("rotateQuarterTurns turns a crop 90° CW, swapping width and height", "[crop]") {
    // (u,v) → (1-v,u): corners (0.1,0.2) and (0.4,0.6) map to (0.8,0.1) and
    // (0.4,0.4); normalised that is x=0.4, y=0.1, w=0.4, h=0.3.
    requireRect(crop::rotateQuarterTurns({0.1, 0.2, 0.3, 0.4}, 1), 0.4, 0.1, 0.4, 0.3);
}

TEST_CASE("rotateQuarterTurns is 4-periodic and normalises negative turns", "[crop]") {
    const QRectF r{0.1, 0.2, 0.3, 0.4};
    requireRect(crop::rotateQuarterTurns(r, 0), r.x(), r.y(), r.width(), r.height());
    requireRect(crop::rotateQuarterTurns(r, 4), r.x(), r.y(), r.width(), r.height());
    // -1 CW == 3 CW == one CCW step; both must agree.
    const QRectF ccw = crop::rotateQuarterTurns(r, -1);
    const QRectF cw3 = crop::rotateQuarterTurns(r, 3);
    requireRect(ccw, cw3.x(), cw3.y(), cw3.width(), cw3.height());
}

TEST_CASE("cropPixelSize scales full-image pixels by the crop rectangle", "[crop]") {
    // Full crop keeps every pixel.
    REQUIRE(crop::cropPixelSize(6000, 4000, QRectF(0, 0, 1, 1)) == QSize(6000, 4000));
    // Half-width, half-height crop -> a quarter-area region.
    REQUIRE(crop::cropPixelSize(6000, 4000, QRectF(0.25, 0.25, 0.5, 0.5)) == QSize(3000, 2000));
}

TEST_CASE("cropPixelSize swaps to the oriented dimensions on a 90° turn", "[crop]") {
    // 6000×4000 native, turned 90° → a full crop is 4000×6000 oriented pixels.
    REQUIRE(
        crop::cropPixelSize(6000, 4000, QRectF(0, 0, 1, 1), orient::Orientation{1, false})
        == QSize(4000, 6000));
}

TEST_CASE("cropPixelSize rounds to the nearest pixel", "[crop]") {
    // 0.3333 * 6000 = 1999.8 -> 2000
    REQUIRE(crop::cropPixelSize(6000, 4000, QRectF(0, 0, 1.0 / 3.0, 0.5)) == QSize(2000, 2000));
}

TEST_CASE("fitRatioInside keeps a matching rect unchanged", "[crop]") {
    // Square image, 1:1 lock, full frame already square in pixels -> no change.
    requireRect(crop::fitRatioInside(QRectF(0, 0, 1, 1), 1.0, 1.0), 0, 0, 1, 1);
}

TEST_CASE("fitRatioInside shrinks to the ratio, centred", "[crop]") {
    // Square image, 2:1 (wide) lock: width stays, height halves, centred vertically.
    requireRect(crop::fitRatioInside(QRectF(0, 0, 1, 1), 2.0, 1.0), 0, 0.25, 1, 0.5);
}

TEST_CASE("fitRatioInside converts the pixel ratio through imageAspect", "[crop]") {
    // 3:2 image (imageAspect 1.5), 1:1 *pixel* lock is NOT a square UV rect:
    // du/dv = ratio/imageAspect = 1/1.5 -> width 0.6667, full height, centred.
    requireRect(crop::fitRatioInside(QRectF(0, 0, 1, 1), 1.0, 1.5), 1.0 / 6.0, 0, 2.0 / 3.0, 1);
}

TEST_CASE("lockedResize leaves edge handles inactive", "[crop]") {
    const QRectF start(0.2, 0.2, 0.4, 0.4);
    for (int edge : {1, 3, 5, 7})
        requireRect(
            crop::lockedResize(edge, start, QPointF(0.9, 0.1), 1.0, 1.0),
            start.x(),
            start.y(),
            start.width(),
            start.height());
}

TEST_CASE("lockedResize drives a corner by the dominant axis, ratio-locked", "[crop]") {
    // Square image, 1:1. Drag BR (4) from anchor TL=(0,0) toward (0.8, 0.6):
    // dominant axis is x (0.8 > 0.6), so the square grows to 0.8 on both axes.
    requireRect(
        crop::lockedResize(4, QRectF(0, 0, 0.5, 0.5), QPointF(0.8, 0.6), 1.0, 1.0), 0, 0, 0.8, 0.8);
}

TEST_CASE("lockedResize anchors the opposite corner", "[crop]") {
    // Drag TL (0); anchor is BR=(0.8,0.8). The BR corner must not move.
    const QRectF r = crop::lockedResize(0, QRectF(0.2, 0.2, 0.6, 0.6), QPointF(0.1, 0.3), 1.0, 1.0);
    REQUIRE(r.right() == Approx(0.8).margin(1e-6));
    REQUIRE(r.bottom() == Approx(0.8).margin(1e-6));
    requireRect(r, 0.1, 0.1, 0.7, 0.7); // dominant axis |dx|=0.7
}

TEST_CASE("lockedResize converts the pixel ratio through imageAspect", "[crop]") {
    // 2:1 image, 1:1 pixel lock -> uvRatio 0.5 (tall UV). Drag BR from (0,0) to (0.5,0.5).
    const QRectF r = crop::lockedResize(4, QRectF(0, 0, 0.4, 0.4), QPointF(0.5, 0.5), 1.0, 2.0);
    requireRect(r, 0, 0, 0.5, 1.0);
    // Pixel aspect = (w/h) * imageAspect = (0.5/1.0)*2 = 1.0 (square pixels).
    REQUIRE((r.width() / r.height()) * 2.0 == Approx(1.0).margin(1e-6));
}

TEST_CASE("presetRatio maps fixed presets and orientation", "[crop]") {
    using crop::AspectPreset;
    REQUIRE(crop::presetRatio(AspectPreset::Free, true, 1.5) == Approx(0.0));
    REQUIRE(crop::presetRatio(AspectPreset::Square, true, 1.5) == Approx(1.0));
    REQUIRE(crop::presetRatio(AspectPreset::Square, false, 1.5) == Approx(1.0));
    REQUIRE(crop::presetRatio(AspectPreset::R16x9, true, 1.5) == Approx(16.0 / 9.0));
    REQUIRE(crop::presetRatio(AspectPreset::R16x9, false, 1.5) == Approx(9.0 / 16.0));
    REQUIRE(crop::presetRatio(AspectPreset::R2x3, true, 1.5) == Approx(3.0 / 2.0));
    REQUIRE(crop::presetRatio(AspectPreset::R2x3, false, 1.5) == Approx(2.0 / 3.0));
}

TEST_CASE("presetRatio derives Original from imageAspect, orientation-normalised", "[crop]") {
    using crop::AspectPreset;
    // Landscape image (3:2): landscape keeps it, portrait flips to the reciprocal.
    REQUIRE(crop::presetRatio(AspectPreset::Original, true, 1.5) == Approx(1.5));
    REQUIRE(crop::presetRatio(AspectPreset::Original, false, 1.5) == Approx(1.0 / 1.5));
    // Portrait image (2:3, imageAspect 0.6667): landscape still yields the wide form.
    REQUIRE(crop::presetRatio(AspectPreset::Original, true, 2.0 / 3.0) == Approx(1.5));
}

TEST_CASE("cropPixelRatio is the rect's pixel width:height", "[crop]") {
    // Full crop on a 3:2 image is 3:2.
    REQUIRE(crop::cropPixelRatio(QRectF(0, 0, 1, 1), 1.5) == Approx(1.5));
    // A square UV rect on a 2:1 image is 2:1 in pixels.
    REQUIRE(crop::cropPixelRatio(QRectF(0.1, 0.1, 0.5, 0.5), 2.0) == Approx(2.0));
    // A half-wide UV rect on a square image is 0.5 (portrait).
    REQUIRE(crop::cropPixelRatio(QRectF(0, 0, 0.5, 1.0), 1.0) == Approx(0.5));
}

TEST_CASE("matchPreset names a ratio, or reports no match", "[crop]") {
    using crop::AspectPreset;
    // Zero ratio is Free (a matched, named state).
    auto free = crop::matchPreset(0.0, 1.5);
    REQUIRE(free.matched);
    REQUIRE(free.preset == AspectPreset::Free);
    // 16:9 landscape on any image matches R16x9 landscape.
    auto wide = crop::matchPreset(16.0 / 9.0, 1.5);
    REQUIRE(wide.matched);
    REQUIRE(wide.preset == AspectPreset::R16x9);
    REQUIRE(wide.landscape);
    // 2:3 portrait matches R2x3 portrait.
    auto tall = crop::matchPreset(2.0 / 3.0, 1.5);
    REQUIRE(tall.matched);
    REQUIRE(tall.preset == AspectPreset::R2x3);
    REQUIRE_FALSE(tall.landscape);
    // An exotic ratio (7:3) matches nothing.
    REQUIRE_FALSE(crop::matchPreset(7.0 / 3.0, 1.5).matched);
}
