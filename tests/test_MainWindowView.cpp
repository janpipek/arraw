#include "DebugLog.h"
#include "support/TempDir.h"
#include "ui/DebugWindow.h"
#include "ui/FilmStrip.h"
#include "ui/MainWindow.h"
#include "ui/PhotoView.h"

#include <QAction>
#include <QDockWidget>
#include <QMenuBar>
#include <QStatusBar>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <functional>

using namespace arraw;
using namespace arraw::app;

/// The View menu's shortcuts: the docks (F7, F8, F9), Full Screen (F11) and Hide Panels (F12).

namespace {

/// A window over one photograph.
struct Window {
    test::TempDir folder;
    DebugLog debugLog;
    MainWindow window{debugLog};

    Window() {
        const std::filesystem::path fixtures(ARRAW_TEST_DATA_DIR);
        std::filesystem::copy_file(fixtures / "preview-32x24.dng", folder.file("a.dng"));
        std::filesystem::copy_file(fixtures / "bayer-32x24.dng", folder.file("b.dng"));
        window.openInitialPath(folder.file("a.dng"));
        window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&window));
        REQUIRE(QTest::qWaitFor(
            [this] { return !window.findChild<PhotoView*>()->wholeFrameImage().isNull(); }, 20000));
    }

    [[nodiscard]] QDockWidget& dock(const char* name) const {
        auto* found = window.findChild<QDockWidget*>(name);
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

    /// Presses a key on the window, as the user does with the menu bar hidden or not.
    void press(Qt::Key key) {
        QTest::keyClick(&window, key);
    }
};

} // namespace

TEST_CASE("F7, F8 and F9 toggle the docks", "[app][window][view]") {
    Window w;
    struct Case {
        Qt::Key key;
        const char* dock;
    };
    for (const Case c : {Case{Qt::Key_F7, "HistoryDock"}, Case{Qt::Key_F8, "DevelopDock"},
                         Case{Qt::Key_F9, "FilmStripDock"}}) {
        CAPTURE(c.dock);
        QDockWidget& dock = w.dock(c.dock);
        REQUIRE(dock.isVisible());
        w.press(c.key);
        CHECK(dock.isHidden());
        w.press(c.key);
        CHECK(dock.isVisible());
    }
}

TEST_CASE("The Develop dock cannot be moved or floated", "[app][window][view]") {
    Window w;
    CHECK(w.dock("DevelopDock").features() == QDockWidget::DockWidgetClosable);
    CHECK(w.action("developPanelAction").isCheckable());
}

TEST_CASE("F12 hides the panels and restores what each was", "[app][window][view]") {
    Window w;
    QDockWidget& history = w.dock("HistoryDock");
    history.hide();

    w.press(Qt::Key_F12);
    CHECK(w.action("hidePanelsAction").isChecked());
    CHECK(w.dock("DevelopDock").isHidden());
    CHECK(history.isHidden());
    CHECK(w.dock("FilmStripDock").isHidden());
    CHECK(w.window.menuBar()->isHidden());
    CHECK(w.window.statusBar()->isHidden());

    // The dock keys rest while the panels are away: disabled, not merely unbound.
    for (const char* name : {"presetsAndHistoryAction", "developPanelAction"}) {
        CHECK_FALSE(w.action(name).isEnabled());
    }
    CHECK_FALSE(w.dock("FilmStripDock").toggleViewAction()->isEnabled());
    w.press(Qt::Key_F8);
    CHECK(w.dock("DevelopDock").isHidden());

    w.press(Qt::Key_F12);
    CHECK_FALSE(w.action("hidePanelsAction").isChecked());
    CHECK(w.dock("DevelopDock").isVisible());
    CHECK(history.isHidden());
    CHECK(w.dock("FilmStripDock").isVisible());
    CHECK(w.window.menuBar()->isVisible());
    CHECK(w.window.statusBar()->isVisible());

    for (const char* name : {"presetsAndHistoryAction", "developPanelAction"}) {
        CHECK(w.action(name).isEnabled());
    }
    CHECK(w.dock("FilmStripDock").toggleViewAction()->isEnabled());

    // The keys work again.
    w.press(Qt::Key_F7);
    CHECK(history.isVisible());
}

