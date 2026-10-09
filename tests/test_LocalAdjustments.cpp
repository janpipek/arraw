// The local adjustments of a state (ADR 044): the model, its validation, the edit rules, the
// description of a change, and the promise that a look never carries them.

#include "support/LocalAdjustmentStates.h"

#include <DevelopState.h>
#include <EditSession.h>
#include <Edits.h>
#include <LocalAdjustmentEdits.h>
#include <Photo.h>
#include <SettingDescriptors.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace arraw;

namespace {

constexpr float notANumber = std::numeric_limits<float>::quiet_NaN();
constexpr float unbounded = std::numeric_limits<float>::infinity();

const ImageMetadata photo{ImageSize{32, 24}, workingEncoding};

/// A state holding sixteen masks.
DevelopState full() {
    DevelopState state;
    for (std::size_t i = 0; i < maximumLocalAdjustments; ++i) {
        state =
            withLocalAdjustmentAdded(state, i % 2 == 0 ? Mask{LinearMask{}} : Mask{RadialMask{}});
    }
    return state;
}

LocalAdjustmentId idAt(const DevelopState& state, std::size_t index) {
    return state.localAdjustments.at(index).id;
}

std::vector<std::uint32_t> idsOf(const DevelopState& state) {
    std::vector<std::uint32_t> ids;
    for (const LocalAdjustment& adjustment : state.localAdjustments) {
        ids.push_back(adjustment.id.value);
    }
    return ids;
}

/// Checks that an edit throws std::invalid_argument and that the state it was given is untouched.
template <class F> void checkRefused(const DevelopState& state, F&& edit) {
    DevelopState copy = state;
    CHECK_THROWS_AS(edit(copy), std::invalid_argument);
    CHECK(copy == state);
}

} // namespace

// The model

TEST_CASE("A new state holds no local adjustments and a counter at 1", "[local][state]") {
    const DevelopState state;
    CHECK(state.localAdjustments.empty());
    CHECK(state.nextLocalAdjustmentId == LocalAdjustmentId{1});
    CHECK_NOTHROW(validate(state));
    CHECK(canAddLocalAdjustment(state));
}

TEST_CASE("The local table has a row for every delta, with the keys the documents spell",
          "[local][descriptors]") {
    CHECK(localAdjustmentDescriptors.size() == 13);
    std::set<std::string_view> keys;
    std::set<std::string_view> pythonNames;
    std::vector<float LocalDeltas::*> members;
    for (const LocalDescriptor& row : localAdjustmentDescriptors) {
        INFO(row.key);
        keys.insert(row.key);
        pythonNames.insert(row.pythonName);
        CHECK(std::ranges::count(members, row.member) == 0);
        members.push_back(row.member);
        // Every global control the row names exists, and the local range sits inside its range.
        if (!row.globalKey.empty()) {
            const FieldDescriptor* global = findDescriptor(row.globalKey);
            REQUIRE(global != nullptr);
            REQUIRE(global->range);
            CHECK(row.range.minimum >= global->range->minimum);
            CHECK(row.range.maximum <= global->range->maximum);
            CHECK(row.key == global->key);
        }
        CHECK(row.range.minimum == -row.range.maximum);
        CHECK(findLocalDescriptor(row.key) == &row);
    }
    CHECK(keys.size() == 13);
    CHECK(pythonNames.size() == 13);
    CHECK(localAdjustmentDescriptors[0].key == "relativeTemperature");
    CHECK(localAdjustmentDescriptors[0].pythonName == "relative_temperature");
    CHECK(localAdjustmentDescriptors[0].globalKey.empty());
    CHECK(localAdjustmentDescriptors[2].key == "exposure");
    CHECK(localAdjustmentDescriptors[2].range.maximum == 4.0);
    CHECK(findLocalDescriptor("filmicHighlights") == nullptr);
}

TEST_CASE("The local table says where in the chain each control acts", "[local][descriptors]") {
    const auto positionOf = [](std::string_view key) { return findLocalDescriptor(key)->position; };
    CHECK(positionOf("relativeTemperature") == LocalChainPosition::AfterToWorking);
    CHECK(positionOf("relativeTint") == LocalChainPosition::AfterToWorking);
    CHECK(positionOf("exposure") == LocalChainPosition::ExposureGain);
    for (const char* key : {"contrast", "highlights", "shadows", "whites", "blacks"}) {
        CHECK(positionOf(key) == LocalChainPosition::ShapeTone);
    }
    for (const char* key : {"texture", "clarity", "dehaze"}) {
        CHECK(positionOf(key) == LocalChainPosition::ApplyPresence);
    }
    for (const char* key : {"saturation", "vibrance"}) {
        CHECK(positionOf(key) == LocalChainPosition::AdjustColor);
        CHECK_FALSE(findLocalDescriptor(key)->beforeCurveTap());
    }
    CHECK(findLocalDescriptor("dehaze")->beforeCurveTap());
}

