#include "MaskEditing.h"
#include "ui/MaskOverlay.h"
#include "ui/PhotoView.h"

#include <BrushStrokes.h>
#include <Develop.h>
#include <DevelopState.h>
#include <LocalAdjustmentEdits.h>

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QTest>
#include <QWheelEvent>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

using namespace arraw;
using namespace arraw::app;
using Catch::Approx;

/// The mask mode's mouse and keys over the photo view, and the edits they report (ADR 044).

namespace {

constexpr ImageSize photoSize{600, 400};

/// An overlay over a photo view that fits a 600 by 400 photograph at one to one, wired to a state
/// as the window wires it.
struct Fixture {
    PhotoView view;
    MaskOverlay& overlay;
    DevelopState state;
    int started = 0;
    int updates = 0;
    int finished = 0;
    int cancelled = 0;
    int leaves = 0;
    std::vector<std::optional<LocalAdjustmentId>> selections;
    std::vector<MaskTool> tools;

    Fixture() : overlay(view.maskOverlay()) {
        view.resize(600, 400);
        view.setFrameSize({600, 400});
        view.resetView();
        view.setMaskMode(true);
        QObject::connect(&overlay, &MaskOverlay::editStarted, [this] { ++started; });
        QObject::connect(&overlay, &MaskOverlay::stateEdited, [this](const DevelopState& next) {
            ++updates;
            state = next;
            show();
        });
        QObject::connect(&overlay, &MaskOverlay::editFinished, [this] { ++finished; });
        QObject::connect(&overlay, &MaskOverlay::editCancelled, [this] { ++cancelled; });
        QObject::connect(&overlay, &MaskOverlay::maskSelected,
                         [this](std::optional<LocalAdjustmentId> id) {
                             selections.push_back(id);
                             overlay.setSelection(id);
                         });
        QObject::connect(&overlay, &MaskOverlay::toolChanged,
                         [this](MaskTool tool) { tools.push_back(tool); });
        QObject::connect(&overlay, &MaskOverlay::leaveRequested, [this] { ++leaves; });
        show();
    }

    void show() {
        overlay.setScene(state, SourceShape{photoSize, ImageOrientation::Normal});
    }

    void add(Mask shape) {
        state = withLocalAdjustmentAdded(state, shape);
        show();
    }

    [[nodiscard]] MaskViewMapping mapping() const {
        return *overlay.mapping();
    }

    [[nodiscard]] QPointF at(double u, double v) const {
        return mapping().widgetFrom(CorrectedPosition{u, v});
    }

    void mouse(QEvent::Type type, QPointF position, Qt::MouseButtons held = Qt::LeftButton,
               Qt::KeyboardModifiers modifiers = {}) {
        const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event(type, position, overlay.mapToGlobal(position), button, held, modifiers);
        QCoreApplication::sendEvent(&overlay, &event);
    }

    void press(QPointF position) {
        mouse(QEvent::MouseButtonPress, position);
    }

    void move(QPointF position, Qt::KeyboardModifiers modifiers = {}) {
        mouse(QEvent::MouseMove, position, Qt::LeftButton, modifiers);
    }

    void release(QPointF position) {
        mouse(QEvent::MouseButtonRelease, position, Qt::NoButton);
    }

    /// Presses, moves in four steps and releases.
    void drag(QPointF from, QPointF to) {
        press(from);
        for (int step = 1; step <= 4; ++step) {
            move(from + (to - from) * step / 4.0);
        }
        release(to);
    }

    [[nodiscard]] const LocalAdjustment& mask(std::size_t index = 0) const {
        return state.localAdjustments.at(index);
    }
};

constexpr LinearMask verticalGradient{{0.5F, 0.25F}, {0.5F, 0.75F}};

} // namespace

TEST_CASE("A handle drag reports begin, one result per move, and finish", "[app][masks][overlay]") {
    Fixture f;
    f.add(verticalGradient);
    f.overlay.setSelection(f.mask().id);

    f.drag(f.at(0.5, 0.75), f.at(0.5, 0.9));
    CHECK(f.started == 1);
    CHECK(f.updates == 4);
    CHECK(f.finished == 1);
    CHECK(f.cancelled == 0);
    const auto& moved = std::get<LinearMask>(f.mask().shape);
    CHECK(moved.to.v == Approx(0.9).margin(1e-4));
    CHECK(moved.to.u == Approx(0.5).margin(1e-4));
    CHECK(moved.from.v == Approx(0.25).margin(1e-4));
    CHECK_FALSE(f.overlay.isDragging());
}

