#include "ui/DevelopPanel.h"
#include "ui/MasksPanel.h"
#include "ui/SettingSlider.h"

#include <DevelopState.h>
#include <LocalAdjustmentEdits.h>

#include <QAbstractButton>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListView>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string_view>
#include <vector>

using namespace arraw;
using namespace arraw::app;
using Catch::Approx;

/// The Masks group of the develop panel: the list, the buttons and the selected mask's rows.

namespace {

/// A panel wired to a state as the window wires it: every edit comes back as the shown state.
struct Fixture {
    MasksPanel panel;
    DevelopState shown;
    int started = 0;
    int updates = 0;
    int finished = 0;
    std::vector<std::optional<LocalAdjustmentId>> selections;
    std::vector<MaskTool> tools;

    Fixture() {
        QObject::connect(&panel, &MasksPanel::editStarted, [this] { ++started; });
        QObject::connect(&panel, &MasksPanel::stateEdited, [this](const DevelopState& next) {
            ++updates;
            show(next);
        });
        QObject::connect(&panel, &MasksPanel::editFinished, [this] { ++finished; });
        QObject::connect(&panel, &MasksPanel::maskSelected,
                         [this](std::optional<LocalAdjustmentId> id) {
                             selections.push_back(id);
                             panel.setSelectedMask(id);
                         });
        QObject::connect(&panel, &MasksPanel::maskToolChosen,
                         [this](MaskTool tool) { tools.push_back(tool); });
    }

    void show(const DevelopState& state) {
        shown = state;
        panel.showState(state);
    }

    void add(Mask shape) {
        show(withLocalAdjustmentAdded(shown, shape));
    }

    void select(std::size_t index) {
        panel.setSelectedMask(shown.localAdjustments.at(index).id);
    }

    [[nodiscard]] SettingSlider* row(std::string_view key) const {
        for (auto* found : panel.findChildren<SettingSlider*>()) {
            if (found->key() == key) {
                return found;
            }
        }
        return nullptr;
    }

    [[nodiscard]] QAbstractButton& button(const char* name) const {
        auto* found = panel.findChild<QAbstractButton*>(name);
        REQUIRE(found != nullptr);
        return *found;
    }

    [[nodiscard]] QListView& list() const {
        return *panel.findChild<QListView*>("maskList");
    }

    [[nodiscard]] bool shows(const QWidget* widget) const {
        return widget->isVisibleTo(&panel);
    }

    void resetCounts() {
        started = updates = finished = 0;
    }
};

constexpr LinearMask gradient{{0.5F, 0.25F}, {0.5F, 0.75F}};
constexpr RadialMask oval{{0.5F, 0.5F}, 0.2F, 0.1F, 0.0F, 0.5F};

} // namespace

TEST_CASE("Without a selection the group shows a hint and no rows", "[app][masks][panel]") {
    Fixture f;
    f.add(gradient);
    CHECK(f.panel.title() == "Masks");
    CHECK(f.shows(f.panel.findChild<QLabel*>("maskHint")));
    CHECK_FALSE(f.shows(f.panel.findChild<QWidget*>("maskControls")));
    CHECK_FALSE(f.button("maskDuplicate").isEnabled());
    CHECK_FALSE(f.button("maskDelete").isEnabled());
    CHECK_FALSE(f.button("maskMoveUp").isEnabled());
    CHECK_FALSE(f.button("maskMoveDown").isEnabled());
    CHECK(f.button("maskLinear").isEnabled());
}

TEST_CASE("A selected mask shows opacity and the thirteen deltas", "[app][masks][panel]") {
    Fixture f;
    f.add(gradient);
    f.select(0);
    CHECK_FALSE(f.shows(f.panel.findChild<QLabel*>("maskHint")));
    REQUIRE(f.shows(f.panel.findChild<QWidget*>("maskControls")));

    int visible = 0;
    for (auto* row : f.panel.findChildren<SettingSlider*>()) {
        visible += f.shows(row) ? 1 : 0;
    }
    CHECK(visible == 14);

    const auto labelOf = [&](std::string_view key) {
        return f.row(key)->findChild<QLabel*>()->text();
    };
    const auto rangeOf = [&](std::string_view key) {
        auto* box = f.row(key)->findChild<QDoubleSpinBox*>();
        return std::pair{box->minimum(), box->maximum()};
    };
    CHECK(labelOf("local.opacity") == "Opacity");
    CHECK(rangeOf("local.opacity") == std::pair{0.0, 100.0});
    CHECK(labelOf("local.relativeTemperature") == "Temp");
    CHECK(labelOf("local.relativeTint") == "Tint");
    CHECK(labelOf("local.exposure") == "Exposure");
    CHECK(rangeOf("local.exposure") == std::pair{-4.0, 4.0});
    CHECK(labelOf("local.contrast") == "Contrast");
    CHECK(rangeOf("local.contrast") == std::pair{-100.0, 100.0});
    CHECK(f.row("local.opacity")->findChild<QDoubleSpinBox*>()->value() == Approx(100.0));
}

