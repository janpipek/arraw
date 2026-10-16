#include "BrushCoverage.h"
#include "DebugLog.h"
#include "TimingTrace.h"
#include "support/BrushGenerators.h"
#include "support/GeneratedPhoto.h"
#include "support/LocalAdjustmentStates.h"
#include "support/StallProbe.h"
#include "support/TempDir.h"
#include "ui/CropOverlay.h"
#include "ui/DevelopPanel.h"
#include "ui/FilmStrip.h"
#include "ui/MainWindow.h"
#include "ui/MaskOverlay.h"
#include "ui/MasksPanel.h"
#include "ui/PhotoView.h"
#include "ui/SettingSlider.h"
#include "ui/StatusLine.h"

#include <BrushStrokes.h>
#include <DevelopState.h>
#include <LocalAdjustmentEdits.h>
#include <Photo.h>
#include <Sidecar.h>

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QEventLoop>
#include <QLabel>
#include <QListView>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QPixmap>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
#include <QTimer>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <variant>
#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The mask mode inside the window: keys, gestures as history steps, selection (ADR 044).

namespace {

/// A window with two generated 640 by 480 photographs, the first one shown and rendered.
struct Window {
    test::TempDir folder;
    DebugLog debugLog;
    MainWindow window{debugLog};

    explicit Window(int width = 640, int height = 480, QSize size = {}, bool second = true,
                    const DevelopState* sidecarState = nullptr) {
        test::writeGeneratedPhoto(folder.file("a.png"), width, height);
        if (sidecarState != nullptr) {
            writeSidecar(openPhoto(folder.file("a.png")).with(*sidecarState));
        }
        if (second) {
            test::writeGeneratedPhoto(folder.file("b.png"), width, height);
        }
        if (size.isValid()) {
            window.resize(size);
        }
        window.openInitialPath(folder.file("a.png"));
        window.show();
        window.activateWindow();
        REQUIRE(QTest::qWaitForWindowActive(&window));
        REQUIRE(QTest::qWaitFor([this] { return !view().wholeFrameImage().isNull(); }, 20000));
    }

    [[nodiscard]] PhotoView& view() const {
        return *window.findChild<PhotoView*>();
    }

    [[nodiscard]] MaskOverlay& overlay() const {
        return view().maskOverlay();
    }

    [[nodiscard]] DevelopPanel& panel() const {
        return *window.findChild<DevelopPanel*>();
    }

    [[nodiscard]] MasksPanel& masks() const {
        return *window.findChild<MasksPanel*>();
    }

    [[nodiscard]] QListView& history() const {
        return *window.findChild<QListView*>("historyList");
    }

    [[nodiscard]] QAbstractButton& button(const char* name) const {
        auto* found = masks().findChild<QAbstractButton*>(name);
        REQUIRE(found != nullptr);
        return *found;
    }

    [[nodiscard]] QAction& action(const QString& name) const {
        for (QAction* candidate : window.findChildren<QAction*>()) {
            if (candidate->objectName() == name) {
                return *candidate;
            }
        }
        FAIL("no action " << name.toStdString());
        throw;
    }

    /// Presses a key where the focus is, through the window's shortcuts as a user's key goes.
    void press(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) const {
        QWidget* focus = QApplication::focusWidget();
        QTest::keyClick(focus != nullptr ? focus : const_cast<MainWindow*>(&window), key,
                        modifiers);
    }

    [[nodiscard]] int steps() const {
        return history().model()->rowCount();
    }

    [[nodiscard]] QString step(int row) const {
        return history().model()->index(row, 0).data(Qt::DisplayRole).toString();
    }

    [[nodiscard]] QPointF at(double u, double v) const {
        return overlay().mapping()->widgetFrom(CorrectedPosition{u, v});
    }