TEST_CASE("Masks compare by what they hold", "[local][state]") {
    const DevelopState state = test::stateWithMasks();
    DevelopState copy = state;
    CHECK(copy == state);
    copy.localAdjustments[0].deltas.contrast = 1.0F;
    CHECK(copy != state);
    copy = state;
    copy.nextLocalAdjustmentId.value += 1;
    CHECK(copy != state);
    copy = state;
    std::get<LinearMask>(copy.localAdjustments[0].shape).to.u += 0.01F;
    CHECK(copy != state);
}

TEST_CASE("Masks have names by kind and place", "[local][names]") {
    DevelopState state;
    state = withLocalAdjustmentAdded(state, LinearMask{});
    state = withLocalAdjustmentAdded(state, RadialMask{});
    state = withLocalAdjustmentAdded(state, LinearMask{});
    CHECK(defaultMaskName(state, idAt(state, 0)) == "Linear 1");
    CHECK(defaultMaskName(state, idAt(state, 1)) == "Radial 1");
    CHECK(defaultMaskName(state, idAt(state, 2)) == "Linear 2");
    CHECK(maskOrdinal(state, idAt(state, 2)) == 2);
    state = withLocalAdjustmentRenamed(state, idAt(state, 2), "Sky");
    CHECK(displayedMaskName(state, idAt(state, 2)) == "Sky");
    CHECK(defaultMaskName(state, idAt(state, 2)) == "Linear 2");
    // Removing the first linear mask moves the second into its place.
    state = withLocalAdjustmentRemoved(state, idAt(state, 0));
    CHECK(defaultMaskName(state, idAt(state, 1)) == "Linear 1");
    CHECK_THROWS_AS(defaultMaskName(state, LocalAdjustmentId{99}), std::invalid_argument);
}

// Adding, duplicating, removing

TEST_CASE("Adding a mask gives it the counter's id and moves the counter on", "[local][edits]") {
    DevelopState state = withLocalAdjustmentAdded(DevelopState{}, test::someLinear());
    REQUIRE(state.localAdjustments.size() == 1);
    CHECK(state.localAdjustments[0].id == LocalAdjustmentId{1});
    CHECK(state.nextLocalAdjustmentId == LocalAdjustmentId{2});
    CHECK(state.localAdjustments[0].enabled);
    CHECK(state.localAdjustments[0].opacity == 1.0F);
    CHECK_FALSE(state.localAdjustments[0].invert);
    CHECK(state.localAdjustments[0].deltas.isZero());
    CHECK(std::get<LinearMask>(state.localAdjustments[0].shape) == test::someLinear());

    state = withLocalAdjustmentAdded(state, test::someRadial());
    CHECK(state.localAdjustments[1].id == LocalAdjustmentId{2});
    CHECK(state.nextLocalAdjustmentId == LocalAdjustmentId{3});
    CHECK_NOTHROW(validate(state));
}

TEST_CASE("Adding a whole adjustment ignores the id it came with and normalises the rest",
          "[local][edits]") {
    LocalAdjustment adjustment;
    adjustment.id = LocalAdjustmentId{77};
    adjustment.name = "Faces";
    adjustment.opacity = 3.0F;
    adjustment.deltas.exposure = 9.0F;
    adjustment.deltas.dehaze = -500.0F;
    adjustment.shape = RadialMask{.centre = {10.0F, -9.0F}, .radiusX = 8.0F, .angle = 200.0F};
    const DevelopState state = withLocalAdjustmentAdded(DevelopState{}, adjustment);
    const LocalAdjustment& added = state.localAdjustments.at(0);
    CHECK(added.id == LocalAdjustmentId{1});
    CHECK(added.name == "Faces");
    CHECK(added.opacity == 1.0F);
    CHECK(added.deltas.exposure == 4.0F);
    CHECK(added.deltas.dehaze == -100.0F);
    const auto& radial = std::get<RadialMask>(added.shape);
    CHECK(radial.centre.u == maximumMaskPosition);
    CHECK(radial.centre.v == minimumMaskPosition);
    CHECK(radial.radiusX == maximumMaskRadius);
    CHECK(radial.angle == -160.0F);
    CHECK_NOTHROW(validate(state));
}

TEST_CASE("A seventeenth add or duplicate is refused and leaves the state equal",
          "[local][edits]") {
    DevelopState state = full();
    REQUIRE(state.localAdjustments.size() == 16);
    CHECK_FALSE(canAddLocalAdjustment(state));
    checkRefused(state, [](DevelopState& s) { s = withLocalAdjustmentAdded(s, LinearMask{}); });
    checkRefused(state,
                 [](DevelopState& s) { s = withLocalAdjustmentAdded(s, LocalAdjustment{}); });
    checkRefused(state, [](DevelopState& s) {
        s = withLocalAdjustmentDuplicated(s, s.localAdjustments[3].id);
    });
    // Removing one makes room again.
    state = withLocalAdjustmentRemoved(state, idAt(state, 5));
    CHECK(canAddLocalAdjustment(state));
    CHECK_NOTHROW(state = withLocalAdjustmentAdded(state, RadialMask{}));
    CHECK(state.localAdjustments.size() == 16);
}