TEST_CASE("The rows show the mask's values", "[app][masks][panel]") {
    Fixture f;
    f.add(gradient);
    f.add(oval);
    f.shown.localAdjustments[1].deltas.exposure = 1.5F;
    f.shown.localAdjustments[1].opacity = 0.25F;
    f.show(f.shown);
    f.select(1);
    CHECK(f.row("local.exposure")->findChild<QDoubleSpinBox*>()->value() == Approx(1.5));
    CHECK(f.row("local.opacity")->findChild<QDoubleSpinBox*>()->value() == Approx(25.0));
    f.select(0);
    CHECK(f.row("local.exposure")->findChild<QDoubleSpinBox*>()->value() == Approx(0.0));
}

TEST_CASE("A slider edit reports the state with that delta", "[app][masks][panel]") {
    Fixture f;
    f.add(gradient);
    f.select(0);
    const DevelopState before = f.shown;

    f.row("local.exposure")->findChild<QDoubleSpinBox*>()->setValue(0.5);
    CHECK(f.started == 1);
    CHECK(f.updates == 1);
    CHECK(f.shown == withLocalDelta(before, before.localAdjustments[0].id, "exposure", 0.5));
    f.panel.finishPendingEdit();
    CHECK(f.finished == 1);

    f.resetCounts();
    f.row("local.opacity")->findChild<QDoubleSpinBox*>()->setValue(40.0);
    f.panel.finishPendingEdit();
    CHECK(f.shown.localAdjustments[0].opacity == Approx(0.4F));
    CHECK(f.started == 1);
    CHECK(f.finished == 1);
}

TEST_CASE("Each list action is one complete edit", "[app][masks][panel]") {
    Fixture f;
    f.add(gradient);
    f.add(oval);
    f.select(0);
    const LocalAdjustmentId first = f.shown.localAdjustments[0].id;
    f.resetCounts();

    SECTION("invert") {
        f.button("maskInvert").click();
        CHECK(f.shown.localAdjustments[0].invert);
    }
    SECTION("duplicate selects the copy") {
        f.button("maskDuplicate").click();
        REQUIRE(f.shown.localAdjustments.size() == 3);
        REQUIRE_FALSE(f.selections.empty());
        CHECK(f.selections.back() == f.shown.localAdjustments[1].id);
        CHECK(f.shown.localAdjustments[1].id != first);
    }
    SECTION("delete") {
        f.button("maskDelete").click();
        REQUIRE(f.shown.localAdjustments.size() == 1);
        CHECK(f.shown.localAdjustments[0].id != first);
        // The window drops the selection of a mask that is gone.
        CHECK_FALSE(f.panel.selectedMask().has_value());
    }
    SECTION("move down") {
        f.button("maskMoveDown").click();
        CHECK(f.shown.localAdjustments[1].id == first);
    }
    SECTION("rename") {
        auto* model = f.list().model();
        model->setData(model->index(0, 0), "Sky", Qt::EditRole);
        CHECK(f.shown.localAdjustments[0].name == "Sky");
    }
    SECTION("enable") {
        auto* model = f.list().model();
        model->setData(model->index(0, 0), Qt::Unchecked, Qt::CheckStateRole);
        CHECK_FALSE(f.shown.localAdjustments[0].enabled);
    }
    CHECK(f.started == 1);
    CHECK(f.updates == 1);
    CHECK(f.finished == 1);
}

TEST_CASE("Move Up and Move Down stop at the ends", "[app][masks][panel]") {
    Fixture f;
    f.add(gradient);
    f.add(oval);
    f.add(gradient);
    f.select(0);
    CHECK_FALSE(f.button("maskMoveUp").isEnabled());
    CHECK(f.button("maskMoveDown").isEnabled());
    f.select(1);
    CHECK(f.button("maskMoveUp").isEnabled());
    CHECK(f.button("maskMoveDown").isEnabled());
    f.select(2);
    CHECK(f.button("maskMoveUp").isEnabled());
    CHECK_FALSE(f.button("maskMoveDown").isEnabled());
}

