#include "ViewTransform.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using arraw::app::ViewTransform;
using Catch::Approx;

namespace {

constexpr QSizeF landscape{6000.0, 4000.0};
constexpr QSizeF portrait{4000.0, 6000.0};
constexpr QSizeF view{1200.0, 800.0};

} // namespace

TEST_CASE("The fit shows the whole frame and never enlarges", "[app][view]") {
    REQUIRE(ViewTransform::fitZoom(landscape, view) == Approx(0.2));
    // Portrait is limited by the height.
    REQUIRE(ViewTransform::fitZoom(portrait, view) == Approx(800.0 / 6000.0));
    // A small photograph stays at its own size.
    REQUIRE(ViewTransform::fitZoom({300.0, 200.0}, view) == 1.0);
    // Nothing to fit.
    REQUIRE(ViewTransform::fitZoom({0.0, 0.0}, view) == 1.0);
    REQUIRE(ViewTransform::fitZoom(landscape, {0.0, 0.0}) == 1.0);
}

TEST_CASE("Fitted, the visible region is the whole frame", "[app][view]") {
    for (const QSizeF frame : {landscape, portrait, QSizeF(300.0, 200.0)}) {
        const ViewTransform fitted = ViewTransform::fitted(frame, view);
        REQUIRE(fitted.isFit());
        REQUIRE(fitted.centre() == QPointF(0.5, 0.5));
        const QRectF region = fitted.visibleRegion();
        REQUIRE(region.left() == Approx(0.0));
        REQUIRE(region.top() == Approx(0.0));
        REQUIRE(region.right() == Approx(1.0));
        REQUIRE(region.bottom() == Approx(1.0));
        REQUIRE(fitted.visiblePixels() ==
                QRect(0, 0, static_cast<int>(frame.width()), static_cast<int>(frame.height())));
    }
}

TEST_CASE("The zoom is kept within its limits", "[app][view]") {
    const ViewTransform fit = ViewTransform::fitted(landscape, view);
    REQUIRE(ViewTransform(landscape, view, 100.0).zoom() == arraw::app::maxZoom);
    // Below the fit of a large photograph nothing is gained.
    REQUIRE(ViewTransform(landscape, view, 0.001).zoom() == Approx(fit.zoom()));
    // A small photograph may be shrunk down to a quarter.
    REQUIRE(ViewTransform({300.0, 200.0}, view, 0.01).zoom() == Approx(0.25));
    REQUIRE(ViewTransform({300.0, 200.0}, view, 0.5).zoom() == Approx(0.5));
}

TEST_CASE("The centre is clamped so the frame fills the view, or is centred", "[app][view]") {
    // At 1:1 the frame is larger than the view: the centre stays inside.
    const ViewTransform big(landscape, view, 1.0, {0.0, 1.0});
    REQUIRE(big.centre().x() == Approx(600.0 / 6000.0));
    REQUIRE(big.centre().y() == Approx(1.0 - 400.0 / 4000.0));
    // A small frame is centred whatever was asked.
    const ViewTransform small({300.0, 200.0}, view, 1.0, {0.1, 0.9});
    REQUIRE(small.centre() == QPointF(0.5, 0.5));
    // Larger in one direction only: clamped in that one.
    const ViewTransform wide({3000.0, 200.0}, view, 1.0, {0.0, 0.0});
    REQUIRE(wide.centre().x() == Approx(0.2));
    REQUIRE(wide.centre().y() == Approx(0.5));
}

TEST_CASE("View and frame coordinates map to each other", "[app][view]") {
    const ViewTransform t(landscape, view, 0.5, {0.4, 0.6});
    for (const QPointF point : {QPointF(0.0, 0.0), QPointF(0.4, 0.6), QPointF(1.0, 0.25)}) {
        const QPointF back = t.frameFromView(t.viewFromFrame(point));
        REQUIRE(back.x() == Approx(point.x()));
        REQUIRE(back.y() == Approx(point.y()));
    }
    // The middle of the view is the centre.
    REQUIRE(t.viewFromFrame(t.centre()) == QPointF(600.0, 400.0));
    REQUIRE(t.frameFromView({600.0, 400.0}).x() == Approx(0.4));
}

TEST_CASE("Zooming about a point keeps the frame point under it", "[app][view]") {
    const ViewTransform t(landscape, view, 0.5, {0.5, 0.5});
    const QPointF cursor(900.0, 250.0);
    const QPointF before = t.frameFromView(cursor);
    for (const double zoom : {0.75, 1.0, 2.0, 8.0}) {
        const ViewTransform zoomed = t.zoomedAbout(zoom, cursor);
        REQUIRE(zoomed.zoom() == Approx(zoom));
        const QPointF after = zoomed.frameFromView(cursor);
        REQUIRE(after.x() == Approx(before.x()));
        REQUIRE(after.y() == Approx(before.y()));
    }
}

