#include "DebugLog.h"
#include "support/TempDir.h"
#include "ui/FilmStrip.h"
#include "ui/MainWindow.h"
#include "ui/PhotoView.h"
#include "ui/SettingSlider.h"

#include <Photo.h>
#include <Sidecar.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QSettings>
#include <QTest>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace arraw;
using namespace arraw::app;

/// Copying the settings of one photograph and pasting them onto another, in the window.

namespace {

/// A window over a folder of two photographs, with an exposure of its own each.
struct Window {
    test::TempDir folder;
    DebugLog debugLog;
    MainWindow window{debugLog};

    Window() {
        QSettings().remove("copySettings/sections");
        const std::filesystem::path fixtures(ARRAW_TEST_DATA_DIR);
        addShot(fixtures / "preview-32x24.dng", "a.dng", 0.5F);
        addShot(fixtures / "bayer-32x24.dng", "b.dng", 1.0F);
        window.openInitialPath(folder.file("a.dng"));
        window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&window));
        REQUIRE(waitForRender());
    }

    void addShot(const std::filesystem::path& from, const char* name, float exposure) const {
        const std::filesystem::path path = folder.file(name);
        std::filesystem::copy_file(from, path);
        const Photo bare = openPhoto(path);
        DevelopState state = bare.state();
        state.settings.tone.exposure = exposure;
        writeSidecar(Photo(path, bare.metadata(), state));
    }

    [[nodiscard]] PhotoView& view() const {
        return *window.findChild<PhotoView*>();
    }

    [[nodiscard]] FilmStrip& strip() const {
        return *window.findChild<FilmStrip*>();
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

    [[nodiscard]] bool waitForRender() const {
        return QTest::qWaitFor([this] { return !view().wholeFrameImage().isNull(); }, 20000);
    }

    void open(const char* name) const {
        emit strip().activationRequested(QString::fromStdU16String(folder.file(name).u16string()));
        REQUIRE(waitForRender());
    }

    [[nodiscard]] double panelExposure() const {
        for (auto* row : window.findChildren<SettingSlider*>()) {
            if (row->key() == "exposure") {
                return row->findChild<QDoubleSpinBox*>()->value();
            }
        }
        FAIL("no exposure row");
        throw;
    }

    /// Triggers Copy and accepts its dialog.
    void copy() const {
        QDialog* seen = nullptr;
        QTimer::singleShot(0, [&seen] {
            seen = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (seen != nullptr) {
                seen->accept();
            }
        });
        action("copySettingsAction").trigger();
        REQUIRE(seen != nullptr);
    }
};

} // namespace

TEST_CASE("Pasting gives the photograph the copied settings, in one history step",
          "[app][window][copy]") {
    Window w;
    CHECK_FALSE(w.action("pasteSettingsAction").isEnabled());
    CHECK(w.action("copySettingsAction").isEnabled());

    w.copy();
    CHECK(w.action("pasteSettingsAction").isEnabled());

    w.open("b.dng");
    REQUIRE(w.panelExposure() == 1.0);
    CHECK(w.action("pasteSettingsAction").isEnabled());
    w.action("pasteSettingsAction").trigger();
    CHECK(w.panelExposure() == 0.5);
    REQUIRE(w.action("undoAction").isEnabled());
    w.action("undoAction").trigger();
    CHECK(w.panelExposure() == 1.0);
    CHECK_FALSE(w.action("undoAction").isEnabled());
}

TEST_CASE("Paste is off in the crop mode", "[app][window][copy][crop]") {
    Window w;
    w.copy();
    REQUIRE(w.action("pasteSettingsAction").isEnabled());
    w.action("cropAction").trigger();
    REQUIRE(w.view().isCropMode());
    CHECK_FALSE(w.action("pasteSettingsAction").isEnabled());
    w.action("cropAction").trigger();
    CHECK(w.action("pasteSettingsAction").isEnabled());
}
