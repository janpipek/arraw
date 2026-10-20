#include "FolderLister.h"
#include "FolderResult.h"
#include "FolderWatcher.h"
#include "ShotModel.h"
#include "support/ListingGate.h"
#include "support/TempDir.h"

#include <Shot.h>
#include <Sidecar.h>

#include <QCoreApplication>
#include <QImage>
#include <QPointer>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#endif

using namespace arraw;
using namespace arraw::app;

/// The background listing of the film strip (incremental-refresh plan, step 3): the model's walk
/// API fed by hand, and the lister and the watcher on their real thread.

namespace {

namespace fs = std::filesystem;

/// Runs the event loop until a condition holds; false if it does not within the time.
bool waitUntil(const std::function<bool()>& condition,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(10000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return true;
}

/// Puts an empty standard image in a folder; listing never opens it.
fs::path addJpeg(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).put('\0');
    return path;
}

/// A shot of one JPEG, without touching the disk.
Shot jpeg(const fs::path& path) {
    return Shot{.primary = path, .companions = {}};
}

/// Shots `IMG_0.jpg` to `IMG_<count - 1>.jpg` of a folder, in natural order.
std::vector<Shot> jpegs(const fs::path& folder, int count) {
    std::vector<Shot> shots;
    for (int n = 0; n < count; ++n) {
        shots.push_back(jpeg(folder / ("IMG_" + std::to_string(n) + ".jpg")));
    }
    return shots;
}

/// Primaries in row order.
std::vector<fs::path> primaries(const ShotModel& model) {
    std::vector<fs::path> result;
    for (int row = 0; row < model.rowCount(); ++row) {
        result.push_back(model.shot(row).primary);
    }
    return result;
}

/// Primaries of a listing.
std::vector<fs::path> primaries(const std::vector<Shot>& shots) {
    std::vector<fs::path> result;
    for (const Shot& shot : shots) {
        result.push_back(shot.primary);
    }
    return result;
}

/// Records the row signals of a model as text such as `+3..4` and `-1..2`.
struct RowLog {
    std::vector<std::string> events;

    explicit RowLog(ShotModel& model) {
        QObject::connect(&model, &QAbstractItemModel::rowsInserted, &model,
                         [this](const QModelIndex&, int first, int last) {
                             events.push_back("+" + std::to_string(first) + ".." +
                                              std::to_string(last));
                         });
        QObject::connect(&model, &QAbstractItemModel::rowsRemoved, &model,
                         [this](const QModelIndex&, int first, int last) {
                             events.push_back("-" + std::to_string(first) + ".." +
                                              std::to_string(last));
                         });
    }
};

} // namespace

