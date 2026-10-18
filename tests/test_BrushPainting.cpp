#include "BrushPainting.h"
#include "BrushRaster.h"
#include "support/LensTables.h"
#include "support/LocalAdjustmentStates.h"
#include "support/TempDir.h"

#include <BrushStrokes.h>
#include <DevelopState.h>
#include <DevelopedFrame.h>
#include <LocalAdjustmentEdits.h>
#include <LocalAdjustments.h>

#include <QPointF>
#include <QSettings>
#include <QString>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <optional>
#include <variant>

using namespace arraw;
using namespace arraw::app;
using namespace arraw::test;
using Catch::Approx;

// The brush tool's app-core logic: settings, refusals and the live stroke (brush GUI plan,
// sections 3, 5 and 6).

namespace {

constexpr QSizeF widgetSize{800.0, 600.0};
constexpr SourceShape landscape{{3000, 2000}, ImageOrientation::Normal};

MaskViewMapping mappingFor(SourceShape source, const GeometrySettings& geometry, double zoom = 0.0,
                           double ratio = 1.0) {
    DevelopedFrameMap map(source, geometry);
    const QSizeF frame(map.width(), map.height());
    const QSizeF view(widgetSize.width() * ratio, widgetSize.height() * ratio);
    const double actual = zoom > 0.0 ? zoom : ViewTransform::fitZoom(frame, view);
    return MaskViewMapping(std::move(map), ViewTransform(frame, view, actual), ratio);
}

MaskViewMapping fitMapping() {
    return mappingFor(landscape, {});
}

/// The widget position of a corrected position, at the fit mapping.
QPointF at(const MaskViewMapping& mapping, double u, double v) {
    return mapping.widgetFrom({u, v});
}

const BrushMask& brushIn(const DevelopState& state, LocalAdjustmentId id) {
    const LocalAdjustment* adjustment = findLocalAdjustment(state, id);
    REQUIRE(adjustment != nullptr);
    const auto* brush = std::get_if<BrushMask>(&adjustment->shape);
    REQUIRE(brush != nullptr);
    return *brush;
}

BrushSettings settingsOf(float radius) {
    BrushSettings settings;
    settings.radius = radius;
    return settings;
}

LiveStroke beginNew(const MaskViewMapping& mapping, QPointF press, const BrushSettings& settings,
                    bool erase = false, const DevelopState& baseline = DevelopState{}) {
    BrushRefusal refused = BrushRefusal::MasksFull;
    auto stroke =
        LiveStroke::begin(baseline, std::nullopt, settings, erase, press, mapping, refused);
    REQUIRE(stroke.has_value());
    REQUIRE(refused == BrushRefusal::None);
    return *stroke;
}

/// A state with one brush whose strokes spend a given budget.
DevelopState stateWithBrush(std::vector<Stroke> strokes) {
    return withLocalAdjustmentAdded(DevelopState{}, brushMaskWith(std::move(strokes)));
}

} // namespace

