#include "CurveEditing.h"
#include "ui/CurveEditor.h"
#include "ui/DevelopPanel.h"
#include "ui/SettingSlider.h"

#include <ColorEncoding.h>
#include <DevelopState.h>
#include <GeometrySettings.h>
#include <NoiseReductionSettings.h>

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>
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

TEST_CASE("The Presence group edits Texture, Clarity and Dehaze in both treatments",
          "[app][panel][presence]") {
    DevelopPanel panel;
    for (const bool grayscale : {false, true}) {
        INFO("grayscale " << grayscale);
        DevelopState state;
        state.settings.blackAndWhite.convertToGrayscale = grayscale;
        panel.showState(state, PanelContext{});
        std::optional<DevelopState> edited;
        const auto connection = QObject::connect(&panel, &DevelopPanel::stateEdited,
                                                 [&](const DevelopState& next) { edited = next; });
        for (const std::string_view key : {"texture", "clarity", "dehaze"}) {
            SettingSlider* row = rowOf(panel, key);
            REQUIRE(row != nullptr);
            auto* group = qobject_cast<QGroupBox*>(row->parentWidget());
            REQUIRE(group != nullptr);
            CHECK(group->title() == QStringLiteral("Presence"));
            CHECK(group->isVisibleTo(&panel));
        }
        emit rowOf(panel, "clarity")->valueEdited(35.0);
        REQUIRE(edited);
        CHECK(edited->settings.presence.clarity == 35.0F);
        CHECK(edited->settings.presence.texture == 0.0F);
        QObject::disconnect(connection);
    }
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
    // Declared first: destroying the scroll area hides the panel, which still announces.
    std::vector<bool> announced;
    QScrollArea scroll;
    auto* panel = new DevelopPanel;
    scroll.setWidget(panel);
    scroll.setWidgetResizable(true);
    scroll.resize(320, 160);
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

namespace {

/// Finds a button of the panel by its object name.
QAbstractButton* buttonOf(const DevelopPanel& panel, const char* name) {
    return panel.findChild<QAbstractButton*>(name);
}

/// Picks the entry of the aspect menu with a text, as the user would.
void chooseAspect(const DevelopPanel& panel, const QString& text) {
    auto* combo = panel.findChild<QComboBox*>("cropAspect");
    REQUIRE(combo != nullptr);
    const int index = combo->findText(text);
    REQUIRE(index >= 0);
    combo->setCurrentIndex(index);
    emit combo->activated(index);
}

} // namespace

TEST_CASE("The Crop group's buttons announce what they ask for", "[app][panel][crop]") {
    DevelopPanel panel;
    panel.showState(DevelopState{}, PanelContext{});
    std::vector<QString> asked;
    QObject::connect(&panel, &DevelopPanel::cropModeToggled,
                     [&](bool on) { asked.push_back(on ? "crop on" : "crop off"); });
    QObject::connect(&panel, &DevelopPanel::straighteningToggled,
                     [&](bool on) { asked.push_back(on ? "level on" : "level off"); });
    QObject::connect(&panel, &DevelopPanel::turned,
                     [&](bool cw) { asked.push_back(cw ? "right" : "left"); });
    QObject::connect(&panel, &DevelopPanel::flipped,
                     [&](bool h) { asked.push_back(h ? "flip h" : "flip v"); });
    QObject::connect(&panel, &DevelopPanel::orientationSwapped, [&] { asked.push_back("swap"); });
    QObject::connect(&panel, &DevelopPanel::lockToggled,
                     [&](bool on) { asked.push_back(on ? "lock" : "unlock"); });

    buttonOf(panel, "cropMode")->click();
    buttonOf(panel, "cropMode")->click();
    buttonOf(panel, "cropLevel")->click();
    buttonOf(panel, "cropTurnLeft")->click();
    buttonOf(panel, "cropTurnRight")->click();
    buttonOf(panel, "cropFlipHorizontal")->click();
    buttonOf(panel, "cropFlipVertical")->click();
    buttonOf(panel, "cropSwap")->click();
    buttonOf(panel, "cropLock")->click();
    buttonOf(panel, "cropLock")->click();

    const std::vector<QString> expected{"crop on", "crop off", "level on", "left", "right",
                                        "flip h",  "flip v",   "swap",     "lock", "unlock"};
    CHECK(asked == expected);
}

TEST_CASE("The panel shows the crop mode and the level tool without announcing them",
          "[app][panel][crop]") {
    DevelopPanel panel;
    int announced = 0;
    QObject::connect(&panel, &DevelopPanel::cropModeToggled, [&](bool) { ++announced; });
    QObject::connect(&panel, &DevelopPanel::straighteningToggled, [&](bool) { ++announced; });

    panel.setCropMode(true);
    panel.setStraightening(true);
    CHECK(buttonOf(panel, "cropMode")->isChecked());
    CHECK(buttonOf(panel, "cropLevel")->isChecked());
    panel.setCropMode(false);
    panel.setStraightening(false);
    CHECK_FALSE(buttonOf(panel, "cropMode")->isChecked());
    CHECK_FALSE(buttonOf(panel, "cropLevel")->isChecked());
    CHECK(announced == 0);
}

TEST_CASE("A preset of the aspect menu announces its landscape ratio", "[app][panel][crop]") {
    DevelopPanel panel;
    panel.showState(DevelopState{}, PanelContext{});
    std::optional<CropAspect> chosen;
    bool matchOrientation = false;
    QObject::connect(&panel, &DevelopPanel::aspectChosen,
                     [&](const CropAspect& aspect, bool match) {
                         chosen = aspect;
                         matchOrientation = match;
                     });

    chooseAspect(panel, "4:5");
    REQUIRE(chosen);
    REQUIRE(std::holds_alternative<CropRatio>(*chosen));
    CHECK(std::get<CropRatio>(*chosen).widthOverHeight == 1.25);
    CHECK(matchOrientation);

    chooseAspect(panel, "Original");
    CHECK(std::holds_alternative<OriginalCropAspect>(*chosen));
    CHECK_FALSE(matchOrientation);

    chooseAspect(panel, "Free");
    CHECK(std::holds_alternative<FreeCropAspect>(*chosen));
}

TEST_CASE("The Custom entry asks for a ratio and takes it as typed", "[app][panel][crop]") {
    DevelopPanel panel;
    panel.showState(DevelopState{}, PanelContext{});
    std::optional<CropAspect> chosen;
    bool matchOrientation = true;
    QObject::connect(&panel, &DevelopPanel::aspectChosen,
                     [&](const CropAspect& aspect, bool match) {
                         chosen = aspect;
                         matchOrientation = match;
                     });

    SECTION("accepted") {
        QTimer::singleShot(0, [] {
            QWidget* dialog = QApplication::activeModalWidget();
            REQUIRE(dialog != nullptr);
            dialog->findChild<QDoubleSpinBox*>("customAspectWidth")->setValue(4.0);
            dialog->findChild<QDoubleSpinBox*>("customAspectHeight")->setValue(5.0);
            static_cast<QDialog*>(dialog)->accept();
        });
        chooseAspect(panel, "Custom…");
        REQUIRE(chosen);
        CHECK(std::get<CropRatio>(*chosen).widthOverHeight == 0.8);
        CHECK_FALSE(matchOrientation);
    }
    SECTION("cancelled") {
        QTimer::singleShot(0, [] {
            REQUIRE(QApplication::activeModalWidget() != nullptr);
            static_cast<QDialog*>(QApplication::activeModalWidget())->reject();
        });
        chooseAspect(panel, "Custom…");
        CHECK_FALSE(chosen);
        // The menu describes the settings again: free.
        CHECK(panel.findChild<QComboBox*>("cropAspect")->currentText() == "Free");
    }
}

TEST_CASE("The aspect menu and lock show the geometry's aspect", "[app][panel][crop]") {
    DevelopPanel panel;
    auto* combo = panel.findChild<QComboBox*>("cropAspect");
    DevelopState state;

    panel.showState(state, PanelContext{});
    CHECK(combo->currentText() == "Free");
    CHECK_FALSE(buttonOf(panel, "cropLock")->isChecked());

    state.settings.geometry.crop.aspect = OriginalCropAspect{};
    panel.showState(state, PanelContext{});
    CHECK(combo->currentText() == "Original");
    CHECK(buttonOf(panel, "cropLock")->isChecked());

    // A portrait crop of a preset shows the preset.
    state.settings.geometry.crop.aspect = CropRatio{0.8};
    panel.showState(state, PanelContext{});
    CHECK(combo->currentText() == "4:5");

    state.settings.geometry.crop.aspect = CropRatio{2.5};
    panel.showState(state, PanelContext{});
    CHECK(combo->currentText() == "Custom (2.5:1)");
    CHECK(buttonOf(panel, "cropLock")->isChecked());

    state.settings.geometry.crop.aspect = FreeCropAspect{};
    panel.showState(state, PanelContext{});
    CHECK(combo->currentText() == "Free");
}

TEST_CASE("The Angle row edits the straighten as it appears on screen", "[app][panel][crop]") {
    DevelopPanel panel;
    panel.showState(DevelopState{}, PanelContext{});
    std::optional<DevelopState> edited;
    QObject::connect(&panel, &DevelopPanel::stateEdited,
                     [&](const DevelopState& next) { edited = next; });
    SettingSlider* angle = rowOf(panel, "straighten");
    REQUIRE(angle != nullptr);

    emit angle->valueEdited(8.0);
    REQUIRE(edited);
    CHECK(edited->settings.geometry.straighten == 8.0);

    // One flip reverses the stored angle's direction on screen.
    DevelopState flipped;
    flipped.settings.geometry.flipHorizontal = true;
    flipped.settings.geometry.straighten = -5.0;
    panel.showState(flipped, PanelContext{});
    CHECK(angle->findChild<QDoubleSpinBox*>()->value() == 5.0);
    emit angle->valueEdited(8.0);
    CHECK(edited->settings.geometry.straighten == -8.0);
    CHECK(edited->settings.geometry.flipHorizontal);
}

TEST_CASE("Reset returns the geometry to its defaults as one edit", "[app][panel][crop]") {
    DevelopPanel panel;
    QAbstractButton* reset = buttonOf(panel, "cropReset");
    panel.showState(DevelopState{}, PanelContext{});
    CHECK_FALSE(reset->isEnabled());

    DevelopState state;
    state.settings.tone.exposure = 0.5F;
    state.settings.geometry.rotation = QuarterTurn::Clockwise90;
    state.settings.geometry.straighten = 3.0;
    state.settings.geometry.crop.rectangle = UprightCropRect{0.1, 0.1, 0.9, 0.9};
    state.settings.geometry.crop.aspect = CropRatio{1.5};
    panel.showState(state, PanelContext{});
    REQUIRE(reset->isEnabled());

    std::vector<QString> events;
    std::optional<DevelopState> edited;
    QObject::connect(&panel, &DevelopPanel::editStarted, [&] { events.push_back("start"); });
    QObject::connect(&panel, &DevelopPanel::stateEdited, [&](const DevelopState& next) {
        events.push_back("edit");
        edited = next;
    });
    QObject::connect(&panel, &DevelopPanel::editFinished, [&] { events.push_back("finish"); });
    reset->click();

    const std::vector<QString> expected{"start", "edit", "finish"};
    CHECK(events == expected);
    REQUIRE(edited);
    CHECK(edited->settings.geometry == GeometrySettings{});
    // Only the geometry went back.
    CHECK(edited->settings.tone.exposure == 0.5F);
}

TEST_CASE("The Crop group's controls leave the focus where it is", "[app][panel][crop]") {
    DevelopPanel panel;
    // In the crop mode the overlay keeps the focus, and with it Enter, Esc, O and X (ADR 040).
    for (const char* name :
         {"cropMode", "cropLevel", "cropLock", "cropSwap", "cropTurnLeft", "cropTurnRight",
          "cropFlipHorizontal", "cropFlipVertical", "cropReset"}) {
        INFO(name);
        REQUIRE(buttonOf(panel, name) != nullptr);
        CHECK(buttonOf(panel, name)->focusPolicy() == Qt::NoFocus);
    }
    CHECK(panel.findChild<QComboBox*>("cropAspect")->focusPolicy() == Qt::NoFocus);
}

TEST_CASE("The crop mode disables every group but Crop", "[app][panel][crop]") {
    DevelopPanel panel;
    panel.showState(DevelopState{}, PanelContext{});
    const auto enabled = [&](const QString& title) {
        for (const auto* group : panel.findChildren<QGroupBox*>()) {
            if (group->title() == title) {
                return group->isEnabled();
            }
        }
        FAIL("no group " << title.toStdString());
        return false;
    };
    panel.setCropMode(true);
    CHECK(enabled("Crop"));
    for (const char* title : {"White Balance", "Tone", "Tone Curve", "Colour", "Colour Grading",
                              "Noise Reduction", "Effects"}) {
        INFO(title);
        CHECK_FALSE(enabled(title));
    }
    CHECK_FALSE(buttonOf(panel, "treatmentColour")->isEnabled());
    panel.setCropMode(false);
    CHECK(enabled("Tone"));
    CHECK(buttonOf(panel, "treatmentColour")->isEnabled());
}

TEST_CASE("The panel fits the develop dock's default width", "[app][panel]") {
    // As the window holds it: in a scroll area, as wide as the dock opens, and tall enough
    // to need a vertical scroll bar. Nothing may scroll sideways.
    QScrollArea scroll;
    auto* panel = new DevelopPanel;
    scroll.setWidget(panel);
    scroll.setWidgetResizable(true);
    scroll.setFrameShape(QFrame::NoFrame);
    panel->showState(DevelopState{}, PanelContext{});
    const int width = panel->defaultDockWidth();
    const int scrollBar = scroll.style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr,
                                                      scroll.verticalScrollBar());
    INFO("panel minimum " << panel->minimumSizeHint().width() << ", scroll bar " << scrollBar
                          << ", dock " << width);
    CHECK(panel->minimumSizeHint().width() + scrollBar <= width);

    scroll.resize(width, 400);
    scroll.show();
    QCoreApplication::processEvents();
    CHECK(scroll.verticalScrollBar()->isVisible());
    CHECK(scroll.horizontalScrollBar()->maximum() == 0);
}