TEST_CASE("Zooming out of a clamped corner recentres rather than leaving the frame",
          "[app][view]") {
    const ViewTransform corner(landscape, view, 1.0, {0.0, 0.0});
    const ViewTransform out = corner.zoomedAbout(0.2, {0.0, 0.0});
    REQUIRE(out.centre() == QPointF(0.5, 0.5));
    REQUIRE(out.isFit());
}

TEST_CASE("Panning moves the frame with the pointer and stops at the edge", "[app][view]") {
    const ViewTransform t(landscape, view, 1.0, {0.5, 0.5});
    const QPointF point = t.frameFromView({100.0, 100.0});
    const ViewTransform moved = t.panned({50.0, -30.0});
    // The point that was at (100, 100) follows the pointer.
    const QPointF where = moved.viewFromFrame(point);
    REQUIRE(where.x() == Approx(150.0));
    REQUIRE(where.y() == Approx(70.0));
    // Dragged far beyond the edge.
    const ViewTransform edge = t.panned({1.0e6, 1.0e6});
    REQUIRE(edge.visiblePixels().topLeft() == QPoint(0, 0));
    // Nothing to pan when the frame fits.
    const ViewTransform fit = ViewTransform::fitted(landscape, view);
    REQUIRE(fit.panned({40.0, 40.0}).centre() == QPointF(0.5, 0.5));
}

TEST_CASE("At 1:1 the region is the view's pixels and the output is as large", "[app][view]") {
    const ViewTransform t(landscape, view, 1.0, {0.5, 0.5});
    const QRect pixels = t.visiblePixels();
    REQUIRE(pixels == QRect(2400, 1600, 1200, 800));
    REQUIRE(t.outputSize() == QSize(1200, 800));
}

TEST_CASE("At 200% the region is half the view and the output stays at its pixels", "[app][view]") {
    const ViewTransform t(landscape, view, 2.0, {0.5, 0.5});
    const QRect pixels = t.visiblePixels();
    REQUIRE(pixels == QRect(2700, 1800, 600, 400));
    // Never more than the photograph has: the view magnifies it.
    REQUIRE(t.outputSize() == QSize(600, 400));
}

TEST_CASE("Below 1:1 the output is the region's pixels times the zoom", "[app][view]") {
    const ViewTransform t(landscape, view, 0.5, {0.5, 0.5});
    const QRect pixels = t.visiblePixels();
    REQUIRE(pixels == QRect(0, 0, 2400, 1600).translated(1800, 1200));
    REQUIRE(t.outputSize() == QSize(1200, 800));
    const ViewTransform fit = ViewTransform::fitted(landscape, view);
    REQUIRE(fit.outputSize() == QSize(1200, 800));
}

TEST_CASE("The output never exceeds the region's pixels", "[app][view]") {
    for (const double zoom : {0.1, 0.25, 0.7, 1.0, 1.5, 4.0, 32.0}) {
        for (const QPointF centre : {QPointF(0.0, 0.0), QPointF(0.33, 0.71), QPointF(1.0, 1.0)}) {
            const ViewTransform t(landscape, view, zoom, centre);
            const QRect pixels = t.visiblePixels();
            REQUIRE(t.outputSize().width() <= pixels.width());
            REQUIRE(t.outputSize().height() <= pixels.height());
            REQUIRE(QRect(0, 0, 6000, 4000).contains(pixels));
            REQUIRE_FALSE(pixels.isEmpty());
        }
    }
}

TEST_CASE("Zoom presets match to half a percentage point", "[app][view]") {
    REQUIRE(arraw::app::matchingZoomPreset(0.25) == 0);
    REQUIRE(arraw::app::matchingZoomPreset(1.0) == 2);
    REQUIRE(arraw::app::matchingZoomPreset(1.003) == 2);
    REQUIRE(arraw::app::matchingZoomPreset(4.0) == 4);
    REQUIRE(arraw::app::matchingZoomPreset(1.2) == -1);
    REQUIRE(arraw::app::matchingZoomPreset(3.0) == -1);
}

TEST_CASE("The zoom button names the zoom", "[app][view]") {
    REQUIRE(arraw::app::zoomLabel(0.2, true) == "Fit");
    REQUIRE(arraw::app::zoomLabel(1.0, true) == "Fit");
    REQUIRE(arraw::app::zoomLabel(1.0, false) == "1:1");
    REQUIRE(arraw::app::zoomLabel(2.0, false) == "200%");
    REQUIRE(arraw::app::zoomLabel(0.333, false) == "33%");
    REQUIRE(arraw::app::zoomPercentLabel(0.25) == "25%");
}