TEST_CASE("An id that is not in the list is refused by every edit", "[local][edits]") {
    const DevelopState state = test::stateWithMasks();
    const LocalAdjustmentId unknown{999};
    checkRefused(state, [&](DevelopState& s) { s = withLocalAdjustmentDuplicated(s, unknown); });
    checkRefused(state, [&](DevelopState& s) { s = withLocalAdjustmentRemoved(s, unknown); });
    checkRefused(state, [&](DevelopState& s) { s = withLocalAdjustmentReordered(s, unknown, 0); });
    checkRefused(state, [&](DevelopState& s) { s = withLocalAdjustmentRenamed(s, unknown, "x"); });
    checkRefused(state,
                 [&](DevelopState& s) { s = withLocalAdjustmentEnabled(s, unknown, false); });
    checkRefused(state,
                 [&](DevelopState& s) { s = withLocalAdjustmentInverted(s, unknown, true); });
    checkRefused(state, [&](DevelopState& s) { s = withLocalOpacity(s, unknown, 0.5F); });
    checkRefused(state, [&](DevelopState& s) { s = withLocalDelta(s, unknown, "exposure", 1.0); });
    checkRefused(state, [&](DevelopState& s) { s = withLocalShape(s, unknown, LinearMask{}); });
    // Zero names none either.
    checkRefused(state, [](DevelopState& s) { s = withLocalAdjustmentRemoved(s, {}); });
    CHECK(findLocalAdjustment(state, unknown) == nullptr);
    CHECK(findLocalAdjustment(state, idAt(state, 1)) == &state.localAdjustments[1]);
}

TEST_CASE("A duplicate gets a new id and sits after the original", "[local][edits]") {
    const DevelopState before = test::stateWithMasks();
    const DevelopState after = withLocalAdjustmentDuplicated(before, idAt(before, 1));
    REQUIRE(after.localAdjustments.size() == 4);
    CHECK(idsOf(before) == std::vector<std::uint32_t>{1, 2, 3});
    CHECK(idsOf(after) == std::vector<std::uint32_t>{1, 2, 4, 3});
    CHECK(after.nextLocalAdjustmentId == LocalAdjustmentId{5});
    LocalAdjustment copy = after.localAdjustments[2];
    copy.id = idAt(before, 1);
    CHECK(copy == before.localAdjustments[1]);
    CHECK_NOTHROW(validate(after));
}

TEST_CASE("Removing a mask never lowers the counter", "[local][edits]") {
    DevelopState state = test::stateWithMasks();
    REQUIRE(state.nextLocalAdjustmentId == LocalAdjustmentId{4});
    state = withLocalAdjustmentRemoved(state, idAt(state, 2));
    CHECK(idsOf(state) == std::vector<std::uint32_t>{1, 2});
    CHECK(state.nextLocalAdjustmentId == LocalAdjustmentId{4});
    state = withLocalAdjustmentAdded(state, LinearMask{});
    CHECK(idAt(state, 2) == LocalAdjustmentId{4});
    // Adding then removing leaves a state that differs from the one before: the counter moved.
    const DevelopState empty;
    const DevelopState back = withLocalAdjustmentRemoved(
        withLocalAdjustmentAdded(empty, LinearMask{}), LocalAdjustmentId{1});
    CHECK(back.localAdjustments.empty());
    CHECK(back != empty);
    CHECK(back.nextLocalAdjustmentId == LocalAdjustmentId{2});
}

TEST_CASE("The ids run out at the largest the counter holds", "[local][edits]") {
    DevelopState state;
    state.nextLocalAdjustmentId.value = std::numeric_limits<std::uint32_t>::max();
    CHECK_FALSE(canAddLocalAdjustment(state));
    checkRefused(state, [](DevelopState& s) { s = withLocalAdjustmentAdded(s, LinearMask{}); });
    state.nextLocalAdjustmentId.value = std::numeric_limits<std::uint32_t>::max() - 1;
    CHECK_NOTHROW(state = withLocalAdjustmentAdded(state, LinearMask{}));
    CHECK_NOTHROW(validate(state));
}

// Reordering, naming, flags, opacity, deltas

