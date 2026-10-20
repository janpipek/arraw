#include "FolderLister.h"
#include "FolderResult.h"
#include "FolderWatcher.h"
#include "ShotModel.h"
#include "support/ListingGate.h"
#include "support/TempDir.h"

#include <Shot.h>

#include <QCoreApplication>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
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
        model.refresh(std::vector<fs::path>{root});
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