TEST_CASE("Shift snaps a linear end to a screen axis", "[app][masks][overlay]") {
    Fixture f;
    f.add(verticalGradient);
    f.overlay.setSelection(f.mask().id);
    const QPointF to = f.at(0.5, 0.75);
    f.press(to);
    f.move(to + QPointF(40.0, 6.0), Qt::ShiftModifier);
    const auto& moved = std::get<LinearMask>(f.mask().shape);
    // Held on the vertical: the sideways move is dropped.
    CHECK(moved.to.u == Approx(0.5).margin(1e-4));
    f.release(to + QPointF(40.0, 6.0));
}

TEST_CASE("A click on a handle leaves no edit to commit", "[app][masks][overlay]") {
    Fixture f;
    f.add(verticalGradient);
    f.overlay.setSelection(f.mask().id);
    const DevelopState before = f.state;
    f.press(f.at(0.5, 0.25));
    f.release(f.at(0.5, 0.25));
    CHECK(f.started == 1);
    CHECK(f.updates == 0);
    CHECK(f.finished == 1);
    CHECK(f.state == before);
}

TEST_CASE("An armed drag creates a mask, selects it and disarms", "[app][masks][overlay]") {
    Fixture f;
    f.overlay.setTool(MaskTool::Linear);
    f.drag(f.at(0.2, 0.2), f.at(0.2, 0.6));
    REQUIRE(f.state.localAdjustments.size() == 1);
    const auto& created = std::get<LinearMask>(f.mask().shape);
    CHECK(created.from.u == Approx(0.2).margin(1e-4));
    CHECK(created.from.v == Approx(0.2).margin(1e-4));
    CHECK(created.to.v == Approx(0.6).margin(1e-4));
    CHECK(f.started == 1);
    CHECK(f.finished == 1);
    REQUIRE_FALSE(f.selections.empty());
    CHECK(f.selections.back() == f.mask().id);
    CHECK(f.overlay.selection() == f.mask().id);
    CHECK(f.overlay.tool() == MaskTool::None);
    REQUIRE_FALSE(f.tools.empty());
    CHECK(f.tools.back() == MaskTool::None);
}

TEST_CASE("A radial drag draws a circle from the centre to the radius", "[app][masks][overlay]") {
    Fixture f;
    f.overlay.setTool(MaskTool::Radial);
    f.drag(f.at(0.5, 0.5), f.at(0.5, 0.5) + QPointF(60.0, 0.0));
    REQUIRE(f.state.localAdjustments.size() == 1);
    const auto& created = std::get<RadialMask>(f.mask().shape);
    CHECK(created.centre.u == Approx(0.5).margin(1e-4));
    CHECK(created.radiusX == Approx(60.0 / 600.0).margin(1e-4));
    CHECK(created.radiusY == Approx(created.radiusX).margin(1e-4));
}

TEST_CASE("A click with a tool armed creates the default mask there", "[app][masks][overlay]") {
    Fixture f;
    f.overlay.setTool(MaskTool::Linear);
    const QPointF click = f.at(0.3, 0.2);
    f.press(click);
    f.release(click);
    REQUIRE(f.state.localAdjustments.size() == 1);
    const auto& created = std::get<LinearMask>(f.mask().shape);
    CHECK(created.from.u == Approx(0.3).margin(1e-4));
    CHECK(created.from.v == Approx(0.2).margin(1e-4));
    // A fifth of the shorter side (80 of 400 pixels) straight down.
    CHECK(created.to.u == Approx(0.3).margin(1e-4));
    CHECK(created.to.v == Approx(0.4).margin(1e-4));
    CHECK(f.finished == 1);

    f.overlay.setTool(MaskTool::Radial);
    f.press(f.at(0.7, 0.7));
    f.release(f.at(0.7, 0.7));
    REQUIRE(f.state.localAdjustments.size() == 2);
    const auto& circle = std::get<RadialMask>(f.mask(1).shape);
    // A sixth of the shorter side, in long-edge units.
    CHECK(circle.radiusX == Approx(400.0 / 6.0 / 600.0).margin(1e-4));
}