TEST_CASE("A model takes its rows from the walk it asked for, and drops other walks",
          "[shotmodel][listing]") {
    // The folder only has to open; its shots are made up and fed by hand.
    const test::TempDir folder;
    const fs::path root = folder.path();
    ShotModel model;
    int walks = 0;
    std::vector<bool> listing;
    int finished = 0;
    QObject::connect(&model, &ShotModel::walkRequested, &model, [&] { ++walks; });
    QObject::connect(&model, &ShotModel::listingChanged, &model,
                     [&](bool now) { listing.push_back(now); });
    QObject::connect(&model, &ShotModel::loadingFinished, &model, [&] { ++finished; });

    model.setFolder(root, true);
    REQUIRE(walks == 1);
    REQUIRE(model.isListing());
    REQUIRE(listing == std::vector<bool>{true});
    REQUIRE(model.rowCount() == 0);
    const std::uint64_t walk = model.walk();
    RowLog log(model);

    SECTION("Batches are appended in walk order, the new folders of a batch in one insert") {
        model.applyFolders(walk, {{root, {jpeg(root / "IMG_1.jpg"), jpeg(root / "IMG_2.jpg")}},
                                  {root / "a", {jpeg(root / "a/IMG_1.jpg")}}});
        model.applyFolders(walk, {{root / "a/deep", {}}, {root / "b", {jpeg(root / "b/X.jpg")}}});
        REQUIRE(log.events == std::vector<std::string>{"+0..2", "+3..3"});
        REQUIRE(model.rowOf(root / "b/X.jpg") == 3);
        REQUIRE(model.isListing());
        model.finishListing(walk, {});
        REQUIRE_FALSE(model.isListing());
        REQUIRE(listing == std::vector<bool>{true, false});
        REQUIRE(model.rowCount() == 4);
    }
    SECTION("A folder that arrives later goes where the folder order puts it") {
        model.applyFolders(
            walk, {{root, {jpeg(root / "IMG_1.jpg")}}, {root / "c", {jpeg(root / "c/IMG_1.jpg")}}});
        model.applyFolders(
            walk, {{root / "b", {jpeg(root / "b/IMG_1.jpg"), jpeg(root / "b/IMG_2.jpg")}}});
        REQUIRE(log.events == std::vector<std::string>{"+0..1", "+1..2"});
        REQUIRE(primaries(model) == std::vector<fs::path>{root / "IMG_1.jpg", root / "b/IMG_1.jpg",
                                                          root / "b/IMG_2.jpg",
                                                          root / "c/IMG_1.jpg"});
        REQUIRE(model.rowOf(root / "c/IMG_1.jpg") == 3);
    }
    SECTION("A large folder is inserted in parts; the folders after it share the last part") {
        model.applyFolders(walk, {{root, jpegs(root, 4500)},
                                  {root / "a", {jpeg(root / "a/IMG_1.jpg")}},
                                  {root / "b", {jpeg(root / "b/IMG_1.jpg")}}});
        REQUIRE(log.events == std::vector<std::string>{"+0..1999", "+2000..3999", "+4000..4501"});
        REQUIRE(model.rowCount() == 4502);
        REQUIRE(model.rowOf(root / "IMG_4499.jpg") == 4499);
        REQUIRE(model.rowOf(root / "b/IMG_1.jpg") == 4501);
        // Its rows are one folder: a refresh that drops a shot merges them as one span.
        model.finishListing(walk, {});
        model.refresh();
        log.events.clear();
        std::vector<Shot> fewer = jpegs(root, 4500);
        fewer.erase(fewer.begin() + 10);
        model.applyFolders(model.walk(), {{root, fewer},
                                          {root / "a", {jpeg(root / "a/IMG_1.jpg")}},
                                          {root / "b", {jpeg(root / "b/IMG_1.jpg")}}});
        model.finishListing(model.walk(), {});
        REQUIRE(log.events == std::vector<std::string>{"-10..10"});
    }
    SECTION("A new folder of a refresh is inserted in parts where it belongs") {
        model.applyFolders(
            walk, {{root, {jpeg(root / "IMG_1.jpg")}}, {root / "c", {jpeg(root / "c/IMG_1.jpg")}}});
        model.finishListing(walk, {});
        model.refresh();
        log.events.clear();
        model.applyFolders(model.walk(), {{root, {jpeg(root / "IMG_1.jpg")}},
                                          {root / "b", jpegs(root / "b", 2500)},
                                          {root / "c", {jpeg(root / "c/IMG_1.jpg")}}});
        model.finishListing(model.walk(), {});
        REQUIRE(log.events == std::vector<std::string>{"+1..2000", "+2001..2500"});
        REQUIRE(model.rowOf(root / "b/IMG_2499.jpg") == 2500);
        REQUIRE(model.rowOf(root / "c/IMG_1.jpg") == 2501);
    }
    SECTION("Queued results are applied from the event loop, in order, the end last") {
        model.queueFolders(walk, {{root, jpegs(root, 4500)}});
        model.queueFolders(walk + 1, {{root / "x", {jpeg(root / "x/IMG_1.jpg")}}});
        model.queueFolders(walk, {{root / "a", {jpeg(root / "a/IMG_1.jpg")}}});
        model.queueFinish(walk, {});
        // Nothing is applied before the event loop runs.
        REQUIRE(model.rowCount() == 0);
        REQUIRE(model.isListing());
        REQUIRE(waitUntil([&] { return !model.isListing(); }));
        REQUIRE(model.rowCount() == 4501);
        REQUIRE(model.rowOf(root / "a/IMG_1.jpg") == 4500);
        REQUIRE(model.rowOf(root / "x/IMG_1.jpg") < 0);
        // Batches are not merged: a is an insert of its own.
        REQUIRE(log.events ==
                std::vector<std::string>{"+0..1999", "+2000..3999", "+4000..4499", "+4500..4500"});
        REQUIRE(listing == std::vector<bool>{true, false});
    }
    SECTION("A new folder drops what is still queued") {
        model.queueFolders(walk, {{root, jpegs(root, 10)}});
        model.queueFinish(walk, {});
        model.setFolder(root, false);
        QCoreApplication::processEvents();
        REQUIRE(model.rowCount() == 0);
        REQUIRE(model.isListing());
    }
    SECTION("A batch or an end of another walk is dropped") {
        model.applyFolders(walk + 1, {{root, {jpeg(root / "IMG_1.jpg")}}});
        model.finishListing(walk + 1, {});
        REQUIRE(model.rowCount() == 0);
        REQUIRE(model.isListing());

        // A new folder makes the walk under way stale; so does setting the same one again.
        model.setFolder(root, false);
        REQUIRE(walks == 2);
        model.applyFolders(walk, {{root, {jpeg(root / "IMG_1.jpg")}}});
        model.finishListing(walk, {});
        REQUIRE(model.rowCount() == 0);
        REQUIRE(model.isListing());
        model.applyFolders(model.walk(), {{root, {jpeg(root / "IMG_2.jpg")}}});
        REQUIRE(primaries(model) == std::vector<fs::path>{root / "IMG_2.jpg"});
    }
    SECTION("The loading finishes only once the walk has") {
        model.applyFolders(walk, {{root, {}}});
        // Nothing to read, but the walk is not over.
        QCoreApplication::processEvents();
        REQUIRE(finished == 0);
        model.finishListing(walk, {});
        REQUIRE(finished == 1);
    }
    SECTION("A refresh asked for during the listing or a refresh is taken in by that walk") {
        std::vector<bool> refreshing;
        QObject::connect(&model, &ShotModel::refreshingChanged, &model,
                         [&](bool now) { refreshing.push_back(now); });
        model.applyFolders(walk, {{root, {jpeg(root / "IMG_1.jpg")}},
                                  {root / "gone", {jpeg(root / "gone/IMG_1.jpg")}}});
        // The listing reads everything: no second walk after it (review R6).
        model.refresh();
        REQUIRE(walks == 1);
        REQUIRE_FALSE(model.isRefreshing());
        model.finishListing(walk, {});
        REQUIRE(walks == 1);
        REQUIRE(refreshing.empty());

        // A refresh, under a number of its own; the loader's generation is not changed. A second
        // press while it runs is folded into it.
        model.refresh();
        model.refresh();
        REQUIRE(walks == 2);
        REQUIRE(model.walk() == walk + 1);
        REQUIRE_FALSE(model.isListing());
        REQUIRE(model.isRefreshing());
        REQUIRE(refreshing == std::vector<bool>{true});
        log.events.clear();
        // The refresh finds a new shot in the root and no "gone": that folder's rows go at the end.
        model.applyFolders(model.walk(),
                           {{root, {jpeg(root / "IMG_1.jpg"), jpeg(root / "IMG_2.jpg")}}});
        REQUIRE(model.rowCount() == 3);
        model.finishListing(model.walk(), {});
        REQUIRE_FALSE(model.isRefreshing());
        REQUIRE(walks == 2);
        REQUIRE(refreshing == std::vector<bool>{true, false});
        REQUIRE(log.events == std::vector<std::string>{"+1..1", "-2..2"});
        REQUIRE(primaries(model) == std::vector<fs::path>{root / "IMG_1.jpg", root / "IMG_2.jpg"});
        REQUIRE(listing == std::vector<bool>{true, false});

        // A new folder ends a refresh under way.
        model.refresh();
        model.setFolder(root, true);
        REQUIRE_FALSE(model.isRefreshing());
        REQUIRE(refreshing == std::vector<bool>{true, false, true, false});
    }
    SECTION("A batch that holds a new folder before the one ahead of it puts each in its place") {
        // Review B1: a parent read again for a change finds a new folder that sorts before the
        // one the walk read last, in the same batch.
        model.applyFolders(walk, {{root, {jpeg(root / "IMG_1.jpg")}},
                                  {root / "2026", {}},
                                  {root / "2026/10-19", {jpeg(root / "2026/10-19/A.jpg")}}});
        model.applyFolders(walk, {{root / "2026/10-21", {jpeg(root / "2026/10-21/C.jpg")}},
                                  {root / "2026", {}},
                                  {root / "2026/10-20", {jpeg(root / "2026/10-20/B.jpg")}}});
        const std::vector<fs::path> expected{root / "IMG_1.jpg", root / "2026/10-19/A.jpg",
                                             root / "2026/10-20/B.jpg", root / "2026/10-21/C.jpg"};
        REQUIRE(primaries(model) == expected);
        for (int row = 0; row < static_cast<int>(expected.size()); ++row) {
            REQUIRE(model.rowOf(expected[static_cast<std::size_t>(row)]) == row);
        }
        // The spans agree with the rows: each folder goes with one remove.
        model.removeSubtree(walk, root / "2026/10-20");
        REQUIRE(primaries(model) == std::vector<fs::path>{root / "IMG_1.jpg",
                                                          root / "2026/10-19/A.jpg",
                                                          root / "2026/10-21/C.jpg"});
        REQUIRE(model.rowOf(root / "2026/10-21/C.jpg") == 2);
    }
    SECTION("A batch that holds a folder twice takes its later read, once") {
        // Review B1: a directory read by the walk, then again for a change, before the batch went.
        model.applyFolders(walk, {{root, {jpeg(root / "IMG_1.jpg")}}});
        model.applyFolders(walk,
                           {{root / "a", {jpeg(root / "a/IMG_1.jpg")}},
                            {root / "b", {jpeg(root / "b/IMG_1.jpg")}},
                            {root / "a", {jpeg(root / "a/IMG_1.jpg"), jpeg(root / "a/IMG_2.jpg")}},
                            {root / "b", {}}});
        const std::vector<fs::path> expected{root / "IMG_1.jpg", root / "a/IMG_1.jpg",
                                             root / "a/IMG_2.jpg"};
        REQUIRE(primaries(model) == expected);
        REQUIRE(model.rowCount() == 3);
        for (int row = 0; row < 3; ++row) {
            REQUIRE(model.rowOf(expected[static_cast<std::size_t>(row)]) == row);
        }
        model.removeSubtree(walk, root / "a");
        REQUIRE(primaries(model) == std::vector<fs::path>{root / "IMG_1.jpg"});
    }
    SECTION("After the walk, a folder read again is merged, a new one inserted, a gone one "
            "removed with its subtree in one go") {
        model.applyFolders(walk, {{root, {jpeg(root / "IMG_1.jpg")}},
                                  {root / "b", {jpeg(root / "b/IMG_1.jpg")}},
                                  {root / "b/deep", {jpeg(root / "b/deep/IMG_1.jpg")}},
                                  {root / "b2", {jpeg(root / "b2/IMG_1.jpg")}}});
        model.finishListing(walk, {});
        log.events.clear();
        // The same walk's number: what the watcher reads again after a change.
        model.applyFolders(walk,
                           {{.folder = root / "b",
                             .shots = {jpeg(root / "b/IMG_1.jpg"), jpeg(root / "b/IMG_2.jpg")},
                             .gone = false,
                             .recheck = true}});
        REQUIRE(log.events == std::vector<std::string>{"+2..2"});
        model.applyFolders(walk,
                           {{.folder = root / "b", .shots = {}, .gone = true, .recheck = false},
                            {root / "a", {jpeg(root / "a/IMG_1.jpg")}}});
        REQUIRE(log.events == std::vector<std::string>{"+2..2", "-1..3", "+1..1"});
        REQUIRE(primaries(model) == std::vector<fs::path>{root / "IMG_1.jpg", root / "a/IMG_1.jpg",
                                                          root / "b2/IMG_1.jpg"});
        REQUIRE(model.rowOf(root / "b2/IMG_1.jpg") == 2);
        // A folder without rows, and a stale walk, change nothing.
        model.removeSubtree(walk, root / "zzz");
        model.removeSubtree(walk + 1, root / "a");
        REQUIRE(model.rowCount() == 3);
        model.removeSubtree(walk, root);
        REQUIRE(model.rowCount() == 0);
        REQUIRE(log.events.back() == "-0..2");
    }
}

TEST_CASE("The lister walks in folder order and hands the shots over in batches",
          "[watcher][listing]") {
    const test::TempDir folder;
    const fs::path root = folder.path();
    addJpeg(root / "IMG_1.jpg");
    // Three folders of 600 shots, so that each makes a batch of its own (512 or more).
    for (const char* sub : {"b", "a", "a/deep"}) {
        for (int n = 0; n < 600; ++n) {
            addJpeg(root / sub / ("IMG_" + std::to_string(n) + ".jpg"));
        }
    }
    addJpeg(root / "c/IMG_1.jpg");
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    std::vector<std::pair<int, int>> inserts;
    QObject::connect(
        &model, &QAbstractItemModel::rowsInserted, &model,
        [&](const QModelIndex&, int first, int last) { inserts.emplace_back(first, last); });
    model.setFolder(root, true);
    REQUIRE(waitUntil([&] { return !model.isListing(); }));

    REQUIRE(primaries(model) == primaries(listShots(root, true)));
    // The root came alone and first; every insert was an append.
    REQUIRE(inserts.size() >= 4);
    REQUIRE(inserts.front() == std::pair{0, 0});
    int next = 0;
    for (const auto& [first, last] : inserts) {
        REQUIRE(first == next);
        next = last + 1;
    }
    REQUIRE(next == model.rowCount());
    REQUIRE(model.rowCount() == 1 + 1800 + 1);
}

TEST_CASE("The lister hands the first shots over at once, however deep", "[watcher][listing]") {
    const test::TempDir folder;
    addJpeg(folder.file("2024/a/IMG_1.jpg"));
    addJpeg(folder.file("2024/b/IMG_1.jpg"));
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    const test::ListingGate gate(folder.file("2024/b"));
    watcher.setReadHook(gate.hook());

    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil([&] { return gate.reached(); }));
    // Neither the root nor 2024 has a shot; a's comes while the walk is held before b, not when
    // the batch interval has passed.
    REQUIRE(waitUntil([&] { return model.rowOf(folder.file("2024/a/IMG_1.jpg")) >= 0; }));
    REQUIRE(model.isListing());
    gate.release();
    REQUIRE(waitUntil([&] { return !model.isListing(); }));
    REQUIRE(model.rowCount() == 2);
}

