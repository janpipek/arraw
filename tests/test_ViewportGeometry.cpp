#include "ViewportGeometry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

TEST_CASE("shrinkInsideRotation leaves an interior crop untouched at 0°", "[viewportgeometry]") {
    const QRectF crop{0.2, 0.2, 0.5, 0.5};
    const QRectF out = viewport::shrinkInsideRotation(crop, 0.0f, 1.5f);
    REQUIRE(out.x() == Approx(crop.x()));
    REQUIRE(out.y() == Approx(crop.y()));
    REQUIRE(out.width() == Approx(crop.width()));
    REQUIRE(out.height() == Approx(crop.height()));
}

namespace {
bool cornersInside(const QRectF& r, float degrees, float aspect) {
    const QPointF corners[4]
        = {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};
    for (const QPointF& c : corners) {
        const QPointF s = viewport::rotateTextureUv(
            float(c.x()), float(c.y()), degrees, aspect, 0.5f, 0.5f);
        if (s.x() < -1e-4f || s.x() > 1.0f + 1e-4f || s.y() < -1e-4f || s.y() > 1.0f + 1e-4f)
            return false;
    }
    return true;
}
} // namespace

TEST_CASE("shrinkInsideRotation pulls an over-large crop inside, keeping centre and aspect",
    "[viewportgeometry]") {
    const float aspect = 1.5f;
    const float degrees = 18.0f;
    const QRectF crop{0.04, 0.04, 0.92, 0.92}; // nearly the whole frame
    REQUIRE_FALSE(cornersInside(crop, degrees, aspect)); // precondition: it pokes out

    const QRectF out = viewport::shrinkInsideRotation(crop, degrees, aspect);

    REQUIRE(cornersInside(out, degrees, aspect));          // now fits
    REQUIRE(out.width() < crop.width());                   // it actually shrank
    REQUIRE(out.center().x() == Approx(crop.center().x())); // centre held
    REQUIRE(out.center().y() == Approx(crop.center().y()));
    // aspect ratio preserved
    REQUIRE(out.width() / out.height() == Approx(crop.width() / crop.height()));
}

TEST_CASE("Viewport geometry round-trips crop UV through zoom and pan", "[viewportgeometry]") {
    viewport::Geometry g;
    g.viewportSize = {1200, 800};
    g.displayAspect = 1.5f;
    g.zoom = 1.7f;
    g.pan = {0.12, -0.08};

    const QPointF uv(0.25, 0.75);
    const QPointF vp = g.cropUvToViewport(float(uv.x()), float(uv.y()));
    const QPointF roundTrip = g.viewportToCropUv(vp);

    CHECK(roundTrip.x() == Approx(uv.x()));
    CHECK(roundTrip.y() == Approx(uv.y()));
}

TEST_CASE(
    "Viewport geometry round-trips original buffer pixels through crop and rotation",
    "[viewportgeometry]") {
    viewport::Geometry g;
    g.viewportSize = {1000, 700};
    g.originalSize = {6000, 4000};
    g.imageAspect = 1.5f;
    g.displayAspect = 1.2f;
    g.zoom = 1.35f;
    g.pan = {-0.1, 0.2};
    g.cropRect = {0.1, 0.15, 0.8, 0.7};
    g.rotation = 11.0f;

    const QPointF pixel(3400.0, 2100.0);
    const QPointF vp = g.bufferPixelToViewport(pixel);
    const QPointF roundTrip = g.viewportToBufferPixel(vp);

    CHECK(roundTrip.x() == Approx(pixel.x()).margin(1e-3));
    CHECK(roundTrip.y() == Approx(pixel.y()).margin(1e-3));
    CHECK(g.bufferRadiusToViewport(pixel, 120.0) > 0.0);
}
