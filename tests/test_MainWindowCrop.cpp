#include "DebugLog.h"
#include "support/TempDir.h"
#include "ui/CropOverlay.h"
#include "ui/DevelopPanel.h"
#include "ui/FilmStrip.h"
#include "ui/MainWindow.h"
#include "ui/PhotoView.h"
#include "ui/SettingSlider.h"

#include <GeometrySettings.h>
#include <PhotoMarks.h>
#include <Sidecar.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QTest>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string_view>

using namespace arraw;
using namespace arraw::app;

/// The crop mode inside the window: focus, shortcuts, undo and the first frame (ADR 040).

namespace {

/// A window with a folder of two photographs open, the first one shown and rendered.
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
        window.activateWindow();
        REQUIRE(QTest::qWaitForWindowActive(&window));
        REQUIRE(QTest::qWaitFor([this] { return !view().wholeFrameImage().isNull(); }, 20000));
    }

    [[nodiscard]] PhotoView& view() const {
        return *window.findChild<PhotoView*>();
    }

    [[nodiscard]] CropOverlay& overlay() const {
        return *window.findChild<CropOverlay*>();
    }

    [[nodiscard]] FilmStrip& strip() const {
        return *window.findChild<FilmStrip*>();
    }

    [[nodiscard]] DevelopPanel& panel() const {
        return *window.findChild<DevelopPanel*>();
    }

    [[nodiscard]] QAbstractButton& button(const char* name) const {
        auto* found = panel().findChild<QAbstractButton*>(name);
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

    [[nodiscard]] const GeometrySettings& geometry() const {
        return overlay().editing().geometry();
    }

    [[nodiscard]] bool landscapeCrop() const {
        return overlay().editing().crop().width > overlay().editing().crop().height;
    }
};

} // namespace

TEST_CASE("In the crop mode the keys stay with the mode wherever the focus went",
          "[app][window][crop]") {
    Window w;
    w.press(Qt::Key_C);
    REQUIRE(w.view().isCropMode());
    CHECK(QApplication::focusWidget() == &w.overlay());
    const int rating = w.strip().activeMarks().rating;
    const bool landscape = w.landscapeCrop();

    // A button takes no focus: X then swaps rather than rejecting the shot.
    QTest::mouseClick(&w.button("cropSwap"), Qt::LeftButton);
    CHECK(w.landscapeCrop() != landscape);
    CHECK(QApplication::focusWidget() == &w.overlay());
    w.press(Qt::Key_X);
    CHECK(w.landscapeCrop() == landscape);
    CHECK(w.strip().activeMarks().rating == rating);
    CHECK_FALSE(w.action("rejectAction").isEnabled());

    // With the focus elsewhere in the window, the window's own crop keys answer.
    w.strip().setFocus();
    w.press(Qt::Key_X);
    CHECK(w.landscapeCrop() != landscape);
    CHECK(w.strip().activeMarks().rating == rating);
    w.press(Qt::Key_Escape);
    CHECK_FALSE(w.view().isCropMode());
    // Esc dropped the session: nothing to save.
    CHECK_FALSE(w.window.isWindowModified());
    CHECK(w.action("rejectAction").isEnabled());
}

TEST_CASE("Enter after typing an angle ends the typing; the next Enter keeps the crop",
          "[app][window][crop]") {
    Window w;
    w.press(Qt::Key_C);
    REQUIRE(w.view().isCropMode());
    QDoubleSpinBox* angle = nullptr;
    for (auto* row : w.panel().findChildren<SettingSlider*>()) {
        if (row->key() == "straighten") {
            angle = row->findChild<QDoubleSpinBox*>();
        }
    }
    REQUIRE(angle != nullptr);
    angle->setFocus();
    angle->selectAll();
    QTest::keyClicks(QApplication::focusWidget(), "5");
    w.press(Qt::Key_Return);
    CHECK(w.view().isCropMode());
    CHECK(w.overlay().editing().displayedAngle() == 5.0);
    CHECK(QApplication::focusWidget() == &w.overlay());

    w.press(Qt::Key_Return);
    CHECK_FALSE(w.view().isCropMode());
    CHECK(w.window.isWindowModified());
}