TEST_CASE("A new folder cancels the walk under way", "[watcher][listing]") {
    const test::TempDir first;
    const test::TempDir second;
    for (const char* name : {"IMG_1.jpg", "a/IMG_1.jpg", "b/IMG_1.jpg", "c/IMG_1.jpg"}) {
        addJpeg(first.file(name));
    }
    addJpeg(second.file("IMG_9.jpg"));
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    const test::ListingGate gate(first.file("b"));
    watcher.setReadHook(gate.hook());

    model.setFolder(first.path(), true);
    REQUIRE(waitUntil([&] { return gate.reached(); }));
    // The root came in its own batch at once; the walk is held before b.
    REQUIRE(waitUntil([&] { return model.rowOf(first.file("IMG_1.jpg")) >= 0; }));
    REQUIRE(model.isListing());

    model.setFolder(second.path(), true);
    gate.release();
    REQUIRE(waitUntil([&] { return !model.isListing(); }));
    REQUIRE(primaries(model) == std::vector<fs::path>{second.file("IMG_9.jpg")});
    // The first walk stopped at b: c was never read, and nothing of it arrived.
    const std::vector<fs::path> read = gate.read();
    REQUIRE(std::ranges::find(read, first.file("c")) == read.end());
    REQUIRE(std::ranges::find(read, second.path()) != read.end());
    // Only the second folder is watched.
    REQUIRE(watcher.watchedDirectories() ==
            QStringList{QString::fromStdU16String(second.path().u16string())});
}

TEST_CASE("A folder that vanishes during the walk is skipped, its siblings listed",
          "[watcher][listing]") {
    const test::TempDir folder;
    for (const char* name :
         {"IMG_1.jpg", "a/IMG_1.jpg", "b/IMG_1.jpg", "b/deep/IMG_1.jpg", "c/IMG_1.jpg"}) {
        addJpeg(folder.file(name));
    }
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    const test::ListingGate gate(folder.file("a"));
    watcher.setReadHook(gate.hook());

    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil([&] { return gate.reached(); }));
    // b was found by the root's read, and goes before the walk gets to it.
    fs::remove_all(folder.file("b"));
    gate.release();
    REQUIRE(waitUntil([&] { return !model.isListing(); }));
    REQUIRE(primaries(model) == std::vector<fs::path>{folder.file("IMG_1.jpg"),
                                                      folder.file("a/IMG_1.jpg"),
                                                      folder.file("c/IMG_1.jpg")});
    const QStringList watched = watcher.watchedDirectories();
    REQUIRE(watched.size() == 3);
    REQUIRE_FALSE(watched.contains(QString::fromStdU16String(folder.file("b").u16string())));
}

TEST_CASE("The watch set follows the walk, changed by what differs", "[watcher][listing]") {
    const test::TempDir folder;
    for (const char* name : {"IMG_1.jpg", "a/IMG_1.jpg", "b/IMG_1.jpg"}) {
        addJpeg(folder.file(name));
    }
    const auto text = [](const fs::path& path) {
        return QString::fromStdU16String(path.u16string());
    };
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil([&] { return !model.isListing(); }));
    REQUIRE(watcher.watchedDirectories().size() == 3);

    // Subfolders off: the same root, so its watch stays and only the others go.
    model.setFolder(folder.path(), false);
    REQUIRE(waitUntil([&] { return !model.isListing(); }));
    REQUIRE(watcher.watchedDirectories() == QStringList{text(folder.path())});

    // On again; then a folder goes, and the refresh it causes drops its watch.
    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil([&] { return !model.isListing(); }));
    REQUIRE(watcher.watchedDirectories().size() == 3);
    fs::remove_all(folder.file("a"));
    REQUIRE(waitUntil([&] {
        return model.rowOf(folder.file("a/IMG_1.jpg")) < 0 && !model.isRefreshing() &&
               !watcher.watchedDirectories().contains(text(folder.file("a")));
    }));
    REQUIRE(watcher.watchedDirectories().size() == 2);
}

// Step 4 of the plan: the lister watches each directory before it reads it, and a change reads
// again only the directory that reported it.

namespace {

/// Directories a gate saw read from a point on.
std::set<fs::path> readSince(const test::ListingGate& gate, std::size_t from) {
    const std::vector<fs::path> read = gate.read();
    return {read.begin() + static_cast<std::ptrdiff_t>(std::min(from, read.size())), read.end()};
}

/// A path as the watcher spells it.
QString qtPath(const fs::path& path) {
    return QString::fromStdU16String(path.u16string());
}

/// A read hook that runs an action once, on the lister's thread, before a directory is read.
FolderLister::ReadHook onceBefore(fs::path at, std::function<void()> action) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    return [at = std::move(at), action = std::move(action), done](const fs::path& directory) {
        if (directory == at && !done->exchange(true)) {
            action();
        }
    };
}

/// Tells whether every row is the one the model's index gives for its shot.
bool indexAgrees(const ShotModel& model) {
    for (int row = 0; row < model.rowCount(); ++row) {
        if (model.rowOf(model.shot(row).primary) != row) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("A change in a subfolder reads that folder again, and no other",
          "[watcher][listing][refresh]") {
    const test::TempDir folder;
    for (const char* name : {"IMG_1.jpg", "a/IMG_1.jpg", "b/IMG_1.jpg", "c/IMG_1.jpg"}) {
        addJpeg(folder.file(name));
    }
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    // Never reached: it only counts the reads.
    const test::ListingGate reads(folder.file("never"));
    watcher.setReadHook(reads.hook());
    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil([&] { return !model.isListing() && model.pendingLoads() == 0; }));
    REQUIRE(watcher.watchedDirectories().size() == 4);
    const std::set<fs::path> onlyA{folder.file("a")};

    std::size_t mark = reads.read().size();
    addJpeg(folder.file("a/IMG_2.jpg"));
    REQUIRE(waitUntil([&] { return model.rowOf(folder.file("a/IMG_2.jpg")) >= 0; }));
    REQUIRE(readSince(reads, mark) == onlyA);

    mark = reads.read().size();
    fs::rename(folder.file("a/IMG_1.jpg"), folder.file("a/IMG_9.jpg"));
    REQUIRE(waitUntil([&] {
        return model.rowOf(folder.file("a/IMG_9.jpg")) >= 0 &&
               model.rowOf(folder.file("a/IMG_1.jpg")) < 0;
    }));
    REQUIRE(readSince(reads, mark) == onlyA);

    mark = reads.read().size();
    fs::remove(folder.file("a/IMG_2.jpg"));
    REQUIRE(waitUntil([&] { return model.rowOf(folder.file("a/IMG_2.jpg")) < 0; }));
    REQUIRE(readSince(reads, mark) == onlyA);
    REQUIRE(primaries(model) == primaries(listShots(folder.path(), true)));
    // No change is a refresh of all.
    REQUIRE_FALSE(model.isRefreshing());
}

TEST_CASE("A new nested subtree is walked, and one moved away goes with one remove",
          "[watcher][listing][refresh]") {
    const test::TempDir folder;
    const test::TempDir outside;
    for (const char* name : {"IMG_1.jpg", "b/IMG_1.jpg", "b/deep/IMG_1.jpg", "c/IMG_1.jpg"}) {
        addJpeg(folder.file(name));
    }
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil([&] { return !model.isListing(); }));
    RowLog log(model);

    // Written as soon as each directory is made, before any watch can be on it (R7): each is
    // in its directory's first read, or reported to its watch.
    std::vector<fs::path> made;
    for (int n = 0; n < 20; ++n) {
        const std::string day = "new/d" + std::to_string(n);
        made.push_back(addJpeg(folder.file(day + "/IMG_1.jpg")));
        made.push_back(addJpeg(folder.file(day + "/x/IMG_1.jpg")));
    }
    REQUIRE(waitUntil([&] {
        return std::ranges::all_of(made, [&](const fs::path& p) { return model.rowOf(p) >= 0; });
    }));
    REQUIRE(
        waitUntil([&] { return primaries(model) == primaries(listShots(folder.path(), true)); }));
    REQUIRE(waitUntil([&] { return watcher.watchedDirectories().size() == 4 + 1 + 40; }));
    REQUIRE(watcher.watchedDirectories().contains(qtPath(folder.file("new/d7/x"))));

    // Moved out at once: b and b/deep are one run of rows, removed by one signal.
    log.events.clear();
    fs::rename(folder.file("b"), outside.file("b"));
    REQUIRE(waitUntil([&] { return model.rowOf(folder.file("b/deep/IMG_1.jpg")) < 0; }));
    REQUIRE(model.rowOf(folder.file("b/IMG_1.jpg")) < 0);
    REQUIRE(log.events == std::vector<std::string>{"-1..2"});
    REQUIRE(waitUntil([&] {
        const QStringList watched = watcher.watchedDirectories();
        return !watched.contains(qtPath(folder.file("b"))) &&
               !watched.contains(qtPath(folder.file("b/deep")));
    }));
    REQUIRE(model.rowOf(folder.file("c/IMG_1.jpg")) >= 0);
}

TEST_CASE("A directory is watched before it is read, and changes during the walk are read",
          "[watcher][listing][refresh]") {
    const test::TempDir folder;
    for (const char* name : {"IMG_1.jpg", "a/IMG_1.jpg", "c/IMG_1.jpg"}) {
        addJpeg(folder.file(name));
    }
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    const test::ListingGate gate(folder.file("c"));
    watcher.setReadHook(gate.hook());
    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil([&] { return gate.reached(); }));
    // Held before c's read, and c is watched already.
    REQUIRE(watcher.watchedDirectories().contains(qtPath(folder.file("c"))));
    // Between c's watch and its read: in the read (and reported too).
    addJpeg(folder.file("c/IMG_2.jpg"));
    // In a, read already: reported, and a is read again once the walk lets it.
    addJpeg(folder.file("a/IMG_2.jpg"));
    gate.release();
    REQUIRE(waitUntil([&] {
        return model.rowOf(folder.file("a/IMG_2.jpg")) >= 0 &&
               model.rowOf(folder.file("c/IMG_2.jpg")) >= 0 && !model.isListing();
    }));
    const std::vector<fs::path> read = gate.read();
    REQUIRE(std::ranges::count(read, folder.file("a")) >= 2);
    REQUIRE(std::ranges::count(read, folder.path()) == 1);
    REQUIRE(model.rowCount() == 5);
}

