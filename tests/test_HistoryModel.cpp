#include "HistoryModel.h"
#include "support/LocalAdjustmentStates.h"

#include <DevelopState.h>
#include <EditSession.h>
#include <LocalAdjustmentEdits.h>

#include <QCoreApplication>
#include <QRegularExpression>
#include <QString>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The History dock's list: its wording, order and roles (src/app/HistoryModel.h).

namespace {

/// A history from the opening state and the steps after it, each made by a change to the one
/// before.
struct Built {
    std::vector<HistoryStep> steps{HistoryStep{DevelopState{}, EditOrigin::Opened, {}}};

    template <class Change>
    Built& then(Change change, EditOrigin origin = EditOrigin::Edit, std::string detail = {}) {
        DevelopState next = steps.back().state;
        change(next.settings);
        steps.push_back(HistoryStep{next, origin, std::move(detail)});
        return *this;
    }
};

QString textAt(const HistoryModel& model, std::size_t index) {
    return model.index(model.rowOfIndex(index)).data(Qt::DisplayRole).toString();
}

} // namespace

TEST_CASE("Each origin has its own wording", "[app][history]") {
    Built built;
    built.then([](DevelopSettings& s) { s.tone.exposure = 1.0F; }, EditOrigin::Paste)
        .then([](DevelopSettings& s) { s.tone.contrast = 10.0F; }, EditOrigin::Preset, "Punchy")
        .then([](DevelopSettings&) {}, EditOrigin::Reset)
        .then([](DevelopSettings& s) { s.geometry.flipHorizontal = true; }, EditOrigin::Crop);
    HistoryModel model;
    model.setHistory(built.steps, 4, DevelopState{});

    CHECK(textAt(model, 0) == "Opened");
    CHECK(textAt(model, 1) == "Paste Settings");
    CHECK(textAt(model, 2) == "Preset: Punchy");
    CHECK(textAt(model, 3) == "Reset");
    CHECK(textAt(model, 4) == "Crop");
}

TEST_CASE("An edit of one setting names it with its value", "[app][history]") {
    Built built;
    built.then([](DevelopSettings& s) { s.tone.exposure = 0.5F; })
        .then([](DevelopSettings& s) { s.tone.exposure = -1.25F; })
        .then([](DevelopSettings& s) { s.color.saturation = 12.0F; });
    HistoryModel model;
    model.setHistory(built.steps, 3, DevelopState{});

    CHECK(textAt(model, 1) == "Exposure +0.50 EV");
    CHECK(textAt(model, 2) == "Exposure -1.25 EV");
    CHECK(textAt(model, 3).startsWith("Saturation "));
}

TEST_CASE("An edit of one setting without a number gives just its name", "[app][history]") {
    Built built;
    built.then([](DevelopSettings& s) { s.geometry.flipVertical = true; });
    HistoryModel model;
    model.setHistory(built.steps, 1, DevelopState{});

    const QString text = textAt(model, 1);
    CHECK_FALSE(text.isEmpty());
    CHECK_FALSE(text.contains(QRegularExpression("[0-9]")));
}

TEST_CASE("The straighten is worded as the panel shows it", "[app][history]") {
    DevelopState flipped;
    flipped.settings.geometry.flipHorizontal = true;
    DevelopState straightened = flipped;
    // One flip reverses the stored angle: +2 degrees on screen is stored as -2.
    straightened.settings.geometry.straighten = -2.0;
    const std::vector<HistoryStep> steps{{flipped, EditOrigin::Opened, {}},
                                         {straightened, EditOrigin::Edit, {}}};
    HistoryModel model;
    model.setHistory(steps, 1, flipped);

    CHECK(textAt(model, 1).contains("+2.0"));
    CHECK_FALSE(textAt(model, 1).contains("-2.0"));
}