TEST_CASE("A gesture is cancelled when the pointer is lost", "[app][masks][overlay]") {
    Fixture f;
    f.add(verticalGradient);
    f.overlay.setSelection(f.mask().id);
    const DevelopState before = f.state;
    const QPointF to = f.at(0.5, 0.75);

    SECTION("Esc") {
        f.press(to);
        f.move(to + QPointF(0.0, 20.0));
        QTest::keyClick(&f.overlay, Qt::Key_Escape);
        CHECK(f.leaves == 0);
    }
    SECTION("a move without the button") {
        f.press(to);
        f.move(to + QPointF(0.0, 20.0));
        f.mouse(QEvent::MouseMove, to + QPointF(0.0, 30.0), Qt::NoButton);
    }
    SECTION("a window deactivation") {
        f.press(to);
        f.move(to + QPointF(0.0, 20.0));
        QEvent deactivated(QEvent::WindowDeactivate);
        QCoreApplication::sendEvent(&f.overlay, &deactivated);
    }
    SECTION("hiding") {
        f.press(to);
        f.move(to + QPointF(0.0, 20.0));
        f.overlay.hide();
    }
    CHECK(f.cancelled == 1);
    CHECK(f.finished == 0);
    CHECK_FALSE(f.overlay.isDragging());
    // What would have been the release is nothing now.
    f.release(to + QPointF(0.0, 20.0));
    CHECK(f.finished == 0);
    CHECK(f.cancelled == 1);
    (void)before;
}

TEST_CASE("Esc cancels, then disarms, then asks to leave", "[app][masks][overlay]") {
    Fixture f;
    f.overlay.setTool(MaskTool::Radial);
    f.press(f.at(0.5, 0.5));
    f.move(f.at(0.5, 0.5) + QPointF(30.0, 0.0));
    QTest::keyClick(&f.overlay, Qt::Key_Escape);
    CHECK(f.cancelled == 1);
    CHECK(f.overlay.tool() == MaskTool::Radial);
    QTest::keyClick(&f.overlay, Qt::Key_Escape);
    CHECK(f.overlay.tool() == MaskTool::None);
    CHECK(f.leaves == 0);
    QTest::keyClick(&f.overlay, Qt::Key_Escape);
    CHECK(f.leaves == 1);
}

TEST_CASE("A drag on empty canvas pans and a click there clears the selection",
          "[app][masks][overlay]") {
    Fixture f;
    f.add(verticalGradient);
    f.overlay.setSelection(f.mask().id);
    f.view.zoomTo(2.0);
    const QPointF before = f.view.transform().centre();

    f.drag(QPointF(100.0, 100.0), QPointF(60.0, 80.0));
    CHECK(f.view.transform().centre() != before);
    CHECK(f.started == 0);
    CHECK(f.updates == 0);
    CHECK(f.selections.empty());

    f.press(QPointF(100.0, 100.0));
    f.release(QPointF(100.0, 100.0));
    REQUIRE(f.selections.size() == 1);
    CHECK_FALSE(f.selections.back().has_value());
    CHECK_FALSE(f.overlay.selection().has_value());
}

TEST_CASE("Space with a drag pans, and the wheel zooms", "[app][masks][overlay]") {
    Fixture f;
    f.add(verticalGradient);
    f.overlay.setSelection(f.mask().id);
    f.view.zoomTo(2.0);
    const QPointF before = f.view.transform().centre();

    // Even on a handle, a drag with Space held is a pan.
    const QPointF onHandle = f.at(0.5, 0.5);
    QTest::keyPress(&f.overlay, Qt::Key_Space);
    f.drag(onHandle, onHandle + QPointF(-40.0, -30.0));
    QTest::keyRelease(&f.overlay, Qt::Key_Space);
    CHECK(f.view.transform().centre() != before);
    CHECK(f.started == 0);
    CHECK(f.updates == 0);

    const double zoom = f.view.zoom();
    const QPointF where(200.0, 150.0);
    QWheelEvent wheel(where, f.overlay.mapToGlobal(where), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&f.overlay, &wheel);
    CHECK(f.view.zoom() > zoom);
}

TEST_CASE("A click on another mask's pin selects it", "[app][masks][overlay]") {
    Fixture f;
    f.add(verticalGradient);
    f.add(RadialMask{{0.8F, 0.3F}, 0.1F, 0.1F, 0.0F, 0.5F});
    f.overlay.setSelection(f.mask(0).id);

    const QPointF pin = *pinPosition(f.mask(1).shape, f.mapping());
    f.press(pin + QPointF(3.0, 2.0));
    f.release(pin + QPointF(3.0, 2.0));
    REQUIRE(f.selections.size() == 1);
    CHECK(f.selections.back() == f.mask(1).id);
    CHECK(f.started == 0);
    CHECK(f.overlay.selection() == f.mask(1).id);
}