TEST_CASE("A change rechecks the sidecars of its folder only, and own writes stay quiet",
          "[watcher][listing][refresh]") {
    const test::TempDir folder;
    const fs::path inA = addJpeg(folder.file("a/IMG_1.jpg"));
    const fs::path inB = addJpeg(folder.file("b/IMG_1.jpg"));
    writeSidecarMarks(inA, {.rating = 1});
    writeSidecarMarks(inB, {.rating = 1});
    // A sidecar of rating 3, to copy into b's file in place: no event reports that.
    const fs::path spare = addJpeg(folder.file("spare/IMG_1.jpg"));
    writeSidecarMarks(spare, {.rating = 3});
    std::string spareText;
    {
        std::ifstream in(sidecarPath(spare), std::ios::binary);
        spareText.assign(std::istreambuf_iterator<char>(in), {});
    }
    fs::remove_all(folder.file("spare"));

    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    std::vector<fs::path> refreshed;
    QObject::connect(
        &model, &ShotModel::sidecarRefreshed, &model,
        [&](const QString& primary, qint64) { refreshed.emplace_back(primary.toStdU16String()); });
    std::vector<fs::path> external;
    QObject::connect(
        &watcher, &FolderWatcher::sidecarChangedExternally, &watcher,
        [&](const QString& primary) { external.emplace_back(primary.toStdU16String()); });
    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil([&] { return !model.isListing() && model.pendingLoads() == 0; }));
    REQUIRE(model.marks(model.rowOf(inB)).rating == 1);

    // b's sidecar rewritten in place: no event.
    {
        std::ofstream out(sidecarPath(inB), std::ios::binary | std::ios::trunc);
        out << spareText;
    }
    // a's replaced by another program: a's event rechecks a's rows only.
    writeSidecarMarks(inA, {.rating = 4});
    REQUIRE(waitUntil([&] { return model.marks(model.rowOf(inA)).rating == 4; }));
    REQUIRE(waitUntil([&] { return model.pendingLoads() == 0; }));
    REQUIRE(refreshed == std::vector<fs::path>{inA});
    REQUIRE(external == std::vector<fs::path>{inA});
    REQUIRE(model.marks(model.rowOf(inB)).rating == 1);

    // b's change shows once b reports one.
    watcher.simulateDirectoryChanged(folder.file("b"));
    REQUIRE(waitUntil([&] { return model.marks(model.rowOf(inB)).rating == 3; }));
    REQUIRE(external == std::vector<fs::path>{inA, inB});

    // The application's own write is rechecked, and not reported as another program's.
    writeSidecarMarks(inA, {.rating = 5});
    model.setMarks(inA, {.rating = 5});
    watcher.noteOwnWrite(sidecarPath(inA));
    REQUIRE(waitUntil([&] { return refreshed.size() == 3; }));
    REQUIRE(refreshed.back() == inA);
    REQUIRE(waitUntil([&] { return model.pendingLoads() == 0; }));
    REQUIRE(external.size() == 2);
}

TEST_CASE("A new folder cancels the change being read for the old one", "[watcher][listing]") {
    const test::TempDir first;
    const test::TempDir second;
    addJpeg(first.file("a/IMG_1.jpg"));
    addJpeg(second.file("IMG_9.jpg"));
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    model.setFolder(first.path(), true);
    REQUIRE(waitUntil([&] { return !model.isListing(); }));

    const test::ListingGate gate(first.file("a"));
    watcher.setReadHook(gate.hook());
    addJpeg(first.file("a/IMG_2.jpg"));
    watcher.simulateDirectoryChanged(first.file("a"));
    REQUIRE(waitUntil([&] { return gate.reached(); }));
    model.setFolder(second.path(), true);
    gate.release();
    REQUIRE(waitUntil([&] { return !model.isListing(); }));
    // Whatever was queued for the old folder is stale.
    QCoreApplication::processEvents();
    REQUIRE(primaries(model) == std::vector<fs::path>{second.file("IMG_9.jpg")});
    REQUIRE(watcher.watchedDirectories() == QStringList{qtPath(second.path())});
    // And a report of the old folder is ignored.
    const std::size_t mark = gate.read().size();
    watcher.simulateDirectoryChanged(first.file("a"));
    watcher.simulateDirectoryChanged(second.path());
    REQUIRE(waitUntil([&] { return readSince(gate, mark).contains(second.path()); }));
    REQUIRE(readSince(gate, mark) == std::set<fs::path>{second.path()});
}

TEST_CASE("The watcher goes while changes are pending or being read", "[watcher][listing]") {
    const test::TempDir folder;
    addJpeg(folder.file("a/IMG_1.jpg"));
    ShotModel model;

    SECTION("Waiting for the debounce") {
        FolderWatcher watcher(model, std::chrono::hours(1));
        model.setFolder(folder.path(), true);
        REQUIRE(waitUntil([&] { return !model.isListing(); }));
        watcher.simulateDirectoryChanged(folder.file("a"));
        addJpeg(folder.file("a/IMG_2.jpg"));
        QCoreApplication::processEvents();
    }
    SECTION("Held in the read of a change") {
        FolderWatcher watcher(model, std::chrono::milliseconds(0));
        model.setFolder(folder.path(), true);
        REQUIRE(waitUntil([&] { return !model.isListing(); }));
        const test::ListingGate gate(folder.file("a"));
        watcher.setReadHook(gate.hook());
        watcher.simulateDirectoryChanged(folder.file("a"));
        REQUIRE(waitUntil([&] { return gate.reached(); }));
        // The gate goes first and lets the read finish; the watcher then stops its thread.
    }
    // Nothing of the gone watcher arrives.
    QCoreApplication::processEvents();
    REQUIRE(model.rowCount() == 1);
}

TEST_CASE("A directory read again within a batch of the first listing keeps its rows once",
          "[watcher][listing][refresh]") {
    // Review B1, end to end and without a debounce: a's read reports a change of a itself, so a
    // is read again a few directories later, in the batch that holds its first read.
    const test::TempDir folder;
    for (const char* name :
         {"IMG_1.jpg", "a/IMG_1.jpg", "b/IMG_1.jpg", "c/IMG_1.jpg", "d/IMG_1.jpg", "e/IMG_1.jpg"}) {
        addJpeg(folder.file(name));
    }
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(0));
    watcher.setReadHook(onceBefore(folder.file("a"), [&] {
        addJpeg(folder.file("a/IMG_2.jpg"));
        watcher.simulateDirectoryChanged(folder.file("a"));
    }));
    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil(
        [&] { return !model.isListing() && model.rowOf(folder.file("a/IMG_2.jpg")) >= 0; }));
    REQUIRE(primaries(model) == primaries(listShots(folder.path(), true)));
    REQUIRE(indexAgrees(model));
}

TEST_CASE("A new folder found by a parent's re-read during the first listing goes in its place",
          "[watcher][listing][refresh]") {
    // Review B1, end to end and without a debounce: bb appears while c is read, the root (which
    // has no shots of its own) reports it, and bb is read in the batch that holds c, while the
    // walk still has c's later siblings to read.
    const test::TempDir folder;
    for (const char* sub : {"a", "b", "c", "d", "e", "f", "g", "h"}) {
        addJpeg(folder.path() / sub / "IMG_1.jpg");
    }
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(0));
    watcher.setReadHook(onceBefore(folder.file("c"), [&] {
        addJpeg(folder.file("bb/IMG_1.jpg"));
        watcher.simulateDirectoryChanged(folder.path());
    }));
    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil(
        [&] { return !model.isListing() && model.rowOf(folder.file("bb/IMG_1.jpg")) >= 0; }));
    REQUIRE(primaries(model) == primaries(listShots(folder.path(), true)));
    REQUIRE(model.rowOf(folder.file("bb/IMG_1.jpg")) == 2);
    REQUIRE(indexAgrees(model));
}

TEST_CASE("A watch Qt drops just before a walk begins is had again by that walk",
          "[watcher][listing][refresh]") {
    // Review R2: the report comes between the start of a refresh and its begin on the lister
    // thread, where it used to be dropped, and with it the only sign that b lost its watch.
    const test::TempDir folder;
    for (const char* name : {"IMG_1.jpg", "a/IMG_1.jpg", "b/IMG_1.jpg"}) {
        addJpeg(folder.file(name));
    }
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(0));
    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil([&] { return !model.isListing() && model.pendingLoads() == 0; }));
    const test::ListingGate gate(folder.file("a"));
    watcher.setReadHook(gate.hook());
    // The lister is held in a change read of a; what is posted to it now waits behind that read,
    // in order: Qt drops b's watch and reports it, then the refresh's walk begins.
    watcher.simulateDirectoryChanged(folder.file("a"));
    REQUIRE(waitUntil([&] { return gate.reached(); }));
    watcher.simulateDirectoryChanged(folder.file("b"), true);
    watcher.refreshAll();
    REQUIRE(model.isRefreshing());
    gate.release();
    REQUIRE(waitUntil([&] { return !model.isRefreshing(); }));

    // b is watched again: a file written there shows without another refresh.
    addJpeg(folder.file("b/IMG_2.jpg"));
    REQUIRE(waitUntil([&] { return model.rowOf(folder.file("b/IMG_2.jpg")) >= 0; }));
    REQUIRE_FALSE(model.isRefreshing());
}

