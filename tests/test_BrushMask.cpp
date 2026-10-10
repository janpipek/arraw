#include "support/BrushGenerators.h"

#include <BrushStrokes.h>
#include <DevelopState.h>
#include <EditSession.h>
#include <LocalAdjustmentEdits.h>
#include <LocalAdjustments.h>
#include <SettingsJson.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace arraw;
using namespace arraw::test;

/// The brush as a mask: the model, the edit rules and how a change is described (ADR 044,
/// sections 1 and 6; the brush plan, sections 1.2 to 1.5).

namespace {

/// A short horizontal stroke.
Stroke someStroke(float v = 0.5F, float radius = 0.02F) {
    return straightStroke({0.2F, v}, {0.6F, v}, 5, radius, 0.5F, 1.0F);
}

/// A list of strokes.
std::shared_ptr<const StrokeList> listOf(std::vector<Stroke> strokes) {
    return std::make_shared<const StrokeList>(std::move(strokes));
}

/// A state holding one brush with the given strokes; its id is `state.localAdjustments[0].id`.
DevelopState withBrush(std::vector<Stroke> strokes = {}) {
    return withLocalAdjustmentAdded(DevelopState{}, Mask{BrushMask{listOf(std::move(strokes))}});
}

const BrushMask& brushOf(const DevelopState& state, std::size_t index = 0) {
    return std::get<BrushMask>(state.localAdjustments.at(index).shape);
}

/// A stroke that spends nothing but one point: any number of them fill a list's stroke count.
Stroke dot() {
    return Stroke{0.02F, 0.5F, 1.0F, false, {{0.5F, 0.5F}}};
}

} // namespace

TEST_CASE("A brush mask is equal by pointer, and by contents behind other pointers",
          "[brush][mask]") {
    const BrushMask a{listOf({someStroke()})};
    const BrushMask same{a.strokes};
    const BrushMask equal{listOf({someStroke()})};
    const BrushMask other{listOf({someStroke(0.6F)})};
    REQUIRE(a == same);
    REQUIRE(a == equal);
    REQUIRE(a.strokes != equal.strokes);
    REQUIRE_FALSE(a == other);
    REQUIRE(BrushMask{} == BrushMask{});
    REQUIRE(BrushMask{}.strokes == emptyStrokeList());
    REQUIRE_FALSE(a == BrushMask{});
    // Through the variant and the adjustment.
    REQUIRE(Mask{a} == Mask{equal});
    REQUIRE_FALSE(Mask{a} == Mask{LinearMask{}});
}

TEST_CASE("A copied state shares the lists and is not an edit", "[brush][mask]") {
    const DevelopState state = withBrush({someStroke(), someStroke(0.7F)});
    const DevelopState copy = state;
    REQUIRE(brushOf(copy).strokes == brushOf(state).strokes);
    REQUIRE(copy == state);
    REQUIRE(describeChange(state, copy).local.empty());
}

TEST_CASE("Adding a brush: empty, and with strokes", "[brush][mask]") {
    const DevelopState empty = withBrush();
    REQUIRE(empty.localAdjustments.size() == 1);
    REQUIRE(brushOf(empty).strokes->empty());
    REQUIRE_NOTHROW(validate(empty));

    const DevelopState painted = withBrush({someStroke()});
    REQUIRE(brushOf(painted).strokes->size() == 1);
    REQUIRE_NOTHROW(validate(painted));

    // The adjustment form, with a null list: normalised to the shared empty list.
    LocalAdjustment adjustment;
    adjustment.shape = BrushMask{nullptr};
    const DevelopState fromNull = withLocalAdjustmentAdded(DevelopState{}, adjustment);
    REQUIRE(brushOf(fromNull).strokes == emptyStrokeList());
}

TEST_CASE("The seventeenth adjustment is refused, whatever its kind", "[brush][mask]") {
    DevelopState state;
    for (std::size_t i = 0; i < maximumLocalAdjustments; ++i) {
        state = withLocalAdjustmentAdded(std::move(state), Mask{BrushMask{}});
    }
    REQUIRE_FALSE(canAddLocalAdjustment(state));
    const DevelopState before = state;
    REQUIRE_THROWS_AS(withLocalAdjustmentAdded(state, Mask{BrushMask{}}), std::invalid_argument);
    REQUIRE(state == before);
}

TEST_CASE("withStrokeAppended appends a normalised stroke and shares the earlier ones",
          "[brush][edit]") {
    const DevelopState state = withBrush({someStroke()});
    const LocalAdjustmentId id = state.localAdjustments[0].id;
    Stroke wild = someStroke(0.5F, 9.0F);
    wild.hardness = 3.0F;
    wild.flow = -1.0F;
    wild.points.push_back({100.0F, -100.0F});
    REQUIRE(canAppendStroke(state, id, wild));
    const DevelopState next = withStrokeAppended(state, id, wild);
    const StrokeList& list = *brushOf(next).strokes;
    REQUIRE(list.size() == 2);
    REQUIRE(list[1].radius == maximumBrushRadius);
    REQUIRE(list[1].hardness == 1.0F);
    REQUIRE(list[1].flow == 0.0F);
    REQUIRE(list[1].points.back().u == maximumMaskPosition);
    REQUIRE(list[1].points.back().v == minimumMaskPosition);
    // The first stroke is the same object, not a copy.
    REQUIRE(list.strokes()[0] == brushOf(state).strokes->strokes()[0]);
    // Appending to an empty brush works too.
    REQUIRE(brushOf(withStrokeAppended(withBrush(), withBrush().localAdjustments[0].id, dot()))
                .strokes->size() == 1);
}