TEST_CASE("Sixteen masks is the limit for adding", "[app][masks][panel]") {
    Fixture f;
    for (std::size_t count = 0; count < maximumLocalAdjustments; ++count) {
        f.add(count % 2 == 0 ? Mask{gradient} : Mask{oval});
    }
    f.select(0);
    for (const char* name : {"maskLinear", "maskRadial", "maskDuplicate"}) {
        CHECK_FALSE(f.button(name).isEnabled());
        CHECK(f.button(name).toolTip().contains("at most 16"));
    }
    CHECK(f.button("maskDelete").isEnabled());
    CHECK(f.panel.findChild<QLabel*>("maskCount")->text() == "16 of 16");

    f.button("maskDelete").click();
    CHECK(f.button("maskLinear").isEnabled());
    CHECK(f.panel.findChild<QLabel*>("maskCount")->text() == "15 of 16");
}

TEST_CASE("The creation buttons arm one tool at a time", "[app][masks][panel]") {
    Fixture f;
    f.button("maskLinear").click();
    REQUIRE(f.tools.size() == 1);
    CHECK(f.tools.back() == MaskTool::Linear);
    f.button("maskRadial").click();
    CHECK(f.tools.back() == MaskTool::Radial);
    CHECK_FALSE(f.button("maskLinear").isChecked());
    f.button("maskRadial").click();
    CHECK(f.tools.back() == MaskTool::None);
    f.panel.setTool(MaskTool::Linear);
    CHECK(f.button("maskLinear").isChecked());
    CHECK(f.tools.size() == 3);
}

TEST_CASE("Black and white hides the local saturation and vibrance", "[app][masks][panel]") {
    Fixture f;
    f.add(gradient);
    f.select(0);
    CHECK(f.shows(f.row("local.saturation")));
    CHECK(f.shows(f.row("local.vibrance")));
    f.shown.settings.blackAndWhite.convertToGrayscale = true;
    f.show(f.shown);
    CHECK_FALSE(f.shows(f.row("local.saturation")));
    CHECK_FALSE(f.shows(f.row("local.vibrance")));
    CHECK(f.shows(f.row("local.contrast")));
    CHECK(f.shows(f.row("local.exposure")));
}

TEST_CASE("The crop mode disables the Masks group with the other non-geometry groups",
          "[app][masks][panel]") {
    DevelopPanel panel;
    auto* masks = panel.findChild<MasksPanel*>();
    REQUIRE(masks != nullptr);
    CHECK(masks->isEnabled());
    panel.setCropMode(true);
    CHECK_FALSE(masks->isEnabled());
    panel.setCropMode(false);
    CHECK(masks->isEnabled());
}

TEST_CASE("The develop panel shares one label column with the Masks rows", "[app][masks][panel]") {
    DevelopPanel panel;
    panel.show();
    SettingSlider* global = nullptr;
    SettingSlider* local = nullptr;
    for (auto* row : panel.findChildren<SettingSlider*>()) {
        global = row->key() == "exposure" ? row : global;
        local = row->key() == "local.exposure" ? row : local;
    }
    REQUIRE(global != nullptr);
    REQUIRE(local != nullptr);
    CHECK(global->findChild<QLabel*>()->minimumWidth() ==
          local->findChild<QLabel*>()->minimumWidth());
}

TEST_CASE("The develop panel forwards the Masks group's edits and choices", "[app][masks][panel]") {
    DevelopPanel panel;
    DevelopState state = withLocalAdjustmentAdded(DevelopState{}, gradient);
    panel.showState(state, PanelContext{});
    panel.setSelectedMask(state.localAdjustments[0].id);

    std::vector<DevelopState> edits;
    int started = 0;
    int finished = 0;
    QObject::connect(&panel, &DevelopPanel::editStarted, [&] { ++started; });
    QObject::connect(&panel, &DevelopPanel::stateEdited,
                     [&](const DevelopState& next) { edits.push_back(next); });
    QObject::connect(&panel, &DevelopPanel::editFinished, [&] { ++finished; });

    for (auto* row : panel.findChildren<SettingSlider*>()) {
        if (row->key() == "local.exposure") {
            row->findChild<QDoubleSpinBox*>()->setValue(1.0);
        }
    }
    panel.finishPendingEdit();
    CHECK(started == 1);
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].localAdjustments[0].deltas.exposure == Approx(1.0F));
    CHECK(finished == 1);
}