// Step 5 of the plan: the cap on watches, the notice's status, and the refresh of all.

namespace {

/// Keeps the last watch status a watcher relays.
struct StatusLog {
    WatchStatus last;
    int count = 0;

    explicit StatusLog(FolderWatcher& watcher) {
        QObject::connect(&watcher, &FolderWatcher::watchStatusChanged, &watcher,
                         [this](const WatchStatus& status) {
                             last = status;
                             ++count;
                         });
    }
};

/// A tree of six directories: `/`, `a`, `b`, `c` and, below them, `a/x` and `b/y`.
void makeSixDirectories(const test::TempDir& folder) {
    for (const char* name : {"IMG_1.jpg", "a/IMG_1.jpg", "b/IMG_1.jpg", "c/IMG_1.jpg",
                             "a/x/IMG_1.jpg", "b/y/IMG_1.jpg"}) {
        addJpeg(folder.file(name));
    }
}

QStringList sortedPaths(QStringList paths) {
    paths.sort();
    return paths;
}

} // namespace

TEST_CASE("With a cap the shallowest directories are watched, and the rest are reported",
          "[watcher][listing][budget]") {
    const test::TempDir folder;
    makeSixDirectories(folder);
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    watcher.setMaxWatchedFolders(3);
    StatusLog status(watcher);
    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil([&] { return !model.isListing() && status.last.directories == 6; }));
    REQUIRE(status.last.watched == 3);
    REQUIRE(status.last.unwatched() == 3);
    REQUIRE(status.last.limit == 3);
    REQUIRE(status.last.unreadable == 0);
    // The root and its first two subfolders, not the deeper ones the walk met first.
    REQUIRE(
        sortedPaths(watcher.watchedDirectories()) ==
        sortedPaths({qtPath(folder.path()), qtPath(folder.file("a")), qtPath(folder.file("b"))}));
    REQUIRE(waitUntil([&] { return model.pendingLoads() == 0; }));

    // A change in a watched folder shows; one in an unwatched folder does not, as nothing says so.
    addJpeg(folder.file("c/IMG_2.jpg"));
    addJpeg(folder.file("a/x/IMG_2.jpg"));
    addJpeg(folder.file("a/IMG_2.jpg"));
    REQUIRE(waitUntil([&] { return model.rowOf(folder.file("a/IMG_2.jpg")) >= 0; }));
    REQUIRE(model.rowOf(folder.file("c/IMG_2.jpg")) < 0);
    REQUIRE(model.rowOf(folder.file("a/x/IMG_2.jpg")) < 0);

    // The refresh of all reads every directory, watched or not.
    watcher.refreshAll();
    REQUIRE(waitUntil([&] {
        return model.rowOf(folder.file("c/IMG_2.jpg")) >= 0 &&
               model.rowOf(folder.file("a/x/IMG_2.jpg")) >= 0 && !model.isRefreshing();
    }));
    REQUIRE(primaries(model) == primaries(listShots(folder.path(), true)));
    REQUIRE(status.last.unwatched() == 3);
    REQUIRE(watcher.watchedDirectories().size() == 3);
}

TEST_CASE("A directory that gains a watch is read once more", "[watcher][listing][budget]") {
    const test::TempDir folder;
    makeSixDirectories(folder);
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    watcher.setMaxWatchedFolders(3);
    StatusLog status(watcher);
    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil([&] { return !model.isListing() && status.last.directories == 6; }));
    REQUIRE(waitUntil([&] { return model.pendingLoads() == 0; }));

    // Written to a folder without a watch, then a watched one goes: its place goes to /c, which
    // is read again with the watch, and so shows what was written (no refresh of all).
    addJpeg(folder.file("c/IMG_9.jpg"));
    fs::remove_all(folder.file("a"));
    REQUIRE(waitUntil([&] {
        return model.rowOf(folder.file("c/IMG_9.jpg")) >= 0 &&
               watcher.watchedDirectories().contains(qtPath(folder.file("c")));
    }));
    REQUIRE(waitUntil([&] { return status.last.directories == 4 && status.last.watched == 3; }));
    REQUIRE(
        sortedPaths(watcher.watchedDirectories()) ==
        sortedPaths({qtPath(folder.path()), qtPath(folder.file("b")), qtPath(folder.file("c"))}));
    REQUIRE_FALSE(model.isRefreshing());
}

TEST_CASE("A shallower directory that appears takes the place of a deeper one",
          "[watcher][listing][budget]") {
    const test::TempDir folder;
    for (const char* name : {"IMG_1.jpg", "a/IMG_1.jpg", "a/x/IMG_1.jpg"}) {
        addJpeg(folder.file(name));
    }
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    watcher.setMaxWatchedFolders(3);
    StatusLog status(watcher);
    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil([&] { return !model.isListing() && status.last.directories == 3; }));
    REQUIRE(watcher.watchedDirectories().size() == 3);
    REQUIRE(status.last.unwatched() == 0);

    addJpeg(folder.file("b/IMG_1.jpg"));
    REQUIRE(waitUntil([&] {
        const QStringList watched = watcher.watchedDirectories();
        return model.rowOf(folder.file("b/IMG_1.jpg")) >= 0 &&
               watched.contains(qtPath(folder.file("b"))) &&
               !watched.contains(qtPath(folder.file("a/x")));
    }));
    REQUIRE(waitUntil([&] { return status.last.directories == 4 && status.last.watched == 3; }));
    REQUIRE(status.last.unwatched() == 1);
}

TEST_CASE("A watch the system refuses shrinks the cap, and the next walk tries again",
          "[watcher][listing][budget]") {
    const test::TempDir folder;
    for (const char* name : {"IMG_1.jpg", "a/IMG_1.jpg", "b/IMG_1.jpg", "c/IMG_1.jpg"}) {
        addJpeg(folder.file(name));
    }
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    StatusLog status(watcher);
    // The system gives two watches and then says no, as inotify does at its limit.
    std::atomic<int> given{0};
    watcher.setWatchGate([&given](const fs::path&) { return given++ < 2; });
    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil([&] { return !model.isListing() && status.last.directories == 4; }));
    REQUIRE(waitUntil([&] { return status.last.watched == 2 && status.last.limit == 2; }));
    REQUIRE(watcher.watchedDirectories().size() == 2);
    REQUIRE(status.last.unwatched() == 2);

    // The limit is lifted; the next walk asks again and the cap is the configured one.
    watcher.setWatchGate({});
    watcher.refreshAll();
    REQUIRE(waitUntil([&] { return status.last.watched == 4 && !model.isRefreshing(); }));
    REQUIRE(status.last.limit == FolderWatcher::maxWatchedFolders);
    REQUIRE(watcher.watchedDirectories().size() == 4);
    REQUIRE(status.last.unwatched() == 0);
}

TEST_CASE("A directory whose watch failed because it did not open is watched once it reads",
          "[watcher][listing][budget]") {
    // Review R4: c is moved away for its watch and back for its read (replaced in between, or a
    // share's hiccup). It must stay in the budget and get its watch, not be left out for good.
    const test::TempDir folder;
    for (const char* name : {"IMG_1.jpg", "a/IMG_1.jpg", "c/IMG_1.jpg"}) {
        addJpeg(folder.file(name));
    }
    const fs::path c = folder.file("c");
    const fs::path aside = folder.file("c-aside");
    auto moved = std::make_shared<std::atomic<bool>>(false);
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    watcher.setWatchGate([moved, c, aside](const fs::path& directory) {
        if (directory == c && !moved->exchange(true)) {
            fs::rename(c, aside);
            return false;
        }
        return true;
    });
    watcher.setReadHook(onceBefore(c, [c, aside] { fs::rename(aside, c); }));
    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil(
        [&] { return !model.isListing() && watcher.watchedDirectories().contains(qtPath(c)); }));
    REQUIRE(moved->load());
    REQUIRE(model.rowOf(folder.file("c/IMG_1.jpg")) >= 0);

    addJpeg(folder.file("c/IMG_2.jpg"));
    REQUIRE(waitUntil([&] { return model.rowOf(folder.file("c/IMG_2.jpg")) >= 0; }));
}

TEST_CASE("A root that vanishes keeps its rows, without a watch tried on every waiting directory",
          "[watcher][listing][budget][failure]") {
    // Review R5: ten subfolders, two watches. The root goes at once (an unmount, a share asleep).
    const test::TempDir folder;
    const fs::path root = folder.file("photos");
    addJpeg(root / "IMG_1.jpg");
    for (int n = 0; n < 10; ++n) {
        addJpeg(root / ("d" + std::to_string(n)) / "IMG_1.jpg");
    }
    auto asked = std::make_shared<std::atomic<int>>(0);
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(0));
    watcher.setMaxWatchedFolders(2);
    StatusLog status(watcher);
    watcher.setWatchGate([asked](const fs::path&) {
        ++*asked;
        return true;
    });
    model.setFolder(root, true);
    REQUIRE(waitUntil([&] { return !model.isListing() && status.last.directories == 11; }));
    REQUIRE(status.last.unwatched() == 9);
    REQUIRE(model.rowCount() == 11);
    REQUIRE_FALSE(status.last.rootUnavailable);

    asked->store(0);
    fs::rename(root, folder.file("gone"));
    // Qt drops the root's watch and reports it; not left to the system's timing.
    watcher.simulateDirectoryChanged(root, true);
    REQUIRE(waitUntil([&] { return status.last.rootUnavailable; }));
    // The rows stay, the tree too, and the root's own watch is tried again before its read and
    // nothing more.
    REQUIRE(status.last.rootError != "");
    REQUIRE(status.last.directories == 11);
    REQUIRE(model.rowCount() == 11);
    REQUIRE(asked->load() <= 2);

    // It comes back: a refresh merges it normally, and the notice's cause goes.
    fs::rename(folder.file("gone"), root);
    addJpeg(root / "d3" / "IMG_2.jpg");
    watcher.refreshAll();
    REQUIRE(waitUntil([&] { return model.rowOf(root / "d3" / "IMG_2.jpg") >= 0; }));
    REQUIRE(waitUntil([&] { return !model.isRefreshing() && !status.last.rootUnavailable; }));
    REQUIRE(model.rowCount() == 12);
}