    void mouse(QEvent::Type type, QPointF position, Qt::MouseButtons held = Qt::LeftButton) const {
        const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event(type, position, overlay().mapToGlobal(position), button, held,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(&overlay(), &event);
    }

    void drag(QPointF from, QPointF to) const {
        mouse(QEvent::MouseButtonPress, from);
        for (int step = 1; step <= 4; ++step) {
            mouse(QEvent::MouseMove, from + (to - from) * step / 4.0);
        }
        mouse(QEvent::MouseButtonRelease, to, Qt::NoButton);
    }

    /// Arms a tool by its button and draws a mask from (0.3, 0.3) to (0.3, 0.7).
    void draw(const char* tool = "maskLinear") const {
        QTest::mouseClick(&button(tool), Qt::LeftButton);
        drag(at(0.3, 0.3), at(0.3, 0.7));
    }

    /// Types a value into a mask row and ends the edit at once.
    void setRow(const char* key, double value) const {
        for (auto* row : window.findChildren<SettingSlider*>()) {
            if (row->key() == key) {
                row->findChild<QDoubleSpinBox*>()->setValue(value);
            }
        }
        panel().finishPendingEdit();
    }

    /// Waits for a render that differs from an image.
    [[nodiscard]] bool waitForRenderOtherThan(const QImage& before) const {
        return QTest::qWaitFor([&] { return view().wholeFrameImage() != before; }, 20000);
    }
};

} // namespace

TEST_CASE("M enters the mask mode with the focus on the overlay, and leaves it",
          "[app][window][masks]") {
    Window w;
    CHECK(w.action("maskAction").isEnabled());
    CHECK_FALSE(w.view().isMaskMode());
    w.press(Qt::Key_M);
    CHECK(w.view().isMaskMode());
    CHECK(w.action("maskAction").isChecked());
    CHECK(QApplication::focusWidget() == &w.overlay());
    CHECK(w.overlay().isVisible());
    w.press(Qt::Key_M);
    CHECK_FALSE(w.view().isMaskMode());
    CHECK_FALSE(w.action("maskAction").isChecked());
    CHECK_FALSE(w.overlay().isVisible());
    // Entering opened no edit.
    CHECK(w.steps() == 1);
}

TEST_CASE("C and M switch between the crop mode and the mask mode", "[app][window][masks][crop]") {
    Window w;
    w.press(Qt::Key_M);
    REQUIRE(w.view().isMaskMode());
    w.press(Qt::Key_C);
    CHECK(w.view().isCropMode());
    CHECK_FALSE(w.view().isMaskMode());
    CHECK_FALSE(w.overlay().isVisible());
    CHECK_FALSE(w.masks().isEnabled());

    // A change in the crop mode is kept when M leaves it.
    QTest::mouseClick(w.panel().findChild<QAbstractButton*>("cropFlipHorizontal"), Qt::LeftButton);
    w.press(Qt::Key_M);
    CHECK_FALSE(w.view().isCropMode());
    CHECK(w.view().isMaskMode());
    CHECK(w.masks().isEnabled());
    CHECK(w.steps() == 2);
    CHECK(w.step(0) == "Crop");
}

TEST_CASE("The develop dock's tab follows the mask mode, and choosing a tab sets the mode",
          "[app][window][masks][tabs]") {
    Window w;
    auto& tabs = *w.window.findChild<QTabWidget*>("developTabs");
    REQUIRE(tabs.count() == 2);
    CHECK(tabs.tabText(0) == "Adjustments");
    CHECK(tabs.tabText(1) == "Masks");
    CHECK(tabs.currentIndex() == 0);
    CHECK_FALSE(w.masks().isVisible());

    // The key shows the tab, and the key back shows Adjustments.
    w.press(Qt::Key_M);
    CHECK(tabs.currentIndex() == 1);
    CHECK(w.masks().isVisible());
    w.press(Qt::Key_M);
    CHECK(tabs.currentIndex() == 0);
    CHECK_FALSE(w.masks().isVisible());

    // Esc leaves the mode, and the tab with it.
    w.press(Qt::Key_M);
    w.press(Qt::Key_Escape);
    CHECK_FALSE(w.view().isMaskMode());
    CHECK(tabs.currentIndex() == 0);

    // Choosing the tab enters the mode, the focus going to the photograph's overlay as with M.
    QTest::mouseClick(tabs.tabBar(), Qt::LeftButton, Qt::NoModifier,
                      tabs.tabBar()->tabRect(1).center());
    CHECK(tabs.currentIndex() == 1);
    CHECK(w.view().isMaskMode());
    CHECK(w.action("maskAction").isChecked());
    CHECK(w.overlay().hasFocus());

    // Drawing a mask selects it; leaving by the tab keeps the selection and ends the mode.
    w.draw();
    REQUIRE(w.overlay().selection().has_value());
    const auto selected = w.overlay().selection();
    QTest::mouseClick(tabs.tabBar(), Qt::LeftButton, Qt::NoModifier,
                      tabs.tabBar()->tabRect(0).center());
    CHECK(tabs.currentIndex() == 0);
    CHECK_FALSE(w.view().isMaskMode());
    CHECK_FALSE(w.action("maskAction").isChecked());
    CHECK(w.overlay().selection() == selected);

    // Entering the crop mode leaves the mask mode, and shows Adjustments.
    w.press(Qt::Key_M);
    REQUIRE(tabs.currentIndex() == 1);
    w.press(Qt::Key_C);
    CHECK(w.view().isCropMode());
    CHECK(tabs.currentIndex() == 0);
}

TEST_CASE("Leaving the mask mode from the mask list keeps the focus out of the dock",
          "[app][window][masks][tabs][focus]") {
    Window w;
    auto& tabs = *w.window.findChild<QTabWidget*>("developTabs");
    const auto inDock = [&] {
        const QWidget* focus = QApplication::focusWidget();
        return focus != nullptr && tabs.isAncestorOf(focus);
    };
    const auto leave = [&](const char* how) {
        INFO(how);
        w.press(Qt::Key_M);
        w.draw();
        REQUIRE(w.view().isMaskMode());
        w.masks().findChild<QListView*>("maskList")->setFocus();
        REQUIRE(w.masks().findChild<QListView*>("maskList")->hasFocus());
        const QString how_ = how;
        if (how_ == "tab") {
            QTest::mouseClick(tabs.tabBar(), Qt::LeftButton, Qt::NoModifier,
                              tabs.tabBar()->tabRect(0).center());
        } else if (how_ == "M") {
            w.press(Qt::Key_M);
        } else if (how_ == "Esc") {
            w.press(Qt::Key_Escape);
        } else {
            w.press(Qt::Key_C);
        }
        CHECK_FALSE(w.view().isMaskMode());
        CHECK_FALSE(inDock());
        if (how_ == "C") {
            w.press(Qt::Key_C);
        }
    };
    leave("tab");
    leave("M");
    leave("Esc");
    leave("C");
}

TEST_CASE("The develop dock holds both tabs without sideways scrolling",
          "[app][window][masks][tabs]") {
    Window w;
    auto& tabs = *w.window.findChild<QTabWidget*>("developTabs");
    w.press(Qt::Key_M);
    QCoreApplication::processEvents();
    for (int index = 0; index < tabs.count(); ++index) {
        tabs.setCurrentIndex(index);
        QCoreApplication::processEvents();
        const auto* scroll = qobject_cast<QScrollArea*>(tabs.widget(index));
        REQUIRE(scroll != nullptr);
        CHECK(scroll->horizontalScrollBar()->maximum() == 0);
        CHECK(scroll->focusPolicy() == Qt::NoFocus);
    }
    CHECK(tabs.minimumWidth() >= w.panel().minimumDockWidth());
}

TEST_CASE("Linear then a drag makes one step, selects the mask, and edits are steps of their own",
          "[app][window][masks]") {
    Window w;
    w.draw();
    REQUIRE(w.steps() == 2);
    CHECK(w.step(0) == "Add Linear 1");
    REQUIRE(w.masks().selectedMask().has_value());
    CHECK(w.overlay().selection() == w.masks().selectedMask());
    CHECK(w.overlay().tool() == MaskTool::None);
    CHECK_FALSE(w.button("maskLinear").isChecked());
    CHECK(w.view().isMaskMode());

    // Dragging the `to` end.
    w.drag(w.at(0.3, 0.7), w.at(0.3, 0.8));
    REQUIRE(w.steps() == 3);
    CHECK(w.step(0) == "Move Linear 1");

    // A row, and the render follows.
    const QImage before = w.view().wholeFrameImage();
    w.setRow("local.exposure", 0.5);
    REQUIRE(w.steps() == 4);
    CHECK(w.step(0) == "Linear 1: Exposure +0.50 EV");
    CHECK(w.waitForRenderOtherThan(before));
}

TEST_CASE("Undo keeps the selection while the mask exists; Redo brings it back unselected",
          "[app][window][masks][history]") {
    Window w;
    w.draw();
    w.setRow("local.exposure", 0.5);
    REQUIRE(w.steps() == 3);
    const auto selected = w.masks().selectedMask();
    REQUIRE(selected.has_value());

    w.press(Qt::Key_Z, Qt::ControlModifier);
    CHECK(w.masks().selectedMask() == selected);
    CHECK(w.overlay().selection() == selected);

    w.press(Qt::Key_Z, Qt::ControlModifier);
    // Past the creation the mask is gone, and so is the selection; the group shows its hint.
    CHECK_FALSE(w.masks().selectedMask().has_value());
    CHECK_FALSE(w.overlay().selection().has_value());
    CHECK(w.masks().findChild<QLabel*>("maskHint")->isVisibleTo(&w.masks()));

    w.press(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK_FALSE(w.masks().selectedMask().has_value());
    CHECK(w.masks().findChild<QListView*>("maskList")->model()->rowCount() == 1);
}

TEST_CASE("Delete removes the selected mask in one step; sixteen masks disable Linear",
          "[app][window][masks]") {
    Window w;
    w.draw();
    REQUIRE(w.steps() == 2);
    w.press(Qt::Key_Delete);
    CHECK(w.steps() == 3);
    CHECK(w.step(0) == "Remove Linear 1");
    CHECK_FALSE(w.masks().selectedMask().has_value());

    for (int count = 0; count < 16; ++count) {
        QTest::mouseClick(&w.button("maskLinear"), Qt::LeftButton);
        const QPointF click = w.at(0.1 + 0.05 * count, 0.5);
        w.mouse(QEvent::MouseButtonPress, click);
        w.mouse(QEvent::MouseButtonRelease, click, Qt::NoButton);
    }
    CHECK(w.masks().findChild<QListView*>("maskList")->model()->rowCount() == 16);
    CHECK_FALSE(w.button("maskLinear").isEnabled());
    CHECK(w.button("maskLinear").toolTip().contains("at most 16"));
}

TEST_CASE("Esc cancels a drag, then disarms the tool, then leaves the mode",
          "[app][window][masks]") {
    Window w;
    QTest::mouseClick(&w.button("maskRadial"), Qt::LeftButton);
    REQUIRE(w.overlay().tool() == MaskTool::Radial);
    w.mouse(QEvent::MouseButtonPress, w.at(0.5, 0.5));
    w.mouse(QEvent::MouseMove, w.at(0.5, 0.5) + QPointF(40.0, 0.0));
    CHECK(w.overlay().isDragging());

    w.press(Qt::Key_Escape);
    CHECK_FALSE(w.overlay().isDragging());
    CHECK(w.steps() == 1);
    CHECK(w.overlay().tool() == MaskTool::Radial);
    CHECK(w.view().isMaskMode());
    // The mask the drag had made is gone with the cancelled edit.
    CHECK(w.masks().findChild<QListView*>("maskList")->model()->rowCount() == 0);

    w.press(Qt::Key_Escape);
    CHECK(w.overlay().tool() == MaskTool::None);
    CHECK_FALSE(w.button("maskRadial").isChecked());
    CHECK(w.view().isMaskMode());

    w.press(Qt::Key_Escape);
    CHECK_FALSE(w.view().isMaskMode());
}

TEST_CASE("Stepping to another photograph in the middle of a drag leaves no step",
          "[app][window][masks]") {
    Window w;
    QTest::mouseClick(&w.button("maskLinear"), Qt::LeftButton);
    w.mouse(QEvent::MouseButtonPress, w.at(0.3, 0.3));
    w.mouse(QEvent::MouseMove, w.at(0.3, 0.5));
    REQUIRE(w.overlay().isDragging());

    w.press(Qt::Key_Right);
    CHECK_FALSE(w.overlay().isDragging());
    CHECK_FALSE(w.view().isMaskMode());
    CHECK(QTest::qWaitFor([&] { return w.window.windowTitle().startsWith("b.png"); }, 20000));
    CHECK(w.steps() == 1);
    CHECK_FALSE(w.window.isWindowModified());

    // And back: nothing of the drag was kept.
    w.press(Qt::Key_Left);
    CHECK(QTest::qWaitFor([&] { return w.window.windowTitle().startsWith("a.png"); }, 20000));
    CHECK(w.steps() == 1);
}

TEST_CASE("The history dock and the zoom stay on in the mask mode, and O shows the tint",
          "[app][window][masks]") {
    Window w;
    w.draw();
    REQUIRE(w.view().isMaskMode());
    CHECK(w.window.findChild<QDockWidget*>("HistoryDock")->isEnabled());
    CHECK(w.action("zoomInAction").isEnabled());
    CHECK(w.action("zoomFitAction").isEnabled());

    CHECK_FALSE(w.overlay().overlayShown());
    w.press(Qt::Key_O);
    CHECK(w.overlay().overlayShown());
    CHECK(w.button("maskOverlay").isChecked());
    w.overlay().updateCoverage();
    CHECK_FALSE(w.overlay().coverage().isNull());

    // With the focus elsewhere the window's own shortcut answers.
    w.panel().setFocus();
    w.press(Qt::Key_O);
    CHECK_FALSE(w.overlay().overlayShown());
    CHECK_FALSE(w.button("maskOverlay").isChecked());
}

TEST_CASE("A photograph whose sidecar holds a brush lists it, renders it and tints it, and "
          "the tab offers a brush",
          "[app][window][masks][brush]") {
    // A stroke wide enough to cover the whole photograph, with a lot of exposure.
    DevelopState state = withLocalAdjustmentAdded(
        DevelopState{}, test::brushMaskWith({Stroke{1.0F, 1.0F, 1.0F, false, {{0.5F, 0.5F}}}}));
    state = withLocalDelta(state, state.localAdjustments[0].id, "exposure", 2.0);

    QImage plain;
    {
        Window bare(640, 480, {}, false);
        plain = bare.view().wholeFrameImage();
    }
    Window w(640, 480, {}, false, &state);
    QListView& list = *w.masks().findChild<QListView*>("maskList");
    REQUIRE(list.model()->rowCount() == 1);
    CHECK(list.model()->index(0, 0).data(Qt::DisplayRole).toString() == "Brush 1");
    CHECK(w.waitForRenderOtherThan(plain));

    // Choosing it enters the mode; it has no handles to draw, and O tints it.
    emit list.clicked(list.model()->index(0, 0));
    REQUIRE(w.view().isMaskMode());
    REQUIRE(w.overlay().selection().has_value());
    w.press(Qt::Key_O);
    REQUIRE(w.overlay().overlayShown());
    w.overlay().updateCoverage();
    const QImage tint = w.overlay().coverage();
    REQUIRE_FALSE(tint.isNull());
    CHECK(qAlpha(tint.pixel(tint.width() / 2, tint.height() / 2)) > 100);
    w.press(Qt::Key_O);
    w.overlay().updateCoverage();
    CHECK(w.overlay().coverage().isNull());

    // Choosing a brush in the list arms the Brush tool, which shows the Brush block.
    CHECK(w.masks().findChild<QAbstractButton*>("maskLinear") != nullptr);
    CHECK(w.masks().findChild<QAbstractButton*>("maskRadial") != nullptr);
    REQUIRE(w.masks().findChild<QAbstractButton*>("maskBrush") != nullptr);
    CHECK(w.button("maskBrush").isChecked());
    CHECK(w.overlay().painting());
    CHECK(w.masks().findChild<QWidget*>("brushControls")->isVisibleTo(&w.masks()));
    // Nothing was added to the history by looking.
    CHECK(w.steps() <= 1);
}

TEST_CASE("Choosing a mask in the list enters the mode and selects it", "[app][window][masks]") {
    Window w;
    w.draw();
    w.press(Qt::Key_M);
    REQUIRE_FALSE(w.view().isMaskMode());
    // The selection outlives the mode.
    REQUIRE(w.masks().selectedMask().has_value());
    w.masks().setSelectedMask(std::nullopt);
    QListView& list = *w.masks().findChild<QListView*>("maskList");
    emit list.clicked(list.model()->index(0, 0));
    CHECK(w.view().isMaskMode());
    CHECK(w.overlay().selection().has_value());
    CHECK(QApplication::focusWidget() == &w.overlay());
}

TEST_CASE("The mask list takes Tab and the arrow keys, and keeps the focus",
          "[app][window][masks]") {
    Window w;
    w.draw();
    w.draw("maskRadial");
    REQUIRE(w.masks().selectedMask().has_value());
    QListView& list = *w.masks().findChild<QListView*>("maskList");
    CHECK(list.focusPolicy() == Qt::StrongFocus);
    CHECK(w.button("maskInvert").focusPolicy() == Qt::TabFocus);
    CHECK(w.button("maskDuplicate").focusPolicy() == Qt::TabFocus);

    // A keyboard user is in the list: Down changes the selection and the focus stays.
    list.setFocus();
    REQUIRE(QApplication::focusWidget() == &list);
    const auto before = w.masks().selectedMask();
    w.press(Qt::Key_Up);
    CHECK(w.masks().selectedMask() != before);
    CHECK(w.overlay().selection() == w.masks().selectedMask());
    CHECK(QApplication::focusWidget() == &list);
    w.press(Qt::Key_Down);
    CHECK(w.masks().selectedMask() == before);
    CHECK(QApplication::focusWidget() == &list);

    // A click hands the keys to the photograph.
    emit list.clicked(list.model()->index(0, 0));
    CHECK(QApplication::focusWidget() == &w.overlay());
}

TEST_CASE("An armed tool is disarmed when the list fills", "[app][window][masks]") {
    Window w;
    for (int count = 0; count < 16; ++count) {
        QTest::mouseClick(&w.button("maskLinear"), Qt::LeftButton);
        const QPointF click = w.at(0.1 + 0.05 * count, 0.5);
        w.mouse(QEvent::MouseButtonPress, click);
        w.mouse(QEvent::MouseButtonRelease, click, Qt::NoButton);
    }
    // Delete one, arm, and bring it back with Undo: sixteen with a tool armed.
    w.press(Qt::Key_Delete);
    QTest::mouseClick(&w.button("maskLinear"), Qt::LeftButton);
    REQUIRE(w.overlay().tool() == MaskTool::Linear);
    w.press(Qt::Key_Z, Qt::ControlModifier);
    CHECK(w.overlay().tool() == MaskTool::None);
    CHECK_FALSE(w.button("maskLinear").isChecked());
}

TEST_CASE("Leaving the mode by the action disarms the tool", "[app][window][masks]") {
    Window w;
    w.press(Qt::Key_M);
    REQUIRE(w.view().isMaskMode());
    QTest::mouseClick(&w.button("maskLinear"), Qt::LeftButton);
    w.action("maskAction").trigger();
    CHECK_FALSE(w.view().isMaskMode());
    CHECK(w.overlay().tool() == MaskTool::None);
    CHECK_FALSE(w.button("maskLinear").isChecked());
}

namespace {

/// Writes a widget's picture into the screenshot folder.
void shoot(QWidget& widget, const QString& folder, const QString& name) {
    QCoreApplication::processEvents();
    const QPixmap picture = widget.grab();
    REQUIRE_FALSE(picture.isNull());
    REQUIRE(picture.save(QDir(folder).filePath(name), "PNG"));
}

/// Scrolls the develop dock so that the Masks group is at the top.
void showMasksGroup(const Window& w) {
    QScrollArea* scroll = nullptr;
    for (auto* area : w.window.findChildren<QScrollArea*>()) {
        if (area->widget() == &w.panel()) {
            scroll = area;
        }
    }
    REQUIRE(scroll != nullptr);
    scroll->verticalScrollBar()->setValue(w.masks().mapTo(&w.panel(), QPoint(0, 0)).y() - 8);
    QCoreApplication::processEvents();
}

} // namespace

// Writes the pictures that show the mask mode when ARRAW_SCREENSHOT_DIR names a folder; run it
// by name: `arraw-widget-tests "[.screenshots]"`. Not a check, a way to look at the window.
TEST_CASE("Mask mode screenshots", "[.screenshots]") {
    const char* directory = std::getenv("ARRAW_SCREENSHOT_DIR");
    if (directory == nullptr) {
        SKIP("ARRAW_SCREENSHOT_DIR names no folder");
    }
    const QString folder = QString::fromUtf8(directory);
    REQUIRE(QDir().mkpath(folder));

    Window w(900, 600, QSize(1500, 900));
    // Until the render of the last edit is on screen.
    const auto settle = [&] {
        QTest::qWait(100);
        CHECK(QTest::qWaitFor([&] { return !w.window.renderIndicator().busy(); }, 30000));
        QTest::qWait(200);
    };
    const auto select = [&](int row) {
        auto* list = w.masks().findChild<QListView*>("maskList");
        emit list->clicked(list->model()->index(row, 0));
    };

    // Nothing yet: the group's hint, and the mode off.
    showMasksGroup(w);
    settle();
    shoot(w.masks(), folder, "masks-panel-none.png");

    // A graduated mask across the sky, with its handles and a little exposure.
    w.press(Qt::Key_M);
    w.draw();
    w.setRow("local.exposure", -0.8);
    w.setRow("local.contrast", 20.0);
    settle();
    shoot(w.masks(), folder, "masks-panel-selected.png");
    shoot(w.window, folder, "masks-linear-selected.png");

    // The tint of the same mask.
    w.press(Qt::Key_O);
    settle();
    shoot(w.window, folder, "masks-overlay-linear.png");
    w.press(Qt::Key_O);

    // Two more masks, none selected but one: pins on the others, and the group with three.
    QTest::mouseClick(&w.button("maskRadial"), Qt::LeftButton);
    w.drag(w.at(0.72, 0.62), w.at(0.72, 0.62) + QPointF(70.0, 0.0));
    w.setRow("local.exposure", 0.6);
    settle();
    shoot(w.masks(), folder, "masks-panel-two.png");
    QTest::mouseClick(&w.button("maskRadial"), Qt::LeftButton);
    w.drag(w.at(0.2, 0.8), w.at(0.2, 0.8) + QPointF(40.0, 0.0));
    w.setRow("local.saturation", 30.0);
    select(1);
    settle();
    shoot(w.window, folder, "masks-pins.png");
    shoot(w.masks(), folder, "masks-panel-three.png");

    // An ellipse, inverted, with the tint.
    select(2);
    w.press(Qt::Key_Delete);
    select(0);
    w.press(Qt::Key_Delete);
    select(0);
    CHECK(w.masks().findChild<QListView*>("maskList")->model()->rowCount() == 1);
    settle();
    shoot(w.window, folder, "masks-radial-selected.png");
    w.press(Qt::Key_O);
    w.masks().findChild<QAbstractButton*>("maskInvert")->click();
    settle();
    shoot(w.window, folder, "masks-overlay-radial-inverted.png");
    w.press(Qt::Key_O);

    // The same ellipse on a photograph turned, flipped and straightened.
    QTest::mouseClick(w.panel().findChild<QAbstractButton*>("cropTurnRight"), Qt::LeftButton);
    QTest::mouseClick(w.panel().findChild<QAbstractButton*>("cropFlipHorizontal"), Qt::LeftButton);
    w.setRow("straighten", 7.5);
    settle();
    shoot(w.window, folder, "masks-radial-turned-straightened.png");

    // Sixteen masks: the group at its limit.
    for (int count = 1; count < 16; ++count) {
        QTest::mouseClick(&w.button(count % 2 == 0 ? "maskLinear" : "maskRadial"), Qt::LeftButton);
        const QPointF click = w.at(0.1 + 0.05 * count, 0.3 + 0.02 * count);
        w.mouse(QEvent::MouseButtonPress, click);
        w.mouse(QEvent::MouseButtonRelease, click, Qt::NoButton);
    }
    select(15);
    showMasksGroup(w);
    settle();
    shoot(w.masks(), folder, "masks-panel-full.png");
}

// Measures how the window answers a handle drag and an Exposure drag with sixteen masks on a 24
// megapixel photograph; run it in a release build by name, with the timing log on:
// `QT_LOGGING_RULES="arraw.timing.debug=true" arraw-widget-tests "[.timing]"`. The log's
// `window.panel`, `mask.coverage` and `preview.render` spans between the two MARK lines are
// the answer. ARRAW_PREVIEW_DEVICE=cpu keeps the GPU out of it.
TEST_CASE("Mask mode timing with sixteen masks", "[.timing]") {
    Window w(6000, 4000, QSize(1500, 900), false);
    const auto idle = [&] {
        CHECK(QTest::qWaitFor([&] { return !w.window.renderIndicator().busy(); }, 240000));
        QTest::qWait(50);
    };
    const auto mark = [](const char* what) {
        std::fprintf(stderr, "MARK %s\n", what);
        std::fflush(stderr);
    };

    // Eight linear and eight radial masks, each with four deltas, as the engine's own measure.
    for (int index = 0; index < 16; ++index) {
        const bool linear = index % 2 == 0;
        QTest::mouseClick(&w.button(linear ? "maskLinear" : "maskRadial"), Qt::LeftButton);
        const QPointF click = w.at(0.1 + 0.05 * index, 0.2 + 0.04 * index);
        w.mouse(QEvent::MouseButtonPress, click);
        w.mouse(QEvent::MouseButtonRelease, click, Qt::NoButton);
        w.setRow("local.exposure", 0.3);
        w.setRow("local.contrast", 10.0);
        w.setRow(index % 4 < 2 ? "local.relativeTemperature" : "local.relativeTint", 8.0);
        w.setRow(index % 3 == 0 ? "local.highlights" : "local.shadows", 15.0);
    }
    idle();
    w.press(Qt::Key_O);
    idle();
    w.overlay().updateCoverage();

    // The last mask is radial and selected: its +x radius point is a handle, a sixth of the
    // shorter side from the click that made it.
    const QPointF centre = w.at(0.1 + 0.05 * 15, 0.2 + 0.04 * 15);
    const double radius = std::min(w.overlay().width(), w.overlay().height()) / 6.0;
    const QPointF handle = centre + QPointF(radius, 0.0);
    mark("handle drag");
    w.mouse(QEvent::MouseButtonPress, handle);
    for (int step = 1; step <= 40; ++step) {
        w.mouse(QEvent::MouseMove, handle + QPointF(step * 2.0, 0.0));
        QTest::qWait(20);
    }
    w.mouse(QEvent::MouseButtonRelease, handle + QPointF(80.0, 0.0), Qt::NoButton);
    idle();

    mark("exposure drag");
    for (auto* row : w.window.findChildren<SettingSlider*>()) {
        if (row->key() == "local.exposure") {
            auto* box = row->findChild<QDoubleSpinBox*>();
            for (int step = 0; step < 40; ++step) {
                box->setValue(-1.0 + step * 0.05);
                QTest::qWait(20);
            }
        }
    }
    w.panel().finishPendingEdit();
    idle();
    for (const QLabel* label : w.window.findChildren<QLabel*>()) {
        if (label->text().startsWith("Preview:")) {
            std::fprintf(stderr, "DEVICE %s\n", label->text().toUtf8().constData());
        }
    }
    mark("end");
}

TEST_CASE("Brush, then a drag, adds a brush and keeps the tool armed",
          "[app][window][masks][brush]") {
    Window w(640, 480, {}, false);
    QTest::mouseClick(&w.button("maskBrush"), Qt::LeftButton);
    REQUIRE(w.overlay().painting());
    w.drag(w.at(0.3, 0.3), w.at(0.6, 0.6));
    CHECK(w.step(0) == "Add Brush 1");
    CHECK(w.steps() == 2);
    CHECK(w.overlay().selection().has_value());
    CHECK(w.overlay().painting());
    CHECK(w.button("maskBrush").isChecked());
    // A second drag paints the same brush.
    w.drag(w.at(0.3, 0.6), w.at(0.6, 0.3));
    CHECK(w.step(0) == "Paint Brush 1");
    CHECK(w.steps() == 3);
}

TEST_CASE("With fifteen masks a brush drag makes the sixteenth and stays armed on it",
          "[app][window][masks][brush]") {
    DevelopState state;
    for (int count = 0; count < 15; ++count) {
        state = withLocalAdjustmentAdded(state, BrushMask{});
    }
    Window w(640, 480, QSize(1400, 900), false, &state);
    w.press(Qt::Key_M);
    REQUIRE(w.view().isMaskMode());
    QTest::mouseClick(&w.button("maskBrush"), Qt::LeftButton);
    REQUIRE(w.overlay().painting());
    DevelopState last;
    QObject::connect(&w.overlay(), &MaskOverlay::stateEdited,
                     [&last](const DevelopState& next) { last = next; });
    w.drag(w.at(0.2, 0.3), w.at(0.8, 0.7));
    CHECK(w.step(0) == "Add Brush 16");
    CHECK(w.overlay().painting());
    CHECK(w.button("maskBrush").isChecked());
    REQUIRE(w.overlay().selection().has_value());
    REQUIRE(last.localAdjustments.size() == 16);
    // The selection is the new sixteenth mask.
    CHECK(*w.overlay().selection() == last.localAdjustments.back().id);
    const auto& painted = std::get<BrushMask>(last.localAdjustments.back().shape);
    REQUIRE(painted.strokes != nullptr);
    // Not cut short: the whole drag is in the stroke.
    CHECK((*painted.strokes)[0].points.size() >= 4);
    CHECK(w.button("maskBrush").isEnabled());
    CHECK_FALSE(w.button("maskLinear").isEnabled());
}

TEST_CASE("Choosing a linear mask in the list disarms the brush", "[app][window][masks][brush]") {
    Window w;
    w.draw();
    QTest::mouseClick(&w.button("maskBrush"), Qt::LeftButton);
    REQUIRE(w.overlay().painting());
    QListView& list = *w.masks().findChild<QListView*>("maskList");
    emit list.clicked(list.model()->index(0, 0));
    CHECK_FALSE(w.overlay().painting());
    CHECK_FALSE(w.button("maskBrush").isChecked());
}

TEST_CASE("Alt held while another widget has the focus shows the erase bar on entering",
          "[app][window][masks][brush]") {
    Window w(640, 480, QSize(1400, 900));
    QTest::mouseClick(&w.button("maskBrush"), Qt::LeftButton);
    REQUIRE(w.overlay().painting());
    BrushSettings settings;
    settings.radius = 0.1F;
    w.overlay().setBrushSettings(settings);
    w.panel().setFocus();
    const QPointF centre = w.at(0.5, 0.5);
    const auto barDrawn = [&] {
        const QImage image = w.overlay().grab().toImage();
        return qAlpha(image.pixel(centre.toPoint())) > 0;
    };
    QEnterEvent enter(centre, centre, w.overlay().mapToGlobal(centre));
    QCoreApplication::sendEvent(&w.overlay(), &enter);
    CHECK_FALSE(barDrawn());

    QTest::keyPress(w.window.windowHandle(), Qt::Key_Alt, Qt::AltModifier);
    QEnterEvent again(centre, centre, w.overlay().mapToGlobal(centre));
    QCoreApplication::sendEvent(&w.overlay(), &again);
    const bool shown = barDrawn();
    QTest::keyRelease(w.window.windowHandle(), Qt::Key_Alt);
    CHECK(shown);
}

TEST_CASE("In the window a stroke repaints only where it painted", "[app][window][masks][brush]") {
    Window w(640, 480, QSize(1400, 900), false);
    w.press(Qt::Key_M);
    REQUIRE(w.view().isMaskMode());
    QTest::mouseClick(&w.button("maskBrush"), Qt::LeftButton);
    w.overlay().setOverlayShown(true);
    // A first stroke makes the brush; the next one is the one measured.
    w.drag(w.at(0.2, 0.5), w.at(0.22, 0.5));
    QTest::qWait(50);

    struct Recorder : QObject {
        QRect painted;
        bool eventFilter(QObject*, QEvent* event) override {
            // The first repaint only: a render that lands later repaints the view whole.
            if (event->type() == QEvent::Paint && painted.isNull()) {
                painted = static_cast<QPaintEvent*>(event)->rect();
            }
            return false;
        }
    } recorder;
    w.overlay().installEventFilter(&recorder);
    w.mouse(QEvent::MouseButtonPress, w.at(0.2, 0.5));
    recorder.painted = {};
    w.mouse(QEvent::MouseMove, w.at(0.22, 0.5));
    w.mouse(QEvent::MouseMove, w.at(0.24, 0.5));
    REQUIRE(QTest::qWaitFor([&] { return !recorder.painted.isNull(); }, 5000));
    REQUIRE_FALSE(recorder.painted.isNull());
    CHECK(recorder.painted.right() < w.overlay().width() / 2);
    CHECK(recorder.painted.height() < w.overlay().height());
    // What was painted covers the dab.
    CHECK(recorder.painted.contains(w.at(0.24, 0.5).toPoint()));
    w.mouse(QEvent::MouseButtonRelease, w.at(0.24, 0.5), Qt::NoButton);
}

TEST_CASE("Choosing a tool in mid-stroke cancels the stroke", "[app][window][masks][brush]") {
    Window w(640, 480, {}, false);
    QTest::mouseClick(&w.button("maskBrush"), Qt::LeftButton);
    w.mouse(QEvent::MouseButtonPress, w.at(0.3, 0.3));
    w.mouse(QEvent::MouseMove, w.at(0.4, 0.4));
    REQUIRE(w.overlay().stroking());
    QTest::mouseClick(&w.button("maskLinear"), Qt::LeftButton);
    CHECK_FALSE(w.overlay().stroking());
    CHECK(w.steps() == 1);
}

namespace {

/// Forgets the brush settings the earlier windows stored, then again when it goes out of scope.
struct BrushSettingsReset {
    BrushSettingsReset() {
        QSettings().remove("brush");
    }
    ~BrushSettingsReset() {
        QSettings().remove("brush");
    }
};

/// A brush of one stroke of a given radius that has used `area` of the swept-area budget.
DevelopState brushUsing(double area, float radius) {
    Stroke stroke{radius, 0.5F, 1.0F, false, {{0.1F, 0.5F}}};
    double length = 0.0;
    const double wanted = area / static_cast<double>(radius);
    bool right = true;
    while (length < wanted) {
        const double step = std::min(0.8, wanted - length);
        const SensorPoint last = stroke.points.back();
        stroke.points.push_back({static_cast<float>(right ? last.u + step : last.u - step), 0.5F});
        right = !right;
        length += step;
    }
    return withLocalAdjustmentAdded(DevelopState{}, test::brushMaskWith({stroke}));
}

/// Starts a stroke at one place and moves to another, leaving the button down.
void startStroke(const Window& w, QPointF from, QPointF to) {
    w.mouse(QEvent::MouseButtonPress, from);
    for (int step = 1; step <= 4; ++step) {
        w.mouse(QEvent::MouseMove, from + (to - from) * step / 4.0);
    }
}

/// One line of the timing trace.
struct TimingEvent {
    double t = 0.0; ///< Milliseconds since the trace's origin.
    std::uint64_t span = 0;
    std::uint64_t request = 0;
    std::string stage;
    std::string event;
    std::string detail;
};

/// One span of the timing trace, begin to end.
struct SpanRecord {
    std::string stage;
    double begin = 0.0;
    double end = 0.0;
    std::uint64_t request = 0;
    std::string note; ///< The last note written in the span.
    int level = -1;   ///< The pyramid level a render span noted, or -1.

    [[nodiscard]] double ms() const {
        return end - begin;
    }
};

/// Collects the timing trace in process, as the session computes its metrics itself.
class TimingCollector {
public:
    TimingCollector()
        : category_(const_cast<QLoggingCategory&>(detail::timingLog())),
          wasEnabled_(category_.isDebugEnabled()) {
        category_.setEnabled(QtDebugMsg, true);
        previous_ = qInstallMessageHandler(&TimingCollector::handle);
        previousHandler() = previous_;
    }
    ~TimingCollector() {
        qInstallMessageHandler(previous_);
        category_.setEnabled(QtDebugMsg, wasEnabled_);
    }
    TimingCollector(const TimingCollector&) = delete;
    TimingCollector& operator=(const TimingCollector&) = delete;

    void clear() const {
        const std::scoped_lock lock(mutex());
        events().clear();
    }

    /// Gives the spans closed since the last clear, in the order they began.
    [[nodiscard]] std::vector<SpanRecord> spans() const {
        std::vector<TimingEvent> copy;
        {
            const std::scoped_lock lock(mutex());
            copy = events();
        }
        std::vector<SpanRecord> out;
        std::vector<std::pair<std::uint64_t, std::size_t>> open;
        const auto find = [&open](std::uint64_t id) {
            for (auto& entry : open) {
                if (entry.first == id) {
                    return &entry;
                }
            }
            return static_cast<std::pair<std::uint64_t, std::size_t>*>(nullptr);
        };
        std::vector<bool> closed;
        for (const TimingEvent& event : copy) {
            if (event.event == "begin") {
                out.push_back({event.stage, event.t, event.t, event.request, {}, -1});
                closed.push_back(false);
                open.emplace_back(event.span, out.size() - 1);
            } else if (auto* entry = find(event.span)) {
                if (event.event == "note") {
                    out[entry->second].note = event.detail;
                    if (event.detail.rfind("level=", 0) == 0) {
                        out[entry->second].level = std::atoi(event.detail.c_str() + 6);
                    }
                } else {
                    out[entry->second].end = event.t;
                    closed[entry->second] = true;
                    *entry = open.back();
                    open.pop_back();
                }
            }
        }
        std::vector<SpanRecord> done;
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (closed[i]) {
                done.push_back(std::move(out[i]));
            }
        }
        return done;
    }

private:
    static std::mutex& mutex() {
        static std::mutex instance;
        return instance;
    }
    static std::vector<TimingEvent>& events() {
        static std::vector<TimingEvent> instance;
        return instance;
    }
    static QtMessageHandler& previousHandler() {
        static QtMessageHandler instance = nullptr;
        return instance;
    }

    static std::string field(const std::string& text, const std::string& key) {
        const std::string needle = (key == "t" ? "" : " ") + key + "=";
        const auto at = text.find(needle);
        if (at == std::string::npos) {
            return {};
        }
        const auto from = at + needle.size();
        return text.substr(from, text.find(' ', from) - from);
    }

    static void handle(QtMsgType type, const QMessageLogContext& context, const QString& message) {
        if (context.category == nullptr || std::string_view(context.category) != "arraw.timing") {
            if (previousHandler() != nullptr) {
                previousHandler()(type, context, message);
            }
            return;
        }
        const std::string text = message.toStdString();
        TimingEvent event;
        event.t = std::atof(field(text, "t").c_str());
        event.span = std::strtoull(field(text, "span").c_str(), nullptr, 10);
        event.request = std::strtoull(field(text, "request").c_str(), nullptr, 10);
        event.stage = field(text, "stage");
        event.event = field(text, "event");
        const auto detail = text.find(" detail=");
        if (detail != std::string::npos) {
            event.detail = text.substr(detail + 8);
        }
        const std::scoped_lock lock(mutex());
        events().push_back(std::move(event));
    }

    QLoggingCategory& category_;
    bool wasEnabled_;
    QtMessageHandler previous_ = nullptr;
};

/// Pyramid levels of the renders the window did since a collector was cleared, in order.
std::vector<int> renderLevels(const TimingCollector& collector) {
    std::vector<int> levels;
    for (const SpanRecord& span : collector.spans()) {
        if (span.stage == "preview.render" && span.level >= 0) {
            levels.push_back(span.level);
        }
    }
    return levels;
}

void armBrush(const Window& w) {
    QTest::mouseClick(&w.button("maskBrush"), Qt::LeftButton);
    REQUIRE(w.overlay().painting());
}

} // namespace

TEST_CASE("Strokes make steps, and Esc starts a new brush; undo and redo take them back",
          "[app][window][masks][brush]") {
    const BrushSettingsReset reset;
    Window w(640, 480, {}, false);
    armBrush(w);
    w.drag(w.at(0.3, 0.3), w.at(0.6, 0.6));
    REQUIRE(w.step(0) == "Add Brush 1");
    const QImage plain = w.view().wholeFrameImage();
    // The brush carries nothing yet; with a delta the render shows where it painted.
    w.setRow("local.exposure", 2.0);
    REQUIRE(w.waitForRenderOtherThan(plain));
    const QImage lit = w.view().wholeFrameImage();
    w.drag(w.at(0.3, 0.7), w.at(0.8, 0.7));
    CHECK(w.step(0) == "Paint Brush 1");
    CHECK(w.waitForRenderOtherThan(lit));

    w.press(Qt::Key_Escape);
    CHECK(w.overlay().painting());
    w.drag(w.at(0.2, 0.2), w.at(0.4, 0.2));
    CHECK(w.step(0) == "Add Brush 2");
    QListView& list = *w.masks().findChild<QListView*>("maskList");
    CHECK(list.model()->rowCount() == 2);

    w.action("undoAction").trigger();
    CHECK(list.model()->rowCount() == 1);
    w.action("redoAction").trigger();
    CHECK(list.model()->rowCount() == 2);
}

TEST_CASE("The brush stays armed with the new mask selected after its first stroke",
          "[app][window][masks][brush]") {
    const BrushSettingsReset reset;
    Window w(640, 480, {}, false);
    armBrush(w);
    w.drag(w.at(0.3, 0.3), w.at(0.6, 0.6));
    CHECK(w.overlay().painting());
    CHECK(w.overlay().selection().has_value());
}

TEST_CASE("C, the arrow keys and Esc in the middle of a stroke leave no step",
          "[app][window][masks][brush]") {
    const BrushSettingsReset reset;
    Window w;
    armBrush(w);
    SECTION("Esc") {
        startStroke(w, w.at(0.3, 0.3), w.at(0.5, 0.5));
        REQUIRE(w.overlay().stroking());
        w.press(Qt::Key_Escape);
        CHECK_FALSE(w.overlay().stroking());
        CHECK(w.overlay().painting());
    }
    SECTION("C") {
        startStroke(w, w.at(0.3, 0.3), w.at(0.5, 0.5));
        REQUIRE(w.overlay().stroking());
        w.press(Qt::Key_C);
        CHECK_FALSE(w.overlay().stroking());
        CHECK(w.view().isCropMode());
    }
    SECTION("The arrow keys") {
        startStroke(w, w.at(0.3, 0.3), w.at(0.5, 0.5));
        REQUIRE(w.overlay().stroking());
        w.press(Qt::Key_Right);
        CHECK_FALSE(w.overlay().stroking());
        CHECK(QTest::qWaitFor([&] { return w.window.windowTitle().startsWith("b.png"); }, 20000));
    }
    CHECK(w.steps() == 1);
    QListView& list = *w.masks().findChild<QListView*>("maskList");
    CHECK(list.model()->rowCount() == 0);
}

TEST_CASE("Undo in the middle of a stroke commits it and takes it back; Redo is off from the press",
          "[app][window][masks][brush]") {
    const BrushSettingsReset reset;
    Window w(640, 480, {}, false);
    armBrush(w);
    w.drag(w.at(0.3, 0.3), w.at(0.6, 0.6));
    w.action("undoAction").trigger();
    REQUIRE(w.action("redoAction").isEnabled());

    startStroke(w, w.at(0.3, 0.3), w.at(0.5, 0.5));
    REQUIRE(w.overlay().stroking());
    CHECK_FALSE(w.action("redoAction").isEnabled());
    const QKeyCombination redoKey = w.action("redoAction").shortcut()[0];
    w.press(redoKey.key(), redoKey.keyboardModifiers());
    CHECK(w.overlay().stroking());

    QListView& list = *w.masks().findChild<QListView*>("maskList");
    REQUIRE(list.model()->rowCount() == 1);
    w.action("undoAction").trigger();
    CHECK_FALSE(w.overlay().stroking());
    CHECK(list.model()->rowCount() == 0);
    // The stroke was committed as a step before it was undone, so Redo restores it.
    REQUIRE(w.action("redoAction").isEnabled());
    w.action("redoAction").trigger();
    CHECK(list.model()->rowCount() == 1);
    CHECK(w.step(0) == "Add Brush 1");
}

TEST_CASE("A rotation or a paste in the middle of a stroke follows the stroke's own step",
          "[app][window][masks][brush]") {
    const BrushSettingsReset reset;
    Window w(640, 480, {}, false);
    armBrush(w);
    w.drag(w.at(0.3, 0.3), w.at(0.6, 0.6));
    REQUIRE(w.steps() == 2);

    SECTION("Rotate") {
        startStroke(w, w.at(0.3, 0.7), w.at(0.6, 0.7));
        REQUIRE(w.overlay().stroking());
        w.press(Qt::Key_BracketRight, Qt::ControlModifier);
        CHECK_FALSE(w.overlay().stroking());
        REQUIRE(w.steps() == 4);
        CHECK(w.step(1) == "Paint Brush 1");
        CHECK(w.step(0) != "Paint Brush 1");
    }
    SECTION("Paste") {
        // Something to paste: the copied settings differ from the photograph's now.
        QDialog* seen = nullptr;
        QTimer::singleShot(0, [&seen] {
            seen = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (seen != nullptr) {
                seen->accept();
            }
        });
        w.action("copySettingsAction").trigger();
        REQUIRE(seen != nullptr);
        w.setRow("exposure", 1.0);
        const int before = w.steps();
        startStroke(w, w.at(0.3, 0.7), w.at(0.6, 0.7));
        REQUIRE(w.overlay().stroking());
        w.action("pasteSettingsAction").trigger();
        CHECK_FALSE(w.overlay().stroking());
        REQUIRE(w.steps() == before + 2);
        CHECK(w.step(1) == "Paint Brush 1");
        CHECK(w.step(0) == "Paste Settings");
    }
}

TEST_CASE("A brush at its budget refuses a press, and a stroke that reaches it commits what fits",
          "[app][window][masks][brush]") {
    const BrushSettingsReset reset;
    BrushSettings big;
    big.radius = 0.4F;
    SECTION("Refused at the press") {
        const DevelopState state = brushUsing(3.95, big.radius);
        Window w(640, 480, QSize(1400, 900), false, &state);
        QListView& list = *w.masks().findChild<QListView*>("maskList");
        emit list.clicked(list.model()->index(0, 0));
        REQUIRE(w.overlay().painting());
        w.panel().setBrushSettings(big);
        w.overlay().setBrushSettings(big);
        const int before = w.steps();
        w.window.findChild<StatusLine*>()->clearMessage();
        w.mouse(QEvent::MouseButtonPress, w.at(0.3, 0.5));
        w.mouse(QEvent::MouseButtonRelease, w.at(0.3, 0.5), Qt::NoButton);
        CHECK_FALSE(w.overlay().stroking());
        CHECK(w.steps() == before);
        CHECK(w.window.findChild<StatusLine*>()->message().contains("full"));
    }
    SECTION("Stopped by the budget") {
        const DevelopState state = brushUsing(3.8, big.radius);
        Window w(640, 480, QSize(1400, 900), false, &state);
        QListView& list = *w.masks().findChild<QListView*>("maskList");
        emit list.clicked(list.model()->index(0, 0));
        REQUIRE(w.overlay().painting());
        w.panel().setBrushSettings(big);
        w.overlay().setBrushSettings(big);
        const int before = w.steps();
        DevelopState last;
        QObject::connect(&w.overlay(), &MaskOverlay::stateEdited,
                         [&last](const DevelopState& next) { last = next; });
        w.window.findChild<StatusLine*>()->clearMessage();
        w.drag(w.at(0.1, 0.5), w.at(0.9, 0.5));
        CHECK(w.steps() == before + 1);
        CHECK(w.step(0) == "Paint Brush 1");
        CHECK(w.overlay().painting());
        // Recomputed at the release: the brush is at its budget now.
        CHECK(w.overlay().cursor().shape() == Qt::ForbiddenCursor);
        CHECK(w.window.findChild<StatusLine*>()->message().contains("stops"));
        REQUIRE(last.localAdjustments.size() == 1);
        const auto& brush = std::get<BrushMask>(last.localAdjustments[0].shape);
        REQUIRE(brush.strokes != nullptr);
        CHECK(brush.strokes->size() == 2);
        CHECK(brush.strokes->budget().sweptArea <= maximumSweptAreaPerMask);
    }
}

TEST_CASE("The brush's keys change the settings, with either widget focused, and are kept",
          "[app][window][masks][brush]") {
    const BrushSettingsReset reset;
    const BrushSettings start;
    {
        Window w(640, 480, QSize(1400, 900), false);
        armBrush(w);
        for (int count = 0; count < 3; ++count) {
            w.press(Qt::Key_BracketRight);
        }
        CHECK(w.masks().brushSettings().radius ==
              Catch::Approx(start.radius * 1.25F * 1.25F * 1.25F));
        // From the photograph view as well.
        w.view().setFocus();
        w.press(Qt::Key_BracketRight);
        CHECK(w.masks().brushSettings().radius ==
              Catch::Approx(start.radius * 1.25F * 1.25F * 1.25F * 1.25F));
        w.press(Qt::Key_BracketLeft);
        w.press(Qt::Key_BraceRight);
        CHECK(w.masks().brushSettings().hardness == Catch::Approx(start.hardness + 0.1F));
        w.press(Qt::Key_BraceLeft);
        w.press(Qt::Key_BraceLeft);
        CHECK(w.masks().brushSettings().hardness == Catch::Approx(start.hardness - 0.1F));
        w.press(Qt::Key_E);
        CHECK(w.masks().brushSettings().erase);
        w.press(Qt::Key_E);
        CHECK_FALSE(w.masks().brushSettings().erase);

        // The next stroke has the new radius.
        DevelopState made;
        QObject::connect(&w.overlay(), &MaskOverlay::stateEdited,
                         [&made](const DevelopState& next) { made = next; });
        w.drag(w.at(0.3, 0.3), w.at(0.5, 0.5));
        REQUIRE(made.localAdjustments.size() == 1);
        const auto& brush = std::get<BrushMask>(made.localAdjustments[0].shape);
        REQUIRE(brush.strokes != nullptr);
        CHECK((*brush.strokes)[0].radius == Catch::Approx(start.radius * 1.25F * 1.25F * 1.25F));
    }
    // A new window starts with what was left; Erase is not kept.
    Window again(640, 480, {}, false);
    CHECK(again.masks().brushSettings().radius ==
          Catch::Approx(start.radius * 1.25F * 1.25F * 1.25F));
    CHECK_FALSE(again.masks().brushSettings().erase);
    // Outside the brush the keys do nothing.
    const float before = again.masks().brushSettings().radius;
    again.press(Qt::Key_M);
    REQUIRE(again.view().isMaskMode());
    QTest::mouseClick(&again.button("maskLinear"), Qt::LeftButton);
    again.press(Qt::Key_BracketRight);
    CHECK(again.masks().brushSettings().radius == before);
}

TEST_CASE("While a stroke is under way no thumbnail follows; the commit brings one",
          "[app][window][masks][brush]") {
    const BrushSettingsReset reset;
    Window w(640, 480, QSize(1400, 900), false);
    armBrush(w);
    BrushSettings big;
    big.radius = 0.3F;
    w.panel().setBrushSettings(big);
    w.overlay().setBrushSettings(big);
    w.drag(w.at(0.2, 0.5), w.at(0.25, 0.5));
    w.setRow("local.exposure", 2.0);
    const QImage painted = w.view().wholeFrameImage();
    QTest::qWait(200);
    REQUIRE(QTest::qWaitFor([&] { return !w.window.renderIndicator().busy(); }, 20000));
    const std::filesystem::path path = w.folder.file("a.png");
    const QImage before = w.window.findChild<FilmStrip*>()->thumbnail(path);

    startStroke(w, w.at(0.5, 0.2), w.at(0.5, 0.8));
    REQUIRE(w.overlay().stroking());
    // The renders come, and the view follows them, but not the strip.
    REQUIRE(w.waitForRenderOtherThan(painted));
    QTest::qWait(200);
    CHECK(w.window.findChild<FilmStrip*>()->thumbnail(path) == before);
    w.mouse(QEvent::MouseButtonRelease, w.at(0.5, 0.8), Qt::NoButton);
    CHECK(QTest::qWaitFor(
        [&] { return w.window.findChild<FilmStrip*>()->thumbnail(path) != before; }, 20000));
}

TEST_CASE("The render after a stroke's commit is the one of the committed state",
          "[app][window][masks][brush]") {
    const BrushSettingsReset reset;
    DevelopState committed;
    QImage shown;
    {
        Window w(640, 480, QSize(1400, 900), false);
        armBrush(w);
        w.drag(w.at(0.2, 0.5), w.at(0.25, 0.5));
        w.setRow("local.exposure", 2.0);
        QObject::connect(&w.overlay(), &MaskOverlay::stateEdited,
                         [&committed](const DevelopState& next) { committed = next; });
        w.drag(w.at(0.2, 0.3), w.at(0.8, 0.7));
        QTest::qWait(100);
        REQUIRE(QTest::qWaitFor([&] { return !w.window.renderIndicator().busy(); }, 20000));
        shown = w.view().wholeFrameImage();
    }
    Window direct(640, 480, QSize(1400, 900), false, &committed);
    REQUIRE(QTest::qWaitFor([&] { return !direct.window.renderIndicator().busy(); }, 20000));
    CHECK(direct.view().wholeFrameImage() == shown);
}

TEST_CASE("During a stroke the pictures are coarser; the commit's is full size",
          "[app][window][masks][brush]") {
    const BrushSettingsReset reset;
    const TimingCollector collector;
    DevelopState committed;
    QImage shown;
    QSize full;
    {
        Window w(4000, 3000, QSize(1400, 900), false);
        armBrush(w);
        BrushSettings big;
        big.radius = 0.2F;
        w.panel().setBrushSettings(big);
        w.overlay().setBrushSettings(big);
        w.drag(w.at(0.2, 0.5), w.at(0.25, 0.5));
        w.setRow("local.exposure", 2.0);
        QTest::qWait(100);
        REQUIRE(QTest::qWaitFor([&] { return !w.window.renderIndicator().busy(); }, 60000));
        full = w.view().wholeFrameImage().size();
        const std::vector<int> before = renderLevels(collector);
        REQUIRE_FALSE(before.empty());
        const int fullLevel = before.back();
        REQUIRE(fullLevel > 0);

        QObject::connect(&w.overlay(), &MaskOverlay::stateEdited,
                         [&committed](const DevelopState& next) { committed = next; });
        collector.clear();
        startStroke(w, w.at(0.3, 0.3), w.at(0.7, 0.7));
        REQUIRE(w.overlay().stroking());
        REQUIRE(QTest::qWaitFor([&] { return !renderLevels(collector).empty(); }, 60000));
        QTest::qWait(300);
        // Every render meanwhile is of a coarser level than the full one.
        const std::vector<int> during = renderLevels(collector);
        for (const int level : during) {
            CHECK(level > fullLevel);
        }
        CHECK(w.view().wholeFrameImage().size().width() < full.width());
        // No queued stroke render may start after the clear and be counted as the commit's.
        REQUIRE(QTest::qWaitFor([&] { return !w.window.renderIndicator().busy(); }, 60000));
        collector.clear();
        w.mouse(QEvent::MouseButtonRelease, w.at(0.7, 0.7), Qt::NoButton);
        // The commit's render replaces the coarse one with the full size.
        REQUIRE(QTest::qWaitFor([&] { return w.view().wholeFrameImage().size() == full; }, 60000));
        QTest::qWait(100);
        REQUIRE(QTest::qWaitFor([&] { return !w.window.renderIndicator().busy(); }, 60000));
        for (const int level : renderLevels(collector)) {
            CHECK(level == fullLevel);
        }
        shown = w.view().wholeFrameImage();
        CHECK(shown.size() == full);
    }
    Window direct(4000, 3000, QSize(1400, 900), false, &committed);
    REQUIRE(QTest::qWaitFor([&] { return !direct.window.renderIndicator().busy(); }, 60000));
    CHECK(direct.view().wholeFrameImage() == shown);
}

TEST_CASE("A stroke's pictures are full size where the full level is the source's",
          "[app][window][masks][brush]") {
    const BrushSettingsReset reset;
    const TimingCollector collector;
    Window w(1000, 750, QSize(1400, 900), false);
    armBrush(w);
    w.drag(w.at(0.2, 0.5), w.at(0.25, 0.5));
    QTest::qWait(100);
    REQUIRE(QTest::qWaitFor([&] { return !w.window.renderIndicator().busy(); }, 20000));
    const QSize full = w.view().wholeFrameImage().size();
    collector.clear();
    startStroke(w, w.at(0.3, 0.3), w.at(0.7, 0.7));
    REQUIRE(w.overlay().stroking());
    REQUIRE(QTest::qWaitFor([&] { return !renderLevels(collector).empty(); }, 20000));
    QTest::qWait(200);
    for (const int level : renderLevels(collector)) {
        CHECK(level == 0);
    }
    CHECK(w.view().wholeFrameImage().size() == full);
    w.mouse(QEvent::MouseButtonRelease, w.at(0.7, 0.7), Qt::NoButton);
}

TEST_CASE("Outside a stroke the pictures are of the full level", "[app][window][masks][brush]") {
    const BrushSettingsReset reset;
    const TimingCollector collector;
    Window w(4000, 3000, QSize(1400, 900), false);
    armBrush(w);
    w.drag(w.at(0.2, 0.5), w.at(0.25, 0.5));
    QTest::qWait(100);
    REQUIRE(QTest::qWaitFor([&] { return !w.window.renderIndicator().busy(); }, 60000));
    const std::vector<int> settled = renderLevels(collector);
    REQUIRE_FALSE(settled.empty());
    const int fullLevel = settled.back();
    const QSize full = w.view().wholeFrameImage().size();
    const QImage before = w.view().wholeFrameImage();
    collector.clear();
    // A drag of a row's value, with the brush armed, is no stroke.
    for (auto* row : w.window.findChildren<SettingSlider*>()) {
        if (row->key() == "local.exposure") {
            for (const double value : {0.5, 1.0, 1.5}) {
                row->findChild<QDoubleSpinBox*>()->setValue(value);
                CHECK_FALSE(w.overlay().stroking());
                QTest::qWait(100);
            }
        }
    }
    w.panel().finishPendingEdit();
    CHECK(w.waitForRenderOtherThan(before));
    REQUIRE(QTest::qWaitFor([&] { return !w.window.renderIndicator().busy(); }, 60000));
    CHECK(w.view().wholeFrameImage().size() == full);
    const std::vector<int> delta = renderLevels(collector);
    CHECK_FALSE(delta.empty());
    for (const int level : delta) {
        CHECK(level == fullLevel);
    }
    // A handle drag of a radial mask is no stroke either.
    QTest::mouseClick(&w.button("maskRadial"), Qt::LeftButton);
    const QPointF click = w.at(0.5, 0.5);
    w.mouse(QEvent::MouseButtonPress, click);
    w.mouse(QEvent::MouseButtonRelease, click, Qt::NoButton);
    w.setRow("local.exposure", 0.5);
    REQUIRE(QTest::qWaitFor([&] { return !w.window.renderIndicator().busy(); }, 60000));
    collector.clear();
    const double radius = std::min(w.overlay().width(), w.overlay().height()) / 6.0;
    const QPointF handle = click + QPointF(radius, 0.0);
    w.mouse(QEvent::MouseButtonPress, handle);
    for (int step = 1; step <= 10; ++step) {
        w.mouse(QEvent::MouseMove, handle + QPointF(step * 4.0, 0.0));
        QTest::qWait(30);
    }
    w.mouse(QEvent::MouseButtonRelease, handle + QPointF(40.0, 0.0), Qt::NoButton);
    REQUIRE(QTest::qWaitFor([&] { return !w.window.renderIndicator().busy(); }, 60000));
    const std::vector<int> handles = renderLevels(collector);
    CHECK_FALSE(handles.empty());
    for (const int level : handles) {
        CHECK(level == fullLevel);
    }
    CHECK(w.view().wholeFrameImage().size() == full);
}

TEST_CASE("A delta drag with the brush armed is not a stroke", "[app][window][masks][brush]") {
    const BrushSettingsReset reset;
    Window w(640, 480, {}, false);
    armBrush(w);
    w.drag(w.at(0.3, 0.3), w.at(0.6, 0.6));
    REQUIRE(w.overlay().painting());
    const QImage before = w.view().wholeFrameImage();
    // A drag of the row's value, as a slider's drag is: it ends with the last value shown.
    for (auto* row : w.window.findChildren<SettingSlider*>()) {
        if (row->key() == "local.exposure") {
            row->findChild<QDoubleSpinBox*>()->setValue(1.0);
            CHECK_FALSE(w.overlay().stroking());
            row->findChild<QDoubleSpinBox*>()->setValue(2.0);
        }
    }
    w.panel().finishPendingEdit();
    CHECK_FALSE(w.overlay().stroking());
    CHECK(w.waitForRenderOtherThan(before));
    QTest::qWait(100);
    REQUIRE(QTest::qWaitFor([&] { return !w.window.renderIndicator().busy(); }, 20000));
    const QImage after = w.view().wholeFrameImage();
    CHECK(after != before);
}

TEST_CASE("A brush that changes nothing is tinted whether O is on or not",
          "[app][window][masks][brush]") {
    const BrushSettingsReset reset;
    SECTION("Without deltas") {
        Window w(640, 480, QSize(1400, 900), false);
        armBrush(w);
        REQUIRE_FALSE(w.overlay().overlayShown());
        w.drag(w.at(0.3, 0.3), w.at(0.6, 0.6));
        w.overlay().updateCoverage();
        CHECK_FALSE(w.overlay().overlayShown());
        CHECK_FALSE(w.overlay().coverage().isNull());
        // Once it carries something the tint follows O again.
        w.setRow("local.exposure", 1.0);
        w.overlay().updateCoverage();
        QTest::qWait(10);
        w.overlay().updateCoverage();
        CHECK(w.overlay().coverage().isNull());
    }
    SECTION("A disabled brush") {
        DevelopState state = withLocalAdjustmentAdded(
            DevelopState{}, test::brushMaskWith({Stroke{0.3F, 1.0F, 1.0F, false, {{0.5F, 0.5F}}}}));
        state = withLocalDelta(state, state.localAdjustments[0].id, "exposure", 1.0);
        state = withLocalAdjustmentEnabled(state, state.localAdjustments[0].id, false);
        Window w(640, 480, QSize(1400, 900), false, &state);
        QListView& list = *w.masks().findChild<QListView*>("maskList");
        emit list.clicked(list.model()->index(0, 0));
        REQUIRE(w.overlay().painting());
        REQUIRE_FALSE(w.overlay().overlayShown());
        // Arming alone, with no press yet, brings the tint.
        CHECK(QTest::qWaitFor([&w] { return !w.overlay().coverage().isNull(); }));
        // Disarmed with the brush still selected, the tint goes; armed again, it comes back.
        QTest::mouseClick(&w.button("maskBrush"), Qt::LeftButton);
        REQUIRE_FALSE(w.overlay().painting());
        CHECK(QTest::qWaitFor([&w] { return w.overlay().coverage().isNull(); }));
        QTest::mouseClick(&w.button("maskBrush"), Qt::LeftButton);
        REQUIRE(w.overlay().painting());
        CHECK(QTest::qWaitFor([&w] { return !w.overlay().coverage().isNull(); }));
    }
}

namespace {

/// Median, 95th and 99th percentile and maximum of some milliseconds.
struct Spread {
    double median = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
    std::size_t count = 0;
};

Spread spreadOf(std::vector<double> values) {
    Spread out;
    out.count = values.size();
    if (values.empty()) {
        return out;
    }
    std::sort(values.begin(), values.end());
    const auto at = [&values](double q) {
        return values[std::min(values.size() - 1,
                               static_cast<std::size_t>(std::ceil(q * values.size())) -
                                   (q * values.size() > 0.0 ? 1 : 0))];
    };
    out.median = at(0.5);
    out.p95 = at(0.95);
    out.p99 = at(0.99);
    out.max = values.back();
    return out;
}

/// Prints one verdict line: a measure against its target.
void verdict(const char* phase, const char* what, double value, const char* unit, double target) {
    std::printf("RESULT %s | %s = %.2f %s | target <= %.2f | %s\n", phase, what, value, unit,
                target, value <= target ? "met" : "MISSED");
}

/// Resident set of the process, in MiB.
double residentMiB() {
    std::ifstream statm("/proc/self/statm");
    std::uint64_t size = 0;
    std::uint64_t resident = 0;
    statm >> size >> resident;
    return static_cast<double>(resident) * 4096.0 / (1024.0 * 1024.0);
}

/// Counts the 128-pixel tiles a stroke's dabs can touch on a raster.
std::size_t tilesTouched(const Stroke& stroke, int width, int height) {
    constexpr int tile = 128;
    double minU = 1.0;
    double maxU = 0.0;
    double minV = 1.0;
    double maxV = 0.0;
    for (const SensorPoint& point : stroke.points) {
        minU = std::min<double>(minU, point.u);
        maxU = std::max<double>(maxU, point.u);
        minV = std::min<double>(minV, point.v);
        maxV = std::max<double>(maxV, point.v);
    }
    const double reach = stroke.radius * std::max(width, height) + 3.0;
    const auto tileRange = [&](double low, double high, int extent) {
        const int first = std::max(0, static_cast<int>(std::floor(low * extent - reach))) / tile;
        const int last =
            std::min(extent - 1, static_cast<int>(std::ceil(high * extent + reach))) / tile;
        return std::size_t(last - first + 1);
    };
    return tileRange(minU, maxU, width) * tileRange(minV, maxV, height);
}

/// Reads the unsigned number after `key=` in a span's note, or 0 when it is absent.
std::uint64_t noteNumber(const std::string& note, const std::string& key) {
    const auto at = note.find(key + "=");
    return at == std::string::npos ? 0
                                   : std::strtoull(note.c_str() + at + key.size() + 1, nullptr, 10);
}

/// Counters of the process-wide coverage cache, to subtract.
struct CacheCounts {
    std::array<std::uint64_t, coverageLookupKinds> lookups{};
    std::uint64_t evictions = 0;

