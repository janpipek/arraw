#include "CurveEditing.h"
#include "ui/CurveEditor.h"
#include "ui/DevelopPanel.h"

#include <DevelopState.h>

#include <QCoreApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The develop panel's wiring of the curve editor (ADR 036).

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
