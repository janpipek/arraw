#include "DebugLog.h"
#include "support/GeneratedPhoto.h"
#include "support/TempDir.h"
#include "ui/CropOverlay.h"
#include "ui/DevelopPanel.h"
#include "ui/FilmStrip.h"
#include "ui/MainWindow.h"
#include "ui/MaskOverlay.h"
#include "ui/MasksPanel.h"
#include "ui/PhotoView.h"
#include "ui/SettingSlider.h"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListView>
#include <QMouseEvent>
#include <QPixmap>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <variant>

using namespace arraw;
using namespace arraw::app;

/// The mask mode inside the window: keys, gestures as history steps, selection (ADR 044).

namespace {

/// A window with two generated 640 by 480 photographs, the first one shown and rendered.
struct Window {
    test::TempDir folder;
    DebugLog debugLog;
    MainWindow window{debugLog};

    explicit Window(int width = 640, int height = 480, QSize size = {}, bool second = true) {
        test::writeGeneratedPhoto(folder.file("a.png"), width, height);
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
