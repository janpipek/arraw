#include "support/ListingGate.h"
#include "support/TempDir.h"
#include "ui/FilmStrip.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace arraw;
using namespace arraw::app;

/// The strip's notice of folders that are not watched, and its ways to refresh the folder
/// (incremental-refresh plan, step 5).

namespace {

namespace fs = std::filesystem;

/// Thumbnails go to a folder of the test's own, not the user's cache.
struct Strip {
    test::TempDir cache;
    FilmStrip strip{nullptr, ThumbnailCache(cache.path())};
};

void touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "x";
}

bool listed(const FilmStrip& strip) {
    return QTest::qWaitFor([&] { return !strip.isListing(); }, 10000);
}

QToolButton* notice(const FilmStrip& strip) {
    return strip.titleBar()->findChild<QToolButton*>("filmStripNotice");
}

/// A root with a shot, and three subfolders with one each; the strip includes them.
struct Tree {
    test::TempDir folder;

    Tree() {
        for (const char* name : {"a.jpg", "sub1/b.jpg", "sub2/c.jpg", "sub3/d.jpg"}) {
            touch(folder.file(name));
        }
    }
};

bool hasShot(const FilmStrip& strip, const fs::path& file) {
    return strip.shotContaining(file).has_value();
}

} // namespace

TEST_CASE("The notice counts the folders that are not watched, and a click refreshes",
          "[app][filmstrip][notice]") {
    Tree tree;
    Strip holder;
    FilmStrip& strip = holder.strip;
    auto* button = notice(strip);
    REQUIRE(button);
    REQUIRE_FALSE(strip.hasWatchNotice());

    strip.setMaxWatchedFolders(2);
    strip.setIncludeSubfolders(true);
    strip.setFolder(tree.folder.path());
    REQUIRE(listed(strip));
    REQUIRE(QTest::qWaitFor([&] { return strip.hasWatchNotice(); }, 10000));
    CHECK(button->text() == "2 folders not watched");
    CHECK(button->toolTip().contains("2 of the 4 folders"));
    CHECK(button->toolTip().contains("at most 2 folders"));
    CHECK(button->toolTip().contains("refresh"));

    // A file in a folder without a watch is not seen; the click walks everything again.
    touch(tree.folder.file("sub3/e.jpg"));
    touch(tree.folder.file("a2.jpg"));
    REQUIRE(QTest::qWaitFor([&] { return hasShot(strip, tree.folder.file("a2.jpg")); }, 10000));
    CHECK_FALSE(hasShot(strip, tree.folder.file("sub3/e.jpg")));
    button->click();
    REQUIRE(QTest::qWaitFor([&] { return hasShot(strip, tree.folder.file("sub3/e.jpg")); }, 10000));

    // A folder that goes takes its place in the count; the places go to those that waited.
    fs::remove_all(tree.folder.file("sub3"));
    REQUIRE(QTest::qWaitFor([&] { return button->text() == "1 folder not watched"; }, 10000));
    fs::remove_all(tree.folder.file("sub2"));
    REQUIRE(QTest::qWaitFor([&] { return !strip.hasWatchNotice(); }, 10000));
    CHECK(button->isHidden());
}

TEST_CASE("The notice stays hidden when every folder is watched, and clears with a new folder",
          "[app][filmstrip][notice]") {
    Tree tree;
    const Tree other;
    Strip holder;
    FilmStrip& strip = holder.strip;
    strip.setMaxWatchedFolders(2);
    strip.setIncludeSubfolders(true);
    strip.setFolder(tree.folder.path());
    REQUIRE(listed(strip));
    REQUIRE(QTest::qWaitFor([&] { return strip.hasWatchNotice(); }, 10000));

    // Another folder, with room for all of it: the old notice goes at once, and stays away.
    strip.setMaxWatchedFolders(100);
    strip.setFolder(other.folder.path());
    CHECK_FALSE(strip.hasWatchNotice());
    REQUIRE(listed(strip));
    REQUIRE(
        QTest::qWaitFor([&] { return hasShot(strip, other.folder.file("sub3/d.jpg")); }, 10000));
    CHECK_FALSE(strip.hasWatchNotice());
}