TEST_CASE("O shows the tint, and a zoom makes it again", "[app][masks][overlay]") {
    Fixture f;
    f.add(RadialMask{{0.5F, 0.5F}, 0.2F, 0.2F, 0.0F, 0.5F});
    f.overlay.setSelection(f.mask().id);
    CHECK_FALSE(f.overlay.overlayShown());
    CHECK(f.overlay.coverage().isNull());

    bool toggled = false;
    QObject::connect(&f.overlay, &MaskOverlay::overlayToggled,
                     [&](bool shown) { toggled = shown; });
    QTest::keyClick(&f.overlay, Qt::Key_O);
    CHECK(f.overlay.overlayShown());
    CHECK(toggled);
    f.overlay.updateCoverage();
    const QImage tint = f.overlay.coverage();
    REQUIRE_FALSE(tint.isNull());
    // Half the widget's logical size.
    CHECK(tint.width() == 300);
    CHECK(tint.height() == 200);
    // Red where the mask applies (the middle) and clear well outside it.
    CHECK(qAlpha(tint.pixel(150, 100)) > 100);
    CHECK(qAlpha(tint.pixel(5, 5)) == 0);

    f.view.zoomTo(2.0);
    f.overlay.updateCoverage();
    CHECK(f.overlay.coverage() != tint);
    // At twice the zoom the oval fills more of the widget: a corner of the old ring is inside.
    CHECK(qAlpha(f.overlay.coverage().pixel(150, 100)) > 100);

    QTest::keyClick(&f.overlay, Qt::Key_O);
    CHECK_FALSE(f.overlay.overlayShown());
    f.overlay.updateCoverage();
    CHECK(f.overlay.coverage().isNull());
}

TEST_CASE("Delete removes the selected mask as one edit", "[app][masks][overlay]") {
    Fixture f;
    f.add(verticalGradient);
    f.overlay.setSelection(f.mask().id);
    QTest::keyClick(&f.overlay, Qt::Key_Delete);
    CHECK(f.state.localAdjustments.empty());
    CHECK(f.started == 1);
    CHECK(f.updates == 1);
    CHECK(f.finished == 1);
    CHECK_FALSE(f.overlay.selection().has_value());
}

TEST_CASE("A pin on the selected mask's band line is selected, not the band grabbed",
          "[app][masks][overlay]") {
    Fixture f;
    f.add(verticalGradient); // Bands run across at v = 0.25 and 0.75.
    f.add(RadialMask{{0.8F, 0.25F}, 0.05F, 0.05F, 0.0F, 0.5F});
    f.overlay.setSelection(f.mask(0).id);

    const QPointF pin = *pinPosition(f.mask(1).shape, f.mapping());
    f.press(pin + QPointF(0.0, 3.0));
    f.release(pin + QPointF(0.0, 3.0));
    REQUIRE(f.selections.size() == 1);
    CHECK(f.selections.back() == f.mask(1).id);
    CHECK(f.started == 0);

    // Away from the pin, the band is still grabbed.
    f.overlay.setSelection(f.mask(0).id);
    f.press(f.at(0.2, 0.25));
    CHECK(f.started == 1);
    f.release(f.at(0.2, 0.25));
}

TEST_CASE("The overlay places a position where the view places the developed frame",
          "[app][masks][overlay]") {
    Fixture f;
    f.state.settings.geometry.straighten = 7.5;
    f.state.settings.geometry.crop.rectangle = UprightCropRect{0.2, 0.1, 0.9, 0.8};
    f.add(verticalGradient);
    f.overlay.setSelection(f.mask().id);
    const ImageSize cropped = croppedSize(photoSize, ImageOrientation::Normal, f.state);
    f.view.setFrameSize(QSize(static_cast<int>(cropped.width), static_cast<int>(cropped.height)));
    f.view.resetView();
    f.view.zoomTo(1.7);
    f.view.panBy(QPointF(-35.0, 22.0));
    f.show();

    const MaskViewMapping map = f.mapping();
    const double ratio = f.view.devicePixelRatioF();
    for (const CorrectedPosition c :
         {CorrectedPosition{0.5, 0.5}, CorrectedPosition{0.2, 0.7}, CorrectedPosition{0.9, 0.1}}) {
        const DevelopedPoint developed = map.frame().developedFrom(c);
        const QPointF expected =
            f.view.transform().viewFromFrame({developed.x, developed.y}) / ratio;
        const QPointF got = map.widgetFrom(c);
        CHECK(got.x() == Approx(expected.x()).margin(1e-6));
        CHECK(got.y() == Approx(expected.y()).margin(1e-6));
        // A click there picks the same position back.
        const CorrectedPosition back = map.correctedFrom(expected);
        CHECK(back.u == Approx(c.u).margin(1e-6));
        CHECK(back.v == Approx(c.v).margin(1e-6));
    }
}

