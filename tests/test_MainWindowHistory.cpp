#include "DebugLog.h"
#include "support/TempDir.h"
#include "ui/CurveEditor.h"
#include "ui/DevelopPanel.h"
#include "ui/FilmStrip.h"
#include "ui/MainWindow.h"
#include "ui/PhotoView.h"
#include "ui/SettingSlider.h"

#include <Photo.h>

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QListView>
#include <QMessageBox>
#include <QSettings>
#include <QTest>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace arraw;
using namespace arraw::app;

/// The History dock in the window: clicking a step goes there, and keeps the steps after it.

namespace {

/// A window over one photograph.
struct Window {
    test::TempDir folder;
    DebugLog debugLog;
    MainWindow window{debugLog};

    Window() {
        const std::filesystem::path fixtures(ARRAW_TEST_DATA_DIR);
        std::filesystem::copy_file(fixtures / "preview-32x24.dng", folder.file("a.dng"));
        window.openInitialPath(folder.file("a.dng"));
        window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&window));
        REQUIRE(QTest::qWaitFor([this] { return !view().wholeFrameImage().isNull(); }, 20000));
    }

    [[nodiscard]] PhotoView& view() const {
        return *window.findChild<PhotoView*>();
    }

    [[nodiscard]] QListView& list() const {
        return *window.findChild<QListView*>("historyList");
    }

    [[nodiscard]] QDockWidget& dock() const {
        return *window.findChild<QDockWidget*>("HistoryDock");
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

    [[nodiscard]] QDoubleSpinBox& exposureBox() const {
        for (auto* row : window.findChildren<SettingSlider*>()) {
            if (row->key() == "exposure") {
                return *row->findChild<QDoubleSpinBox*>();
            }
        }
        FAIL("no exposure row");
        throw;
    }

    [[nodiscard]] DevelopPanel& panel() const {
        return *window.findChild<DevelopPanel*>();
    }

    [[nodiscard]] QAbstractButton& button(const char* name) const {
        auto* found = panel().findChild<QAbstractButton*>(name);
        REQUIRE(found != nullptr);
        return *found;
    }

    /// Types an exposure into the panel, leaving the edit pending as it is while the user drags.
    void typeExposure(double value) const {
        exposureBox().setValue(value);
    }

    /// Types an exposure into the panel and ends the edit at once, as one step, without waiting
    /// out the pause that would end it.
    void setExposure(double value) const {
        typeExposure(value);
        panel().finishPendingEdit();
    }

    /// Clicks the tone curve's plot, which adds a point, and ends that edit.
    void bendCurve() const {
        CurveEditor& editor = *panel().findChild<CurveEditor*>("curveEditor");
        const QRectF plot = editor.plotRect();
        const QPoint at(static_cast<int>(plot.left() + plot.width() * 0.5),
                        static_cast<int>(plot.top() + plot.height() * 0.3));
        QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, at);
        QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, at);
        panel().finishPendingEdit();
    }

    [[nodiscard]] int rows() const {
        return list().model()->rowCount();
    }

    [[nodiscard]] QString text(int row) const {
        return list().model()->index(row, 0).data(Qt::DisplayRole).toString();
    }

    void click(int row) const {
        const QModelIndex index = list().model()->index(row, 0);
        emit list().clicked(index);
    }
};

} // namespace

TEST_CASE("Clicking an older step goes back and keeps the later ones", "[app][window][history]") {
    Window w;
    CHECK(w.dock().isEnabled());
    CHECK(w.rows() == 1);
    CHECK(w.text(0) == "Opened");

    w.setExposure(0.5);
    w.setExposure(1.5);
    REQUIRE(w.rows() == 3);
    CHECK(w.text(0).startsWith("Exposure"));

    // Row 1 is the first edit: the step before the newest.
    w.click(1);
    CHECK(w.exposureBox().value() == 0.5);
    CHECK(w.rows() == 3);
    CHECK(w.action("redoAction").isEnabled());

    // A new edit drops the redoable step.
    w.setExposure(-1.0);
    CHECK(w.rows() == 3);
    CHECK_FALSE(w.action("redoAction").isEnabled());
    CHECK(w.exposureBox().value() == -1.0);
}

TEST_CASE("The History dock is disabled in the crop mode", "[app][window][history]") {
    Window w;
    REQUIRE(w.dock().isEnabled());
    w.action("cropAction").trigger();
    CHECK_FALSE(w.dock().isEnabled());
    w.action("cropAction").trigger();
    CHECK(w.dock().isEnabled());
}

TEST_CASE("The View menu shows and hides the History dock", "[app][window][history]") {
    Window w;
    QAction& toggle = w.action("presetsAndHistoryAction");
    CHECK(toggle.isChecked());
    toggle.trigger();
    CHECK(w.dock().isHidden());
}

TEST_CASE("A pending slider edit is not committed as the curve's Reset", "[app][window][history]") {
    Window w;
    w.bendCurve();
    REQUIRE(w.rows() == 2);
    CHECK(w.text(0) == "Tone Curve");

    // The exposure edit is still pending when Reset is pressed.
    w.typeExposure(0.5);
    w.button("curveReset").click();

    REQUIRE(w.rows() == 4);
    CHECK(w.text(0) == "Reset Tone Curve");
    CHECK(w.text(1).startsWith("Exposure"));
    CHECK(w.text(2) == "Tone Curve");
}

TEST_CASE("A crop reset is worded as a reset", "[app][window][history]") {
    Window w;
    w.button("cropFlipHorizontal").click();
    REQUIRE(w.rows() == 2);
    CHECK_FALSE(w.text(0).startsWith("Reset"));

    w.button("cropReset").click();
    REQUIRE(w.rows() == 3);
    CHECK(w.text(0) == "Reset Crop");
}

TEST_CASE("Leaving the crop mode with a change makes a Crop step", "[app][window][history]") {
    Window w;
    w.action("cropAction").trigger();
    REQUIRE(w.view().isCropMode());
    w.button("cropTurnRight").click();
    w.action("cropAction").trigger();
    REQUIRE_FALSE(w.view().isCropMode());

    REQUIRE(w.rows() == 2);
    CHECK(w.text(0) == "Crop");
}

TEST_CASE("Enter in the list goes to the highlighted step", "[app][window][history]") {
    Window w;
    w.setExposure(0.5);
    w.setExposure(1.5);
    REQUIRE(w.rows() == 3);

    w.list().setCurrentIndex(w.list().model()->index(1, 0));
    // Moving the highlight alone goes nowhere.
    CHECK(w.exposureBox().value() == 1.5);
    QTest::keyClick(&w.list(), Qt::Key_Return);
    CHECK(w.exposureBox().value() == 0.5);
    CHECK(w.rows() == 3);
    CHECK(w.action("redoAction").isEnabled());
}

TEST_CASE("Discarding the changes leaves the dock with the opening step",
          "[app][window][history]") {
    Window w;
    w.setExposure(0.5);
    REQUIRE(w.rows() == 2);

    QTimer::singleShot(0, [] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        REQUIRE(box != nullptr);
        box->button(QMessageBox::Discard)->click();
    });
    w.window.close();

    REQUIRE(w.rows() == 1);
    CHECK(w.text(0) == "Opened");
}