TEST_CASE("Undo in the crop mode steps through its gestures; leaving keeps one step",
          "[app][window][crop][history]") {
    Window w;
    QAction& undo = w.action("undoAction");
    QAction& redo = w.action("redoAction");
    w.press(Qt::Key_C);
    REQUIRE(w.view().isCropMode());
    CHECK_FALSE(undo.isEnabled());

    QTest::mouseClick(&w.button("cropTurnRight"), Qt::LeftButton);
    QTest::mouseClick(&w.button("cropTurnRight"), Qt::LeftButton);
    CHECK(w.geometry().rotation == QuarterTurn::Clockwise180);
    CHECK(undo.isEnabled());

    w.press(Qt::Key_Z, Qt::ControlModifier);
    CHECK(w.geometry().rotation == QuarterTurn::Clockwise90);
    w.press(Qt::Key_Z, Qt::ControlModifier);
    CHECK(w.geometry().rotation == QuarterTurn::None);
    CHECK_FALSE(undo.isEnabled());
    // Past the session's start nothing happens, and the mode stays.
    w.press(Qt::Key_Z, Qt::ControlModifier);
    CHECK(w.view().isCropMode());
    CHECK(w.geometry().rotation == QuarterTurn::None);
    CHECK(redo.isEnabled());
    w.press(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(w.geometry().rotation == QuarterTurn::Clockwise90);

    w.press(Qt::Key_Return);
    REQUIRE_FALSE(w.view().isCropMode());
    CHECK(w.window.isWindowModified());
    CHECK(undo.isEnabled());
    w.press(Qt::Key_Z, Qt::ControlModifier);
    CHECK_FALSE(w.window.isWindowModified());
    CHECK_FALSE(undo.isEnabled());
}

TEST_CASE("The crop mode opens on the photograph, not an empty frame", "[app][window][crop]") {
    Window w;
    w.press(Qt::Key_C);
    REQUIRE(w.view().isCropMode());
    // Before any render of the mode could arrive: no event has been processed since. The
    // developed frame stands in at once; the camera's preview is read off the GUI thread
    // (ADR 043) and fills the rest until the render.
    CHECK(w.overlay().showsPhotograph());

    // Left and entered again with nothing changed, it shows its own last render at once.
    REQUIRE(QTest::qWaitFor([&] { return w.overlay().hasRender(); }, 20000));
    w.press(Qt::Key_Return);
    REQUIRE_FALSE(w.view().isCropMode());
    w.press(Qt::Key_C);
    REQUIRE(w.view().isCropMode());
    CHECK(w.overlay().hasRender());
    w.press(Qt::Key_Escape);
}

TEST_CASE("The crop mode leaves only the Crop group to edit", "[app][window][crop]") {
    Window w;
    const auto tone = [&] {
        for (auto* group : w.panel().findChildren<QGroupBox*>()) {
            if (group->title() == "Tone") {
                return group;
            }
        }
        return static_cast<QGroupBox*>(nullptr);
    };
    REQUIRE(tone() != nullptr);
    CHECK(tone()->isEnabled());
    w.press(Qt::Key_C);
    CHECK_FALSE(tone()->isEnabled());
    w.press(Qt::Key_C);
    CHECK_FALSE(w.view().isCropMode());
    CHECK(tone()->isEnabled());
}

TEST_CASE("Colour labels take the colour's initial; C is the crop mode", "[app][window]") {
    Window w;
    REQUIRE_FALSE(w.strip().activeMarks().label.has_value());
    w.press(Qt::Key_G);
    CHECK(w.strip().activeMarks().label == ColorLabel::Green);
    w.press(Qt::Key_Y);
    CHECK(w.strip().activeMarks().label == ColorLabel::Yellow);
    w.press(Qt::Key_R);
    CHECK(w.strip().activeMarks().label == ColorLabel::Red);
    CHECK_FALSE(w.view().isCropMode());
    w.press(Qt::Key_C);
    CHECK(w.view().isCropMode());
    CHECK(w.strip().activeMarks().label == ColorLabel::Red);
}

TEST_CASE("Right in the crop mode keeps the crop and steps to the next photograph",
          "[app][window][crop]") {
    Window w;
    w.press(Qt::Key_C);
    QTest::mouseClick(&w.button("cropTurnRight"), Qt::LeftButton);
    // Leaving the photograph asks about the unsaved crop; the test saves it.
    QTimer::singleShot(0, [] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        REQUIRE(box != nullptr);
        box->button(QMessageBox::Save)->click();
    });
    w.press(Qt::Key_Right);
    CHECK_FALSE(w.view().isCropMode());
    CHECK(w.strip().activePrimary() == w.folder.file("b.dng"));
    const auto saved = readSidecar(w.folder.file("a.dng"));
    REQUIRE(saved);
    REQUIRE(saved->state);
    CHECK(saved->state->settings.geometry.rotation == QuarterTurn::Clockwise90);
}