TEST_CASE("withStrokeAppended refuses what cannot be appended and leaves the state alone",
          "[brush][edit]") {
    const DevelopState linear = withLocalAdjustmentAdded(DevelopState{}, Mask{LinearMask{}});
    const DevelopState state = withBrush({someStroke()});
    const LocalAdjustmentId id = state.localAdjustments[0].id;
    const DevelopState before = state;

    const auto refused = [&](const DevelopState& in, LocalAdjustmentId which, const Stroke& s) {
        INFO("canAppendStroke must agree with the throw");
        REQUIRE_FALSE(canAppendStroke(in, which, s));
        REQUIRE_THROWS_AS(withStrokeAppended(in, which, s), std::invalid_argument);
    };
    SECTION("an unknown id") {
        refused(state, LocalAdjustmentId{99}, someStroke());
    }
    SECTION("a mask that is not a brush") {
        refused(linear, linear.localAdjustments[0].id, someStroke());
    }
    SECTION("a stroke that is not finite") {
        Stroke bad = someStroke();
        bad.radius = std::numeric_limits<float>::quiet_NaN();
        refused(state, id, bad);
        bad = someStroke();
        bad.points[1].u = std::numeric_limits<float>::infinity();
        refused(state, id, bad);
    }
    SECTION("a stroke without points") {
        Stroke bad = someStroke();
        bad.points.clear();
        refused(state, id, bad);
    }
    SECTION("a stroke with more points than the cap") {
        Stroke bad = someStroke();
        bad.points.assign(maximumStrokePoints + 1, SensorPoint{0.5F, 0.5F});
        refused(state, id, bad);
    }
    SECTION("a stroke that alone passes the swept area") {
        const Stroke bad = straightStroke({-2.0F, 0.0F}, {3.0F, 3.0F}, 2, 1.0F, 1.0F, 1.0F);
        refused(state, id, bad);
    }
    REQUIRE(state == before);
}

TEST_CASE("A mask refuses a stroke past each of its caps and budgets", "[brush][edit]") {
    SECTION("the stroke count") {
        const DevelopState full = withBrush(std::vector<Stroke>(maximumStrokesPerMask, dot()));
        const LocalAdjustmentId id = full.localAdjustments[0].id;
        REQUIRE_FALSE(canAppendStroke(full, id, dot()));
        REQUIRE_THROWS_AS(withStrokeAppended(full, id, dot()), std::invalid_argument);
        REQUIRE(brushOf(full).strokes->size() == maximumStrokesPerMask);
    }
    SECTION("the points of the mask") {
        Stroke wide = dot();
        wide.points.assign(maximumStrokePoints, SensorPoint{0.5F, 0.5F});
        const DevelopState state =
            withBrush(std::vector<Stroke>(maximumPointsPerMask / maximumStrokePoints, wide));
        const LocalAdjustmentId id = state.localAdjustments[0].id;
        REQUIRE_FALSE(canAppendStroke(state, id, dot()));
        REQUIRE_THROWS_AS(withStrokeAppended(state, id, dot()), std::invalid_argument);
    }
    SECTION("the swept area") {
        const Stroke sweep = straightStroke({0.0F, 0.0F}, {3.0F, 0.0F}, 2, 1.0F, 1.0F, 1.0F);
        const DevelopState state = withBrush({sweep});
        const LocalAdjustmentId id = state.localAdjustments[0].id;
        REQUIRE_FALSE(canAppendStroke(state, id, sweep));
        REQUIRE_THROWS_AS(withStrokeAppended(state, id, sweep), std::invalid_argument);
        // A stroke that fits in what is left is accepted.
        const Stroke small = straightStroke({0.0F, 0.0F}, {0.5F, 0.0F}, 2, 1.0F, 1.0F, 1.0F);
        REQUIRE(canAppendStroke(state, id, small));
    }
    SECTION("the dabs") {
        const Stroke fine =
            straightStroke({-2.0F, 0.0F}, {3.0F, 0.0F}, 2, minimumBrushRadius, 1.0F, 1.0F);
        const DevelopState state = withBrush(std::vector<Stroke>(50, fine));
        const LocalAdjustmentId id = state.localAdjustments[0].id;
        REQUIRE_FALSE(canAppendStroke(state, id, fine));
        REQUIRE_THROWS_AS(withStrokeAppended(state, id, fine), std::invalid_argument);
        REQUIRE(brushOf(state).strokes->size() == 50);
    }
}

