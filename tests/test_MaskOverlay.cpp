#include "MaskEditing.h"
#include "ui/MaskOverlay.h"
#include "ui/PhotoView.h"

#include <Develop.h>
#include <DevelopState.h>
#include <LocalAdjustmentEdits.h>

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTest>
#include <QWheelEvent>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
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

    const QPointF pin = pinPosition(f.mask(1).shape, f.mapping());
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

    const QPointF pin = pinPosition(f.mask(1).shape, f.mapping());
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
