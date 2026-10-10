#include "MaskPresentation.h"
#include "MasksModel.h"
#include "support/LocalAdjustmentStates.h"

#include <DevelopState.h>
#include <LocalAdjustmentEdits.h>

#include <QObject>
#include <QString>
#include <QVariant>

#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The masks list of the Masks group (src/app/MasksModel.h).

namespace {

/// @brief Counts the emissions of a signal.
struct Count {
    int calls = 0;
    std::vector<std::pair<LocalAdjustmentId, QVariant>> arguments;

    [[nodiscard]] bool isEmpty() const {
        return calls == 0;
    }
    [[nodiscard]] int size() const {
        return calls;
    }
};

DevelopState threeMasks() {
    DevelopState state;
    state = withLocalAdjustmentAdded(state, LinearMask{});
    state = withLocalAdjustmentAdded(state, RadialMask{});
    state = withLocalAdjustmentAdded(state, LinearMask{});
    return state;
}

} // namespace

TEST_CASE("The model lists a state's masks with their roles", "[app][masks-model]") {
    DevelopState state = threeMasks();
    const auto ids = [&state] {
        std::vector<LocalAdjustmentId> found;
        for (const LocalAdjustment& adjustment : state.localAdjustments) {
            found.push_back(adjustment.id);
        }
        return found;
    }();
    state = withLocalAdjustmentRenamed(state, ids[1], "Sky");
    state = withLocalAdjustmentEnabled(state, ids[2], false);
    state = withLocalAdjustmentInverted(state, ids[0], true);

    MasksModel model;
    model.setState(state);
    REQUIRE(model.rowCount() == 3);

    SECTION("default names count within the kind") {
        CHECK(model.index(0).data(Qt::DisplayRole).toString() == "Linear 1");
        CHECK(model.index(1).data(Qt::DisplayRole).toString() == "Sky");
        CHECK(model.index(2).data(Qt::DisplayRole).toString() == "Linear 2");
    }
    SECTION("the edit role is the own name, empty when unnamed") {
        CHECK(model.index(0).data(Qt::EditRole).toString().isEmpty());
        CHECK(model.index(1).data(Qt::EditRole).toString() == "Sky");
    }
    SECTION("the check state is Enabled") {
        CHECK(model.index(0).data(Qt::CheckStateRole).toInt() == Qt::Checked);
        CHECK(model.index(2).data(Qt::CheckStateRole).toInt() == Qt::Unchecked);
    }
    SECTION("id, kind and inverted") {
        CHECK(model.index(1).data(MasksModel::IdRole).toUInt() == ids[1].value);
        CHECK(model.index(0).data(MasksModel::KindRole).toInt() == 1);
        CHECK(model.index(1).data(MasksModel::KindRole).toInt() == 2);
        CHECK(model.index(0).data(MasksModel::InvertedRole).toBool());
        CHECK_FALSE(model.index(1).data(MasksModel::InvertedRole).toBool());
        CHECK(model.idAt(2) == ids[2]);
        CHECK_FALSE(model.idAt(3));
        CHECK_FALSE(model.idAt(-1));
        CHECK(model.rowOf(ids[1]) == 1);
        CHECK(model.rowOf(LocalAdjustmentId{99}) == -1);
    }
    SECTION("rows can be selected, checked and edited") {
        const Qt::ItemFlags flags = model.flags(model.index(0));
        CHECK(flags.testFlag(Qt::ItemIsUserCheckable));
        CHECK(flags.testFlag(Qt::ItemIsEditable));
        CHECK(flags.testFlag(Qt::ItemIsSelectable));
    }
}