    static CacheCounts now() {
        CacheCounts out;
        for (std::size_t kind = 0; kind < coverageLookupKinds; ++kind) {
            out.lookups[kind] =
                detail::brushCoverageCache().lookups(static_cast<CoverageLookup>(kind));
        }
        out.evictions = detail::brushCoverageCache().evictions();
        return out;
    }
    [[nodiscard]] std::uint64_t of(CoverageLookup kind) const {
        return lookups[static_cast<std::size_t>(kind)];
    }
};

/// What a phase of a session measured.
struct PhaseMeasure {
    CacheCounts before;
    CacheCounts after;
};

} // namespace

// Measures a long painting session in the window; run it in a release build by name (the plan
// is docs/ideas/brush-gui-plan.md section 11.2): `arraw-widget-tests "[.timing]" "Brush painting
// session"`. ARRAW_PREVIEW_DEVICE=cpu keeps the GPU out of it, and QT_SCALE_FACTOR=2 makes the
// device-pixel ratio 2. The numbers are printed as RESULT lines against their targets.
TEST_CASE("Brush painting session", "[.timing]") {
    const BrushSettingsReset forgetSettings;
    const TimingCollector collector;

    // Fifteen masks first, set through the sidecar rather than painted: fourteen of B2's
    // realistic brushes and a radial, each with Exposure and Shadows.
    DevelopState state;
    for (std::uint32_t index = 0; index < 14; ++index) {
        LocalAdjustment adjustment;
        adjustment.shape = BrushMask{
            test::paintedMask(20261009 + index, 60, test::everydayStyle, 4000.0 / 6000.0)};
        adjustment.deltas.exposure = 1.0F;
        adjustment.deltas.shadows = 30.0F;
        state = withLocalAdjustmentAdded(std::move(state), adjustment);
    }
    {
        LocalAdjustment radial;
        radial.shape = RadialMask{};
        radial.deltas.exposure = 1.0F;
        radial.deltas.shadows = 30.0F;
        state = withLocalAdjustmentAdded(std::move(state), radial);
    }
    Window w(6000, 4000, QSize(1500, 900), false, &state);
    const auto idle = [&] {
        CHECK(QTest::qWaitFor([&] { return !w.window.renderIndicator().busy(); }, 240000));
        QTest::qWait(60);
    };
    idle();
    const double ratio = w.window.devicePixelRatio();
    std::printf("SESSION ratio=%.1f view=%dx%d device-pixels=%dx%d\n", ratio, w.overlay().width(),
                w.overlay().height(), w.view().devicePixels().width(),
                w.view().devicePixels().height());
    const double residentAfterMasks = residentMiB();

    w.press(Qt::Key_M);
    REQUIRE(w.view().isMaskMode());
    QTest::mouseClick(&w.button("maskBrush"), Qt::LeftButton);
    REQUIRE(w.overlay().painting());

    DevelopState committed;
    QObject::connect(&w.overlay(), &MaskOverlay::stateEdited,
                     [&committed](const DevelopState& next) { committed = next; });
    std::vector<Stroke> painted;
    std::mt19937_64 g(20261010);
    test::StallProbe probe;

    /// The time window of one stroke, press to release, and the longest stall within it.
    struct StrokeWindow {
        double pressT = 0.0;
        double releaseT = 0.0;
        double stallMs = 0.0;
    };

    // A wandering path of 120 moves, in widget pixels, inside the picture and the widget.
    const auto path = [&] {
        const QPointF corner0 = w.at(0.0, 0.0);
        const QPointF corner1 = w.at(1.0, 1.0);
        QRectF bounds =
            QRectF(corner0, corner1).normalized().intersected(QRectF(w.overlay().rect()));
        bounds.adjust(40, 40, -40, -40);
        const auto between = [&](double lo, double hi) {
            return lo + (hi - lo) * std::uniform_real_distribution<double>(0.0, 1.0)(g);
        };
        QPointF at(between(bounds.left(), bounds.right()), between(bounds.top(), bounds.bottom()));
        double angle = between(0.0, 6.283);
        std::vector<QPointF> points{at};
        for (int move = 1; move < 120; ++move) {
            angle += between(-0.35, 0.35);
            QPointF next = at + QPointF(std::cos(angle), std::sin(angle)) * 8.0;
            if (!bounds.contains(next)) {
                angle += 3.14159;
                next = at + QPointF(std::cos(angle), std::sin(angle)) * 8.0;
                next.setX(std::clamp(next.x(), bounds.left(), bounds.right()));
                next.setY(std::clamp(next.y(), bounds.top(), bounds.bottom()));
            }
            at = next;
            points.push_back(at);
        }
        return points;
    };

    // One stroke of 120 moves, an event every 8 ms from the event loop (120 Hz).
    const auto paintStroke = [&] {
        const std::vector<QPointF> points = path();
        {
            const detail::TimingSpan span("bench.press");
            w.mouse(QEvent::MouseButtonPress, points.front());
        }
        probe.start();
        QEventLoop loop;
        QTimer timer;
        timer.setTimerType(Qt::PreciseTimer);
        timer.setInterval(8);
        std::size_t next = 1;
        QObject::connect(&timer, &QTimer::timeout, [&] {
            if (next < points.size()) {
                w.mouse(QEvent::MouseMove, points[next++]);
            } else {
                loop.quit();
            }
        });
        timer.start();
        loop.exec();
        const double stall = probe.longestMs();
        probe.stop();
        {
            const detail::TimingSpan span("bench.release");
            w.mouse(QEvent::MouseButtonRelease, points.back(), Qt::NoButton);
        }
        painted.clear();
        const auto selected = w.overlay().selection();
        for (const LocalAdjustment& adjustment : committed.localAdjustments) {
            const auto* brush = std::get_if<BrushMask>(&adjustment.shape);
            if (brush != nullptr && brush->strokes && selected && adjustment.id == *selected) {
                for (std::size_t i = 0; i < brush->strokes->size(); ++i) {
                    painted.push_back((*brush->strokes)[i]);
                }
            }
        }
        return stall;
    };

    // The warm-up stroke makes the sixteenth brush (a Miss, as a creation is), and gives it
    // deltas.
    paintStroke();
    idle();
    REQUIRE(w.overlay().selection().has_value());
    w.setRow("local.exposure", 1.0);
    w.setRow("local.shadows", 30.0);
    idle();
    if (!w.overlay().painting()) {
        QTest::mouseClick(&w.button("maskBrush"), Qt::LeftButton);
    }
    REQUIRE(w.overlay().painting());
    w.overlay().setFocus();
    for (const QLabel* label : w.window.findChildren<QLabel*>()) {
        if (label->text().startsWith("Preview:")) {
            std::printf("DEVICE %s\n", label->text().toUtf8().constData());
        }
    }

    // Runs a phase of strokes and prints its metrics against the targets of the plan.
    const auto runPhase = [&](const char* phase, int count, double guiMedian, double guiP99,
                              double stallLimit, bool fitView, bool tint) {
        collector.clear();
        PhaseMeasure measure;
        measure.before = CacheCounts::now();
        std::vector<StrokeWindow> windows;
        for (int index = 0; index < count; ++index) {
            const double stall = paintStroke();
            windows.push_back({0.0, 0.0, stall});
            idle();
        }
        measure.after = CacheCounts::now();
        const double residentMiBNow = residentMiB();
        const std::vector<SpanRecord> spans = collector.spans();
        // The stroke windows, from the marks the test wrote.
        std::vector<const SpanRecord*> presses;
        std::vector<const SpanRecord*> releases;
        for (const SpanRecord& span : spans) {
            if (span.stage == "bench.press") {
                presses.push_back(&span);
            } else if (span.stage == "bench.release") {
                releases.push_back(&span);
            }
        }
        REQUIRE(presses.size() == windows.size());
        REQUIRE(releases.size() == windows.size());
        for (std::size_t i = 0; i < windows.size(); ++i) {
            windows[i].pressT = presses[i]->begin;
            windows[i].releaseT = releases[i]->begin;
        }
        const auto duringStroke = [&](double t) {
            for (const StrokeWindow& window : windows) {
                if (t >= window.pressT && t <= window.releaseT) {
                    return true;
                }
            }
            return false;
        };
        std::vector<const SpanRecord*> updates;
        std::vector<const SpanRecord*> paints;
        std::vector<const SpanRecord*> results;
        std::vector<double> tintMs;
        std::vector<double> renderMs;
        for (const SpanRecord& span : spans) {
            if (span.stage == "brush.update") {
                updates.push_back(&span);
            } else if (span.stage == "overlay.paint") {
                paints.push_back(&span);
            } else if (span.stage == "window.result") {
                results.push_back(&span);
            } else if (span.stage == "mask.coverage" && duringStroke(span.begin)) {
                tintMs.push_back(span.ms());
            } else if (span.stage == "preview.render") {
                renderMs.push_back(span.ms());
            }
        }
        // GUI time per update: the handling plus the overlay's repaints that follow it within
        // 20 ms, before the next update.
        std::vector<double> guiMs;
        for (std::size_t i = 0; i < updates.size(); ++i) {
            double total = updates[i]->ms();
            const double until = i + 1 < updates.size() ? updates[i + 1]->begin : 1e18;
            for (const SpanRecord* paint : paints) {
                if (paint->begin >= updates[i]->begin && paint->begin < until &&
                    paint->begin - updates[i]->begin < 20.0) {
                    total += paint->ms();
                }
            }
            guiMs.push_back(total);
        }
        // Latency: an update's start to the first result of at least its request.
        std::vector<double> latencyMs;
        for (const SpanRecord* update : updates) {
            const std::uint64_t wanted = noteNumber(update->note, "request");
            for (const SpanRecord* result : results) {
                if (result->end >= update->begin && result->request >= wanted) {
                    latencyMs.push_back(result->end - update->begin);
                    break;
                }
            }
        }
        // Renders shown per second of stroke, and the commit: the release to the last result.
        std::vector<double> perSecond;
        std::vector<double> commitMs;
        for (std::size_t i = 0; i < windows.size(); ++i) {
            const double from = windows[i].pressT;
            const double to = windows[i].releaseT;
            std::size_t shown = 0;
            double last = to;
            const double next = i + 1 < windows.size() ? windows[i + 1].pressT : 1e18;
            for (const SpanRecord* result : results) {
                if (result->end >= from && result->end <= to) {
                    ++shown;
                }
                if (result->begin >= to && result->begin < next) {
                    last = std::max(last, result->end);
                }
            }
            perSecond.push_back(static_cast<double>(shown) / ((to - from) / 1000.0));
            commitMs.push_back(last - to);
        }
        std::vector<double> stalls;
        for (const StrokeWindow& window : windows) {
            stalls.push_back(window.stallMs);
        }
        const Spread gui = spreadOf(guiMs);
        const Spread latency = spreadOf(latencyMs);
        const Spread rate = spreadOf(perSecond);
        const Spread tintSpread = spreadOf(tintMs);
        const Spread commit = spreadOf(commitMs);
        const Spread stall = spreadOf(stalls);
        const Spread render = spreadOf(renderMs);
        const std::uint64_t misses =
            measure.after.of(CoverageLookup::Miss) - measure.before.of(CoverageLookup::Miss);
        const std::uint64_t evictions = measure.after.evictions - measure.before.evictions;
        std::printf("PHASE %s strokes=%d updates=%zu renders(median %.1f ms, p95 %.1f ms, n=%zu)\n",
                    phase, count, updates.size(), render.median, render.p95, render.count);
        std::printf(
            "PHASE %s cache: hit=%llu contentHit=%llu extended=%llu grown=%llu miss=%llu "
            "evictions=%llu bytes=%.1f MiB rss=%.0f MiB\n",
            phase,
            static_cast<unsigned long long>(measure.after.of(CoverageLookup::Hit) -
                                            measure.before.of(CoverageLookup::Hit)),
            static_cast<unsigned long long>(measure.after.of(CoverageLookup::ContentHit) -
                                            measure.before.of(CoverageLookup::ContentHit)),
            static_cast<unsigned long long>(measure.after.of(CoverageLookup::Extended) -
                                            measure.before.of(CoverageLookup::Extended)),
            static_cast<unsigned long long>(measure.after.of(CoverageLookup::Grown) -
                                            measure.before.of(CoverageLookup::Grown)),
            static_cast<unsigned long long>(misses), static_cast<unsigned long long>(evictions),
            static_cast<double>(detail::brushCoverageCache().memoryBytes()) / (1024.0 * 1024.0),
            residentMiBNow);
        if (fitView) {
            verdict(phase, "GUI time per update, median", gui.median, "ms", guiMedian);
            verdict(phase, "GUI time per update, p99", gui.p99, "ms", guiP99);
            verdict(phase, "longest GUI stall", stall.max, "ms", stallLimit);
            if (ratio <= 1.0) {
                verdict(phase, "latency, median", latency.median, "ms", 100.0);
                verdict(phase, "latency, p95", latency.p95, "ms", 160.0);
                std::printf(
                    "RESULT %s | renders shown per second, median = %.1f | target >= 10 | %s\n",
                    phase, rate.median, rate.median >= 10.0 ? "met" : "MISSED");
            } else {
                std::printf("RESULT %s | latency median %.0f ms, p95 %.0f ms; renders per second "
                            "%.1f (reported)\n",
                            phase, latency.median, latency.p95, rate.median);
            }
            if (tint) {
                verdict(phase, "tint mask.coverage, median", tintSpread.median, "ms", 4.0);
                verdict(phase, "tint mask.coverage, p95", tintSpread.p95, "ms", 8.0);
            }
            // Counted in this phase's strokes, the commit is a render or two behind the release.
            std::printf("RESULT %s | commit, release to last result: median %.1f ms, p95 %.1f ms "
                        "(one render: median %.1f ms) | target one render | %s\n",
                        phase, commit.median, commit.p95, render.median,
                        commit.median <= render.median ? "met" : "MISSED");
            // Asserted before the 1:1 phase empties the cache; the first fit render after it
            // may draw what 1:1 evicted, which the line above reports.
            if (!tint) {
                // At ratio 2 the level is four times the pixels: the cache keeps the live lists
                // and drops the superseded Extended parents first, so no commit redraws a brush.
                // The evictions are reported, not asserted (they are mostly those parents; the
                // cache is nearly full, so the margin on the misses is a few MiB).
                CHECK(misses == 0);
                if (ratio <= 1.0) {
                    CHECK(evictions == 0);
                }
            }
        } else {
            std::printf(
                "RESULT %s | reported: GUI median %.2f ms, p99 %.2f ms, stall max %.1f ms, "
                "latency median %.0f ms p95 %.0f ms, renders per second %.1f, commit median "
                "%.0f ms\n",
                phase, gui.median, gui.p99, stall.max, latency.median, latency.p95, rate.median,
                commit.median);
        }
        CHECK(detail::brushCoverageCache().memoryBytes() <= (std::size_t{512} << 20));
    };

    // Fit view, the tint off: 60 strokes. The warm-up stroke is the 61st on the list below.
    const double residentBeforeFit = residentMiB();
    runPhase("fit", 60, 1.5, 5.0, 16.0, true, false);

    // Undo and redo 20 strokes: every undo a cache Hit, packing only the stroke's tiles.
    {
        const std::vector<Stroke> history = painted;
        const CacheCounts before = CacheCounts::now();
        std::vector<double> undoMs;
        std::vector<double> redoMs;
        std::size_t packedMost = 0;
        bool packsOnlyStroke = true;
        for (int step = 0; step < 20; ++step) {
            collector.clear();
            {
                const detail::TimingSpan span("bench.undo");
                w.press(Qt::Key_Z, Qt::ControlModifier);
            }
            idle();
            const std::vector<SpanRecord> spans = collector.spans();
            double begin = 0.0;
            for (const SpanRecord& span : spans) {
                if (span.stage == "bench.undo") {
                    begin = span.begin;
                }
            }
            std::uint64_t packed = 0;
            int width = 0;
            int height = 0;
            for (const SpanRecord& span : spans) {
                if (span.stage == "cpu.coverage") {
                    packed += noteNumber(span.note, "tilesPacked");
                    const auto at = span.note.find("size=");
                    if (at != std::string::npos) {
                        width = std::atoi(span.note.c_str() + at + 5);
                        height = std::atoi(span.note.c_str() + span.note.find('x', at) + 1);
                    }
                }
            }
            for (const SpanRecord& span : spans) {
                if (span.stage == "window.result" && span.end >= begin) {
                    undoMs.push_back(span.end - begin);
                    break;
                }
            }
            const Stroke& undone = history[history.size() - 1 - static_cast<std::size_t>(step)];
            const std::size_t limit = width > 0 ? tilesTouched(undone, width, height) : 0;
            packedMost = std::max<std::size_t>(packedMost, packed);
            CHECK(width > 0);
            CHECK(packed > 0);
            if (width > 0 && packed > limit) {
                packsOnlyStroke = false;
                std::printf("UNDO %d packed %llu tiles, the stroke can touch %zu\n", step,
                            static_cast<unsigned long long>(packed), limit);
            }
        }
        const CacheCounts after = CacheCounts::now();
        for (int step = 0; step < 20; ++step) {
            collector.clear();
            {
                const detail::TimingSpan span("bench.undo");
                w.press(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
            }
            idle();
            double begin = 0.0;
            const std::vector<SpanRecord> spans = collector.spans();
            for (const SpanRecord& span : spans) {
                if (span.stage == "bench.undo") {
                    begin = span.begin;
                }
            }
            for (const SpanRecord& span : spans) {
                if (span.stage == "window.result" && span.end >= begin) {
                    redoMs.push_back(span.end - begin);
                    break;
                }
            }
        }
        const std::uint64_t notHits =
            (after.of(CoverageLookup::Miss) - before.of(CoverageLookup::Miss)) +
            (after.of(CoverageLookup::Extended) - before.of(CoverageLookup::Extended)) +
            (after.of(CoverageLookup::Grown) - before.of(CoverageLookup::Grown));
        const Spread undo = spreadOf(undoMs);
        const Spread redo = spreadOf(redoMs);
        std::printf(
            "RESULT undo | Ctrl+Z to result shown: median %.1f ms, p95 %.1f ms (redo median "
            "%.1f ms); cache lookups other than Hit/ContentHit: %llu; most tiles packed by "
            "one undo: %zu; packs only the stroke's tiles: %s\n",
            undo.median, undo.p95, redo.median, static_cast<unsigned long long>(notHits),
            packedMost, packsOnlyStroke ? "yes" : "NO");
        const std::uint64_t hits = after.of(CoverageLookup::Hit) - before.of(CoverageLookup::Hit);
        std::printf("RESULT undo | cache Hit lookups over 20 undos: %llu\n",
                    static_cast<unsigned long long>(hits));
        CHECK(hits >= 20);
        CHECK(notHits == 0);
        CHECK(packsOnlyStroke);
    }

    // 1:1, the tint off: 10 strokes. Reported only.
    const double residentBeforeOneToOne = residentMiB();
    w.view().zoomTo(1.0);
    idle();
    runPhase("1:1", 10, 0.0, 0.0, 0.0, false, false);
    const double residentAfterOneToOne = residentMiB();

    // Back to fit with the tint on: 20 strokes (the cache may have been emptied by 1:1).
    w.view().zoomToFit();
    idle();
    w.overlay().setFocus();
    w.press(Qt::Key_O);
    idle();
    REQUIRE(w.overlay().overlayShown());
    runPhase("fit+tint", 20, 1.5, 5.0, 24.0, true, true);

    std::printf("MEMORY rss after the 15 masks %.0f MiB, before fit strokes %.0f MiB, before 1:1 "
                "%.0f MiB, after 1:1 %.0f MiB, at the end %.0f MiB; cache %.1f MiB\n",
                residentAfterMasks, residentBeforeFit, residentBeforeOneToOne,
                residentAfterOneToOne, residentMiB(),
                static_cast<double>(detail::brushCoverageCache().memoryBytes()) /
                    (1024.0 * 1024.0));
    // The last render equals a direct render of the committed state is the engine bench's job.
}
