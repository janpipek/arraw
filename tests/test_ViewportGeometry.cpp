#include "ViewportGeometry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

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