TEST_CASE("A reset names the group it reset", "[app][history]") {
    Built built;
    built.then([](DevelopSettings& s) { s.toneCurve.luma.points = {{0.0F, 0.1F}, {1.0F, 1.0F}}; })
        .then([](DevelopSettings& s) { s.toneCurve = ToneCurveSettings{}; }, EditOrigin::Reset)
        .then(
            [](DevelopSettings& s) {
                s.geometry.flipHorizontal = true;
                s.tone.exposure = 1.0F;
            },
            EditOrigin::Reset);
    HistoryModel model;
    model.setHistory(built.steps, 3, DevelopState{});

    CHECK(textAt(model, 2) == "Reset Tone Curve");
    // Two groups at once: nothing to name.
    CHECK(textAt(model, 3) == "Reset");
}

TEST_CASE("An edit of several settings in one group names the group", "[app][history]") {
    Built built;
    built.then([](DevelopSettings& s) {
        s.tone.exposure = 0.5F;
        s.tone.contrast = 20.0F;
    });
    HistoryModel model;
    model.setHistory(built.steps, 1, DevelopState{});

    CHECK(textAt(model, 1) == groupDisplayName(SettingGroup::Tone));
    CHECK(textAt(model, 1) == "Tone");
}

TEST_CASE("An edit across groups counts its settings", "[app][history]") {
    Built built;
    built
        .then([](DevelopSettings& s) {
            s.tone.exposure = 0.5F;
            s.color.saturation = 10.0F;
        })
        .then([](DevelopSettings& s) {
            s.tone.contrast = 5.0F;
            s.color.vibrance = 10.0F;
            s.geometry.flipHorizontal = true;
        });
    HistoryModel model;
    model.setHistory(built.steps, 2, DevelopState{});

    CHECK(textAt(model, 1) == "2 settings");
    CHECK(textAt(model, 2) == "3 settings");
}

TEST_CASE("An edit that changes nothing is worded plainly", "[app][history]") {
    Built built;
    built.then([](DevelopSettings&) {});
    HistoryModel model;
    model.setHistory(built.steps, 1, DevelopState{});

    CHECK(textAt(model, 1) == "Edit");
}

TEST_CASE("Rows run newest first and map to history indices", "[app][history]") {
    Built built;
    built.then([](DevelopSettings& s) { s.tone.exposure = 1.0F; }).then([](DevelopSettings& s) {
        s.tone.contrast = 10.0F;
    });
    HistoryModel model;
    model.setHistory(built.steps, 2, DevelopState{});

    REQUIRE(model.rowCount() == 3);
    CHECK(model.index(2).data(Qt::DisplayRole).toString() == "Opened");
    CHECK(model.rowOfIndex(0) == 2);
    CHECK(model.rowOfIndex(2) == 0);
    CHECK(model.indexOfRow(0) == 2);
    CHECK(model.indexOfRow(2) == 0);
    CHECK(model.indexOfRow(model.rowOfIndex(1)) == 1);
}

TEST_CASE("Rows say whether they are current, redoable or saved", "[app][history]") {
    Built built;
    built.then([](DevelopSettings& s) { s.tone.exposure = 1.0F; }).then([](DevelopSettings& s) {
        s.tone.contrast = 10.0F;
    });
    HistoryModel model;
    // Gone back to the first edit, which is what is on disk.
    model.setHistory(built.steps, 1, built.steps[1].state);

    const auto flag = [&](std::size_t index, int role) {
        return model.index(model.rowOfIndex(index)).data(role).toBool();
    };
    CHECK_FALSE(flag(0, HistoryModel::CurrentRole));
    CHECK(flag(1, HistoryModel::CurrentRole));
    CHECK_FALSE(flag(2, HistoryModel::CurrentRole));

    CHECK_FALSE(flag(0, HistoryModel::RedoableRole));
    CHECK_FALSE(flag(1, HistoryModel::RedoableRole));
    CHECK(flag(2, HistoryModel::RedoableRole));

    CHECK_FALSE(flag(0, HistoryModel::SavedRole));
    CHECK(flag(1, HistoryModel::SavedRole));
    CHECK_FALSE(flag(2, HistoryModel::SavedRole));
}

TEST_CASE("Clearing the model leaves no rows", "[app][history]") {
    Built built;
    HistoryModel model;
    model.setHistory(built.steps, 0, DevelopState{});
    CHECK(model.rowCount() == 1);
    model.clear();
    CHECK(model.rowCount() == 0);
}

// Masks