TEST_CASE("Refresh Folder is in the strip's context menu", "[app][filmstrip][notice]") {
    Tree tree;
    Strip holder;
    FilmStrip& strip = holder.strip;
    strip.setMaxWatchedFolders(1);
    strip.setIncludeSubfolders(true);
    strip.resize(700, 160);
    strip.setFolder(tree.folder.path());
    strip.show();
    REQUIRE(QTest::qWaitForWindowExposed(&strip));
    REQUIRE(listed(strip));
    auto* list = strip.findChild<QListView*>();
    REQUIRE(list);
    REQUIRE(QTest::qWaitFor([&] { return list->model()->rowCount() == 4; }, 10000));

    touch(tree.folder.file("sub2/f.jpg"));
    // Opens on a shot and on the empty part of the strip; the timer picks the action from the
    // menu that is open and closes it.
    const auto chooseRefresh = [&](const QPoint& at) {
        bool chosen = false;
        QTimer pick;
        QObject::connect(&pick, &QTimer::timeout, &pick, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (menu == nullptr) {
                return;
            }
            for (QAction* action : menu->actions()) {
                if (action->text() == "Refresh Folder") {
                    chosen = true;
                    action->trigger();
                }
            }
            menu->close();
        });
        pick.start(10);
        QContextMenuEvent event(QContextMenuEvent::Mouse, at, list->viewport()->mapToGlobal(at));
        QApplication::sendEvent(list->viewport(), &event);
        return chosen;
    };
    REQUIRE(chooseRefresh(list->visualRect(list->model()->index(0, 0)).center()));
    REQUIRE(QTest::qWaitFor([&] { return hasShot(strip, tree.folder.file("sub2/f.jpg")); }, 10000));
    // A press while that refresh runs would be taken in by it.
    REQUIRE(QTest::qWaitFor([&] { return !strip.isRefreshing(); }, 10000));

    touch(tree.folder.file("sub3/g.jpg"));
    const QPoint empty(list->viewport()->width() - 4, list->viewport()->height() / 2);
    REQUIRE_FALSE(list->indexAt(empty).isValid());
    REQUIRE(chooseRefresh(empty));
    REQUIRE(QTest::qWaitFor([&] { return hasShot(strip, tree.folder.file("sub3/g.jpg")); }, 10000));
}

TEST_CASE("Refresh Folder says that it runs, and a press during a walk is taken in",
          "[app][filmstrip][notice]") {
    // Review R6.
    Tree tree;
    Strip holder;
    FilmStrip& strip = holder.strip;
    const auto& label = *strip.titleBar()->findChild<QLabel*>("filmStripFolderLabel");
    strip.setIncludeSubfolders(true);

    // During the first listing, which reads everything anyway: no walk after it.
    const test::ListingGate listing(tree.folder.file("sub2"));
    strip.setListingHook(listing.hook());
    strip.setFolder(tree.folder.path());
    REQUIRE(QTest::qWaitFor([&] { return listing.reached(); }, 10000));
    strip.refreshFolder();
    CHECK_FALSE(strip.isRefreshing());
    listing.release();
    REQUIRE(listed(strip));
    CHECK_FALSE(strip.isRefreshing());

    // A refresh held in its walk: the folder label says so.
    const test::ListingGate refresh(tree.folder.file("sub2"));
    strip.setListingHook(refresh.hook());
    strip.refreshFolder();
    REQUIRE(strip.isRefreshing());
    REQUIRE(QTest::qWaitFor([&] { return refresh.reached(); }, 10000));
    REQUIRE(QTest::qWaitFor([&] { return label.text().contains("refreshing… 4 shots"); }, 10000));
    CHECK_FALSE(label.text().contains("listing…"));
    // A second press is folded into it.
    strip.refreshFolder();
    refresh.release();
    REQUIRE(QTest::qWaitFor([&] { return !strip.isRefreshing(); }, 10000));
    // Ended for good, not followed by a second walk; the note goes with it.
    QApplication::processEvents();
    CHECK_FALSE(strip.isRefreshing());
    CHECK_FALSE(label.text().contains("refreshing"));
    const std::vector<fs::path> read = refresh.read();
    CHECK(std::ranges::count(read, tree.folder.path()) == 1);
}
