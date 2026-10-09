#include "MaskEditing.h"
#include "support/LocalAdjustmentStates.h"

#include <CropGeometry.h>
#include <DevelopState.h>
#include <DevelopedFrame.h>
#include <LocalAdjustmentEdits.h>
#include <LocalAdjustments.h>

#include <QPointF>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <numbers>
#include <random>
#include <variant>
#include <vector>

using namespace arraw;
using namespace arraw::app;
using namespace arraw::test;
using Catch::Approx;

/// The geometry of the mask handles: mapping, hits, drags and creation (ADR 044, section 10).

namespace {

constexpr QSizeF widgetSize{800.0, 600.0};

/// @brief A mapping of a source and geometry into the widget.
/// @param zoom Device pixels per frame pixel; 0 fits.
/// @param centre Developed-frame fraction at the widget's middle.
MaskViewMapping mappingFor(SourceShape source, const GeometrySettings& geometry, double zoom = 0.0,
                           QPointF centre = {0.5, 0.5}, double ratio = 1.0) {
    DevelopedFrameMap map(source, geometry);
    const QSizeF frame(map.width(), map.height());
    const QSizeF view(widgetSize.width() * ratio, widgetSize.height() * ratio);
    const double actual = zoom > 0.0 ? zoom : ViewTransform::fitZoom(frame, view);
    return MaskViewMapping(std::move(map), ViewTransform(frame, view, actual, centre), ratio);
}

constexpr SourceShape landscape{{3000, 2000}, ImageOrientation::Normal};

double distance(QPointF a, QPointF b) {
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

QPointF handleOf(const Mask& mask, const MaskViewMapping& mapping, MaskHandle handle) {
    for (const HandlePosition& dot : handlePositions(mask, mapping)) {
        if (dot.handle == handle) {
            return dot.position;
        }
    }
    FAIL("no such handle");
    return {};
}

/// @brief Whether a shape passes the edit rules as the new shape of a mask of its kind.
bool acceptedAs(const Mask& shape) {
    const DevelopState state = withLocalAdjustmentAdded(DevelopState{}, shape);
    try {
        static_cast<void>(withLocalShape(state, state.localAdjustments.front().id, shape));
        return true;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

constexpr LinearMask someLine{{0.25F, 0.4F}, {0.7F, 0.55F}};
constexpr RadialMask someOval{
    .centre = {0.45F, 0.5F}, .radiusX = 0.3F, .radiusY = 0.18F, .angle = 25.0F, .feather = 0.5F};

GeometrySettings flippedAndTurned() {
    GeometrySettings geometry;
    geometry.rotation = QuarterTurn::Clockwise90;
    geometry.flipHorizontal = true;
    geometry.straighten = 7.5;
    return geometry;
}

} // namespace

TEST_CASE("The mapping round-trips at several zooms, pans and pixel ratios",
          "[app][mask-editing]") {
    for (const double ratio : {1.0, 2.0}) {
        for (const double zoom : {0.0, 0.25, 1.0, 4.0}) {
            for (const QPointF centre : {QPointF{0.5, 0.5}, QPointF{0.3, 0.7}}) {
                CAPTURE(ratio, zoom, centre);
                const MaskViewMapping mapping =
                    mappingFor(landscape, flippedAndTurned(), zoom, centre, ratio);
                for (const CorrectedPosition at :
                     {CorrectedPosition{0.1, 0.2}, {0.9, 0.8}, {-0.4, 1.6}}) {
                    const QPointF widget = mapping.widgetFrom(at);
                    const CorrectedPosition back = mapping.correctedFrom(widget);
                    CHECK(back.u == Approx(at.u).margin(1e-9));
                    CHECK(back.v == Approx(at.v).margin(1e-9));
                    const QPointF viaLongEdge =
                        mapping.widgetFromLongEdge(mapping.longEdgeFromWidget(widget));
                    CHECK(distance(viaLongEdge, widget) < 1e-6);
                }
                // A long-edge unit is the same length everywhere: the map is a similarity.
                const QTransform transform = mapping.longEdgeToWidget();
                const QPointF a = transform.map(QPointF(0.2, 0.1));
                const QPointF b = transform.map(QPointF(0.5, 0.45));
                CHECK(distance(a, b) ==
                      Approx(mapping.scale() * std::hypot(0.3, 0.35)).epsilon(1e-9));
                CHECK(distance(transform.map(QPointF(0, 0)), transform.map(QPointF(1, 0))) ==
                      Approx(mapping.scale()).epsilon(1e-9));
                CHECK(distance(transform.map(QPointF(0, 0)), transform.map(QPointF(0, 1))) ==
                      Approx(mapping.scale()).epsilon(1e-9));
            }
        }
    }
    CHECK_THROWS_AS(MaskViewMapping(DevelopedFrameMap(landscape, {}),
                                    ViewTransform::fitted({1, 1}, {1, 1}), 0.0),
                    std::invalid_argument);
}

TEST_CASE("The scale is the zoom over the pixel ratio, in long-edge units", "[app][mask-editing]") {
    const MaskViewMapping mapping = mappingFor(landscape, {}, 1.0, {0.5, 0.5}, 2.0);
    CHECK(mapping.scale() == Approx(3000.0 / 2.0));
}

TEST_CASE("A linear mask's handles sit where a turned picture puts them", "[app][mask-editing]") {
    GeometrySettings geometry;
    geometry.rotation = QuarterTurn::Clockwise90;
    const MaskViewMapping mapping = mappingFor(landscape, geometry);
    // Horizontal in the source, so vertical on a picture turned a quarter clockwise, with the
    // source's left edge on top.
    const LinearMask mask{{0.2F, 0.5F}, {0.8F, 0.5F}};
    const QPointF from = handleOf(mask, mapping, MaskHandle::LinearFrom);
    const QPointF to = handleOf(mask, mapping, MaskHandle::LinearTo);
    const QPointF middle = handleOf(mask, mapping, MaskHandle::LinearMiddle);
    CHECK(from.x() == Approx(to.x()).margin(1e-6));
    CHECK(from.y() < to.y());
    CHECK(distance(middle, (from + to) / 2.0) < 1e-6);
    CHECK(pinPosition(mask, mapping) == middle);
    // The middle of the picture is the middle of the widget.
    CHECK(middle.x() == Approx(widgetSize.width() / 2).margin(1e-6));
    CHECK(middle.y() == Approx(widgetSize.height() / 2).margin(1e-6));
}

TEST_CASE("A radial mask's handles are the ellipse's axes", "[app][mask-editing]") {
    const MaskViewMapping mapping = mappingFor(landscape, flippedAndTurned());
    const QPointF centre = handleOf(someOval, mapping, MaskHandle::RadialCentre);
    const QPointF plusX = handleOf(someOval, mapping, MaskHandle::RadiusPlusX);
    const QPointF minusX = handleOf(someOval, mapping, MaskHandle::RadiusMinusX);
    const QPointF plusY = handleOf(someOval, mapping, MaskHandle::RadiusPlusY);
    const QPointF minusY = handleOf(someOval, mapping, MaskHandle::RadiusMinusY);
    CHECK(distance(centre, mapping.widgetFrom(someOval.centre)) < 1e-6);
    CHECK(distance(plusX, centre) == Approx(someOval.radiusX * mapping.scale()).epsilon(1e-9));
    CHECK(distance(minusX, centre) == Approx(distance(plusX, centre)).epsilon(1e-9));
    CHECK(distance(plusY, centre) == Approx(someOval.radiusY * mapping.scale()).epsilon(1e-9));
    CHECK(distance(minusY, centre) == Approx(distance(plusY, centre)).epsilon(1e-9));
    // The axes are perpendicular on screen.
    const QPointF xAxis = plusX - centre;
    const QPointF yAxis = plusY - centre;
    CHECK(xAxis.x() * yAxis.x() + xAxis.y() * yAxis.y() ==
          Approx(0.0).margin(1e-6 * mapping.scale()));
    // The rotation knob is a stem beyond the x radius point, on the same ray.
    const QPointF knob = handleOf(someOval, mapping, MaskHandle::RadialRotation);
    CHECK(distance(knob, plusX) == Approx(maskRotationStem).epsilon(1e-9));
    CHECK(distance(knob, centre) ==
          Approx(distance(plusX, centre) + maskRotationStem).epsilon(1e-9));
    // The feather knob is on the inner ring, between the positive axes.
    const QPointF feather = handleOf(someOval, mapping, MaskHandle::RadialFeather);
    const QPointF offset = feather - centre;
    const double alongX =
        (offset.x() * xAxis.x() + offset.y() * xAxis.y()) / distance(plusX, centre);
    const double alongY =
        (offset.x() * yAxis.x() + offset.y() * yAxis.y()) / distance(plusY, centre);
    const double inner = 1.0 - someOval.feather;
    CHECK(alongX / (someOval.radiusX * mapping.scale()) == Approx(inner * std::numbers::sqrt2 / 2));
    CHECK(alongY / (someOval.radiusY * mapping.scale()) == Approx(inner * std::numbers::sqrt2 / 2));
    // At full feather the knob keeps off the centre.
    RadialMask soft = someOval;
    soft.feather = 1.0F;
    CHECK(distance(handleOf(soft, mapping, MaskHandle::RadialFeather),
                   handleOf(soft, mapping, MaskHandle::RadialCentre)) > 5.0);
}

TEST_CASE("A handle is grabbed within ten pixels at every zoom", "[app][mask-editing]") {
    const std::array<MaskHandle, 7> radialHandles{
        MaskHandle::RadialCentre, MaskHandle::RadiusPlusX,  MaskHandle::RadiusMinusX,
        MaskHandle::RadiusPlusY,  MaskHandle::RadiusMinusY, MaskHandle::RadialRotation,
        MaskHandle::RadialFeather};
    const std::array<MaskHandle, 3> linearHandles{MaskHandle::LinearFrom, MaskHandle::LinearTo,
                                                  MaskHandle::LinearMiddle};
    for (const double zoom : {0.0, 0.25, 1.0, 4.0}) {
        for (const double ratio : {1.0, 2.0}) {
            CAPTURE(zoom, ratio);
            const MaskViewMapping mapping =
                mappingFor(landscape, flippedAndTurned(), zoom, {0.5, 0.5}, ratio);
            for (const MaskHandle handle : radialHandles) {
                const QPointF at = handleOf(someOval, mapping, handle);
                CAPTURE(static_cast<int>(handle));
                CHECK(handleAt(someOval, mapping, at) == handle);
                CHECK(handleAt(someOval, mapping, at + QPointF(0.0, 9.5)) == handle);
                CHECK(handleAt(someOval, mapping, at + QPointF(-6.0, -6.0)) == handle);
                CHECK(handleAt(someOval, mapping, at + QPointF(0.0, 10.5)) == MaskHandle::None);
                CHECK(handleAt(someOval, mapping, at, 3.0) == handle);
            }
            for (const MaskHandle handle : linearHandles) {
                const QPointF at = handleOf(someLine, mapping, handle);
                CAPTURE(static_cast<int>(handle));
                CHECK(handleAt(someLine, mapping, at) == handle);
                CHECK(handleAt(someLine, mapping, at + QPointF(7.0, 7.0)) == handle);
            }
        }
    }
    const MaskViewMapping mapping = mappingFor(landscape, {});
    CHECK(handleAt(someOval, mapping, {-500.0, -500.0}) == MaskHandle::None);
}

TEST_CASE("A band line is grabbed away from its dots", "[app][mask-editing]") {
    const MaskViewMapping mapping = mappingFor(landscape, {});
    const LinearMask mask{{0.3F, 0.3F}, {0.3F, 0.7F}}; // Vertical axis, horizontal bands.
    const QPointF from = handleOf(mask, mapping, MaskHandle::LinearFrom);
    const QPointF to = handleOf(mask, mapping, MaskHandle::LinearTo);
    // Along the band, 100 px from its dot.
    CHECK(handleAt(mask, mapping, from + QPointF(100.0, 4.0)) == MaskHandle::LinearFromBand);
    CHECK(handleAt(mask, mapping, to + QPointF(-100.0, -4.0)) == MaskHandle::LinearToBand);
    CHECK(handleAt(mask, mapping, from + QPointF(100.0, 12.0)) == MaskHandle::None);
    // On the dot, the dot wins over the line through it.
    CHECK(handleAt(mask, mapping, from + QPointF(3.0, 1.0)) == MaskHandle::LinearFrom);
    // The middle line is no handle.
    const QPointF middle = handleOf(mask, mapping, MaskHandle::LinearMiddle);
    CHECK(handleAt(mask, mapping, middle + QPointF(100.0, 0.0)) == MaskHandle::None);
}

TEST_CASE("Dragging an end of a linear mask moves it by the pointer", "[app][mask-editing]") {
    for (const GeometrySettings& geometry : {GeometrySettings{}, flippedAndTurned()}) {
        const MaskViewMapping mapping = mappingFor(landscape, geometry);
        const QPointF press = handleOf(someLine, mapping, MaskHandle::LinearTo);
        const QPointF pointer = press + QPointF(37.0, -22.0);

        const Mask moved =
            draggedShape(someLine, MaskHandle::LinearTo, press, pointer, mapping, false);
        const LinearMask line = std::get<LinearMask>(moved);
        CHECK(line.from == someLine.from);
        CHECK(distance(mapping.widgetFrom(line.to), pointer) < 1e-3);

        const Mask start =
            draggedShape(someLine, MaskHandle::LinearFrom, press, pointer, mapping, false);
        const LinearMask other = std::get<LinearMask>(start);
        CHECK(other.to == someLine.to);
        CHECK(distance(mapping.widgetFrom(other.from),
                       mapping.widgetFrom(someLine.from) + (pointer - press)) < 1e-3);
    }
}

TEST_CASE("Dragging the middle moves both ends", "[app][mask-editing]") {
    const MaskViewMapping mapping = mappingFor(landscape, flippedAndTurned());
    const QPointF press = handleOf(someLine, mapping, MaskHandle::LinearMiddle);
    const QPointF delta(-31.0, 45.0);
    const LinearMask line = std::get<LinearMask>(
        draggedShape(someLine, MaskHandle::LinearMiddle, press, press + delta, mapping, false));
    CHECK(distance(mapping.widgetFrom(line.from), mapping.widgetFrom(someLine.from) + delta) <
          1e-3);
    CHECK(distance(mapping.widgetFrom(line.to), mapping.widgetFrom(someLine.to) + delta) < 1e-3);
}

TEST_CASE("Dragging a band line changes only the spread", "[app][mask-editing]") {
    const MaskViewMapping mapping = mappingFor(landscape, flippedAndTurned());
    const QPointF from = mapping.widgetFrom(someLine.from);
    const QPointF to = mapping.widgetFrom(someLine.to);
    const QPointF axis = (to - from) / distance(from, to);
    const QPointF across(-axis.y(), axis.x());
    const QPointF press = from + across * 80.0;
    const QPointF pointer = press + QPointF(50.0, 20.0);

    const LinearMask line = std::get<LinearMask>(
        draggedShape(someLine, MaskHandle::LinearFromBand, press, pointer, mapping, false));
    CHECK(line.to == someLine.to);
    const QPointF newFrom = mapping.widgetFrom(line.from);
    // Still on the old axis, moved by the pointer's component along it.
    const QPointF moved = newFrom - from;
    CHECK(moved.x() * across.x() + moved.y() * across.y() == Approx(0.0).margin(1e-3));
    const QPointF delta = pointer - press;
    CHECK(moved.x() * axis.x() + moved.y() * axis.y() ==
          Approx(delta.x() * axis.x() + delta.y() * axis.y()).margin(1e-3));
}

TEST_CASE("Shift snaps an end's direction to horizontal or vertical on screen",
          "[app][mask-editing]") {
    for (const GeometrySettings& geometry : {GeometrySettings{}, flippedAndTurned()}) {
        const MaskViewMapping mapping = mappingFor(landscape, geometry);
        const QPointF fixed = mapping.widgetFrom(someLine.from);
        const QPointF press = handleOf(someLine, mapping, MaskHandle::LinearTo);
        for (const QPointF pointer : {fixed + QPointF(200.0, 14.0), fixed + QPointF(-9.0, 180.0),
                                      fixed + QPointF(-120.0, -130.0)}) {
            const LinearMask line = std::get<LinearMask>(
                draggedShape(someLine, MaskHandle::LinearTo, press, pointer, mapping, true));
            const QPointF direction = mapping.widgetFrom(line.to) - mapping.widgetFrom(line.from);
            CHECK(std::min(std::abs(direction.x()), std::abs(direction.y())) < 1e-2);
            CHECK(distance(mapping.widgetFrom(line.from), fixed) < 1e-3);
        }
    }
}

TEST_CASE("An end dragged onto the other stops short on its own side", "[app][mask-editing]") {
    const MaskViewMapping mapping = mappingFor(landscape, {});
    const QPointF fixed = mapping.widgetFrom(someLine.from);
    const QPointF press = handleOf(someLine, mapping, MaskHandle::LinearTo);
    const auto original = QPointF(someLine.to.u - someLine.from.u, someLine.to.v - someLine.from.v);
    // Onto the other end, and a hair across it: closer than the minimum, so held short.
    for (const QPointF pointer :
         {fixed, fixed - (press - fixed) * 1e-4, fixed + (press - fixed) * 1e-4}) {
        const Mask shape =
            draggedShape(someLine, MaskHandle::LinearTo, press, pointer, mapping, false);
        const LinearMask line = std::get<LinearMask>(shape);
        const double du = line.to.u - line.from.u;
        const double dv = line.to.v - line.from.v;
        CHECK(std::hypot(du, dv) >= minimumMaskExtent);
        CHECK(du * original.x() + dv * original.y() > 0.0);
        CHECK(acceptedAs(shape));
    }
    // Well across it, the end is on the other side: the gradient is reversed, and valid.
    CHECK(acceptedAs(draggedShape(someLine, MaskHandle::LinearTo, press,
                                  fixed - (press - fixed) * 0.5, mapping, false)));
}

TEST_CASE("Dragging a radial mask's centre moves it", "[app][mask-editing]") {
    const MaskViewMapping mapping = mappingFor(landscape, flippedAndTurned());
    const QPointF press = handleOf(someOval, mapping, MaskHandle::RadialCentre);
    const QPointF pointer = press + QPointF(-60.0, 25.0);
    const RadialMask oval = std::get<RadialMask>(
        draggedShape(someOval, MaskHandle::RadialCentre, press, pointer, mapping, false));
    CHECK(distance(mapping.widgetFrom(oval.centre), pointer) < 1e-3);
    CHECK(oval.radiusX == someOval.radiusX);
    CHECK(oval.radiusY == someOval.radiusY);
    CHECK(oval.angle == someOval.angle);
    CHECK(oval.feather == someOval.feather);
}

TEST_CASE("Dragging a radius point sets that radius, both sides moving", "[app][mask-editing]") {
    for (const GeometrySettings& geometry : {GeometrySettings{}, flippedAndTurned()}) {
        const MaskViewMapping mapping = mappingFor(landscape, geometry);
        const QPointF centre = handleOf(someOval, mapping, MaskHandle::RadialCentre);
        for (const MaskHandle handle : {MaskHandle::RadiusPlusX, MaskHandle::RadiusMinusX}) {
            const QPointF press = handleOf(someOval, mapping, handle);
            const QPointF outward = (press - centre) / distance(press, centre);
            const RadialMask oval = std::get<RadialMask>(
                draggedShape(someOval, handle, press, press + outward * 30.0, mapping, false));
            CHECK(oval.radiusX == Approx(someOval.radiusX + 30.0 / mapping.scale()).epsilon(1e-5));
            CHECK(oval.radiusY == someOval.radiusY);
            CHECK(oval.centre == someOval.centre);
        }
        for (const MaskHandle handle : {MaskHandle::RadiusPlusY, MaskHandle::RadiusMinusY}) {
            const QPointF press = handleOf(someOval, mapping, handle);
            const QPointF outward = (press - centre) / distance(press, centre);
            const RadialMask oval = std::get<RadialMask>(
                draggedShape(someOval, handle, press, press - outward * 20.0, mapping, false));
            CHECK(oval.radiusY == Approx(someOval.radiusY - 20.0 / mapping.scale()).epsilon(1e-5));
            CHECK(oval.radiusX == someOval.radiusX);
        }
    }
}

TEST_CASE("Shift on a radius point keeps the ratio of the radii", "[app][mask-editing]") {
    const MaskViewMapping mapping = mappingFor(landscape, {});
    const QPointF centre = handleOf(someOval, mapping, MaskHandle::RadialCentre);
    const QPointF press = handleOf(someOval, mapping, MaskHandle::RadiusPlusY);
    const QPointF outward = (press - centre) / distance(press, centre);
    const RadialMask oval = std::get<RadialMask>(draggedShape(
        someOval, MaskHandle::RadiusPlusY, press, press + outward * 40.0, mapping, true));
    CHECK(oval.radiusX / oval.radiusY == Approx(someOval.radiusX / someOval.radiusY).epsilon(1e-5));
    CHECK(oval.radiusY == Approx(someOval.radiusY + 40.0 / mapping.scale()).epsilon(1e-5));
}

TEST_CASE("Radii stay within their limits", "[app][mask-editing]") {
    const MaskViewMapping mapping = mappingFor(landscape, {});
    const QPointF centre = handleOf(someOval, mapping, MaskHandle::RadialCentre);
    const QPointF press = handleOf(someOval, mapping, MaskHandle::RadiusPlusX);
    const QPointF outward = (press - centre) / distance(press, centre);
    const RadialMask tiny = std::get<RadialMask>(draggedShape(
        someOval, MaskHandle::RadiusPlusX, press, centre - outward * 1e6, mapping, false));
    CHECK(tiny.radiusX >= minimumMaskExtent);
    const RadialMask huge = std::get<RadialMask>(draggedShape(
        someOval, MaskHandle::RadiusPlusX, press, centre + outward * 1e9, mapping, false));
    CHECK(huge.radiusX == maximumMaskRadius);
    const RadialMask locked = std::get<RadialMask>(draggedShape(
        someOval, MaskHandle::RadiusPlusX, press, centre + outward * 1e9, mapping, true));
    CHECK(locked.radiusX <= maximumMaskRadius);
    CHECK(locked.radiusY <= maximumMaskRadius);
    CHECK(locked.radiusX / locked.radiusY ==
          Approx(someOval.radiusX / someOval.radiusY).epsilon(1e-4));
}

TEST_CASE("The rotation knob turns the ellipse the way the pointer turns", "[app][mask-editing]") {
    GeometrySettings mirrored;
    mirrored.flipHorizontal = true;
    GeometrySettings turned;
    turned.rotation = QuarterTurn::Clockwise90;
    for (const GeometrySettings& geometry :
         {GeometrySettings{}, mirrored, turned, flippedAndTurned()}) {
        const MaskViewMapping mapping = mappingFor(landscape, geometry);
        const QPointF centre = handleOf(someOval, mapping, MaskHandle::RadialCentre);
        const QPointF press = handleOf(someOval, mapping, MaskHandle::RadialRotation);
        // A quarter turn clockwise on screen about the centre (y points down).
        const QPointF radius = press - centre;
        const QPointF pointer = centre + QPointF(-radius.y(), radius.x());
        const Mask shape =
            draggedShape(someOval, MaskHandle::RadialRotation, press, pointer, mapping, false);
        const RadialMask oval = std::get<RadialMask>(shape);
        // The knob now lies on the ray from the centre to the pointer.
        const QPointF knob = handleOf(oval, mapping, MaskHandle::RadialRotation) - centre;
        const QPointF wanted = pointer - centre;
        CHECK(knob.x() * wanted.y() - knob.y() * wanted.x() ==
              Approx(0.0).margin(1e-3 * distance(pointer, centre)));
        CHECK(knob.x() * wanted.x() + knob.y() * wanted.y() > 0.0);
        // Radii and centre are untouched.
        CHECK(oval.radiusX == someOval.radiusX);
        CHECK(oval.centre == someOval.centre);
    }
    // Without a flip the angle grows with a clockwise turn on screen, since y points down.
    const MaskViewMapping plain = mappingFor(landscape, {});
    const QPointF centre = handleOf(someOval, plain, MaskHandle::RadialCentre);
    const QPointF press = handleOf(someOval, plain, MaskHandle::RadialRotation);
    const QPointF radius = press - centre;
    const RadialMask oval =
        std::get<RadialMask>(draggedShape(someOval, MaskHandle::RadialRotation, press,
                                          centre + QPointF(-radius.y(), radius.x()), plain, false));
    CHECK(oval.angle == Approx(someOval.angle + 90.0F).margin(1e-3));
    // A press on the centre turns nothing.
    CHECK(std::get<RadialMask>(draggedShape(someOval, MaskHandle::RadialRotation, centre, press,
                                            plain, false)) == someOval);
}

TEST_CASE("The feather knob sets the inner ring", "[app][mask-editing]") {
    for (const GeometrySettings& geometry : {GeometrySettings{}, flippedAndTurned()}) {
        const MaskViewMapping mapping = mappingFor(landscape, geometry);
        const QPointF centre = handleOf(someOval, mapping, MaskHandle::RadialCentre);
        const QPointF press = handleOf(someOval, mapping, MaskHandle::RadialFeather);
        // Out along the same ray to an elliptical distance of 0.8: the feather becomes 0.2.
        const QPointF pointer = centre + (press - centre) * (0.8 / 0.5);
        const RadialMask oval = std::get<RadialMask>(
            draggedShape(someOval, MaskHandle::RadialFeather, press, pointer, mapping, false));
        CHECK(oval.feather == Approx(0.2F).margin(1e-4));
        // Beyond the rim and into the centre: clamped to the range.
        const RadialMask hard =
            std::get<RadialMask>(draggedShape(someOval, MaskHandle::RadialFeather, press,
                                              centre + (press - centre) * 10.0, mapping, false));
        CHECK(hard.feather == 0.0F);
        const RadialMask soft = std::get<RadialMask>(
            draggedShape(someOval, MaskHandle::RadialFeather, press, centre, mapping, false));
        CHECK(soft.feather == 1.0F);
    }
}

TEST_CASE("A handle of another kind leaves the shape as it is", "[app][mask-editing]") {
    const MaskViewMapping mapping = mappingFor(landscape, {});
    CHECK(draggedShape(someLine, MaskHandle::RadialCentre, {0, 0}, {50, 50}, mapping, false) ==
          Mask{someLine});
    CHECK(draggedShape(someOval, MaskHandle::LinearFrom, {0, 0}, {50, 50}, mapping, false) ==
          Mask{someOval});
    CHECK(draggedShape(someOval, MaskHandle::None, {0, 0}, {50, 50}, mapping, false) ==
          Mask{someOval});
}

TEST_CASE("Random degenerate drags are always accepted by the edit rules", "[app][mask-editing]") {
    std::mt19937 random(20261009);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    const std::array<QPointF, 6> extremes{
        {{0, 0}, {1e6, 1e6}, {-1e6, 3.0}, {1e12, -1e12}, {0.5, 0.5}, {-3e4, 4e4}}};
    const std::array<MaskHandle, 7> radialHandles{
        MaskHandle::RadialCentre, MaskHandle::RadiusPlusX,  MaskHandle::RadiusMinusX,
        MaskHandle::RadiusPlusY,  MaskHandle::RadiusMinusY, MaskHandle::RadialRotation,
        MaskHandle::RadialFeather};
    const std::array<MaskHandle, 5> linearHandles{
        MaskHandle::LinearFrom, MaskHandle::LinearTo, MaskHandle::LinearMiddle,
        MaskHandle::LinearFromBand, MaskHandle::LinearToBand};
    const std::array<LinearMask, 3> lines{
        someLine, LinearMask{{-2.0F, -2.0F}, {3.0F, 3.0F}},
        LinearMask{{0.5F, 0.5F}, {0.5F + 2.0F * minimumMaskExtent, 0.5F}}};
    const std::array<RadialMask, 3> ovals{someOval,
                                          RadialMask{.centre = {3.0F, -2.0F},
                                                     .radiusX = maximumMaskRadius,
                                                     .radiusY = minimumMaskExtent,
                                                     .angle = -179.0F,
                                                     .feather = 1.0F},
                                          RadialMask{.centre = {0.5F, 0.5F},
                                                     .radiusX = minimumMaskExtent,
                                                     .radiusY = minimumMaskExtent,
                                                     .angle = 90.0F,
                                                     .feather = 0.0F}};
    const std::array<GeometrySettings, 3> geometries{
        GeometrySettings{}, flippedAndTurned(),
        GeometrySettings{QuarterTurn::Clockwise270, true, true, -30.0, {}}};
    for (int trial = 0; trial < 3000; ++trial) {
        const MaskViewMapping mapping = mappingFor(landscape, geometries[trial % geometries.size()],
                                                   trial % 2 == 0 ? 0.0 : 4.0);
        const auto pick = [&random](std::size_t count) {
            return std::uniform_int_distribution<std::size_t>(0, count - 1)(random);
        };
        const auto pointerAt = [&] {
            if (pick(4) == 0) {
                return extremes[pick(extremes.size())];
            }
            const double reach = std::pow(10.0, 1.0 + 4.0 * (unit(random) + 1.0) / 2.0);
            return QPointF(unit(random) * reach, unit(random) * reach);
        };
        const bool constrain = pick(2) == 0;
        Mask shape;
        MaskHandle handle;
        if (pick(2) == 0) {
            const LinearMask& base = lines[pick(lines.size())];
            handle = linearHandles[pick(linearHandles.size())];
            shape = base;
        } else {
            const RadialMask& base = ovals[pick(ovals.size())];
            handle = radialHandles[pick(radialHandles.size())];
            shape = base;
        }
        const QPointF dot = pick(3) == 0 ? pointerAt() : [&] {
            for (const HandlePosition& found : handlePositions(shape, mapping)) {
                if (found.handle == handle) {
                    return found.position;
                }
            }
            return pinPosition(shape, mapping);
        }();
        const Mask dragged = draggedShape(shape, handle, dot, pointerAt(), mapping, constrain);
        CAPTURE(trial, static_cast<int>(handle), constrain);
        REQUIRE(dragged.index() == shape.index());
        REQUIRE_NOTHROW(withLocalShape(withLocalAdjustmentAdded(DevelopState{}, shape),
                                       LocalAdjustmentId{1}, dragged));
    }
}

TEST_CASE("Creating by drag", "[app][mask-editing]") {
    for (const GeometrySettings& geometry : {GeometrySettings{}, flippedAndTurned()}) {
        const MaskViewMapping mapping = mappingFor(landscape, geometry);
        const QPointF press(300.0, 200.0);
        const QPointF pointer(520.0, 340.0);

        SECTION("a linear mask runs from the press to the pointer") {
            const LinearMask line = std::get<LinearMask>(
                createdShape(MaskTool::Linear, press, pointer, mapping, widgetSize));
            CHECK(distance(mapping.widgetFrom(line.from), press) < 1e-3);
            CHECK(distance(mapping.widgetFrom(line.to), pointer) < 1e-3);
        }
        SECTION("a radial mask is a circle about the press out to the pointer") {
            const RadialMask oval = std::get<RadialMask>(
                createdShape(MaskTool::Radial, press, pointer, mapping, widgetSize));
            CHECK(distance(mapping.widgetFrom(oval.centre), press) < 1e-3);
            CHECK(oval.radiusX == oval.radiusY);
            CHECK(oval.radiusX * mapping.scale() == Approx(distance(press, pointer)).epsilon(1e-5));
            CHECK(oval.feather == 0.5F);
            // The x axis is horizontal on screen, pointing right.
            const QPointF across = handleOf(oval, mapping, MaskHandle::RadiusPlusX) - press;
            CHECK(across.y() == Approx(0.0).margin(1e-2));
            CHECK(across.x() > 0.0);
        }
    }
}

TEST_CASE("Creating by click makes the default size at the click", "[app][mask-editing]") {
    for (const GeometrySettings& geometry : {GeometrySettings{}, flippedAndTurned()}) {
        const MaskViewMapping mapping = mappingFor(landscape, geometry);
        const QPointF press(400.0, 250.0);
        for (const QPointF pointer : {press, press + QPointF(2.0, -2.0)}) {
            const LinearMask line = std::get<LinearMask>(
                createdShape(MaskTool::Linear, press, pointer, mapping, widgetSize));
            CHECK(distance(mapping.widgetFrom(line.from), press) < 1e-3);
            // A fifth of the shorter side, straight down on screen.
            CHECK(distance(mapping.widgetFrom(line.to), press + QPointF(0.0, 120.0)) < 1e-3);

            const RadialMask oval = std::get<RadialMask>(
                createdShape(MaskTool::Radial, press, pointer, mapping, widgetSize));
            CHECK(distance(mapping.widgetFrom(oval.centre), press) < 1e-3);
            CHECK(oval.radiusX * mapping.scale() == Approx(100.0).epsilon(1e-5));
            CHECK(oval.radiusX == oval.radiusY);
        }
    }
    CHECK_THROWS_AS(createdShape(MaskTool::None, {}, {}, mappingFor(landscape, {}), widgetSize),
                    std::invalid_argument);
}

TEST_CASE("A created shape is always accepted, however small the drag or deep the zoom",
          "[app][mask-editing]") {
    for (const double zoom : {0.0, 1.0, 32.0}) {
        const MaskViewMapping mapping = mappingFor(landscape, flippedAndTurned(), zoom);
        for (const QPointF pointer :
             {QPointF(401.0, 300.0), QPointF(405.0, 300.0), QPointF(-9e9, 1e9)}) {
            for (const MaskTool tool : {MaskTool::Linear, MaskTool::Radial}) {
                const Mask shape = createdShape(tool, {400.0, 300.0}, pointer, mapping, widgetSize);
                CHECK(acceptedAs(shape));
                CHECK(shape.index() == (tool == MaskTool::Linear ? 0U : 1U));
                if (const auto* line = std::get_if<LinearMask>(&shape)) {
                    CHECK(std::hypot(line->to.u - line->from.u, line->to.v - line->from.v) >=
                          minimumMaskExtent);
                }
            }
        }
    }
}

TEST_CASE("The linear marks are three lines across the view, perpendicular to the axis",
          "[app][mask-editing]") {
    for (const GeometrySettings& geometry : {GeometrySettings{}, flippedAndTurned()}) {
        const MaskViewMapping mapping = mappingFor(landscape, geometry);
        const QRectF bounds(0.0, 0.0, widgetSize.width(), widgetSize.height());
        const LinearMarks marks = linearMarks(someLine, mapping, bounds);
        CHECK(distance(marks.from, mapping.widgetFrom(someLine.from)) < 1e-9);
        CHECK(distance(marks.to, mapping.widgetFrom(someLine.to)) < 1e-9);
        CHECK(distance(marks.middle, (marks.from + marks.to) / 2.0) < 1e-9);
        const QPointF axis = marks.to - marks.from;
        const std::array<std::pair<std::optional<QLineF>, QPointF>, 3> lines{
            {{marks.fromLine, marks.from},
             {marks.toLine, marks.to},
             {marks.middleLine, marks.middle}}};
        for (const auto& [line, through] : lines) {
            REQUIRE(line);
            const QPointF direction = line->p2() - line->p1();
            CHECK(direction.x() * axis.x() + direction.y() * axis.y() ==
                  Approx(0.0).margin(1e-6 * distance(line->p1(), line->p2()) *
                                     distance(marks.from, marks.to)));
            // Through its dot, and ending on the bounds.
            const QPointF toDot = through - line->p1();
            CHECK(std::abs(toDot.x() * direction.y() - toDot.y() * direction.x()) /
                      distance(line->p1(), line->p2()) <
                  1e-6);
            for (const QPointF end : {line->p1(), line->p2()}) {
                CHECK(bounds.adjusted(-1e-6, -1e-6, 1e-6, 1e-6).contains(end));
            }
        }
    }
    SECTION("ends that coincide on screen give no lines") {
        const MaskViewMapping mapping = mappingFor(landscape, {}, 0.25);
        const LinearMask tiny{{0.5F, 0.5F}, {0.5F, 0.5F + 2.0F * minimumMaskExtent}};
        const LinearMarks marks = linearMarks(tiny, mapping, QRectF(0, 0, 800, 600));
        CHECK(marks.fromLine.has_value() == (distance(marks.from, marks.to) > 1e-12));
    }
    SECTION("a line that misses the view is empty") {
        const MaskViewMapping mapping = mappingFor(landscape, {});
        const LinearMask far{{0.5F, 2.5F}, {0.5F, 2.9F}};
        const LinearMarks marks = linearMarks(far, mapping, QRectF(0, 0, 800, 600));
        CHECK_FALSE(marks.fromLine);
        CHECK_FALSE(marks.toLine);
    }
}

TEST_CASE("The radial marks take the unit circle to the ellipse", "[app][mask-editing]") {
    for (const GeometrySettings& geometry : {GeometrySettings{}, flippedAndTurned()}) {
        const MaskViewMapping mapping = mappingFor(landscape, geometry);
        const RadialMarks marks = radialMarks(someOval, mapping);
        CHECK(marks.inner == Approx(1.0 - someOval.feather));
        CHECK(distance(marks.centre, handleOf(someOval, mapping, MaskHandle::RadialCentre)) < 1e-6);
        CHECK(distance(marks.unitToWidget.map(QPointF(0.0, 0.0)), marks.centre) < 1e-6);
        CHECK(distance(marks.unitToWidget.map(QPointF(1.0, 0.0)),
                       handleOf(someOval, mapping, MaskHandle::RadiusPlusX)) < 1e-6);
        CHECK(distance(marks.unitToWidget.map(QPointF(0.0, 1.0)),
                       handleOf(someOval, mapping, MaskHandle::RadiusPlusY)) < 1e-6);
        CHECK(distance(marks.unitToWidget.map(QPointF(-1.0, 0.0)),
                       handleOf(someOval, mapping, MaskHandle::RadiusMinusX)) < 1e-6);
    }
}

TEST_CASE("Pins select other masks", "[app][mask-editing]") {
    DevelopState state;
    state = withLocalAdjustmentAdded(state, someLine);
    RadialMask apart = someOval;
    apart.centre = {0.85F, 0.8F};
    state = withLocalAdjustmentAdded(state, apart);
    const LocalAdjustmentId line = state.localAdjustments[0].id;
    const LocalAdjustmentId oval = state.localAdjustments[1].id;
    const MaskViewMapping mapping = mappingFor(landscape, flippedAndTurned());
    const QPointF linePin = pinPosition(someLine, mapping);
    const QPointF ovalPin = pinPosition(apart, mapping);
    REQUIRE(distance(linePin, ovalPin) > 30.0);

    CHECK(pinAt(state, mapping, linePin) == line);
    CHECK(pinAt(state, mapping, ovalPin + QPointF(6.0, -6.0)) == oval);
    CHECK_FALSE(pinAt(state, mapping, ovalPin + QPointF(0.0, 11.0)));
    CHECK_FALSE(pinAt(state, mapping, ovalPin + QPointF(0.0, 11.0), 10.0, line));
    // The selected mask has handles, not a pin.
    CHECK_FALSE(pinAt(state, mapping, linePin, 10.0, line));
    // Two pins at one place: the later mask, drawn on top, is taken.
    state = withLocalAdjustmentAdded(state, RadialMask{.centre = apart.centre});
    CHECK(pinAt(state, mapping, ovalPin) == state.localAdjustments[2].id);
    CHECK(pinAt(state, mapping, ovalPin, 10.0, state.localAdjustments[2].id) == oval);
}

TEST_CASE("The selection is kept while its mask exists", "[app][mask-editing]") {
    DevelopState state = withLocalAdjustmentAdded(DevelopState{}, someLine);
    state = withLocalAdjustmentAdded(state, someOval);
    const LocalAdjustmentId first = state.localAdjustments[0].id;
    const LocalAdjustmentId second = state.localAdjustments[1].id;
    CHECK(reconciledSelection(state, first) == first);
    CHECK(reconciledSelection(state, second) == second);
    CHECK_FALSE(reconciledSelection(state, std::nullopt));
    CHECK_FALSE(reconciledSelection(state, LocalAdjustmentId{42}));
    const DevelopState removed = withLocalAdjustmentRemoved(state, first);
    CHECK_FALSE(reconciledSelection(removed, first));
    CHECK(reconciledSelection(removed, second) == second);
    // An id freed and not handed out again stays gone, even when a mask is added.
    const DevelopState added = withLocalAdjustmentAdded(removed, RadialMask{});
    CHECK_FALSE(reconciledSelection(added, first));
    CHECK_FALSE(reconciledSelection(DevelopState{}, first));
}

TEST_CASE("A band line is left out when the caller asks for dots only", "[app][mask-editing]") {
    const MaskViewMapping mapping = mappingFor(landscape, {});
    const LinearMask mask{{0.3F, 0.3F}, {0.3F, 0.7F}};
    const QPointF from = handleOf(mask, mapping, MaskHandle::LinearFrom);
    CHECK(handleAt(mask, mapping, from + QPointF(100.0, 4.0), maskHandleReach, false) ==
          MaskHandle::None);
    CHECK(handleAt(mask, mapping, from + QPointF(3.0, 1.0), maskHandleReach, false) ==
          MaskHandle::LinearFrom);
}

TEST_CASE("Dragging the middle against a position limit keeps the angle and spread",
          "[app][mask-editing]") {
    const MaskViewMapping mapping = mappingFor(landscape, {});
    const QPointF press = handleOf(someLine, mapping, MaskHandle::LinearMiddle);
    const QPointF before = mapping.widgetFrom(someLine.to) - mapping.widgetFrom(someLine.from);
    // Far to the right: the end further right reaches the limit first.
    const LinearMask line =
        std::get<LinearMask>(draggedShape(someLine, MaskHandle::LinearMiddle, press,
                                          press + QPointF(50000.0, 300.0), mapping, false));
    const QPointF after = mapping.widgetFrom(line.to) - mapping.widgetFrom(line.from);
    CHECK(after.x() == Approx(before.x()).margin(1e-2));
    CHECK(after.y() == Approx(before.y()).margin(1e-2));
    CHECK(std::max(line.from.u, line.to.u) == Approx(maximumMaskPosition).margin(1e-4));
}

TEST_CASE("The feather knob follows the pointer from where it is drawn", "[app][mask-editing]") {
    const MaskViewMapping mapping = mappingFor(landscape, {});
    RadialMask soft = someOval;
    soft.feather = 0.95F; // Drawn at the floor, 0.15, rather than at 0.05.
    const QPointF press = handleOf(soft, mapping, MaskHandle::RadialFeather);
    const QPointF centre = mapping.widgetFrom(soft.centre);
    // A small move outwards along the knob's own direction moves the knob by as much.
    const QPointF pointer = press + (press - centre) * 0.1;
    const RadialMask moved = std::get<RadialMask>(
        draggedShape(soft, MaskHandle::RadialFeather, press, pointer, mapping, false));
    CHECK(distance(handleOf(moved, mapping, MaskHandle::RadialFeather), pointer) < 1e-2);
}