namespace {

/// A history over states, each a change to the one before; the first is the opening state.
struct BuiltStates {
    std::vector<HistoryStep> steps;

    explicit BuiltStates(DevelopState first = {}) {
        steps.push_back({std::move(first), EditOrigin::Opened, {}});
    }

    BuiltStates& then(DevelopState next, EditOrigin origin = EditOrigin::Edit) {
        steps.push_back({std::move(next), origin, {}});
        return *this;
    }

    const DevelopState& last() const {
        return steps.back().state;
    }

    QString textOfLast() const {
        HistoryModel model;
        model.setHistory(steps, steps.size() - 1, DevelopState{});
        return textAt(model, steps.size() - 1);
    }
};

LocalAdjustmentId idOf(const DevelopState& state, std::size_t index) {
    return state.localAdjustments.at(index).id;
}

} // namespace

TEST_CASE("Adding and removing a mask name it by kind and place", "[app][history][local]") {
    BuiltStates built;
    built.then(withLocalAdjustmentAdded(built.last(), RadialMask{}));
    CHECK(built.textOfLast() == "Add Radial 1");
    built.then(withLocalAdjustmentAdded(built.last(), LinearMask{}));
    CHECK(built.textOfLast() == "Add Linear 1");
    built.then(withLocalAdjustmentAdded(built.last(), LinearMask{}));
    CHECK(built.textOfLast() == "Add Linear 2");
    // The name of a mask that goes is the one it had in the state before.
    built.then(withLocalAdjustmentRemoved(built.last(), idOf(built.last(), 1)));
    CHECK(built.textOfLast() == "Remove Linear 1");
    built.then(withLocalAdjustmentRemoved(built.last(), idOf(built.last(), 1)));
    // The second linear mask became the first when the first went.
    CHECK(built.textOfLast() == "Remove Linear 1");
    built.then(withLocalAdjustmentRemoved(built.last(), idOf(built.last(), 0)));
    CHECK(built.textOfLast() == "Remove Radial 1");
}

TEST_CASE("A named mask is worded by its name", "[app][history][local]") {
    BuiltStates built{withLocalAdjustmentAdded(DevelopState{}, LinearMask{})};
    const LocalAdjustmentId id = idOf(built.last(), 0);
    built.then(withLocalAdjustmentRenamed(built.last(), id, "Sky"));
    CHECK(built.textOfLast() == "Rename Sky");
    built.then(withLocalDelta(built.last(), id, "exposure", 0.5));
    CHECK(built.textOfLast() == "Sky: Exposure +0.50 EV");
    built.then(withLocalAdjustmentRemoved(built.last(), id));
    CHECK(built.textOfLast() == "Remove Sky");
}

TEST_CASE("One delta of a mask is worded with its control and value", "[app][history][local]") {
    BuiltStates built{withLocalAdjustmentAdded(
        withLocalAdjustmentAdded(DevelopState{}, LinearMask{}), LinearMask{})};
    const LocalAdjustmentId second = idOf(built.last(), 1);

    built.then(withLocalDelta(built.last(), second, "exposure", 0.5));
    CHECK(built.textOfLast() == "Linear 2: Exposure +0.50 EV");
    built.then(withLocalDelta(built.last(), second, "exposure", -1.25));
    CHECK(built.textOfLast() == "Linear 2: Exposure -1.25 EV");
    built.then(withLocalDelta(built.last(), second, "dehaze", 20.0));
    CHECK(built.textOfLast() == "Linear 2: Dehaze +20");
    built.then(withLocalDelta(built.last(), second, "shadows", -35.0));
    CHECK(built.textOfLast() == "Linear 2: Shadows -35");
    built.then(withLocalDelta(built.last(), second, "relativeTemperature", 40.0));
    CHECK(built.textOfLast() == "Linear 2: Temp +40");
    built.then(withLocalDelta(built.last(), second, "relativeTint", -8.0));
    CHECK(built.textOfLast() == "Linear 2: Tint -8");
    built.then(withLocalDelta(built.last(), second, "vibrance", 12.0));
    CHECK(built.textOfLast().startsWith("Linear 2: Vibrance +12"));
    built.then(withLocalDelta(built.last(), second, "vibrance", 0.0));
    CHECK(built.textOfLast().startsWith("Linear 2: Vibrance 0"));
}