TEST_CASE("A press or drag with a brush selected makes no edit", "[app][masks][overlay][brush]") {
    Fixture f;
    f.add(BrushMask{});
    f.overlay.setSelection(f.mask().id);
    const DevelopState before = f.state;

    f.drag(f.at(0.3, 0.3), f.at(0.6, 0.5));
    CHECK(f.started == 0);
    CHECK(f.updates == 0);
    CHECK(f.cancelled == 0);
    CHECK(f.state.localAdjustments == before.localAdjustments);
}

TEST_CASE("O tints a selected brush, and nothing when O is off", "[app][masks][overlay][brush]") {
    Fixture f;
    f.add(BrushMask{std::make_shared<const StrokeList>(
        std::vector<Stroke>{Stroke{0.1F, 1.0F, 1.0F, false, {{0.3F, 0.5F}, {0.7F, 0.5F}}}})});
    f.overlay.setSelection(f.mask().id);
    f.overlay.updateCoverage();
    CHECK(f.overlay.coverage().isNull());

    QTest::keyClick(&f.overlay, Qt::Key_O);
    REQUIRE(f.overlay.overlayShown());
    f.overlay.updateCoverage();
    const QImage tint = f.overlay.coverage();
    REQUIRE_FALSE(tint.isNull());
    // Half the widget's logical size; red along the stroke (at v = 0.5) and clear above it.
    CHECK(tint.width() == 300);
    CHECK(tint.height() == 200);
    CHECK(qAlpha(tint.pixel(150, 100)) > 100);
    CHECK(qAlpha(tint.pixel(150, 20)) == 0);
    CHECK(qAlpha(tint.pixel(5, 100)) == 0);

    // Inverted, it is the other way round.
    f.state = withLocalAdjustmentInverted(f.state, f.mask().id, true);
    f.show();
    f.overlay.updateCoverage();
    CHECK(qAlpha(f.overlay.coverage().pixel(150, 100)) == 0);
    CHECK(qAlpha(f.overlay.coverage().pixel(150, 20)) > 100);

    QTest::keyClick(&f.overlay, Qt::Key_O);
    CHECK_FALSE(f.overlay.overlayShown());
    f.overlay.updateCoverage();
    CHECK(f.overlay.coverage().isNull());
}

namespace {

/// A fixture with the brush armed, over a visible view so that it paints.
struct BrushFixture : Fixture {
    int refusals = 0;
    BrushRefusal lastRefusal = BrushRefusal::None;

    BrushFixture() {
        QObject::connect(&overlay, &MaskOverlay::brushRefused,
                         [this](BrushRefusal refusal, const QString&) {
                             ++refusals;
                             lastRefusal = refusal;
                         });
        BrushSettings settings;
        settings.radius = 0.05F;
        overlay.setBrushSettings(settings);
        overlay.setTool(MaskTool::Brush);
    }

    /// Lets the zero-delay timers run.
    static void turn() {
        QTest::qWait(5);
    }

    [[nodiscard]] const Stroke& stroke(std::size_t mask = 0, std::size_t index = 0) const {
        const auto& brush = std::get<BrushMask>(state.localAdjustments.at(mask).shape);
        return (*brush.strokes)[index];
    }
};

/// A brush that can take no more strokes.
BrushMask fullBrush() {
    std::vector<Stroke> strokes(maximumStrokesPerMask,
                                Stroke{0.01F, 1.0F, 1.0F, false, {{0.5F, 0.5F}}});
    return BrushMask{std::make_shared<const StrokeList>(std::move(strokes))};
}

} // namespace