namespace {

/// A read hook that fails as a share does that does not answer, for the folders in a set.
///
/// The hook shares the set, so a read the lister makes after this goes (a real report during the
/// watcher's teardown) finds it still there (review R6).
class FailingReads {
public:
    [[nodiscard]] FolderLister::ReadHook hook() const {
        return [state = state_](const fs::path& directory) {
            const std::scoped_lock lock(state->mutex);
            if (state->failing.contains(directory)) {
                throw fs::filesystem_error("simulated", directory,
                                           std::make_error_code(std::errc::io_error));
            }
        };
    }

    void set(const fs::path& directory, bool fails) const {
        const std::scoped_lock lock(state_->mutex);
        if (fails) {
            state_->failing.insert(directory);
        } else {
            state_->failing.erase(directory);
        }
    }

private:
    /// What the hook and the test share.
    struct State {
        std::mutex mutex;
        std::set<fs::path> failing;
    };

    std::shared_ptr<State> state_ = std::make_shared<State>();
};

/// Takes a folder's rights away, and gives them back.
struct NoAccess {
    fs::path folder;

    explicit NoAccess(fs::path path) : folder(std::move(path)) {
        fs::permissions(folder, fs::perms::none);
    }
    void restore() const {
        fs::permissions(folder, fs::perms::owner_all);
    }
    ~NoAccess() {
        restore();
    }
};

} // namespace

TEST_CASE("A root that cannot be read is unavailable, keeps its rows, and recovers",
          "[watcher][listing][failure]") {
    const test::TempDir folder;
    const fs::path root = folder.path();
    addJpeg(root / "IMG_1.jpg");
    addJpeg(root / "a" / "IMG_1.jpg");
    addJpeg(root / "a" / "deep" / "IMG_1.jpg");
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(0));
    StatusLog status(watcher);
    const FailingReads reads;
    watcher.setReadHook(reads.hook());
    model.setFolder(root, true);
    REQUIRE(waitUntil([&] { return !model.isListing() && status.last.directories == 3; }));
    REQUIRE(model.rowCount() == 3);

    // The share is asleep: a refresh finds the root unreadable. Nothing is swept.
    reads.set(root, true);
    watcher.refreshAll();
    REQUIRE(waitUntil([&] { return !model.isRefreshing() && status.last.rootUnavailable; }));
    REQUIRE(model.rowCount() == 3);
    REQUIRE(status.last.rootError != "");
    REQUIRE(status.last.unreadable == 0);

    // It wakes: the next report merges the root normally.
    reads.set(root, false);
    addJpeg(root / "IMG_2.jpg");
    watcher.simulateDirectoryChanged(root);
    REQUIRE(waitUntil([&] { return model.rowOf(root / "IMG_2.jpg") >= 0; }));
    REQUIRE(waitUntil([&] { return !status.last.rootUnavailable; }));
    REQUIRE(model.rowCount() == 4);
}

TEST_CASE("A subfolder that loses its permission is removed, and returns with it",
          "[watcher][listing][failure]") {
    const test::TempDir folder;
    const fs::path root = folder.path();
    addJpeg(root / "IMG_1.jpg");
    addJpeg(root / "a" / "IMG_1.jpg");
    addJpeg(root / "b" / "IMG_1.jpg");
    addJpeg(root / "b" / "deep" / "IMG_1.jpg");
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(0));
    StatusLog status(watcher);
    model.setFolder(root, true);
    REQUIRE(waitUntil([&] { return !model.isListing() && status.last.directories == 4; }));
    REQUIRE(model.rowCount() == 4);
    REQUIRE(status.last.watched == 4);

    NoAccess locked(root / "b");
    {
        std::error_code error;
        if (fs::directory_iterator(root / "b", error); !error) {
            SKIP("the rights are not enforced here (running as root)");
        }
    }
    watcher.simulateDirectoryChanged(root / "b");
    // Its rows and its subtree's go, as do their watches; it is counted as unreadable.
    REQUIRE(waitUntil([&] { return model.rowCount() == 2 && status.last.unreadable == 1; }));
    REQUIRE(status.last.unreadableFolders == std::vector<fs::path>{root / "b"});
    REQUIRE_FALSE(status.last.rootUnavailable);
    REQUIRE(status.last.directories == 2);
    REQUIRE(model.rowOf(root / "b" / "deep" / "IMG_1.jpg") < 0);
    REQUIRE(sortedPaths(watcher.watchedDirectories()).size() == 2);

    // A refresh tries it again, and it is still refused: counted once.
    watcher.refreshAll();
    REQUIRE(waitUntil([&] { return !model.isRefreshing(); }));
    REQUIRE(model.rowCount() == 2);
    REQUIRE(status.last.unreadable == 1);

    // Readable again: the parent's next read finds it, and its subtree comes back.
    locked.restore();
    watcher.simulateDirectoryChanged(root);
    REQUIRE(waitUntil([&] { return model.rowCount() == 4 && status.last.unreadable == 0; }));
    REQUIRE(status.last.directories == 4);
}

TEST_CASE("A subfolder with an I/O error keeps its rows as stale and is read again",
          "[watcher][listing][failure]") {
    const test::TempDir folder;
    const fs::path root = folder.path();
    addJpeg(root / "IMG_1.jpg");
    addJpeg(root / "a" / "IMG_1.jpg");
    addJpeg(root / "b" / "IMG_1.jpg");
    addJpeg(root / "b" / "deep" / "IMG_1.jpg");
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(0));
    StatusLog status(watcher);
    const FailingReads reads;
    watcher.setReadHook(reads.hook());
    model.setFolder(root, true);
    REQUIRE(waitUntil([&] { return !model.isListing() && status.last.directories == 4; }));
    REQUIRE(model.rowCount() == 4);

    SECTION("On a report of its own") {
        reads.set(root / "b", true);
        fs::remove(root / "b" / "IMG_1.jpg");
        watcher.simulateDirectoryChanged(root / "b");
        REQUIRE(waitUntil([&] { return status.last.unreadable == 1; }));
        // Not read, so not changed: the rows stay as they were, the subtree's too.
        REQUIRE(model.rowCount() == 4);
        REQUIRE(status.last.directories == 4);
        reads.set(root / "b", false);
        watcher.simulateDirectoryChanged(root / "b");
        REQUIRE(waitUntil([&] { return status.last.unreadable == 0; }));
        REQUIRE(model.rowCount() == 3);
    }
    SECTION("In a refresh, which sweeps nothing below it") {
        reads.set(root / "b", true);
        fs::remove(root / "a" / "IMG_1.jpg");
        watcher.refreshAll();
        REQUIRE(waitUntil([&] { return !model.isRefreshing() && status.last.unreadable == 1; }));
        // a was read and has no shots now; b and below it were not, and are kept.
        REQUIRE(model.rowOf(root / "a" / "IMG_1.jpg") < 0);
        REQUIRE(model.rowOf(root / "b" / "IMG_1.jpg") >= 0);
        REQUIRE(model.rowOf(root / "b" / "deep" / "IMG_1.jpg") >= 0);
        REQUIRE(model.rowCount() == 3);

        // The next refresh reads it again.
        reads.set(root / "b", false);
        fs::remove(root / "b" / "IMG_1.jpg");
        watcher.refreshAll();
        REQUIRE(waitUntil([&] { return !model.isRefreshing() && status.last.unreadable == 0; }));
        REQUIRE(model.rowCount() == 2);
    }
    SECTION("A folder that is gone loses its rows, and is not counted") {
        fs::remove_all(root / "b");
        watcher.refreshAll();
        REQUIRE(waitUntil([&] { return !model.isRefreshing() && status.last.directories == 2; }));
        REQUIRE(model.rowCount() == 2);
        REQUIRE(status.last.unreadable == 0);
    }
}