TEST_CASE("Duplicating a brush shares its list", "[brush][edit]") {
    const DevelopState state = withBrush({someStroke()});
    const DevelopState twice = withLocalAdjustmentDuplicated(state, state.localAdjustments[0].id);
    REQUIRE(twice.localAdjustments.size() == 2);
    REQUIRE(brushOf(twice, 0).strokes == brushOf(twice, 1).strokes);
    REQUIRE(twice.localAdjustments[0].id != twice.localAdjustments[1].id);
}

TEST_CASE("withLocalShape replaces the strokes whole, and only of a brush", "[brush][edit]") {
    const DevelopState state = withBrush({someStroke()});
    const LocalAdjustmentId id = state.localAdjustments[0].id;
    const auto replacement = listOf({someStroke(0.1F), someStroke(0.9F)});
    const DevelopState next = withLocalShape(state, id, BrushMask{replacement});
    REQUIRE(brushOf(next).strokes == replacement);
    REQUIRE_THROWS_AS(withLocalShape(state, id, LinearMask{}), std::invalid_argument);
    const DevelopState linear = withLocalAdjustmentAdded(DevelopState{}, Mask{LinearMask{}});
    REQUIRE_THROWS_AS(withLocalShape(linear, linear.localAdjustments[0].id, BrushMask{}),
                      std::invalid_argument);
}

TEST_CASE("A brush with no list or another rasteriser is not valid", "[brush][mask]") {
    DevelopState state = withBrush({someStroke()});
    REQUIRE_NOTHROW(validate(state));

    DevelopState nulled = state;
    nulled.localAdjustments[0].shape = BrushMask{nullptr};
    REQUIRE_THROWS_AS(validate(nulled), std::invalid_argument);

    DevelopState old = state;
    old.localAdjustments[0].shape = BrushMask{std::make_shared<const StrokeList>(
        std::vector<Stroke>{someStroke()}, brushRasteriserVersion + 1)};
    REQUIRE_THROWS_AS(validate(old), std::invalid_argument);
    REQUIRE_THROWS_AS(normalised(old.localAdjustments[0].shape), std::invalid_argument);
}

TEST_CASE("Brushes are named and counted by kind", "[brush][mask]") {
    DevelopState state;
    state = withLocalAdjustmentAdded(std::move(state), Mask{LinearMask{}});
    state = withLocalAdjustmentAdded(std::move(state), Mask{BrushMask{}});
    state = withLocalAdjustmentAdded(std::move(state), Mask{RadialMask{}});
    state = withLocalAdjustmentAdded(std::move(state), Mask{BrushMask{}});
    const auto& list = state.localAdjustments;
    REQUIRE(maskTypeName(list[1].shape) == "brush");
    REQUIRE(maskTypeName(list[0].shape) == "linear");
    REQUIRE(maskTypeName(list[2].shape) == "radial");
    REQUIRE(maskOrdinal(state, list[1].id) == 1);
    REQUIRE(maskOrdinal(state, list[3].id) == 2);
    REQUIRE(defaultMaskName(state, list[1].id) == "Brush 1");
    REQUIRE(defaultMaskName(state, list[3].id) == "Brush 2");
    REQUIRE(defaultMaskName(state, list[2].id) == "Radial 1");
    REQUIRE(displayedMaskName(state, list[3].id) == "Brush 2");
    state = withLocalAdjustmentRenamed(std::move(state), list[3].id, "Sky");
    REQUIRE(displayedMaskName(state, list[3].id) == "Sky");
}

TEST_CASE("describeChange tells the strokes from the shape", "[brush][edit]") {
    const DevelopState state = withBrush({someStroke()});
    const LocalAdjustmentId id = state.localAdjustments[0].id;

    const DevelopState painted = withStrokeAppended(state, id, someStroke(0.7F));
    const ChangeDescription change = describeChange(state, painted);
    REQUIRE(change.local.changed.size() == 1);
    REQUIRE(change.local.changed[0].strokes);
    REQUIRE_FALSE(change.local.changed[0].shape);
    REQUIRE(change.local.changed[0].deltas.empty());
    REQUIRE(change.keys.empty());

    // A replaced list with equal contents is no change at all.
    const DevelopState same = withLocalShape(state, id, BrushMask{listOf({someStroke()})});
    REQUIRE(describeChange(state, same).local.empty());

    // A delta alone leaves the strokes flag clear.
    const DevelopState louder = withLocalDelta(state, id, "exposure", 1.0);
    REQUIRE_FALSE(describeChange(state, louder).local.changed[0].strokes);

    // Another kind of shape is a shape change, not a strokes change.
    DevelopState swapped = state;
    swapped.localAdjustments[0].shape = LinearMask{};
    const MaskChange kind = describeChange(state, swapped).local.changed[0];
    REQUIRE(kind.shape);
    REQUIRE_FALSE(kind.strokes);
}

TEST_CASE("Brush masks are not persisted yet", "[brush][mask]") {
    const DevelopState state = withBrush({someStroke()});
    REQUIRE_THROWS_AS(stateToJson(state), std::logic_error);
    REQUIRE_THROWS_AS(localAdjustmentsToJson(state), std::logic_error);
}
