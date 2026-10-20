#include "FolderLister.h"
#include "FolderResult.h"
#include "FolderWatcher.h"
#include "ShotModel.h"
#include "support/ListingGate.h"
#include "support/TempDir.h"

#include <Shot.h>
#include <Sidecar.h>

#include <QCoreApplication>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <vector>

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
    SECTION("A refresh asked for during the listing waits for its end, then merges") {
        model.applyFolders(walk, {{root, {jpeg(root / "IMG_1.jpg")}},
                                  {root / "gone", {jpeg(root / "gone/IMG_1.jpg")}}});
        model.refresh();
        model.refresh();
        REQUIRE(walks == 1);
        REQUIRE(model.isRefreshing());
        model.finishListing(walk, {});
        // One walk for both, under a number of its own; the loader's generation is not changed.
        REQUIRE(walks == 2);
        REQUIRE(model.walk() == walk + 1);
        REQUIRE_FALSE(model.isListing());
        REQUIRE(model.isRefreshing());
        log.events.clear();
        // The refresh finds a new shot in the root and no "gone": that folder's rows go at the end.
        model.applyFolders(model.walk(),
                           {{root, {jpeg(root / "IMG_1.jpg"), jpeg(root / "IMG_2.jpg")}}});
        REQUIRE(model.rowCount() == 3);
        model.finishListing(model.walk(), {});
        REQUIRE_FALSE(model.isRefreshing());
        REQUIRE(log.events == std::vector<std::string>{"+1..1", "-2..2"});
        REQUIRE(primaries(model) == std::vector<fs::path>{root / "IMG_1.jpg", root / "IMG_2.jpg"});
        REQUIRE(listing == std::vector<bool>{true, false});
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