TEST_CASE("A pointer maps to the dab of a one-point stroke in every geometry",
          "[app][brush-painting]") {
    std::size_t checked = 0;
    for (const ImageOrientation orientation :
         {ImageOrientation::Normal, ImageOrientation::Rotate90, ImageOrientation::Rotate180,
          ImageOrientation::Rotate270, ImageOrientation::MirrorHorizontal,
          ImageOrientation::MirrorVertical, ImageOrientation::Transpose,
          ImageOrientation::Transverse}) {
        for (const QuarterTurn turn : {QuarterTurn::None, QuarterTurn::Clockwise90,
                                       QuarterTurn::Clockwise180, QuarterTurn::Clockwise270}) {
            for (const bool flipH : {false, true}) {
                for (const bool flipV : {false, true}) {
                    for (const double straighten : {0.0, 7.5}) {
                        for (const bool crop : {false, true}) {
                            GeometrySettings geometry;
                            geometry.rotation = turn;
                            geometry.flipHorizontal = flipH;
                            geometry.flipVertical = flipV;
                            geometry.straighten = straighten;
                            if (crop) {
                                geometry.crop.rectangle = UprightCropRect{0.4, 0.45, 0.6, 0.55};
                            }
                            const SourceShape source{{3000, 2000}, orientation};
                            for (const double zoom : {0.25, 0.0, 4.0}) {
                                for (const double ratio : {1.0, 2.0}) {
                                    const MaskViewMapping mapping =
                                        mappingFor(source, geometry, zoom, ratio);
                                    const QPointF press(401.3, 297.9);
                                    BrushRefusal refused;
                                    const auto stroke = LiveStroke::begin(
                                        DevelopState{}, std::nullopt, settingsOf(0.05F), false,
                                        press, mapping, refused);
                                    REQUIRE(stroke.has_value());
                                    const Stroke& painted =
                                        *brushIn(stroke->state(), stroke->mask())
                                             .strokes->strokes()[0];
                                    const auto centres = dabCentres(painted, source.size);
                                    REQUIRE(centres.size() == 1);
                                    const QPointF back = mapping.widgetFrom(
                                        {centres[0].x / static_cast<double>(source.size.width),
                                         centres[0].y / static_cast<double>(source.size.height)});
                                    CHECK(std::hypot(back.x() - press.x(), back.y() - press.y()) <
                                          0.01);
                                    ++checked;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK(checked == 8 * 4 * 4 * 2 * 2 * 3 * 2);
}

TEST_CASE("The stroke has the settings' radius, hardness, flow and the erase flag",
          "[app][brush-painting]") {
    const MaskViewMapping mapping = fitMapping();
    BrushSettings settings{0.037F, 0.3F, 0.6F, false};
    const LiveStroke plain = beginNew(mapping, at(mapping, 0.5, 0.5), settings);
    const Stroke& first = *brushIn(plain.state(), plain.mask()).strokes->strokes()[0];
    CHECK(first.radius == 0.037F);
    CHECK(first.hardness == 0.3F);
    CHECK(first.flow == 0.6F);
    CHECK_FALSE(first.erase);
    CHECK(first.points.size() == 1);

    // Alt at the press inverts the toggle for that stroke; the caller decides, the stroke keeps it.
    const LiveStroke erasing = beginNew(mapping, at(mapping, 0.5, 0.5), settings, true);
    CHECK(brushIn(erasing.state(), erasing.mask()).strokes->strokes()[0]->erase);
    settings.erase = true;
    const LiveStroke inverted = beginNew(mapping, at(mapping, 0.5, 0.5), settings, false);
    CHECK_FALSE(brushIn(inverted.state(), inverted.mask()).strokes->strokes()[0]->erase);
}

TEST_CASE("Creating a brush adds it with the next id", "[app][brush-painting]") {
    const MaskViewMapping mapping = fitMapping();
    DevelopState baseline = withLocalAdjustmentAdded(DevelopState{}, someRadial());
    baseline = withLocalAdjustmentAdded(baseline, someLinear());
    const LiveStroke stroke =
        beginNew(mapping, at(mapping, 0.5, 0.5), settingsOf(0.02F), false, baseline);
    REQUIRE(stroke.state().localAdjustments.size() == 3);
    CHECK(stroke.mask() == stroke.state().localAdjustments.back().id);
    CHECK(stroke.mask() == baseline.nextLocalAdjustmentId);
    CHECK(brushIn(stroke.state(), stroke.mask()).strokes->size() == 1);
    CHECK(baseline.localAdjustments.size() == 2);
}

TEST_CASE("A position closer than the thinning rule is dropped and the release is kept",
          "[app][brush-painting]") {
    const MaskViewMapping mapping = fitMapping();
    const BrushSettings settings = settingsOf(0.05F);
    LiveStroke stroke = beginNew(mapping, at(mapping, 0.5, 0.5), settings);
    const double spacing =
        std::max(1.0 / mapping.scale(), 0.1 * static_cast<double>(settings.radius));
    const double spacingPixels = spacing * mapping.scale();
    const QPointF start = at(mapping, 0.5, 0.5);

    CHECK_FALSE(stroke.extend(start + QPointF(spacingPixels * 0.5, 0.0), mapping));
    CHECK(brushIn(stroke.state(), stroke.mask()).strokes->strokes()[0]->points.size() == 1);
    CHECK(stroke.extend(start + QPointF(spacingPixels * 1.2, 0.0), mapping));
    // Measured from the last kept point, not from the dropped ones.
    CHECK_FALSE(stroke.extend(start + QPointF(spacingPixels * 1.7, 0.0), mapping));
    CHECK(stroke.extend(start + QPointF(spacingPixels * 2.5, 0.0), mapping));
    // The release is kept however close.
    CHECK_FALSE(stroke.extend(start + QPointF(spacingPixels * 2.6, 0.0), mapping));
    CHECK(stroke.extend(start + QPointF(spacingPixels * 2.6, 0.0), mapping, true));
    CHECK(brushIn(stroke.state(), stroke.mask()).strokes->strokes()[0]->points.size() == 4);
    // A release that repeats the last point changes nothing.
    CHECK_FALSE(stroke.extend(start + QPointF(spacingPixels * 2.6, 0.0), mapping, true));
}

TEST_CASE("A tiny brush is thinned by one logical pixel", "[app][brush-painting]") {
    const MaskViewMapping mapping = fitMapping();
    LiveStroke stroke = beginNew(mapping, at(mapping, 0.5, 0.5), settingsOf(minimumBrushRadius));
    const QPointF start = at(mapping, 0.5, 0.5);
    CHECK_FALSE(stroke.extend(start + QPointF(0.6, 0.0), mapping));
    CHECK(stroke.extend(start + QPointF(1.2, 0.0), mapping));
}

TEST_CASE("Consecutive states share the earlier strokes and the live points grow as a prefix",
          "[app][brush-painting]") {
    const MaskViewMapping mapping = fitMapping();
    const DevelopState baseline =
        stateWithBrush({paintedStroke({0.1F, 0.2F}, {0.9F, 0.3F}, 12, 0.08F, 0.3F, 1.0F)});
    const LocalAdjustmentId id = baseline.localAdjustments.front().id;
    BrushRefusal refused;
    auto started = LiveStroke::begin(baseline, id, settingsOf(0.03F), false, at(mapping, 0.3, 0.5),
                                     mapping, refused);
    REQUIRE(started.has_value());
    LiveStroke& stroke = *started;
    const auto earlier = brushIn(baseline, id).strokes->strokes()[0];
    std::vector<SensorPoint> previous = brushIn(stroke.state(), id).strokes->strokes()[1]->points;
    for (int step = 1; step <= 20; ++step) {
        REQUIRE(stroke.extend(at(mapping, 0.3 + step * 0.01, 0.5), mapping));
        const StrokeList& list = *brushIn(stroke.state(), id).strokes;
        REQUIRE(list.size() == 2);
        CHECK(list.strokes()[0] == earlier);
        const std::vector<SensorPoint>& points = list.strokes()[1]->points;
        REQUIRE(points.size() == previous.size() + 1);
        CHECK(std::equal(previous.begin(), previous.end(), points.begin()));
        previous = points;
    }
}

TEST_CASE("A stroke at the point cap splits in the same step and keeps the reported pointer",
          "[app][brush-painting]") {
    // Zoomed in, so that a few logical pixels are far enough apart to keep.
    const MaskViewMapping mapping = mappingFor(landscape, {}, 4.0);
    const auto pointer = [](int k) { return QPointF(300.0 + std::fmod(k * 3.6, 200.0), 300.0); };
    LiveStroke stroke = beginNew(mapping, pointer(0), settingsOf(0.002F));
    const LocalAdjustmentId id = stroke.mask();
    // Points far enough from the last, until the stroke is full.
    int step = 1;
    while (!stroke.atPointCap()) {
        REQUIRE(stroke.extend(pointer(step), mapping));
        ++step;
        REQUIRE(step < 20000);
    }
    CHECK(brushIn(stroke.state(), id).strokes->size() == 1);
    CHECK(brushIn(stroke.state(), id).strokes->strokes()[0]->points.size() == maximumStrokePoints);
    CHECK(stroke.pendingReport());
    stroke.reported();
    CHECK_FALSE(stroke.pendingReport());
    const auto reportedLast = brushIn(stroke.state(), id).strokes->strokes().back();

    REQUIRE(stroke.extend(pointer(step), mapping));
    const StrokeList& list = *brushIn(stroke.state(), id).strokes;
    REQUIRE(list.size() == 2);
    CHECK(list.strokes()[0] == reportedLast);
    CHECK(list.strokes()[0]->points.size() == maximumStrokePoints);
    REQUIRE(list.strokes()[1]->points.size() == 2);
    CHECK(list.strokes()[1]->points.front() == list.strokes()[0]->points.back());
    CHECK(list.strokes()[1]->radius == list.strokes()[0]->radius);
    CHECK(stroke.refusal() == BrushRefusal::None);
}

TEST_CASE("Each refusal is told at the press", "[app][brush-painting]") {
    const MaskViewMapping mapping = fitMapping();
    const BrushSettings settings = settingsOf(0.02F);
    const QPointF press = at(mapping, 0.5, 0.5);
    BrushRefusal refused = BrushRefusal::None;

    SECTION("a full list of masks") {
        DevelopState full;
        for (std::size_t i = 0; i < maximumLocalAdjustments; ++i) {
            full = withLocalAdjustmentAdded(full, someRadial());
        }
        CHECK_FALSE(
            LiveStroke::begin(full, std::nullopt, settings, false, press, mapping, refused));
        CHECK(refused == BrushRefusal::MasksFull);
    }
    SECTION("a mask that is not a brush") {
        const DevelopState state = withLocalAdjustmentAdded(DevelopState{}, someRadial());
        CHECK_FALSE(LiveStroke::begin(state, state.localAdjustments.front().id, settings, false,
                                      press, mapping, refused));
        CHECK(refused == BrushRefusal::NotABrush);
    }
    SECTION("a brush whose swept area is nearly spent refuses a press") {
        // Swept area 3.99 of 4: a probe of radius 0.2 spends 0.04.
        Stroke spent{0.5F, 0.5F, 1.0F, false, {{0.0F, 0.5F}, {1.0F, 0.5F}}};
        // 0.5 * 1.0 per stroke: seven of them is 3.5; add radius 0.5 * 0.99.
        std::vector<Stroke> strokes(7, spent);
        strokes.push_back(Stroke{0.5F, 0.5F, 1.0F, false, {{0.0F, 0.5F}, {0.98F, 0.5F}}});
        const DevelopState state = stateWithBrush(strokes);
        const LocalAdjustmentId id = state.localAdjustments.front().id;
        REQUIRE(state.localAdjustments.front().id == id);
        CHECK_FALSE(LiveStroke::begin(state, id, settingsOf(0.2F), false, press, mapping, refused));
        CHECK(refused == BrushRefusal::SweptArea);
        // A smaller brush still fits.
        CHECK(LiveStroke::begin(state, id, settingsOf(0.05F), false, press, mapping, refused));
        CHECK(refused == BrushRefusal::None);
    }
    SECTION("a brush whose dab budget is nearly spent refuses a press") {
        // Dabs: length / (0.25 radius). A radius 0.001 stroke of length 1 is 4000 dabs.
        std::vector<Stroke> strokes;
        for (int i = 0; i < 499; ++i) {
            strokes.push_back(Stroke{0.001F, 0.5F, 1.0F, false, {{0.0F, 0.5F}, {1.0F, 0.5F}}});
        }
        const DevelopState state = stateWithBrush(strokes);
        const LocalAdjustmentId id = state.localAdjustments.front().id;
        // 499 * 4000 = 1 996 000; the probe at 0.001 adds 4; at that point it still fits, so use
        // a last stroke that leaves fewer than 4.
        Stroke nearly{0.001F, 0.5F, 1.0F, false, {{0.0F, 0.5F}, {0.9999F, 0.5F}}};
        std::vector<Stroke> more = strokes;
        more.push_back(nearly);
        const DevelopState nearState = stateWithBrush(more);
        CHECK(refusalOf(nearState, nearState.localAdjustments.front().id,
                        probeStroke(settingsOf(0.001F), {0.5F, 0.5F})) == BrushRefusal::Dabs);
        CHECK(refusalOf(state, id, probeStroke(settingsOf(0.001F), {0.5F, 0.5F})) ==
              BrushRefusal::None);
        BrushRefusal pressed;
        CHECK_FALSE(LiveStroke::begin(nearState, nearState.localAdjustments.front().id,
                                      settingsOf(0.001F), false, press, mapping, pressed));
        CHECK(pressed == BrushRefusal::Dabs);
    }
    SECTION("a full list of strokes") {
        std::vector<Stroke> strokes(maximumStrokesPerMask,
                                    Stroke{0.01F, 0.5F, 1.0F, false, {{0.5F, 0.5F}}});
        const DevelopState state = stateWithBrush(strokes);
        CHECK(refusalOf(state, state.localAdjustments.front().id,
                        probeStroke(settings, {0.5F, 0.5F})) == BrushRefusal::Strokes);
    }
    SECTION("a mask whose points are spent") {
        std::vector<Stroke> strokes(
            10, Stroke{0.0005F, 0.5F, 1.0F, false,
                       std::vector<SensorPoint>(maximumStrokePoints, SensorPoint{0.5F, 0.5F})});
        const DevelopState state = stateWithBrush(strokes);
        CHECK(refusalOf(state, state.localAdjustments.front().id,
                        probeStroke(settings, {0.5F, 0.5F})) == BrushRefusal::Points);
    }
    SECTION("a stroke that fits") {
        const DevelopState state = stateWithBrush({});
        CHECK(refusalOf(state, state.localAdjustments.front().id,
                        probeStroke(settings, {0.5F, 0.5F})) == BrushRefusal::None);
    }
}

TEST_CASE("A budget reached mid-stroke stops it with the state at the last accepted one",
          "[app][brush-painting]") {
    const MaskViewMapping mapping = fitMapping();
    // Swept area 3.99: only a little length of a radius 0.1 stroke fits.
    std::vector<Stroke> strokes(7, Stroke{0.5F, 0.5F, 1.0F, false, {{0.0F, 0.5F}, {1.0F, 0.5F}}});
    strokes.push_back(Stroke{0.5F, 0.5F, 1.0F, false, {{0.0F, 0.5F}, {0.97F, 0.5F}}});
    const DevelopState baseline = stateWithBrush(strokes);
    const LocalAdjustmentId id = baseline.localAdjustments.front().id;
    BrushRefusal refused;
    auto started = LiveStroke::begin(baseline, id, settingsOf(0.05F), false, at(mapping, 0.1, 0.5),
                                     mapping, refused);
    REQUIRE(started.has_value());
    LiveStroke& stroke = *started;
    CHECK(stroke.extend(at(mapping, 0.12, 0.5), mapping));
    const DevelopState accepted = stroke.state();
    // 0.05 * 0.8 more length would pass the budget.
    CHECK_FALSE(stroke.extend(at(mapping, 0.92, 0.5), mapping));
    CHECK(stroke.refusal() == BrushRefusal::SweptArea);
    CHECK(stroke.state().localAdjustments.front().shape == accepted.localAdjustments.front().shape);
    // Later positions are ignored, even a near one.
    CHECK_FALSE(stroke.extend(at(mapping, 0.13, 0.5), mapping));
    CHECK_FALSE(stroke.extend(at(mapping, 0.14, 0.5), mapping, true));
    CHECK(brushIn(stroke.state(), id).strokes->strokes().back()->points.size() == 2);
}

TEST_CASE("Refusals are worded for the status line", "[app][brush-painting]") {
    const QString name("Brush 2");
    CHECK(describeRefusal(BrushRefusal::None, name).isEmpty());
    CHECK(describeRefusal(BrushRefusal::NotABrush, name).isEmpty());
    CHECK(describeRefusal(BrushRefusal::MasksFull, name) ==
          QString("A photograph holds at most 16 masks."));
    for (const BrushRefusal refusal : {BrushRefusal::Strokes, BrushRefusal::Points,
                                       BrushRefusal::SweptArea, BrushRefusal::Dabs}) {
        CHECK(describeRefusal(refusal, name) == QString("Brush 2 is full. Paint on a new brush."));
        CHECK(describeRefusal(refusal, name, true) ==
              QString("Brush 2 is full: the stroke stops here."));
    }
}

TEST_CASE("The brush settings round-trip through QSettings and clamp what is stored",
          "[app][brush-painting]") {
    const TempDir directory;
    const QString path = QString::fromStdU16String(directory.file("brush.ini").u16string());
    {
        QSettings store(path, QSettings::IniFormat);
        CHECK(restoreBrushSettings(store) == BrushSettings{});
        saveBrushSettings(BrushSettings{0.123F, 0.25F, 0.4F, true}, store);
        store.sync();
    }
    {
        QSettings store(path, QSettings::IniFormat);
        const BrushSettings restored = restoreBrushSettings(store);
        CHECK(restored.radius == Approx(0.123F));
        CHECK(restored.hardness == Approx(0.25F));
        CHECK(restored.flow == Approx(0.4F));
        CHECK_FALSE(restored.erase);
    }
    QSettings store(path, QSettings::IniFormat);
    store.setValue("brush/radius", 7.0);
    store.setValue("brush/hardness", -3.0);
    store.setValue("brush/flow", 0.0);
    BrushSettings clamped = restoreBrushSettings(store);
    CHECK(clamped.radius == maximumBrushRadius);
    CHECK(clamped.hardness == 0.0F);
    CHECK(clamped.flow == minimumBrushFlow);
    store.setValue("brush/radius", 0.0);
    store.setValue("brush/flow", 5.0);
    clamped = restoreBrushSettings(store);
    CHECK(clamped.radius == minimumBrushRadius);
    CHECK(clamped.flow == 1.0F);
    store.setValue("brush/radius", "wide");
    store.setValue("brush/hardness", "nan");
    CHECK(restoreBrushSettings(store).radius == BrushSettings{}.radius);
    CHECK(restoreBrushSettings(store).hardness == BrushSettings{}.hardness);
}

TEST_CASE("Resizing and hardening clamp", "[app][brush-painting]") {
    BrushSettings settings;
    CHECK(resized(settings, 1).radius == Approx(0.02F * 1.25F));
    CHECK(resized(settings, -2).radius == Approx(0.02F / 1.5625F));
    CHECK(resized(settings, 0) == settings);
    CHECK(resized(settings, 100).radius == maximumBrushRadius);
    CHECK(resized(settings, -100).radius == minimumBrushRadius);
    CHECK(hardened(settings, 1).hardness == Approx(0.6F));
    CHECK(hardened(settings, -2).hardness == Approx(0.3F));
    CHECK(hardened(settings, 50).hardness == 1.0F);
    CHECK(hardened(settings, -50).hardness == 0.0F);
    // Only the asked setting moves.
    CHECK(hardened(settings, 1).radius == settings.radius);
    CHECK(resized(settings, 1).hardness == settings.hardness);
}

TEST_CASE("The probe stroke is one radius long", "[app][brush-painting]") {
    const Stroke probe = probeStroke(BrushSettings{0.1F, 0.2F, 0.3F, true}, {0.4F, 0.5F});
    REQUIRE(probe.points.size() == 2);
    CHECK(probe.erase);
    CHECK(probe.points[1].u - probe.points[0].u == Approx(0.1F));
    const StrokeBudget cost = budgetOf(probe);
    CHECK(cost.sweptArea == Approx(0.01));
    CHECK(cost.dabs == Approx(4.0));
}

TEST_CASE("A press is refused by the stroke and point caps through begin",
          "[app][brush-painting]") {
    const MaskViewMapping mapping = fitMapping();
    const QPointF press = at(mapping, 0.5, 0.5);
    BrushRefusal refused = BrushRefusal::None;
    SECTION("strokes") {
        const DevelopState state = stateWithBrush(std::vector<Stroke>(
            maximumStrokesPerMask, Stroke{0.01F, 0.5F, 1.0F, false, {{0.5F, 0.5F}}}));
        CHECK_FALSE(LiveStroke::begin(state, state.localAdjustments.front().id, settingsOf(0.02F),
                                      false, press, mapping, refused));
        CHECK(refused == BrushRefusal::Strokes);
    }
    SECTION("points") {
        const DevelopState state = stateWithBrush(std::vector<Stroke>(
            10, Stroke{0.0005F, 0.5F, 1.0F, false,
                       std::vector<SensorPoint>(maximumStrokePoints, SensorPoint{0.5F, 0.5F})}));
        CHECK_FALSE(LiveStroke::begin(state, state.localAdjustments.front().id, settingsOf(0.02F),
                                      false, press, mapping, refused));
        CHECK(refused == BrushRefusal::Points);
    }
}

TEST_CASE("A brush without a stroke list counts as empty", "[app][brush-painting]") {
    const DevelopState state = withLocalAdjustmentAdded(DevelopState{}, BrushMask{});
    CHECK(refusalOf(state, state.localAdjustments.front().id,
                    probeStroke(settingsOf(0.02F), {0.5F, 0.5F})) == BrushRefusal::None);
}

TEST_CASE("A stroke dragged off the picture stores clamped positions", "[app][brush-painting]") {
    const MaskViewMapping mapping = mappingFor(landscape, {}, 0.25);
    LiveStroke stroke = beginNew(mapping, at(mapping, 0.5, 0.5), settingsOf(0.3F));
    // Far outside the widget, in steps of a few pixels so that none is refused.
    for (int k = 1; k <= 40; ++k) {
        stroke.extend(QPointF(400.0 + 150.0 * k, 300.0 - 100.0 * k), mapping);
    }
    CHECK(stroke.refusal() == BrushRefusal::None);
    const Stroke& painted = *brushIn(stroke.state(), stroke.mask()).strokes->strokes()[0];
    REQUIRE(painted.points.size() > 2);
    for (const SensorPoint& point : painted.points) {
        CHECK(point.u >= minimumMaskPosition);
        CHECK(point.u <= maximumMaskPosition);
        CHECK(point.v >= minimumMaskPosition);
        CHECK(point.v <= maximumMaskPosition);
    }
}

TEST_CASE("A radius outside the core's range is clamped for the stroke", "[app][brush-painting]") {
    const MaskViewMapping mapping = fitMapping();
    LiveStroke stroke = beginNew(mapping, at(mapping, 0.5, 0.5), settingsOf(0.0001F));
    CHECK(brushIn(stroke.state(), stroke.mask()).strokes->strokes()[0]->radius ==
          minimumBrushRadius);
}

TEST_CASE("A zoom change in the middle of a stroke keeps the points on the picture",
          "[app][brush-painting]") {
    const MaskViewMapping fit = fitMapping();
    LiveStroke stroke = beginNew(fit, at(fit, 0.5, 0.5), settingsOf(0.02F));
    REQUIRE(stroke.extend(at(fit, 0.55, 0.5), fit));
    // Zoomed in on the same spot: the next pointer position maps through the new mapping.
    const MaskViewMapping zoomed = mappingFor(landscape, {}, 2.0);
    const QPointF pointer = zoomed.widgetFrom({0.6, 0.5});
    REQUIRE(stroke.extend(pointer, zoomed));
    const Stroke& painted = *brushIn(stroke.state(), stroke.mask()).strokes->strokes()[0];
    REQUIRE(painted.points.size() == 3);
    CHECK(painted.points.back().u == Approx(0.6F).margin(1e-3));
    CHECK(painted.points.back().v == Approx(0.5F).margin(1e-3));
}

TEST_CASE("A refusal at the split stops the stroke and keeps the finished state",
          "[app][brush-painting]") {
    const MaskViewMapping mapping = mappingFor(landscape, {}, 4.0);
    const auto pointer = [](int k) { return QPointF(300.0 + std::fmod(k * 3.6, 200.0), 300.0); };
    // The mask already holds all but one of its strokes, so the split would be the last.
    const DevelopState baseline = stateWithBrush(std::vector<Stroke>(
        maximumStrokesPerMask - 1, Stroke{0.002F, 0.5F, 1.0F, false, {{0.5F, 0.5F}}}));
    const LocalAdjustmentId id = baseline.localAdjustments.front().id;
    BrushRefusal refused;
    auto started =
        LiveStroke::begin(baseline, id, settingsOf(0.002F), false, pointer(0), mapping, refused);
    REQUIRE(started.has_value());
    LiveStroke& stroke = *started;
    int step = 1;
    while (!stroke.atPointCap()) {
        REQUIRE(stroke.extend(pointer(step++), mapping));
    }
    stroke.reported();
    const DevelopState reported = stroke.state();
    CHECK_FALSE(stroke.extend(pointer(step), mapping));
    CHECK(stroke.refusal() == BrushRefusal::Strokes);
    CHECK(brushIn(stroke.state(), id).strokes == brushIn(reported, id).strokes);
}

TEST_CASE("Under a lens a stroke stores the sensor point the pointer is mapped to",
          "[brush][painting][lens]") {
    // 3000x2000 is 3:2.
    const auto barrel = std::make_shared<const LensCorrection>(test::barrelCorrection(-0.10F, 1.5));
    DevelopedFrameMap map(landscape, {});
    const QSizeF frame(map.width(), map.height());
    const QSizeF view(widgetSize);
    const MaskViewMapping mapping(std::move(map),
                                  ViewTransform(frame, view, ViewTransform::fitZoom(frame, view)),
                                  1.0, barrel);
    const MaskViewMapping plain = fitMapping();

    // Near a corner, where the lens moves things the most.
    const QPointF press = at(plain, 0.92, 0.1);
    const CorrectedPosition corrected = mapping.correctedFrom(press);
    const SensorPoint point = mapping.sensorFrom(corrected);
    const SensorPoint unmapped = plain.sensorFrom(plain.correctedFrom(press));
    CHECK(std::abs(point.u - unmapped.u) > 0.005F);

    // Pointer, sensor frame and corrected frame round-trip: the inverse puts it back.
    const auto back = correctedFrom(*barrel, point);
    REQUIRE(back.has_value());
    CHECK(std::abs(back->u - corrected.u) * 3000.0 < 1e-3);
    CHECK(std::abs(back->v - corrected.v) * 2000.0 < 1e-3);

    const LiveStroke stroke = beginNew(mapping, press, settingsOf(0.03F));
    const Stroke& painted = *brushIn(stroke.state(), stroke.mask()).strokes->strokes()[0];
    REQUIRE(painted.points.size() == 1);
    CHECK(painted.points[0] == point);

    // Every point a drag adds is mapped the same way.
    LiveStroke dragged = beginNew(mapping, press, settingsOf(0.03F));
    const QPointF next = at(plain, 0.88, 0.14);
    REQUIRE(dragged.extend(next, mapping));
    const Stroke& path = *brushIn(dragged.state(), dragged.mask()).strokes->strokes()[0];
    REQUIRE(path.points.size() == 2);
    CHECK(path.points[1] == mapping.sensorFrom(mapping.correctedFrom(next)));
}
