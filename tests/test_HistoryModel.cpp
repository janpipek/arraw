#include "HistoryModel.h"

#include <DevelopState.h>
#include <EditSession.h>

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
