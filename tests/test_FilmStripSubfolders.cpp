#include "DebugLog.h"
#include "support/ListingGate.h"
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
#include <QItemSelectionModel>
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
#include <set>
#include <string>
#include <vector>

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

/// Waits for the strip's listing, which runs on the watcher's thread, to bring every shot.
bool listed(const FilmStrip& strip) {
    return QTest::qWaitFor([&] { return !strip.isListing(); }, 10000);
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
    REQUIRE(listed(strip));
    auto* button = subfoldersButton(strip);
    REQUIRE(button);
    REQUIRE_FALSE(button->isChecked());
    strip.setActive(folder.file("a.jpg"));

    QSignalSpy toggled(&strip, &FilmStrip::includeSubfoldersToggled);
    button->click();
    REQUIRE(strip.includeSubfolders());
    REQUIRE(toggled.count() == 1);
    REQUIRE(listed(strip));
    REQUIRE(strip.shotContaining(folder.file("sub/b.jpg")));
    REQUIRE(strip.activePrimary() == folder.file("a.jpg"));

    // A new folder keeps the flag.
    const test::TempDir other;
    fs::create_directories(other.file("deeper"));
    touch(other.file("deeper/c.jpg"));
    strip.setFolder(other.path());
    REQUIRE(listed(strip));
    REQUIRE(strip.shotContaining(other.file("deeper/c.jpg")));

    // Off again: the active shot in a subfolder goes with it.
    strip.setActive(other.file("deeper/c.jpg"));
    button->click();
    REQUIRE_FALSE(strip.includeSubfolders());
    REQUIRE(listed(strip));
    REQUIRE_FALSE(strip.shotContaining(other.file("deeper/c.jpg")));
    REQUIRE_FALSE(strip.activePrimary());
}

TEST_CASE("The film strip's subfolder toggle resets nothing, keeps the selection, and reads the "
          "subfolders once",
          "[app][filmstrip][subfolders]") {
    const test::TempDir folder;
    touch(folder.file("a.jpg"));
    touch(folder.file("z.jpg"));
    fs::create_directories(folder.file("sub/deep"));
    touch(folder.file("sub/b.jpg"));
    touch(folder.file("sub/deep/c.jpg"));
    Strip holder;
    FilmStrip& strip = holder.strip;
    strip.setFolder(folder.path());
    REQUIRE(listed(strip));
    auto* button = subfoldersButton(strip);
    REQUIRE(button);
    auto* list = strip.findChild<QListView*>();
    REQUIRE(list);
    strip.setActive(folder.file("z.jpg"));
    const test::ListingGate reads(folder.file("never"));
    strip.setListingHook(reads.hook());
    QSignalSpy resets(list->model(), &QAbstractItemModel::modelReset);
    QSignalSpy folderChanged(&strip, &FilmStrip::folderChanged);

    button->click();
    REQUIRE(listed(strip));
    REQUIRE(strip.shotContaining(folder.file("sub/deep/c.jpg")));
    CHECK(strip.activePrimary() == folder.file("z.jpg"));
    CHECK(list->selectionModel()->selectedIndexes().size() == 1);
    CHECK(resets.count() == 0);
    CHECK(folderChanged.count() == 0);
    // The root once more (where the subfolders are found), then each subfolder once.
    CHECK(reads.read() ==
          std::vector<fs::path>{folder.path(), folder.file("sub"), folder.file("sub/deep")});

    // Off while the active shot is in the folder itself: it stays, selected.
    button->click();
    CHECK_FALSE(strip.shotContaining(folder.file("sub/b.jpg")));
    CHECK(strip.activePrimary() == folder.file("z.jpg"));
    CHECK(list->selectionModel()->selectedIndexes().size() == 1);
    CHECK(resets.count() == 0);
    REQUIRE(QTest::qWaitFor([&] { return reads.read().size() == 4; }, 10000));
    CHECK(reads.read().back() == folder.path());
    CHECK(list->model()->rowCount() == 2);
}