TEST_CASE("Every local control has wording", "[app][history][local]") {
    for (const LocalDescriptor& row : localAdjustmentDescriptors) {
        INFO(row.key);
        BuiltStates built{withLocalAdjustmentAdded(DevelopState{}, LinearMask{})};
        built.then(withLocalDelta(built.last(), idOf(built.last(), 0), row.key, 10.0));
        HistoryModel model;
        model.setHistory(built.steps, 1, DevelopState{});
        const QString text = textAt(model, 1);
        CHECK(text.startsWith("Linear 1: "));
        CHECK(text.contains("+"));
    }
}

TEST_CASE("Several deltas of one mask are counted", "[app][history][local]") {
    BuiltStates built{withLocalAdjustmentAdded(DevelopState{}, RadialMask{})};
    const LocalAdjustmentId id = idOf(built.last(), 0);
    DevelopState next = withLocalDelta(built.last(), id, "exposure", 0.5);
    next = withLocalDelta(next, id, "clarity", 10.0);
    built.then(next);
    CHECK(built.textOfLast() == "Radial 1: 2 settings");
}

TEST_CASE("The flags, opacity and handles of a mask have their own wording",
          "[app][history][local]") {
    BuiltStates built{withLocalAdjustmentAdded(DevelopState{}, RadialMask{})};
    const LocalAdjustmentId id = idOf(built.last(), 0);

    built.then(withLocalAdjustmentEnabled(built.last(), id, false));
    CHECK(built.textOfLast() == "Disable Radial 1");
    built.then(withLocalAdjustmentEnabled(built.last(), id, true));
    CHECK(built.textOfLast() == "Enable Radial 1");
    built.then(withLocalAdjustmentInverted(built.last(), id, true));
    CHECK(built.textOfLast() == "Invert Radial 1");
    built.then(withLocalOpacity(built.last(), id, 0.5F));
    CHECK(built.textOfLast() == "Radial 1: Opacity 50%");
    built.then(withLocalShape(built.last(), id, RadialMask{.centre = {0.1F, 0.1F}}));
    CHECK(built.textOfLast() == "Move Radial 1");
    // More than one aspect of the mask at once.
    DevelopState both = withLocalOpacity(built.last(), id, 0.25F);
    both = withLocalDelta(both, id, "exposure", 1.0);
    built.then(both);
    CHECK(built.textOfLast() == "Edit Radial 1");
}

TEST_CASE("Reordering, and several masks at once, are worded", "[app][history][local]") {
    DevelopState three;
    for (int i = 0; i < 3; ++i) {
        three = withLocalAdjustmentAdded(three, i == 1 ? Mask{RadialMask{}} : Mask{LinearMask{}});
    }
    BuiltStates built{three};
    built.then(withLocalAdjustmentReordered(built.last(), idOf(built.last(), 0), 2));
    CHECK(built.textOfLast() == "Reorder Masks");

    DevelopState edited = withLocalDelta(built.last(), idOf(built.last(), 0), "exposure", 1.0);
    edited = withLocalDelta(edited, idOf(edited, 1), "exposure", 1.0);
    built.then(edited);
    CHECK(built.textOfLast() == "2 mask changes");

    BuiltStates added;
    DevelopState two = withLocalAdjustmentAdded(
        withLocalAdjustmentAdded(DevelopState{}, LinearMask{}), RadialMask{});
    added.then(two);
    CHECK(added.textOfLast() == "2 mask changes");
}

TEST_CASE("A step that changes masks and settings counts them together", "[app][history][local]") {
    BuiltStates built;
    DevelopState next = withLocalAdjustmentAdded(built.last(), LinearMask{});
    next.settings.tone.exposure = 1.0F;
    built.then(next);
    CHECK(built.textOfLast() == "2 changes");
}

TEST_CASE("A step that changes settings alone is worded as before", "[app][history][local]") {
    BuiltStates built{test::stateWithMasks()};
    DevelopState next = built.last();
    next.settings.tone.exposure = 0.5F;
    built.then(next);
    CHECK(built.textOfLast() == "Exposure +0.50 EV");
}