TEST_CASE("A press starts a stroke with a one-point result at once",
          "[app][masks][overlay][brush]") {
    BrushFixture f;
    REQUIRE(f.overlay.painting());
    f.press(f.at(0.3, 0.5));
    CHECK(f.overlay.stroking());
    CHECK(f.started == 1);
    CHECK(f.updates == 1);
    REQUIRE(f.state.localAdjustments.size() == 1);
    CHECK(f.stroke().points.size() == 1);
    CHECK(f.stroke().radius == Approx(0.05F));
    // The new brush is announced as selected, and the tool stays armed.
    REQUIRE(f.selections.size() == 1);
    CHECK(f.selections.back() == f.mask().id);
    CHECK(f.overlay.tool() == MaskTool::Brush);
    f.release(f.at(0.3, 0.5));
    CHECK(f.finished == 1);
    CHECK_FALSE(f.overlay.stroking());
    CHECK(f.overlay.tool() == MaskTool::Brush);
    CHECK(f.tools.empty());
}

TEST_CASE("Moves in one turn give one result, and the release flushes them",
          "[app][masks][overlay][brush]") {
    BrushFixture f;
    f.press(f.at(0.2, 0.5));
    REQUIRE(f.updates == 1);
    f.move(f.at(0.3, 0.5));
    f.move(f.at(0.4, 0.5));
    f.move(f.at(0.5, 0.5));
    CHECK(f.updates == 1);
    BrushFixture::turn();
    CHECK(f.updates == 2);
    CHECK(f.stroke().points.size() == 4);
    f.move(f.at(0.6, 0.5));
    f.release(f.at(0.7, 0.5));
    // The release reports what is left without waiting for a turn.
    CHECK(f.updates == 3);
    CHECK(f.finished == 1);
    CHECK(f.stroke().points.size() == 6);
    BrushFixture::turn();
    CHECK(f.updates == 3);
}

TEST_CASE("Alt at the press erases, and a stroke on a selected brush adds no mask",
          "[app][masks][overlay][brush]") {
    BrushFixture f;
    f.add(BrushMask{});
    f.overlay.setSelection(f.mask().id);
    f.mouse(QEvent::MouseButtonPress, f.at(0.4, 0.4), Qt::LeftButton, Qt::AltModifier);
    f.release(f.at(0.4, 0.4));
    REQUIRE(f.state.localAdjustments.size() == 1);
    CHECK(f.stroke().erase);
    CHECK(f.selections.empty());

    // The Erase setting inverts the other way.
    BrushSettings settings;
    settings.erase = true;
    f.overlay.setBrushSettings(settings);
    f.press(f.at(0.6, 0.6));
    f.release(f.at(0.6, 0.6));
    CHECK(f.stroke(0, 1).erase);
    f.mouse(QEvent::MouseButtonPress, f.at(0.6, 0.6), Qt::LeftButton, Qt::AltModifier);
    f.release(f.at(0.6, 0.6));
    CHECK_FALSE(f.stroke(0, 2).erase);
}

TEST_CASE("A stroke is cancelled when the pointer is lost", "[app][masks][overlay][brush]") {
    const auto lose = GENERATE(0, 1, 2, 3, 4);
    BrushFixture f;
    f.press(f.at(0.3, 0.5));
    f.move(f.at(0.4, 0.5));
    REQUIRE(f.overlay.stroking());
    const int updates = f.updates;
    switch (lose) {
    case 0:
        QTest::keyClick(&f.overlay, Qt::Key_Escape);
        break;
    case 1: {
        QEvent event(QEvent::WindowDeactivate);
        QCoreApplication::sendEvent(&f.overlay, &event);
        break;
    }
    case 2: {
        QEvent event(QEvent::Hide);
        QCoreApplication::sendEvent(&f.overlay, &event);
        break;
    }
    case 3: {
        QFocusEvent event(QEvent::FocusOut);
        QCoreApplication::sendEvent(&f.overlay, &event);
        break;
    }
    default:
        f.mouse(QEvent::MouseMove, f.at(0.5, 0.5), Qt::NoButton);
        break;
    }
    CHECK(f.cancelled == 1);
    CHECK(f.finished == 0);
    CHECK_FALSE(f.overlay.stroking());
    // The pending move is not reported afterwards.
    BrushFixture::turn();
    CHECK(f.updates == updates);
    CHECK(f.overlay.tool() == MaskTool::Brush);
}

