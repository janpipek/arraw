#include "DebugLog.h"
#include "support/TempDir.h"
#include "ui/DevelopPanel.h"
#include "ui/FilmStrip.h"
#include "ui/MainWindow.h"
#include "ui/PhotoView.h"
#include "ui/SettingSlider.h"

#include <Photo.h>
#include <Sidecar.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QImage>
#include <QListView>
#include <QMessageBox>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace arraw;
using namespace arraw::app;

namespace {

namespace fs = std::filesystem;

/// Thumbnails go to a folder of the test's own, not the user's cache.
struct Strip {
    test::TempDir cache;
    FilmStrip strip{nullptr, ThumbnailCache(cache.path())};
};

void touch(const fs::path& path) {
    std::ofstream(path) << "x";
}

QToolButton* subfoldersButton(const FilmStrip& strip) {
    for (auto* button : strip.titleBar()->findChildren<QToolButton*>()) {
        if (button->isCheckable() && button->toolTip() == "Include subfolders") {
            return button;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("The film strip's subfolder toggle re-lists the folder and keeps the active shot",
          "[app][filmstrip][subfolders]") {
    const test::TempDir folder;
    fs::create_directories(folder.file("sub"));
    touch(folder.file("a.jpg"));
    touch(folder.file("sub/b.jpg"));
    Strip holder;
    FilmStrip& strip = holder.strip;
    strip.setFolder(folder.path());
    auto* button = subfoldersButton(strip);
    REQUIRE(button);
    REQUIRE_FALSE(button->isChecked());
    strip.setActive(folder.file("a.jpg"));

    QSignalSpy toggled(&strip, &FilmStrip::includeSubfoldersToggled);
    button->click();
    REQUIRE(strip.includeSubfolders());
    REQUIRE(toggled.count() == 1);
    REQUIRE(strip.shotContaining(folder.file("sub/b.jpg")));
    REQUIRE(strip.activePrimary() == folder.file("a.jpg"));

    // A new folder keeps the flag.
    const test::TempDir other;
    fs::create_directories(other.file("deeper"));
    touch(other.file("deeper/c.jpg"));
    strip.setFolder(other.path());
    REQUIRE(strip.shotContaining(other.file("deeper/c.jpg")));

    // Off again: the active shot in a subfolder goes with it.
    strip.setActive(other.file("deeper/c.jpg"));
    button->click();
    REQUIRE_FALSE(strip.includeSubfolders());
    REQUIRE_FALSE(strip.shotContaining(other.file("deeper/c.jpg")));
    REQUIRE_FALSE(strip.activePrimary());
}

TEST_CASE("The film strip's cells are taller than wide, by a caption line, and fit the strip",
          "[app][filmstrip]") {
    const test::TempDir folder;
    touch(folder.file("IMG_0042.jpg"));
    Strip holder;
    FilmStrip& strip = holder.strip;
    strip.resize(600, 200);
    strip.setFolder(folder.path());
    strip.show();
    auto* list = strip.findChild<QListView*>();
    REQUIRE(list);
    QCoreApplication::processEvents();
    const QSize cell = list->sizeHintForIndex(list->model()->index(0, 0));
    CHECK(cell.height() > cell.width());
    CHECK(cell.height() <= list->viewport()->height());
    const QImage grabbed = strip.grab().toImage();
    CHECK_FALSE(grabbed.isNull());
    if (const char* dir = std::getenv("ARRAW_TEST_SCREENSHOT_DIR")) {
        grabbed.save(QString::fromUtf8(dir) + "/filmstrip.png");
    }
}

namespace {

/// Starts and ends with the subfolder flag and the remembered folder unset, in the shared store.
struct CleanSettings {
    static void clear() {
        QSettings store;
        store.remove("filmStrip/includeSubfolders");
        store.remove("lastFolder");
        store.remove("lastFile");
    }
    CleanSettings() {
        clear();
    }
    ~CleanSettings() {
        clear();
    }
};

/// A window over `a.dng`, with `sub/b.dng` and `sub/c.dng` below it and `.hid/h.dng` beside them.
struct Window {
    CleanSettings settings;
    test::TempDir folder;
    DebugLog debugLog;
    MainWindow window{debugLog};

    Window(bool withRootShot = true) {
        const fs::path fixtures(ARRAW_TEST_DATA_DIR);
        fs::create_directories(folder.file("sub"));
        fs::create_directories(folder.file(".hid"));
        if (withRootShot) {
            fs::copy_file(fixtures / "preview-32x24.dng", folder.file("a.dng"));
        } else {
            touch(folder.file("note.txt"));
        }
        fs::copy_file(fixtures / "bayer-32x24.dng", folder.file("sub/b.dng"));
        fs::copy_file(fixtures / "bayer-32x24.dng", folder.file("sub/c.dng"));
        fs::copy_file(fixtures / "bayer-32x24.dng", folder.file(".hid/h.dng"));
        window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&window));
    }

    [[nodiscard]] FilmStrip& strip() const {
        return *window.findChild<FilmStrip*>();
    }

    [[nodiscard]] PhotoView& view() const {
        return *window.findChild<PhotoView*>();
    }

    /// Opens a file as File > Open does, and waits for its first render.
    void open(const fs::path& path) {
        window.openInitialPath(path);
        REQUIRE(QTest::qWaitFor([this] { return !view().wholeFrameImage().isNull(); }, 20000));
    }

    /// Edits the open photograph, so that leaving it asks.
    void edit() const {
        for (auto* row : window.findChildren<SettingSlider*>()) {
            if (row->key() == "exposure") {
                row->findChild<QDoubleSpinBox*>()->setValue(0.75);
            }
        }
        window.findChild<DevelopPanel*>()->finishPendingEdit();
    }

    /// Answers the next Save/Discard/Cancel box; counts the boxes seen.
    void answer(QMessageBox::StandardButton button) {
        QTimer::singleShot(0, [this, button] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            REQUIRE(box != nullptr);
            ++boxes;
            box->button(button)->click();
        });
    }

    int boxes = 0;
};

QString remembered(const char* key) {
    return QSettings().value(key).toString();
}

QString text(const fs::path& path) {
    return QString::fromStdU16String(path.u16string());
}

} // namespace

TEST_CASE("A photograph the strip does not list switches the strip to its folder",
          "[app][window][subfolders]") {
    Window w;
    w.open(w.folder.file("a.dng"));
    w.strip().setIncludeSubfolders(true);
    REQUIRE(w.strip().shotContaining(w.folder.file("sub/b.dng")));

    // Below the folder, but in a hidden one, which the listing skips.
    w.open(w.folder.file(".hid/h.dng"));
    CHECK(w.strip().folder() == w.folder.file(".hid"));
    CHECK(w.strip().activePrimary() == w.folder.file(".hid/h.dng"));
    CHECK(remembered("lastFolder") == text(w.folder.file(".hid")));
}

TEST_CASE("Only the strip's folder is remembered, not the folder of a photograph below it",
          "[app][window][subfolders]") {
    Window w;
    w.open(w.folder.file("a.dng"));
    w.strip().setIncludeSubfolders(true);

    w.open(w.folder.file("sub/b.dng"));
    CHECK(w.strip().folder() == w.folder.path());
    CHECK(w.strip().activePrimary() == w.folder.file("sub/b.dng"));
    CHECK(remembered("lastFolder") == text(w.folder.path()));
    CHECK(remembered("lastFile") == text(w.folder.file("sub/b.dng")));

    // A photograph outside the strip's folder moves the strip, and the memory with it.
    const test::TempDir other;
    fs::copy_file(fs::path(ARRAW_TEST_DATA_DIR) / "bayer-32x24.dng", other.file("o.dng"));
    w.open(other.file("o.dng"));
    CHECK(w.strip().folder() == other.path());
    CHECK(remembered("lastFolder") == text(other.path()));

    // Opening a photograph by the strip, from below the root, leaves the memory alone too.
    w.open(w.folder.file("a.dng"));
    w.strip().setActive(w.folder.file("a.dng"));
    emit w.strip().activationRequested(text(w.folder.file("sub/c.dng")));
    CHECK(remembered("lastFolder") == text(w.folder.path()));
}

TEST_CASE("Switching subfolders off with a subfolder photograph open asks about its edits",
          "[app][window][subfolders]") {
    SECTION("Cancel keeps the toggle on, the strip and the photograph") {
        Window w;
        w.open(w.folder.file("a.dng"));
        w.strip().setIncludeSubfolders(true);
        w.open(w.folder.file("sub/b.dng"));
        w.edit();
        auto* button = subfoldersButton(w.strip());
        REQUIRE(button);
        w.answer(QMessageBox::Cancel);
        button->click();
        CHECK(w.boxes == 1);
        CHECK(button->isChecked());
        CHECK(w.strip().includeSubfolders());
        CHECK(w.strip().shotContaining(w.folder.file("sub/b.dng")));
        CHECK(w.strip().activePrimary() == w.folder.file("sub/b.dng"));
        CHECK(QSettings().value("filmStrip/includeSubfolders").toBool());
    }
    SECTION("Discard lists the folder alone and opens its first shot") {
        Window w;
        w.open(w.folder.file("a.dng"));
        w.strip().setIncludeSubfolders(true);
        w.open(w.folder.file("sub/b.dng"));
        w.edit();
        auto* button = subfoldersButton(w.strip());
        w.answer(QMessageBox::Discard);
        button->click();
        CHECK(w.boxes == 1);
        CHECK_FALSE(w.strip().includeSubfolders());
        CHECK_FALSE(w.strip().shotContaining(w.folder.file("sub/b.dng")));
        REQUIRE(QTest::qWaitFor([&] { return w.strip().activePrimary() == w.folder.file("a.dng"); },
                                5000));
        CHECK(w.boxes == 1);
    }
    SECTION("Save writes the sidecar first") {
        Window w;
        w.open(w.folder.file("a.dng"));
        w.strip().setIncludeSubfolders(true);
        w.open(w.folder.file("sub/b.dng"));
        w.edit();
        w.answer(QMessageBox::Save);
        subfoldersButton(w.strip())->click();
        CHECK(w.boxes == 1);
        CHECK(fs::exists(sidecarPath(w.folder.file("sub/b.dng"))));
        CHECK(w.strip().activePrimary() == w.folder.file("a.dng"));
    }
    SECTION("No edits, no question") {
        Window w;
        w.open(w.folder.file("a.dng"));
        w.strip().setIncludeSubfolders(true);
        w.open(w.folder.file("sub/b.dng"));
        subfoldersButton(w.strip())->click();
        CHECK(w.boxes == 0);
        CHECK(w.strip().activePrimary() == w.folder.file("a.dng"));
    }
    SECTION("A photograph that stays listed is not asked about") {
        Window w;
        w.open(w.folder.file("a.dng"));
        w.strip().setIncludeSubfolders(true);
        w.edit();
        subfoldersButton(w.strip())->click();
        CHECK(w.boxes == 0);
        CHECK_FALSE(w.strip().includeSubfolders());
        CHECK(w.strip().activePrimary() == w.folder.file("a.dng"));
    }
    SECTION("An empty listing leaves the photograph open") {
        Window w(false);
        w.open(w.folder.file("sub/b.dng"));
        w.strip().setFolder(w.folder.path());
        w.strip().setIncludeSubfolders(true);
        w.strip().setActive(w.folder.file("sub/b.dng"));
        subfoldersButton(w.strip())->click();
        CHECK(w.boxes == 0);
        CHECK_FALSE(w.strip().includeSubfolders());
        CHECK_FALSE(w.strip().activePrimary());
        CHECK_FALSE(w.view().wholeFrameImage().isNull());
    }
}