TEST_CASE("Reordering moves one mask and keeps the others in order", "[local][edits]") {
    const DevelopState state = full();
    const auto ids = idsOf(state);
    const auto moved = [&](std::size_t from, std::size_t to) {
        return idsOf(withLocalAdjustmentReordered(state, idAt(state, from), to));
    };

    auto expected = ids;
    std::rotate(expected.begin(), expected.begin() + 5, expected.begin() + 6);
    CHECK(moved(5, 0) == expected);

    expected = ids;
    std::rotate(expected.begin() + 2, expected.begin() + 3, expected.end());
    CHECK(moved(2, 15) == expected);

    expected = ids;
    std::rotate(expected.begin() + 4, expected.begin() + 5, expected.begin() + 9);
    CHECK(moved(4, 8) == expected);

    CHECK(moved(7, 7) == ids);
    CHECK(withLocalAdjustmentReordered(state, idAt(state, 0), 3).nextLocalAdjustmentId ==
          state.nextLocalAdjustmentId);
    checkRefused(state,
                 [](DevelopState& s) { s = withLocalAdjustmentReordered(s, idAt(s, 0), 16); });
}

TEST_CASE("Renaming sets the name, and an empty one returns to the default", "[local][edits]") {
    DevelopState state = withLocalAdjustmentAdded(DevelopState{}, LinearMask{});
    state = withLocalAdjustmentRenamed(state, idAt(state, 0), "Horizon");
    CHECK(state.localAdjustments[0].name == "Horizon");
    state = withLocalAdjustmentRenamed(state, idAt(state, 0), "");
    CHECK(state.localAdjustments[0].name.empty());
    CHECK(displayedMaskName(state, idAt(state, 0)) == "Linear 1");
    // Whatever can be typed that XML can carry.
    state = withLocalAdjustmentRenamed(state, idAt(state, 0), "Zon\xC3\xA9 <1> & \"2\"");
    CHECK(state.localAdjustments[0].name == "Zon\xC3\xA9 <1> & \"2\"");
    checkRefused(state, [](DevelopState& s) {
        s = withLocalAdjustmentRenamed(s, idAt(s, 0), std::string("two\nlines"));
    });
    checkRefused(state, [](DevelopState& s) {
        s = withLocalAdjustmentRenamed(s, idAt(s, 0), std::string("nul\0inside", 10));
    });
}

TEST_CASE("The flags and the opacity are set, and the opacity is kept in range", "[local][edits]") {
    DevelopState state = withLocalAdjustmentAdded(DevelopState{}, LinearMask{});
    const LocalAdjustmentId id = idAt(state, 0);
    state = withLocalAdjustmentEnabled(state, id, false);
    state = withLocalAdjustmentInverted(state, id, true);
    CHECK_FALSE(state.localAdjustments[0].enabled);
    CHECK(state.localAdjustments[0].invert);
    state = withLocalAdjustmentEnabled(state, id, true);
    state = withLocalAdjustmentInverted(state, id, false);
    CHECK(state.localAdjustments[0].enabled);
    CHECK_FALSE(state.localAdjustments[0].invert);

    CHECK(withLocalOpacity(state, id, 0.25F).localAdjustments[0].opacity == 0.25F);
    CHECK(withLocalOpacity(state, id, 4.0F).localAdjustments[0].opacity == 1.0F);
    CHECK(withLocalOpacity(state, id, -1.0F).localAdjustments[0].opacity == 0.0F);
    checkRefused(state, [&](DevelopState& s) { s = withLocalOpacity(s, id, notANumber); });
    checkRefused(state, [&](DevelopState& s) { s = withLocalOpacity(s, id, unbounded); });
}

TEST_CASE("A delta is set by its key and clamped to its local range", "[local][edits]") {
    DevelopState state = withLocalAdjustmentAdded(DevelopState{}, LinearMask{});
    const LocalAdjustmentId id = idAt(state, 0);
    for (const LocalDescriptor& row : localAdjustmentDescriptors) {
        INFO(row.key);
        const DevelopState set = withLocalDelta(state, id, row.key, 1.5);
        CHECK(set.localAdjustments[0].deltas.*row.member == 1.5F);
        // Nothing else moved.
        LocalDeltas expected;
        expected.*row.member = 1.5F;
        CHECK(set.localAdjustments[0].deltas == expected);
        CHECK(withLocalDelta(state, id, row.key, 1e6).localAdjustments[0].deltas.*row.member ==
              static_cast<float>(row.range.maximum));
        CHECK(withLocalDelta(state, id, row.key, -1e6).localAdjustments[0].deltas.*row.member ==
              static_cast<float>(row.range.minimum));
    }
    checkRefused(state, [&](DevelopState& s) { s = withLocalDelta(s, id, "bogus", 1.0); });
    checkRefused(state,
                 [&](DevelopState& s) { s = withLocalDelta(s, id, "filmicHighlights", 1.0); });
    checkRefused(state, [&](DevelopState& s) {
        s = withLocalDelta(s, id, "exposure", std::numeric_limits<double>::quiet_NaN());
    });
    checkRefused(state, [&](DevelopState& s) {
        s = withLocalDelta(s, id, "exposure", std::numeric_limits<double>::infinity());
    });
}