TEST_CASE("setData only asks", "[app][masks-model]") {
    const DevelopState state = threeMasks();
    MasksModel model;
    model.setState(state);
    Count renames;
    Count enables;
    Count changes;
    QObject::connect(&model, &MasksModel::renameRequested, &model,
                     [&renames](LocalAdjustmentId id, const QString& name) {
                         ++renames.calls;
                         renames.arguments.emplace_back(id, name);
                     });
    QObject::connect(&model, &MasksModel::enabledRequested, &model,
                     [&enables](LocalAdjustmentId id, bool enabled) {
                         ++enables.calls;
                         enables.arguments.emplace_back(id, enabled);
                     });
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
                     [&changes] { ++changes.calls; });

    REQUIRE(model.setData(model.index(1), "Hills", Qt::EditRole));
    REQUIRE(renames.size() == 1);
    CHECK(renames.arguments.front().first == state.localAdjustments[1].id);
    CHECK(renames.arguments.front().second.toString() == "Hills");

    REQUIRE(model.setData(model.index(2), Qt::Unchecked, Qt::CheckStateRole));
    REQUIRE(enables.size() == 1);
    CHECK(enables.arguments.front().first == state.localAdjustments[2].id);
    CHECK_FALSE(enables.arguments.front().second.toBool());

    CHECK_FALSE(model.setData(model.index(0), 1, MasksModel::InvertedRole));
    CHECK_FALSE(model.setData(model.index(7), "x", Qt::EditRole));
    // Nothing changed.
    CHECK(changes.isEmpty());
    CHECK(model.index(1).data(Qt::DisplayRole).toString() == "Radial 1");
    CHECK(model.index(2).data(Qt::CheckStateRole).toInt() == Qt::Checked);
}

TEST_CASE("The same ids give dataChanged, anything else a reset", "[app][masks-model]") {
    const DevelopState state = threeMasks();
    MasksModel model;
    model.setState(state);
    Count changes;
    Count resets;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
                     [&changes] { ++changes.calls; });
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model,
                     [&resets] { ++resets.calls; });

    SECTION("an edit that keeps the list") {
        model.setState(withLocalAdjustmentRenamed(state, state.localAdjustments[0].id, "Edge"));
        CHECK(changes.size() == 1);
        CHECK(resets.isEmpty());
        CHECK(model.index(0).data(Qt::DisplayRole).toString() == "Edge");
    }
    SECTION("the same state changes nothing") {
        model.setState(state);
        CHECK(changes.isEmpty());
        CHECK(resets.isEmpty());
    }
    SECTION("a reorder resets") {
        model.setState(withLocalAdjustmentReordered(state, state.localAdjustments[2].id, 0));
        CHECK(resets.size() == 1);
        CHECK(model.idAt(0) == state.localAdjustments[2].id);
    }
    SECTION("an add resets") {
        model.setState(withLocalAdjustmentAdded(state, RadialMask{}));
        CHECK(resets.size() == 1);
        CHECK(model.rowCount() == 4);
        CHECK(model.index(3).data(Qt::DisplayRole).toString() == "Radial 2");
    }
    SECTION("a removal resets, and an empty state empties the list") {
        model.setState(withLocalAdjustmentRemoved(state, state.localAdjustments[1].id));
        CHECK(resets.size() == 1);
        CHECK(model.rowCount() == 2);
        model.setState(DevelopState{});
        CHECK(model.rowCount() == 0);
        CHECK(resets.size() == 2);
    }
}

TEST_CASE("A full list of sixteen masks is listed", "[app][masks-model]") {
    DevelopState state;
    for (std::size_t index = 0; index < maximumLocalAdjustments; ++index) {
        state = withLocalAdjustmentAdded(state,
                                         index % 2 == 0 ? Mask{LinearMask{}} : Mask{RadialMask{}});
    }
    MasksModel model;
    model.setState(state);
    CHECK(model.rowCount() == 16);
    CHECK(model.index(15).data(Qt::DisplayRole).toString() == "Radial 8");
    CHECK(model.index(14).data(Qt::DisplayRole).toString() == "Linear 8");
}

TEST_CASE("A brush is listed as Brush 1, with kind 3", "[app][masks-model][brush]") {
    DevelopState state = withLocalAdjustmentAdded(DevelopState{}, LinearMask{});
    state = withLocalAdjustmentAdded(state, BrushMask{});
    state = withLocalAdjustmentAdded(state, BrushMask{});
    state = withLocalAdjustmentRenamed(state, state.localAdjustments[2].id, "Face");
    MasksModel model;
    model.setState(state);
    REQUIRE(model.rowCount() == 3);
    CHECK(model.index(1).data(Qt::DisplayRole).toString() == "Brush 1");
    CHECK(model.index(2).data(Qt::DisplayRole).toString() == "Face");
    CHECK(model.index(0).data(MasksModel::KindRole).toInt() == 1);
    CHECK(model.index(1).data(MasksModel::KindRole).toInt() == 3);
    CHECK(model.index(2).data(MasksModel::KindRole).toInt() == 3);
    CHECK(maskDisplayName(state, state.localAdjustments[1].id) == "Brush 1");
}