TEST_CASE("Esc cancels the stroke, then deselects the brush, then disarms, then leaves",
          "[app][masks][overlay][brush]") {
    BrushFixture f;
    f.add(BrushMask{});
    f.overlay.setSelection(f.mask().id);
    f.press(f.at(0.3, 0.5));
    REQUIRE(f.overlay.stroking());

    f.overlay.escape();
    CHECK(f.cancelled == 1);
    CHECK(f.selections.empty());

    f.overlay.escape();
    REQUIRE(f.selections.size() == 1);
    CHECK_FALSE(f.selections.back().has_value());
    CHECK(f.overlay.tool() == MaskTool::Brush);
    CHECK(f.leaves == 0);

    f.overlay.escape();
    CHECK(f.overlay.tool() == MaskTool::None);
    REQUIRE(f.tools.size() == 1);
    CHECK(f.tools.back() == MaskTool::None);

    f.overlay.escape();
    CHECK(f.leaves == 1);
}

TEST_CASE("Space with a drag and a middle drag pan while the tool paints",
          "[app][masks][overlay][brush]") {
    BrushFixture f;
    f.view.zoomTo(2.0);
    const QPointF before = f.view.transform().centre();
    QTest::keyPress(&f.overlay, Qt::Key_Space);
    f.drag(f.at(0.5, 0.5), f.at(0.5, 0.5) + QPointF(-40.0, -30.0));
    QTest::keyRelease(&f.overlay, Qt::Key_Space);
    CHECK(f.view.transform().centre() != before);
    CHECK(f.started == 0);
    CHECK(f.state.localAdjustments.empty());

    QMouseEvent middle(QEvent::MouseButtonPress, f.at(0.5, 0.5),
                       f.overlay.mapToGlobal(f.at(0.5, 0.5)), Qt::MiddleButton, Qt::MiddleButton,
                       Qt::NoModifier);
    QCoreApplication::sendEvent(&f.overlay, &middle);
    // Left to the view beneath, which pans with it: no edit begins.
    CHECK(f.started == 0);
    CHECK_FALSE(f.overlay.stroking());
    CHECK(f.state.localAdjustments.empty());
}

TEST_CASE("The wheel is ignored in the middle of a stroke", "[app][masks][overlay][brush]") {
    BrushFixture f;
    f.press(f.at(0.3, 0.5));
    const double zoom = f.view.zoom();
    const QPointF where(200.0, 150.0);
    QWheelEvent wheel(where, f.overlay.mapToGlobal(where), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&f.overlay, &wheel);
    CHECK(f.view.zoom() == zoom);
    f.release(f.at(0.3, 0.5));
    QCoreApplication::sendEvent(&f.overlay, &wheel);
    CHECK(f.view.zoom() > zoom);
}

TEST_CASE("Pins are not grabbable while the tool paints, and are not drawn",
          "[app][masks][overlay][brush]") {
    BrushFixture f;
    f.add(RadialMask{{0.5F, 0.5F}, 0.2F, 0.1F, 0.0F, 0.5F});
    f.overlay.setSelection(std::nullopt);
    // A stroke across the radial's centre paints rather than selecting it.
    f.drag(f.at(0.3, 0.5), f.at(0.7, 0.5));
    CHECK(f.started == 1);
    REQUIRE(f.state.localAdjustments.size() == 2);
    CHECK(std::holds_alternative<BrushMask>(f.state.localAdjustments.back().shape));
    CHECK(f.selections.back() == f.state.localAdjustments.back().id);
}

TEST_CASE("With the brush unchecked and a brush selected, a drag pans and a click deselects",
          "[app][masks][overlay][brush]") {
    Fixture f;
    f.add(BrushMask{});
    f.overlay.setSelection(f.mask().id);
    f.view.zoomTo(2.0);
    const QPointF before = f.view.transform().centre();
    f.drag(f.at(0.5, 0.5), f.at(0.5, 0.5) + QPointF(-40.0, -30.0));
    CHECK(f.view.transform().centre() != before);
    CHECK(f.started == 0);
    f.press(f.at(0.5, 0.5));
    f.release(f.at(0.5, 0.5));
    REQUIRE_FALSE(f.selections.empty());
    CHECK_FALSE(f.selections.back().has_value());
    CHECK(f.started == 0);
}