TEST_CASE("The film strip's cells are taller than wide, by a caption line, and fit the strip",
          "[app][filmstrip]") {
    const test::TempDir folder;
    touch(folder.file("IMG_0042.jpg"));
    Strip holder;
    FilmStrip& strip = holder.strip;
    strip.resize(600, 200);
    strip.setFolder(folder.path());
    REQUIRE(listed(strip));
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

    /// Opens a file as File > Open does, and waits for its first render and the strip's listing.
    void open(const fs::path& path) {
        window.openInitialPath(path);
        REQUIRE(QTest::qWaitFor([this] { return !view().wholeFrameImage().isNull(); }, 20000));
        REQUIRE(listed(strip()));
    }

    /// Sets the strip's subfolder flag as a menu would, and waits for the listing.
    void includeSubfolders(bool include) const {
        strip().setIncludeSubfolders(include);
        REQUIRE(listed(strip()));
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
    w.includeSubfolders(true);
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
    w.includeSubfolders(true);

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
        w.includeSubfolders(true);
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
        w.includeSubfolders(true);
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
        w.includeSubfolders(true);
        w.open(w.folder.file("sub/b.dng"));
        w.edit();
        w.answer(QMessageBox::Save);
        subfoldersButton(w.strip())->click();
        CHECK(w.boxes == 1);
        CHECK(fs::exists(sidecarPath(w.folder.file("sub/b.dng"))));
        REQUIRE(QTest::qWaitFor([&] { return w.strip().activePrimary() == w.folder.file("a.dng"); },
                                5000));
    }
    SECTION("No edits, no question") {
        Window w;
        w.open(w.folder.file("a.dng"));
        w.includeSubfolders(true);
        w.open(w.folder.file("sub/b.dng"));
        subfoldersButton(w.strip())->click();
        CHECK(w.boxes == 0);
        REQUIRE(QTest::qWaitFor([&] { return w.strip().activePrimary() == w.folder.file("a.dng"); },
                                5000));
    }
    SECTION("A photograph that stays listed is not asked about") {
        Window w;
        w.open(w.folder.file("a.dng"));
        w.includeSubfolders(true);
        w.edit();
        subfoldersButton(w.strip())->click();
        CHECK(w.boxes == 0);
        CHECK_FALSE(w.strip().includeSubfolders());
        REQUIRE(listed(w.strip()));
        CHECK(w.strip().activePrimary() == w.folder.file("a.dng"));
    }
    SECTION("An empty listing leaves the photograph open") {
        Window w(false);
        w.open(w.folder.file("sub/b.dng"));
        w.strip().setFolder(w.folder.path());
        w.includeSubfolders(true);
        w.strip().setActive(w.folder.file("sub/b.dng"));
        subfoldersButton(w.strip())->click();
        CHECK(w.boxes == 0);
        CHECK_FALSE(w.strip().includeSubfolders());
        REQUIRE(listed(w.strip()));
        CHECK_FALSE(w.strip().activePrimary());
        CHECK_FALSE(w.view().wholeFrameImage().isNull());
    }
}

TEST_CASE("The film strip keeps its selection when a refresh is too scattered to merge by runs",
          "[app][filmstrip][merge]") {
    const test::TempDir folder;
    const auto name = [](int n) { return "IMG_" + std::to_string(1000 + n) + ".jpg"; };
    for (int n = 0; n < 200; n += 2) {
        touch(folder.file(name(n)));
    }
    Strip holder;
    FilmStrip& strip = holder.strip;
    strip.setFolder(folder.path());
    REQUIRE(listed(strip));
    auto* list = strip.findChild<QListView*>();
    REQUIRE(list);
    REQUIRE(list->model()->rowCount() == 100);
    strip.setActive(folder.file(name(40)));

    // Three more shots are selected by hand, the active one among them.
    QItemSelectionModel* selection = list->selectionModel();
    for (const int n : {20, 40, 60}) {
        const QString path = QString::fromStdU16String(folder.file(name(n)).u16string());
        for (int row = 0; row < list->model()->rowCount(); ++row) {
            if (list->model()->index(row, 0).data(ShotModel::PathRole).toString() == path) {
                selection->select(list->model()->index(row, 0), QItemSelectionModel::Select);
            }
        }
    }
    REQUIRE(selection->selectedIndexes().size() == 3);

    // The odd numbers arrive between them all: far past the limit of runs, so the model resets.
    for (int n = 1; n < 200; n += 2) {
        touch(folder.file(name(n)));
    }
    REQUIRE(QTest::qWaitFor([&] { return list->model()->rowCount() == 200; }, 10000));

    std::set<std::string> selected;
    for (const QModelIndex& index : selection->selectedIndexes()) {
        selected.insert(index.data(ShotModel::PathRole).toString().toStdString());
    }
    REQUIRE(selected == std::set<std::string>{folder.file(name(20)).string(),
                                              folder.file(name(40)).string(),
                                              folder.file(name(60)).string()});
    REQUIRE(strip.activePrimary() == folder.file(name(40)));
    REQUIRE(selection->currentIndex().isValid());
    REQUIRE(selection->currentIndex().data(ShotModel::PathRole).toString().toStdString() ==
            folder.file(name(40)).string());
}