TEST_CASE("Switching the subfolders on and off changes no reset and reads no tree twice",
          "[watcher][listing][toggle]") {
    const test::TempDir folder;
    const fs::path root = folder.path();
    addJpeg(root / "IMG_1.jpg");
    for (const char* sub : {"a", "a/deep", "b", "c"}) {
        addJpeg(root / sub / "IMG_1.jpg");
    }
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(0));
    StatusLog status(watcher);
    int resets = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&] { ++resets; });
    RowLog log(model);
    const test::ListingGate reads(root / "never");

    model.setFolder(root, false);
    REQUIRE(waitUntil([&] { return !model.isListing() && status.last.directories == 1; }));
    REQUIRE(model.rowCount() == 1);
    resets = 0;
    log.events.clear();
    watcher.setReadHook(reads.hook());

    // On: the root is read once more, for its list of subfolders, and the subtree once.
    model.setRecursive(true);
    REQUIRE(model.recursive());
    REQUIRE(model.isListing());
    REQUIRE(model.rowCount() == 1);
    REQUIRE(waitUntil([&] { return !model.isListing() && status.last.directories == 5; }));
    REQUIRE(primaries(model) == primaries(listShots(root, true)));
    REQUIRE(resets == 0);
    REQUIRE(log.events.front() == "+1..4");
    REQUIRE(reads.read() ==
            std::vector<fs::path>{root, root / "a", root / "a" / "deep", root / "b", root / "c"});
    REQUIRE(watcher.watchedDirectories().size() == 5);

    // Off: the rows of the subfolders go at once with one signal; only the root is read, and
    // the subfolders' watches go.
    log.events.clear();
    model.setRecursive(false);
    REQUIRE(model.rowCount() == 1);
    REQUIRE(log.events == std::vector<std::string>{"-1..4"});
    REQUIRE_FALSE(model.isListing());
    REQUIRE(waitUntil([&] { return status.last.directories == 1; }));
    REQUIRE(waitUntil([&] { return watcher.watchedDirectories().size() == 1; }));
    REQUIRE(reads.read().size() == 6);
    REQUIRE(reads.read().back() == root);
    REQUIRE(resets == 0);
    REQUIRE(primaries(model) == std::vector<fs::path>{root / "IMG_1.jpg"});
}

TEST_CASE("A toggle during a walk abandons it before its next directory",
          "[watcher][listing][toggle]") {
    const test::TempDir folder;
    const fs::path root = folder.path();
    addJpeg(root / "IMG_1.jpg");
    for (const char* sub : {"b", "c"}) {
        addJpeg(root / sub / "IMG_1.jpg");
    }
    // Enough shots in a for its batch to go before the walk is held at b.
    for (int n = 0; n < 600; ++n) {
        addJpeg(root / "a" / ("IMG_" + std::to_string(n) + ".jpg"));
    }
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(0));
    StatusLog status(watcher);
    const test::ListingGate gate(root / "b");
    watcher.setReadHook(gate.hook());

    model.setFolder(root, true);
    REQUIRE(waitUntil([&] { return gate.reached(); }));
    REQUIRE(waitUntil([&] { return model.rowCount() == 601; }));
    // Off while the listing is held before b: the rows of a go, the walk of the listing is
    // stale, and the one that replaces it reads the root alone.
    model.setRecursive(false);
    REQUIRE(model.isListing());
    REQUIRE(model.rowCount() == 1);
    gate.release();
    REQUIRE(waitUntil([&] { return !model.isListing() && status.last.directories == 1; }));
    REQUIRE(primaries(model) == std::vector<fs::path>{root / "IMG_1.jpg"});
    REQUIRE(waitUntil([&] { return watcher.watchedDirectories().size() == 1; }));
    QCoreApplication::processEvents();
    REQUIRE(model.rowCount() == 1);
}

TEST_CASE("A watcher whose lister hangs in a read is let go at exit, within the wait",
          "[watcher][listing][exit]") {
    const test::TempDir folder;
    addJpeg(folder.file("IMG_1.jpg"));
    addJpeg(folder.file("a/IMG_1.jpg"));
    ShotModel model;
    // Never released until the end: a read on a share that does not answer.
    const test::ListingGate gate(folder.file("a"));
    auto returned = std::make_shared<std::atomic<bool>>(false);
    auto lister = std::make_shared<std::atomic<QThread*>>(nullptr);
    auto watcher = std::make_unique<FolderWatcher>(model, std::chrono::milliseconds(0));
    watcher->setExitWait(std::chrono::milliseconds(100));
    watcher->setReadHook([hook = gate.hook(), returned, lister](const fs::path& directory) {
        lister->store(QThread::currentThread());
        hook(directory);
        *returned = *returned || directory.filename() == "a";
    });
    model.setFolder(folder.path(), true);
    REQUIRE(waitUntil([&] { return gate.reached(); }));
    // Held in the hook, so the thread is there to point at.
    const QPointer<QThread> thread(lister->load());
    REQUIRE(thread);

    const auto start = std::chrono::steady_clock::now();
    watcher.reset();
    const auto took = std::chrono::steady_clock::now() - start;
    REQUIRE(took < std::chrono::milliseconds(1500));
    REQUIRE_FALSE(returned->load());

    // The call returns later: the orphan finds its walk cancelled, delivers nothing to the gone
    // watcher or the model, and ends.
    const int rows = model.rowCount();
    gate.release();
    REQUIRE(waitUntil([&] { return returned->load(); }));
    // The orphan ends, and deletes itself on this thread.
    REQUIRE(waitUntil([&] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return thread.isNull();
    }));
    REQUIRE(model.rowCount() == rows);
}

// The fixes of the review of step 6 (claude-opus-5-5_2026-10-20_failure-handling-and-toggle.md).

namespace {

/// Waits until the lister has read a directory once more after what it was doing, and applies
/// what it delivered before that read: a walk's batches go out before the next read begins.
void settle(ShotModel& model, FolderWatcher& watcher, const test::ListingGate& reads,
            const fs::path& directory) {
    const auto before = std::ranges::count(reads.read(), directory);
    watcher.simulateDirectoryChanged(directory);
    REQUIRE(waitUntil([&] { return std::ranges::count(reads.read(), directory) > before; }));
    QCoreApplication::processEvents();
    // Applies everything queued, at once.
    model.applyFolders(model.walk(), {});
}

/// A thumbnail to tell a row that stayed from one inserted again.
QImage aThumbnail() {
    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::red);
    return image;
}

bool hasThumbnail(const ShotModel& model, const fs::path& primary) {
    return !model.data(model.index(model.rowOf(primary)), ShotModel::ThumbnailRole)
                .value<QImage>()
                .isNull();
}

} // namespace

TEST_CASE("Under a root that is unmounted the subfolders keep their rows, and all come back",
          "[watcher][listing][failure]") {
    // Review B1 and R1. An unmount reports every watch, the root's first, and Qt drops them all.
    const test::TempDir folder;
    const fs::path root = folder.file("photos");
    const fs::path away = folder.file("unmounted");
    for (const char* name : {"IMG_1.jpg", "a/IMG_1.jpg", "a/deep/IMG_1.jpg", "b/IMG_1.jpg"}) {
        addJpeg(root / name);
    }
    const fs::path inA = root / "a" / "IMG_1.jpg";
    writeSidecarMarks(inA, {.rating = 3});
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(0));
    StatusLog status(watcher);
    model.setFolder(root, true);
    REQUIRE(waitUntil([&] {
        return !model.isListing() && model.pendingLoads() == 0 && status.last.directories == 4;
    }));
    model.setThumbnail(inA, aThumbnail());
    RowLog log(model);
    const test::ListingGate reads(root / "never");
    watcher.setReadHook(reads.hook());

    fs::rename(root, away);
    for (const fs::path& directory : {root, root / "a", root / "a" / "deep", root / "b"}) {
        watcher.simulateDirectoryChanged(directory, true);
    }
    REQUIRE(waitUntil([&] {
        const std::vector<fs::path> read = reads.read();
        return std::ranges::count(read, root / "a" / "deep") == 1 &&
               std::ranges::count(read, root / "b") == 1;
    }));
    settle(model, watcher, reads, root);
    // Nothing is dropped: the root is unavailable, and the tree and the rows stay.
    REQUIRE(status.last.rootUnavailable);
    REQUIRE(status.last.directories == 4);
    REQUIRE(status.last.unreadable == 0);
    REQUIRE(model.rowCount() == 4);
    REQUIRE(log.events.empty());
    REQUIRE(watcher.watchedDirectories().isEmpty());

    // Mounted again, with a file new deep below. A report of the root walks its whole subtree
    // and watches it again; the notice clears only once all of it is read.
    fs::rename(away, root);
    addJpeg(root / "a" / "deep" / "IMG_2.jpg");
    const int statuses = status.count;
    const test::ListingGate deep(root / "a" / "deep");
    watcher.setReadHook(deep.hook());
    watcher.simulateDirectoryChanged(root);
    REQUIRE(waitUntil([&] { return deep.reached(); }));
    QCoreApplication::processEvents();
    REQUIRE(status.count == statuses);
    REQUIRE(status.last.rootUnavailable);
    deep.release();
    REQUIRE(waitUntil([&] {
        return !status.last.rootUnavailable && model.rowOf(root / "a" / "deep" / "IMG_2.jpg") >= 0;
    }));
    // Merged into the rows there were: no duplicate, and the marks, the EXIF and the thumbnail
    // of a row that stayed are kept.
    REQUIRE(primaries(model) == primaries(listShots(root, true)));
    REQUIRE(log.events == std::vector<std::string>{"+3..3"});
    REQUIRE(indexAgrees(model));
    REQUIRE(model.marks(model.rowOf(inA)).rating == 3);
    REQUIRE(model.exif(model.rowOf(inA)).has_value());
    REQUIRE(hasThumbnail(model, inA));
    REQUIRE(waitUntil([&] { return watcher.watchedDirectories().size() == 4; }));
    // Watched again for real: a new file shows without a refresh.
    addJpeg(root / "b" / "IMG_2.jpg");
    REQUIRE(waitUntil([&] { return model.rowOf(root / "b" / "IMG_2.jpg") >= 0; }));
    REQUIRE_FALSE(model.isRefreshing());
}