TEST_CASE("The brush's ring has the radius of the brush on screen",
          "[app][masks][overlay][brush]") {
    BrushFixture f;
    const double radius = 0.05;
    for (const double zoom : {0.25, 0.0, 2.0}) {
        if (zoom == 0.0) {
            f.view.resetView();
        } else {
            f.view.zoomTo(zoom);
        }
        const double ratio = f.view.devicePixelRatioF();
        const double expected = radius * 600.0 * f.view.zoom() / ratio;
        CHECK(f.overlay.ringRadius() == Approx(expected).margin(0.5));
    }
    // The same chain at a device pixel ratio of two: logical pixels are half the device ones.
    for (const double ratio : {1.0, 2.0}) {
        const MaskViewMapping map(f.mapping().frame(), f.view.transform(), ratio);
        CHECK(radius * map.scale() == Approx(radius * 600.0 * f.view.zoom() / ratio).margin(0.5));
    }
}

TEST_CASE("The cursor is forbidden on a brush that is full", "[app][masks][overlay][brush]") {
    BrushFixture f;
    f.add(fullBrush());
    f.overlay.setSelection(f.mask().id);
    CHECK(f.overlay.cursor().shape() == Qt::ForbiddenCursor);

    // A press on it is refused: no edit, and the reason is announced.
    f.press(f.at(0.3, 0.5));
    CHECK(f.started == 0);
    CHECK(f.refusals == 1);
    CHECK(f.lastRefusal == BrushRefusal::Strokes);
    CHECK_FALSE(f.overlay.stroking());

    // A new brush takes a stroke again.
    f.overlay.setSelection(std::nullopt);
    CHECK(f.overlay.cursor().shape() == Qt::BlankCursor);
    f.press(f.at(0.3, 0.5));
    CHECK(f.started == 1);
    f.release(f.at(0.3, 0.5));
}

TEST_CASE("A stroke stopped by a budget keeps what fits and says so once",
          "[app][masks][overlay][brush]") {
    BrushFixture f;
    // Nine strokes of the point cap and one of 9 995: five points are left of the mask's budget.
    std::vector<Stroke> strokes(
        9, Stroke{0.01F, 1.0F, 1.0F, false,
                  std::vector<SensorPoint>(maximumStrokePoints, SensorPoint{0.5F, 0.5F})});
    strokes.push_back(
        Stroke{0.01F, 1.0F, 1.0F, false,
               std::vector<SensorPoint>(maximumStrokePoints - 5, SensorPoint{0.5F, 0.5F})});
    f.add(BrushMask{std::make_shared<const StrokeList>(std::move(strokes))});
    f.overlay.setSelection(f.mask().id);
    f.press(f.at(0.1, 0.5));
    REQUIRE(f.overlay.stroking());
    REQUIRE(f.refusals == 0);
    for (int step = 1; step <= 8; ++step) {
        f.move(f.at(0.1 + 0.05 * step, 0.5));
    }
    CHECK(f.refusals == 1);
    CHECK(f.lastRefusal == BrushRefusal::Points);
    CHECK(f.overlay.cursor().shape() == Qt::ForbiddenCursor);
    f.release(f.at(0.6, 0.5));
    // The release commits what fitted.
    CHECK(f.finished == 1);
    CHECK(f.refusals == 1);
    CHECK(f.stroke(0, 10).points.size() == 5);
    // The brush is full now: the next press is refused.
    f.press(f.at(0.3, 0.5));
    CHECK(f.refusals == 2);
    CHECK(f.started == 1);
}

TEST_CASE("While stroking only the ring and the new dabs repaint", "[app][masks][overlay][brush]") {
    BrushFixture f;
    f.view.show();
    REQUIRE(QTest::qWaitForWindowExposed(&f.view));
    QTest::keyClick(&f.overlay, Qt::Key_O);
    QTest::qWait(20);

    struct Recorder : QObject {
        QRect painted;
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::Paint) {
                painted |= static_cast<QPaintEvent*>(event)->rect();
            }
            return false;
        }
    } recorder;
    f.overlay.installEventFilter(&recorder);

    f.press(f.at(0.2, 0.5));
    f.release(f.at(0.2, 0.5));
    QTest::qWait(30);
    f.press(f.at(0.2, 0.5));
    recorder.painted = {};
    // A stroke at the left edge: the right of the widget is none of its business.
    f.move(f.at(0.22, 0.5));
    f.move(f.at(0.24, 0.5));
    QTest::qWait(30);
    REQUIRE_FALSE(recorder.painted.isNull());
    CHECK(recorder.painted.right() < f.overlay.width() / 2);
    CHECK(recorder.painted.height() < f.overlay.height());
    // And what it painted covers the new dab.
    CHECK(recorder.painted.contains(f.at(0.24, 0.5).toPoint()));
    f.release(f.at(0.24, 0.5));
}