// Moving handles

TEST_CASE("Moving the handles replaces the geometry of the same kind", "[local][edits]") {
    DevelopState state = test::stateWithMasks();
    const LocalAdjustmentId linear = idAt(state, 0);
    const LocalAdjustmentId radial = idAt(state, 1);

    const LinearMask newLinear{.from = {0.1F, 0.1F}, .to = {0.9F, 0.2F}};
    state = withLocalShape(state, linear, newLinear);
    CHECK(std::get<LinearMask>(state.localAdjustments[0].shape) == newLinear);
    // Handles may lie outside the frame, up to the limit.
    state = withLocalShape(state, linear, LinearMask{.from = {-1.5F, 2.5F}, .to = {2.0F, -1.0F}});
    CHECK(std::get<LinearMask>(state.localAdjustments[0].shape).from.u == -1.5F);

    const RadialMask newRadial{
        .centre = {0.7F, 0.2F}, .radiusX = 0.5F, .radiusY = 0.1F, .angle = -45.0F, .feather = 0.0F};
    state = withLocalShape(state, radial, newRadial);
    CHECK(std::get<RadialMask>(state.localAdjustments[1].shape) == newRadial);
    // The rest of the mask stays.
    CHECK(state.localAdjustments[1].invert);
    CHECK(state.localAdjustments[1].name == "Sky \"left\" & <more>");

    checkRefused(state, [&](DevelopState& s) { s = withLocalShape(s, linear, RadialMask{}); });
    checkRefused(state, [&](DevelopState& s) { s = withLocalShape(s, radial, LinearMask{}); });
}

TEST_CASE("Handles are clamped, angles wrapped, and degenerate geometry refused",
          "[local][edits]") {
    DevelopState state = test::stateWithMasks();
    const LocalAdjustmentId linear = idAt(state, 0);
    const LocalAdjustmentId radial = idAt(state, 1);

    const auto radialShape = [&](RadialMask mask) {
        return std::get<RadialMask>(withLocalShape(state, radial, mask).localAdjustments[1].shape);
    };
    CHECK(radialShape({.centre = {99.0F, -99.0F}}).centre == CorrectedPoint{3.0F, -2.0F});
    CHECK(radialShape({.radiusX = 99.0F, .radiusY = 99.0F}).radiusX == maximumMaskRadius);
    CHECK(radialShape({.feather = 3.0F}).feather == 1.0F);
    CHECK(radialShape({.feather = -3.0F}).feather == 0.0F);
    CHECK(radialShape({.angle = 180.0F}).angle == -180.0F);
    CHECK(radialShape({.angle = -180.0F}).angle == -180.0F);
    CHECK(radialShape({.angle = 190.0F}).angle == -170.0F);
    CHECK(radialShape({.angle = -190.0F}).angle == 170.0F);
    CHECK(radialShape({.angle = 725.0F}).angle == 5.0F);
    CHECK(wrappedAngle(179.0F) == 179.0F);

    // Non-finite numbers are refused, whichever field holds them.
    for (const float bad : {notANumber, unbounded, -unbounded}) {
        checkRefused(state, [&](DevelopState& s) {
            s = withLocalShape(s, radial, RadialMask{.centre = {bad, 0.5F}});
        });
        checkRefused(state, [&](DevelopState& s) {
            s = withLocalShape(s, radial, RadialMask{.radiusY = bad});
        });
        checkRefused(state, [&](DevelopState& s) {
            s = withLocalShape(s, radial, RadialMask{.angle = bad});
        });
        checkRefused(state, [&](DevelopState& s) {
            s = withLocalShape(s, radial, RadialMask{.feather = bad});
        });
        checkRefused(state, [&](DevelopState& s) {
            s = withLocalShape(s, linear, LinearMask{.from = {0.1F, bad}});
        });
        checkRefused(state, [&](DevelopState& s) {
            s = withLocalShape(s, linear, LinearMask{.to = {bad, 0.1F}});
        });
    }
    // A radius below the minimum, and ends that are (nearly) one point.
    checkRefused(state, [&](DevelopState& s) {
        s = withLocalShape(s, radial, RadialMask{.radiusX = 0.0F});
    });
    checkRefused(state, [&](DevelopState& s) {
        s = withLocalShape(s, radial, RadialMask{.radiusY = 0.0005F});
    });
    checkRefused(state, [&](DevelopState& s) {
        s = withLocalShape(s, linear, LinearMask{.from = {0.5F, 0.5F}, .to = {0.5F, 0.5F}});
    });
    checkRefused(state, [&](DevelopState& s) {
        s = withLocalShape(s, linear, LinearMask{.from = {0.5F, 0.5F}, .to = {0.5F, 0.5005F}});
    });
    // The smallest extent is allowed.
    CHECK_NOTHROW(
        withLocalShape(state, linear, LinearMask{.from = {0.5F, 0.5F}, .to = {0.5F, 0.5015F}}));
    CHECK_NOTHROW(withLocalShape(state, radial, RadialMask{.radiusX = 0.001F, .radiusY = 0.001F}));
    // Far apart before clamping, one point after: refused rather than clamped into nothing.
    checkRefused(state, [&](DevelopState& s) {
        s = withLocalShape(s, linear, LinearMask{.from = {4.0F, 0.0F}, .to = {9.0F, 0.0F}});
    });
}

