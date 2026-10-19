#include "support/TempDir.h"
#include "ui/FilmStrip.h"

#include <QCoreApplication>
#include <QImage>
#include <QListView>
#include <QSignalSpy>
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
