#include "CurveEditing.h"
#include "ui/CurveEditor.h"
#include "ui/DevelopPanel.h"
#include "ui/SettingSlider.h"

#include <ColorEncoding.h>
#include <DevelopState.h>
#include <NoiseReductionSettings.h>

#include <QCoreApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The develop panel's wiring of the curve editor (ADR 036) and of the grain seed (ADR 038).

namespace {

/// Finds the slider row of a setting.
SettingSlider* rowOf(const DevelopPanel& panel, std::string_view key) {
    for (auto* row : panel.findChildren<SettingSlider*>()) {
        if (row->key() == key) {
            return row;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Turning grain on gives the photograph a seed, which later edits keep",
          "[app][panel][grain]") {
    DevelopPanel panel;
    panel.showState(DevelopState{}, PanelContext{});
    std::optional<DevelopState> edited;
    QObject::connect(&panel, &DevelopPanel::stateEdited,
                     [&](const DevelopState& next) { edited = next; });
    SettingSlider* amount = rowOf(panel, "grainAmount");
    SettingSlider* size = rowOf(panel, "grainSize");
    REQUIRE(amount != nullptr);
    REQUIRE(size != nullptr);
    // The seed has no row of its own.
    REQUIRE(rowOf(panel, "grainSeed") == nullptr);

    // Another setting leaves the seed unchosen.
    emit size->valueEdited(30.0);
    REQUIRE(edited);
    CHECK(edited->settings.effects.grain.seed == 0U);

    emit amount->valueEdited(40.0);
    REQUIRE(edited);
    const std::uint32_t seed = edited->settings.effects.grain.seed;
    CHECK(seed != 0U);
    CHECK(edited->settings.effects.grain.amount == 40.0F);

    // Shown back, as the window does, then edited again: the seed stays.
    panel.showState(*edited, PanelContext{});
    emit amount->valueEdited(0.0);
    CHECK(edited->settings.effects.grain.seed == seed);
    panel.showState(*edited, PanelContext{});
    emit amount->valueEdited(70.0);
    CHECK(edited->settings.effects.grain.seed == seed);
}

TEST_CASE("Grain a photograph already has keeps seed zero through other edits",
          "[app][panel][grain]") {
    // A sidecar written by hand or by the command line: grain on, no seed. It
    // renders the fixed pattern, and an unrelated edit must not re-roll it.
    DevelopState state;
    state.settings.effects.grain = {.amount = 50.0F};
    DevelopPanel panel;
    panel.showState(state, PanelContext{});
    std::optional<DevelopState> edited;
    QObject::connect(&panel, &DevelopPanel::stateEdited,
                     [&](const DevelopState& next) { edited = next; });
    SettingSlider* exposure = rowOf(panel, "exposure");
    SettingSlider* amount = rowOf(panel, "grainAmount");
    REQUIRE(exposure != nullptr);
    REQUIRE(amount != nullptr);

    emit exposure->valueEdited(0.5);
    REQUIRE(edited);
    CHECK(edited->settings.effects.grain.seed == 0U);
    panel.showState(*edited, PanelContext{});
    emit amount->valueEdited(80.0);
    CHECK(edited->settings.effects.grain.seed == 0U);

    // Turned off and on again, it is grain turned on: it gets a seed then.
    panel.showState(*edited, PanelContext{});
    emit amount->valueEdited(0.0);
    CHECK(edited->settings.effects.grain.seed == 0U);
    panel.showState(*edited, PanelContext{});
    emit amount->valueEdited(30.0);
    CHECK(edited->settings.effects.grain.seed != 0U);
}

TEST_CASE("The curve histogram is wanted only while the editor is on screen",
          "[app][panel][histogram]") {
    QScrollArea scroll;
    auto* panel = new DevelopPanel;
    scroll.setWidget(panel);
    scroll.setWidgetResizable(true);
    scroll.resize(320, 160);
    std::vector<bool> announced;
    QObject::connect(panel, &DevelopPanel::curveHistogramWantedChanged,
                     [&](bool wanted) { announced.push_back(wanted); });
    auto* editor = panel->findChild<CurveEditor*>("curveEditor");
    REQUIRE(editor != nullptr);

    // Not shown at all.
    CHECK_FALSE(panel->curveHistogramWanted());

    // Shown, but scrolled to the top, far above the Tone Curve group.
    scroll.show();
    QCoreApplication::processEvents();
    CHECK_FALSE(panel->curveHistogramWanted());

    scroll.ensureWidgetVisible(editor);
    QCoreApplication::processEvents();
    CHECK(panel->curveHistogramWanted());

    scroll.verticalScrollBar()->setValue(0);
    QCoreApplication::processEvents();
    CHECK_FALSE(panel->curveHistogramWanted());

    scroll.ensureWidgetVisible(editor);
    QCoreApplication::processEvents();
    REQUIRE(panel->curveHistogramWanted());
    // The dock hides its contents with it.
    scroll.hide();
    CHECK_FALSE(panel->curveHistogramWanted());
    scroll.show();
    QCoreApplication::processEvents();
    CHECK(panel->curveHistogramWanted());

    CHECK(announced == std::vector<bool>{true, false, true, false, true});
}

TEST_CASE("A curve edit replaces only the curve of its channel", "[app][panel][curve]") {
    DevelopPanel panel;
    DevelopState state;
    state.settings.toneCurve.luma.points = {{0.0F, 0.0F}, {0.5F, 0.6F}, {1.0F, 1.0F}};
    state.settings.toneCurve.red.points = {{0.0F, 0.1F}, {1.0F, 1.0F}};
    state.settings.tone.exposure = 0.5F;
    panel.showState(state, PanelContext{});
    std::optional<DevelopState> edited;
    QObject::connect(&panel, &DevelopPanel::stateEdited,
                     [&](const DevelopState& next) { edited = next; });

    auto* editor = panel.findChild<CurveEditor*>("curveEditor");
    REQUIRE(editor != nullptr);
    editor->setChannel(CurveChannel::Red);
    editor->resetChannel();

    REQUIRE(edited.has_value());
    DevelopState expected = state;
    expected.settings.toneCurve.red = ToneCurve{};
    CHECK(*edited == expected);
}

TEST_CASE("The readout under the plot follows the selected point", "[app][panel][curve]") {
    DevelopPanel panel;
    auto* editor = panel.findChild<CurveEditor*>("curveEditor");
    auto* readout = panel.findChild<QLabel*>("curveReadout");
    REQUIRE(editor != nullptr);
    REQUIRE(readout != nullptr);
    CHECK(readout->text().isEmpty());
    QKeyEvent pageDown(QEvent::KeyPress, Qt::Key_PageDown, Qt::NoModifier);
    QCoreApplication::sendEvent(editor, &pageDown);
    CHECK(readout->text() == editor->readout());
    CHECK_FALSE(readout->text().isEmpty());
}

TEST_CASE("A reset restores the photograph's own default", "[app][panel][noise]") {
    // A RAW starts with colour noise reduction (ADR 039), and a double-click
    // on the row's label goes back there, not to the neutral zero.
    const auto resetValue = [](const PanelContext& context) {
        DevelopState state;
        state.settings.noiseReduction.color = 60.0F;
        DevelopPanel panel;
        panel.showState(state, context);
        std::optional<DevelopState> edited;
        QObject::connect(&panel, &DevelopPanel::stateEdited,
                         [&](const DevelopState& next) { edited = next; });
        SettingSlider* row = rowOf(panel, "colorNoiseReduction");
        REQUIRE(row != nullptr);
        auto* label = row->findChild<QLabel*>();
        REQUIRE(label != nullptr);
        QMouseEvent click(QEvent::MouseButtonDblClick, QPointF(1, 1),
                          label->mapToGlobal(QPointF(1, 1)), Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(label, &click);
        REQUIRE(edited);
        return edited->settings.noiseReduction.color;
    };
    PanelContext raw;
    raw.defaults = defaultStateFor(CameraNative{}).settings;
    CHECK(resetValue(raw) == rawDefaultColorNoiseReduction);
    PanelContext rendered{.raw = false};
    rendered.defaults = defaultStateFor(workingEncoding).settings;
    CHECK(resetValue(rendered) == 0.0F);
}