// Validation of a whole state

TEST_CASE("A state with local adjustments out of order is refused by validate", "[local][state]") {
    const DevelopState good = test::stateWithMasks();
    REQUIRE_NOTHROW(validate(good));
    const auto refuses = [&](auto&& change) {
        DevelopState state = good;
        change(state);
        CHECK_THROWS_AS(validate(state), std::invalid_argument);
        CHECK_THROWS_AS(Photo("p.dng", photo, state), std::invalid_argument);
    };

    SECTION("a counter that is zero, or not above every id") {
        refuses([](DevelopState& s) { s.nextLocalAdjustmentId = {}; });
        refuses([](DevelopState& s) { s.nextLocalAdjustmentId = LocalAdjustmentId{3}; });
    }
    SECTION("ids") {
        refuses([](DevelopState& s) { s.localAdjustments[0].id = {}; });
        refuses([](DevelopState& s) { s.localAdjustments[1].id = s.localAdjustments[0].id; });
        refuses([](DevelopState& s) { s.localAdjustments[2].id = LocalAdjustmentId{4}; });
    }
    SECTION("more than sixteen") {
        refuses([](DevelopState& s) {
            for (std::uint32_t id = 10; id < 24; ++id) {
                LocalAdjustment extra;
                extra.id = LocalAdjustmentId{id};
                s.localAdjustments.push_back(extra);
            }
            s.nextLocalAdjustmentId = LocalAdjustmentId{99};
        });
    }
    SECTION("numbers that are not finite") {
        refuses([](DevelopState& s) { s.localAdjustments[0].opacity = notANumber; });
        refuses([](DevelopState& s) { s.localAdjustments[0].deltas.contrast = unbounded; });
        refuses([](DevelopState& s) {
            std::get<LinearMask>(s.localAdjustments[0].shape).from.u = notANumber;
        });
        refuses([](DevelopState& s) {
            std::get<RadialMask>(s.localAdjustments[1].shape).angle = unbounded;
        });
    }
    SECTION("numbers out of range") {
        refuses([](DevelopState& s) { s.localAdjustments[0].opacity = 1.5F; });
        refuses([](DevelopState& s) { s.localAdjustments[0].opacity = -0.1F; });
        refuses([](DevelopState& s) { s.localAdjustments[0].deltas.exposure = 4.5F; });
        refuses([](DevelopState& s) { s.localAdjustments[0].deltas.relativeTint = -101.0F; });
        refuses(
            [](DevelopState& s) { std::get<LinearMask>(s.localAdjustments[0].shape).to.v = 3.5F; });
        refuses([](DevelopState& s) {
            std::get<RadialMask>(s.localAdjustments[1].shape).radiusX = 4.5F;
        });
        refuses([](DevelopState& s) {
            std::get<RadialMask>(s.localAdjustments[1].shape).feather = 1.5F;
        });
        refuses([](DevelopState& s) {
            std::get<RadialMask>(s.localAdjustments[1].shape).angle = 180.0F;
        });
        refuses([](DevelopState& s) {
            std::get<RadialMask>(s.localAdjustments[1].shape).angle = -181.0F;
        });
    }
    SECTION("degenerate geometry") {
        refuses([](DevelopState& s) {
            std::get<LinearMask>(s.localAdjustments[0].shape).to =
                std::get<LinearMask>(s.localAdjustments[0].shape).from;
        });
        refuses([](DevelopState& s) {
            std::get<RadialMask>(s.localAdjustments[1].shape).radiusY = 0.0F;
        });
    }
    SECTION("a name no XML can carry") {
        refuses([](DevelopState& s) { s.localAdjustments[0].name = "bad\x01name"; });
    }
    SECTION("a mistake is named by its place") {
        DevelopState state = good;
        state.localAdjustments[1].opacity = 2.0F;
        try {
            validate(state);
            FAIL("validate accepted an opacity of 2");
        } catch (const std::invalid_argument& problem) {
            const std::string what = problem.what();
            CHECK(what.find("opacity") != std::string::npos);
            CHECK(what.find("2") != std::string::npos);
        }
    }
}