TEST_CASE("Under a root renamed away a subfolder that reports keeps its rows, and brings it back",
          "[watcher][listing][failure]") {
    // Review B1. The subfolder's watch follows it, and reports its old path.
    const test::TempDir folder;
    const fs::path root = folder.file("photos");
    const fs::path moved = folder.file("moved");
    addJpeg(root / "IMG_1.jpg");
    addJpeg(root / "a" / "IMG_1.jpg");
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(0));
    StatusLog status(watcher);
    model.setFolder(root, true);
    REQUIRE(waitUntil([&] { return !model.isListing() && status.last.directories == 2; }));
    model.setThumbnail(root / "a" / "IMG_1.jpg", aThumbnail());
    RowLog log(model);
    const test::ListingGate reads(root / "never");
    watcher.setReadHook(reads.hook());

    fs::rename(root, moved);
    addJpeg(moved / "a" / "IMG_2.jpg");
    REQUIRE(waitUntil([&] { return std::ranges::count(reads.read(), root / "a") >= 1; }));
    settle(model, watcher, reads, root);
    REQUIRE(model.rowCount() == 2);
    REQUIRE(log.events.empty());
    REQUIRE(status.last.rootUnavailable);
    REQUIRE(status.last.directories == 2);

    // Renamed back: the subfolder's next report reads it, and the root with its subtree.
    fs::rename(moved, root);
    addJpeg(root / "a" / "IMG_3.jpg");
    REQUIRE(waitUntil([&] {
        return model.rowOf(root / "a" / "IMG_3.jpg") >= 0 && !status.last.rootUnavailable;
    }));
    REQUIRE(waitUntil([&] { return primaries(model) == primaries(listShots(root, true)); }));
    REQUIRE(indexAgrees(model));
    REQUIRE(hasThumbnail(model, root / "a" / "IMG_1.jpg"));
}

TEST_CASE("Switching the subfolders on before the walk of a switch off ends is a listing",
          "[watcher][listing][toggle]") {
    // Review B2.
    const test::TempDir folder;
    const fs::path root = folder.path();
    addJpeg(root / "IMG_1.jpg");
    addJpeg(root / "a" / "IMG_1.jpg");
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(0));
    model.setFolder(root, true);
    REQUIRE(waitUntil([&] { return !model.isListing(); }));
    std::vector<bool> listing;
    QObject::connect(&model, &ShotModel::listingChanged, &model,
                     [&](bool now) { listing.push_back(now); });
    const test::ListingGate gate(root);
    watcher.setReadHook(gate.hook());

    model.setRecursive(false);
    REQUIRE(waitUntil([&] { return gate.reached(); }));
    REQUIRE_FALSE(model.isListing());
    model.setRecursive(true);
    REQUIRE(model.isListing());
    REQUIRE(listing == std::vector<bool>{true});
    gate.release();
    REQUIRE(waitUntil([&] { return !model.isListing(); }));
    REQUIRE(listing == std::vector<bool>{true, false});
    REQUIRE(primaries(model) == primaries(listShots(root, true)));
}

TEST_CASE("A walk cancelled while a watch is asked for does not go on to read the directory",
          "[watcher][listing][budget]") {
    // Review R3: the watch can block on a dead mount as long as the read.
    const test::TempDir first;
    const test::TempDir second;
    addJpeg(first.file("IMG_1.jpg"));
    addJpeg(first.file("a/IMG_1.jpg"));
    addJpeg(second.file("IMG_9.jpg"));
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(0));
    const test::ListingGate watching(first.file("a"));
    const test::ListingGate reads(first.file("never"));
    watcher.setWatchGate([hook = watching.hook()](const fs::path& directory) {
        hook(directory);
        return true;
    });
    watcher.setReadHook(reads.hook());

    model.setFolder(first.path(), true);
    REQUIRE(waitUntil([&] { return watching.reached(); }));
    model.setFolder(second.path(), true);
    watching.release();
    REQUIRE(waitUntil([&] { return !model.isListing(); }));
    REQUIRE(primaries(model) == std::vector<fs::path>{second.file("IMG_9.jpg")});
    REQUIRE(std::ranges::count(reads.read(), first.file("a")) == 0);
}

TEST_CASE("A toggle drops the batches of the walk before it, queued or late",
          "[shotmodel][listing][toggle]") {
    // Review N6: a batch of the abandoned walk that reached the model is not applied.
    const test::TempDir folder;
    const fs::path root = folder.path();
    ShotModel model;
    model.setFolder(root, true);
    const std::uint64_t walk = model.walk();
    model.applyFolders(walk, {{root, {jpeg(root / "IMG_1.jpg")}}});
    // Queued, and not applied yet, when the subfolders are switched off.
    model.queueFolders(walk, {{root / "a", {jpeg(root / "a/IMG_1.jpg")}}});
    model.setRecursive(false);
    // And one delivered after the switch, as the lister's thread may.
    model.queueFolders(walk, {{root / "b", {jpeg(root / "b/IMG_1.jpg")}}});
    model.finishListing(model.walk(), {});
    REQUIRE(primaries(model) == std::vector<fs::path>{root / "IMG_1.jpg"});
    REQUIRE_FALSE(model.isListing());
}

TEST_CASE("The end of a walk keeps the rows below thousands of stale folders in one pass",
          "[shotmodel][listing][refresh]") {
    // Review R5: 20 000 folders, 2 000 of them stale (listed in no particular order), none read.
    const test::TempDir folder;
    const fs::path root = folder.path();
    ShotModel model;
    model.setFolder(root, true);
    std::vector<FolderResult> batch;
    const auto name = [](int n) {
        std::string digits = std::to_string(n);
        return "d" + std::string(5 - digits.size(), '0') + digits;
    };
    for (int n = 0; n < 20000; ++n) {
        const fs::path sub = root / name(n);
        batch.push_back({sub, {jpeg(sub / "IMG_1.jpg")}});
    }
    model.applyFolders(model.walk(), batch);
    model.finishListing(model.walk(), {});
    REQUIRE(model.rowCount() == 20000);

    // The share failed after the first 2 000: those are stale, and the rest is gone. (The rows
    // that go are the last ones, so that their removal costs little beside the sweep.)
    ListingSummary summary;
    for (int n = 1999; n >= 0; --n) {
        summary.stale.push_back(root / name(n));
    }
    // A stale folder below another stale one changes nothing.
    summary.stale.push_back(root / name(10) / "deeper");
    model.refresh();
    const auto start = std::chrono::steady_clock::now();
    model.finishListing(model.walk(), summary);
    const auto took = std::chrono::steady_clock::now() - start;
    REQUIRE(model.rowCount() == 2000);
    REQUIRE(model.rowOf(root / name(0) / "IMG_1.jpg") == 0);
    REQUIRE(model.rowOf(root / name(1999) / "IMG_1.jpg") == 1999);
    REQUIRE(model.rowOf(root / name(2000) / "IMG_1.jpg") < 0);
    REQUIRE(indexAgrees(model));
    // It took 1.9 s in a Debug build when each span looked through every stale folder.
    CHECK(took < std::chrono::milliseconds(500));
}

#ifndef _WIN32

namespace {

/// A FIFO in place of a photograph: the loader's read of its EXIF blocks in the open until
/// released, so that the jobs behind it wait in the queue.
class HeldFile {
public:
    explicit HeldFile(fs::path path) : path_(std::move(path)) {
        REQUIRE(::mkfifo(path_.c_str(), 0600) == 0);
    }

    HeldFile(const HeldFile&) = delete;
    HeldFile& operator=(const HeldFile&) = delete;

    ~HeldFile() {
        release();
    }

    /// Lets a read blocked in the open go on to the end of the file, and any later read find no
    /// file at all.
    void release() {
        if (released_) {
            return;
        }
        released_ = true;
        // Read and write: this open never blocks, and lets a blocked reader's open return.
        const int fd = ::open(path_.c_str(), O_RDWR | O_NONBLOCK);
        std::error_code ignored;
        fs::remove(path_, ignored);
        if (fd >= 0) {
            ::close(fd);
        }
    }

private:
    fs::path path_;
    bool released_ = false;
};

} // namespace

TEST_CASE("Switching the subfolders off drops the marks and EXIF still queued for their rows",
          "[shotmodel][listing][toggle]") {
    // Review R4. The rows' marks are read first, then the EXIF, which holds at IMG_0.
    const test::TempDir folder;
    const fs::path root = folder.path();
    ShotModel model;
    // Declared after the model, so that it lets the loader go before the model joins it.
    HeldFile held(root / "IMG_0.jpg");
    model.setFolder(root, true);
    const std::vector<Shot> own{jpeg(root / "IMG_0.jpg"), jpeg(root / "IMG_1.jpg")};
    const std::vector<Shot> below = jpegs(root / "sub", 3);
    model.applyFolders(model.walk(), {{root, own}, {root / "sub", below}});
    // Ten jobs: the marks of five rows are back, and the loader is held in the first EXIF.
    REQUIRE(waitUntil([&] { return model.pendingLoads() == 5; }));

    model.setRecursive(false);
    // The held one and the root's other; the subfolder's three are dropped.
    REQUIRE(model.pendingLoads() == 2);
    held.release();
    REQUIRE(waitUntil([&] { return model.pendingLoads() == 0; }));

    // On again: the subfolder's rows are listed anew, and their reads queued again.
    model.setRecursive(true);
    model.applyFolders(model.walk(), {{root, own}, {root / "sub", below}});
    REQUIRE(model.pendingLoads() == 6);
    model.finishListing(model.walk(), {});
    REQUIRE(waitUntil([&] { return model.pendingLoads() == 0; }));
    REQUIRE(model.rowCount() == 5);
}

#endif