TEST_CASE("The dock keys and the debug log work with the menu bar hidden", "[app][window][view]") {
    Window w;
    w.window.menuBar()->hide();
    struct Case {
        Qt::Key key;
        const char* dock;
    };
    for (const Case c : {Case{Qt::Key_F7, "HistoryDock"}, Case{Qt::Key_F8, "DevelopDock"},
                         Case{Qt::Key_F9, "FilmStripDock"}}) {
        CAPTURE(c.dock);
        QDockWidget& dock = w.dock(c.dock);
        w.press(c.key);
        CHECK(dock.isHidden());
        w.press(c.key);
        CHECK(dock.isVisible());
    }

    REQUIRE(w.window.findChild<DebugWindow*>() == nullptr);
    QTest::keyClick(&w.window, Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
    auto* debug = w.window.findChild<DebugWindow*>();
    REQUIRE(debug != nullptr);
    CHECK(debug->isVisible());
}

TEST_CASE("Menu-only shortcuts work in lights-out mode", "[app][window][view]") {
    Window w;
    auto* strip = w.window.findChild<FilmStrip*>();
    REQUIRE(strip != nullptr);
    REQUIRE(strip->activePrimary() == w.folder.file("a.dng"));

    w.press(Qt::Key_F12);
    REQUIRE(w.window.menuBar()->isHidden());
    w.press(Qt::Key_Right);
    CHECK(strip->activePrimary() == w.folder.file("b.dng"));
}

TEST_CASE("Every menu shortcut is also on the window", "[app][window][view]") {
    Window w;
    const QList<QAction*> onWindow = w.window.actions();
    std::function<void(QWidget*)> walk = [&](QWidget* menu) {
        for (QAction* action : menu->actions()) {
            if (action->menu() != nullptr) {
                walk(action->menu());
            } else if (!action->shortcut().isEmpty()) {
                INFO("action " << action->text().toStdString());
                CHECK(onWindow.contains(action));
            }
        }
    };
    walk(w.window.menuBar());
}

TEST_CASE("F11 toggles full screen", "[app][window][view]") {
    Window w;
    QAction& full = w.action("fullScreenAction");
    CHECK(full.isCheckable());
    CHECK_FALSE(full.isChecked());

    w.press(Qt::Key_F11);
    if (!QTest::qWaitFor([&] { return w.window.isFullScreen(); }, 2000)) {
        SKIP("this platform does not enter full screen");
    }
    CHECK(full.isChecked());

    w.press(Qt::Key_F11);
    REQUIRE(QTest::qWaitFor([&] { return !w.window.isFullScreen(); }, 2000));
    CHECK_FALSE(full.isChecked());
}

TEST_CASE("F11 returns to the maximised state and follows outside changes", "[app][window][view]") {
    Window w;
    QAction& full = w.action("fullScreenAction");
    w.window.showMaximized();
    if (!QTest::qWaitFor([&] { return w.window.isMaximized(); }, 2000)) {
        SKIP("this platform does not maximise");
    }

    w.press(Qt::Key_F11);
    REQUIRE(QTest::qWaitFor([&] { return w.window.isFullScreen(); }, 2000));
    w.press(Qt::Key_F11);
    REQUIRE(QTest::qWaitFor([&] { return !w.window.isFullScreen(); }, 2000));
    CHECK(w.window.isMaximized());

    w.window.showNormal();
    REQUIRE(QTest::qWaitFor([&] { return !w.window.isMaximized(); }, 2000));
    w.window.showFullScreen();
    REQUIRE(QTest::qWaitFor([&] { return w.window.isFullScreen(); }, 2000));
    CHECK(full.isChecked());
}