TEST_CASE("A history session refuses a state with an invalid mask", "[local][session]") {
    EditSession session(Photo("p.dng", photo));
    DevelopState bad = test::stateWithMasks();
    bad.localAdjustments[0].opacity = 7.0F;
    CHECK_THROWS_AS(session.setState(bad), std::invalid_argument);
    CHECK_FALSE(session.editing());
    CHECK(session.history().size() == 1);
    session.setState(test::stateWithMasks());
    CHECK(session.history().size() == 2);
    CHECK(session.hasUnsavedChanges());
}

// Looks never carry masks

TEST_CASE("A look leaves the target's masks and counter alone", "[local][look]") {
    const DevelopState target = test::stateWithMasks();
    DevelopSettings source;
    source.tone.exposure = 1.5F;
    source.color.saturation = 12.0F;
    const Look look = lookOf(photo, source);

    const AppliedLook applied = withLook(photo, target, look, copyableSections);
    CHECK(applied.state.settings.tone.exposure == 1.5F);
    CHECK(applied.state.localAdjustments == target.localAdjustments);
    CHECK(applied.state.nextLocalAdjustmentId == target.nextLocalAdjustmentId);

    // A look taken from a photograph that has masks of its own carries none of them.
    DevelopState sourceState = test::stateWithEveryDelta();
    sourceState.settings = source;
    const Look fromMasked = lookOf(photo, sourceState.settings);
    const AppliedLook onPlain = withLook(photo, DevelopState{}, fromMasked, copyableSections);
    CHECK(onPlain.state.localAdjustments.empty());
    CHECK(onPlain.state.nextLocalAdjustmentId == LocalAdjustmentId{1});

    // The values of a paste, one by one, leave them too.
    const DevelopState single = withValueFrom(photo, target, "exposure", source);
    CHECK(single.localAdjustments == target.localAdjustments);
    const std::string_view keys[] = {"exposure", "contrast", "saturation"};
    CHECK(withValues(photo, target, keys, source).localAdjustments == target.localAdjustments);
}

TEST_CASE("Resetting the crop or the tone curve leaves the masks alone", "[local][look]") {
    DevelopState state = test::stateWithMasks();
    state.settings.geometry.crop.rectangle =
        UprightCropRect{.left = 0.1, .top = 0.1, .right = 0.9, .bottom = 0.9};
    state.settings.toneCurve.luma.points = {{0.0F, 0.1F}, {1.0F, 1.0F}};
    const DevelopState noCrop = withCropReset(photo, state);
    CHECK_FALSE(noCrop.settings.geometry.crop.rectangle);
    CHECK(noCrop.localAdjustments == state.localAdjustments);
    CHECK(noCrop.nextLocalAdjustmentId == state.nextLocalAdjustmentId);

    const DevelopState flat = withValue(photo, state, "toneCurveLuma", ToneCurve{});
    CHECK(flat.settings.toneCurve.luma == ToneCurve{});
    CHECK(flat.localAdjustments == state.localAdjustments);
    CHECK(flat.nextLocalAdjustmentId == state.nextLocalAdjustmentId);
}

// Describing a change

TEST_CASE("Two equal states have no local change", "[local][history]") {
    const DevelopState state = test::stateWithMasks();
    const ChangeDescription change = describeChange(state, state);
    CHECK(change.local.empty());
    CHECK(change.keys.empty());
    CHECK(describeChange(DevelopState{}, DevelopState{}).local.empty());
}

TEST_CASE("Adding and removing masks are described by id", "[local][history]") {
    const DevelopState empty;
    const DevelopState two =
        withLocalAdjustmentAdded(withLocalAdjustmentAdded(empty, LinearMask{}), RadialMask{});

    const ChangeDescription added = describeChange(empty, two);
    CHECK(added.local.added == std::vector<LocalAdjustmentId>{{1}, {2}});
    CHECK(added.local.removed.empty());
    CHECK_FALSE(added.local.reordered);
    CHECK(added.local.changed.empty());
    CHECK(added.keys.empty());
    CHECK_FALSE(added.group);

    const ChangeDescription removed = describeChange(two, empty);
    CHECK(removed.local.removed == std::vector<LocalAdjustmentId>{{1}, {2}});
    CHECK(removed.local.added.empty());

    // The one in the middle goes; the ones either side are neither changed nor moved.
    const DevelopState three = withLocalAdjustmentAdded(two, LinearMask{});
    const DevelopState gap = withLocalAdjustmentRemoved(three, LocalAdjustmentId{2});
    const ChangeDescription middle = describeChange(three, gap);
    CHECK(middle.local.removed == std::vector<LocalAdjustmentId>{{2}});
    CHECK_FALSE(middle.local.reordered);
    CHECK(middle.local.changed.empty());

    // A duplicate is an added id.
    const ChangeDescription copy = describeChange(two, withLocalAdjustmentDuplicated(two, {1}));
    CHECK(copy.local.added == std::vector<LocalAdjustmentId>{{3}});
}

TEST_CASE("Reordering is described, and an add beside it is not a reorder", "[local][history]") {
    const DevelopState three = full();
    const DevelopState moved = withLocalAdjustmentReordered(three, idAt(three, 0), 2);
    const ChangeDescription change = describeChange(three, moved);
    CHECK(change.local.reordered);
    CHECK(change.local.added.empty());
    CHECK(change.local.removed.empty());
    CHECK(change.local.changed.empty());

    const DevelopState four = withLocalAdjustmentRemoved(three, idAt(three, 15));
    const DevelopState fresh = withLocalAdjustmentAdded(four, RadialMask{});
    CHECK_FALSE(describeChange(four, fresh).local.reordered);
}

TEST_CASE("A changed mask names what differs in it", "[local][history]") {
    const DevelopState before = test::stateWithMasks();
    const LocalAdjustmentId first = idAt(before, 0);
    const LocalAdjustmentId second = idAt(before, 1);

    const auto onlyChange = [&](const DevelopState& after) {
        const ChangeDescription change = describeChange(before, after);
        REQUIRE(change.local.changed.size() == 1);
        CHECK(change.local.added.empty());
        CHECK(change.local.removed.empty());
        CHECK_FALSE(change.local.reordered);
        CHECK(change.keys.empty());
        return change.local.changed.front();
    };

    SECTION("a delta") {
        const MaskChange change = onlyChange(withLocalDelta(before, second, "clarity", 33.0));
        CHECK(change.id == second);
        CHECK(change.deltas == std::vector<std::string_view>{"clarity"});
        CHECK_FALSE(change.name);
        CHECK_FALSE(change.enabled);
        CHECK_FALSE(change.opacity);
        CHECK_FALSE(change.invert);
        CHECK_FALSE(change.shape);
    }
    SECTION("several deltas, in table order") {
        DevelopState after = withLocalDelta(before, first, "vibrance", 5.0);
        after = withLocalDelta(std::move(after), first, "exposure", -1.0);
        after = withLocalDelta(std::move(after), first, "relativeTint", 5.0);
        const MaskChange change = onlyChange(after);
        CHECK(change.deltas ==
              std::vector<std::string_view>{"relativeTint", "exposure", "vibrance"});
    }
    SECTION("a delta put back to its value is no change") {
        const DevelopState again =
            withLocalDelta(withLocalDelta(before, first, "exposure", 3.0), first, "exposure", 0.5);
        CHECK(describeChange(before, again).local.empty());
    }
    SECTION("the name") {
        const MaskChange change = onlyChange(withLocalAdjustmentRenamed(before, first, "Hills"));
        CHECK(change.name);
        CHECK(change.deltas.empty());
        CHECK_FALSE(change.shape);
    }
    SECTION("the flags and the opacity") {
        MaskChange change = onlyChange(withLocalAdjustmentEnabled(before, first, true));
        CHECK(change.enabled);
        change = onlyChange(withLocalAdjustmentInverted(before, first, true));
        CHECK(change.invert);
        change = onlyChange(withLocalOpacity(before, first, 0.1F));
        CHECK(change.opacity);
    }
    SECTION("the geometry") {
        const MaskChange change = onlyChange(
            withLocalShape(before, first, LinearMask{.from = {0.0F, 0.0F}, .to = {1.0F, 1.0F}}));
        CHECK(change.shape);
        CHECK(change.deltas.empty());
    }
    SECTION("a mask and a setting at once") {
        DevelopState after = withLocalDelta(before, first, "dehaze", 50.0);
        after.settings.tone.exposure = 1.0F;
        const ChangeDescription change = describeChange(before, after);
        CHECK(change.keys == std::vector<std::string_view>{"exposure"});
        CHECK(change.local.changed.size() == 1);
    }
    SECTION("two masks, in the later state's order") {
        DevelopState after = withLocalDelta(before, second, "dehaze", 1.0);
        after = withLocalDelta(std::move(after), first, "dehaze", 1.0);
        after = withLocalAdjustmentReordered(std::move(after), second, 0);
        const ChangeDescription change = describeChange(before, after);
        REQUIRE(change.local.changed.size() == 2);
        CHECK(change.local.changed[0].id == second);
        CHECK(change.local.changed[1].id == first);
        CHECK(change.local.reordered);
    }
}

TEST_CASE("A settings change alone has no local part", "[local][history]") {
    DevelopState after = test::stateWithMasks();
    after.settings.tone.exposure = 2.0F;
    const ChangeDescription change = describeChange(test::stateWithMasks(), after);
    CHECK(change.local.empty());
    CHECK(change.keys == std::vector<std::string_view>{"exposure"});
}
