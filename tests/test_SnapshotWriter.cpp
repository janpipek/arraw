#include "ShotModel.h"
#include "SidecarLimits.h"
#include "SnapshotWriter.h"
#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <DevelopState.h>
#include <Photo.h>
#include <Sidecar.h>
#include <SnapshotThumbnails.h>
#include <SnapshotWrites.h>
#include <Snapshots.h>

#include <QLockFile>
#include <QString>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace arraw;
using Catch::Matchers::ContainsSubstring;
using Failure = app::SnapshotWriteResult::Failure;

/// The writer thread of the snapshot panel (src/app/SnapshotWriter.h): order, batching, failures,
/// shutdown, and living beside a Save on another thread.

namespace {

namespace fs = std::filesystem;

std::string slurp(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

DevelopState exposed(float exposure) {
    DevelopState state;
    state.settings.tone.exposure = exposure;
    return state;
}

/// A photograph in a folder of its own.
struct Rig {
    test::TempDir directory;
    fs::path photo = directory.file("IMG_1.dng");

    Rig() {
        fs::copy_file(test::fixture("preview-32x24.dng"), photo);
    }

    [[nodiscard]] Snapshot draft(std::string name, float exposure = 0.5F) const {
        Snapshot snapshot;
        snapshot.name = std::move(name);
        snapshot.created = std::chrono::sys_seconds{std::chrono::seconds{1'790'000'000}};
        snapshot.state = exposed(exposure);
        return snapshot;
    }

    [[nodiscard]] SnapshotWrite create(std::uint64_t revision, std::string name,
                                       float exposure = 0.5F) const {
        return {photo, revision, SnapshotCreate{draft(std::move(name), exposure), 0}};
    }

    [[nodiscard]] SnapshotWrite rename(std::uint64_t revision, SnapshotId id,
                                       std::string name) const {
        return {photo, revision, SnapshotRename{id, std::move(name)}};
    }

    /// A patch with a thumbnail rendered for the snapshot's recipe, as the worker would.
    [[nodiscard]] SnapshotWrite patch(const Snapshot& snapshot) const {
        const auto rendered = renderSnapshotThumbnail(
            photo, SnapshotRecipe{snapshot.id, snapshot.state, snapshot.negative});
        REQUIRE(rendered);
        return {photo, 0,
                SnapshotThumbnailPatch{snapshot.id, *rendered, snapshot.state, snapshot.negative}};
    }

    [[nodiscard]] SnapshotCollection collection() const {
        const auto contents = readSidecar(photo);
        REQUIRE(contents);
        return contents->snapshots;
    }
};

/// Lowers the lock wait for the life of the object.
struct ShortLockTimeout {
    int before = detail::sidecarLockTimeout;

    ShortLockTimeout() {
        detail::sidecarLockTimeout = 100;
    }
    ~ShortLockTimeout() {
        detail::sidecarLockTimeout = before;
    }
    ShortLockTimeout(const ShortLockTimeout&) = delete;
    ShortLockTimeout& operator=(const ShortLockTimeout&) = delete;
};

/// Blocks a thread until released.
class Gate {
public:
    void wait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return open_; });
    }

    [[nodiscard]] bool entered() {
        const std::scoped_lock lock(mutex_);
        return entered_;
    }

    void release() {
        {
            const std::scoped_lock lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool open_ = false;
};

/// Opens a gate when it goes out of scope, so that a failed check cannot leave a thread blocked.
struct GateGuard {
    explicit GateGuard(Gate& gate) : gate(gate) {}
    ~GateGuard() {
        gate.release();
    }
    GateGuard(const GateGuard&) = delete;
    GateGuard& operator=(const GateGuard&) = delete;

    Gate& gate;
};

bool waitUntil(const std::function<bool()>& condition,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(20000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

/// Collects the results, from the writer's thread.
class Collector {
public:
    [[nodiscard]] std::function<void(app::SnapshotWriteResult)> callback() {
        return [this](app::SnapshotWriteResult result) {
            const std::scoped_lock lock(mutex_);
            results_.push_back(std::move(result));
        };
    }

    [[nodiscard]] std::vector<app::SnapshotWriteResult> results() const {
        const std::scoped_lock lock(mutex_);
        return results_;
    }

    [[nodiscard]] std::size_t count() const {
        const std::scoped_lock lock(mutex_);
        return results_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<app::SnapshotWriteResult> results_;
};

/// Counts the lock acquisitions of every writer, for the life of the object.
struct CountedLocks {
    std::atomic<int> taken = 0;

    CountedLocks() {
        detail::sidecarLockHeldHook = [this] { ++taken; };
    }
    ~CountedLocks() {
        detail::sidecarLockHeldHook = nullptr;
    }
    CountedLocks(const CountedLocks&) = delete;
    CountedLocks& operator=(const CountedLocks&) = delete;
};

} // namespace

TEST_CASE("Results come back in submission order with their ticket and owner",
          "[snapshots][writer]") {
    const Rig rig;
    Collector collector;
    app::SnapshotWriter writer(collector.callback());

    const std::uint64_t first = writer.submit(rig.create(0, "A"), 7);
    const std::uint64_t second = writer.submit(rig.rename(1, SnapshotId{1}, "A renamed"), 8);
    const std::uint64_t third = writer.submit(rig.create(2, "B"), 9);
    CHECK(second == first + 1);
    CHECK(third == second + 1);
    REQUIRE(waitUntil([&] { return collector.count() == 3 && writer.pending() == 0; }));

    const auto results = collector.results();
    CHECK(results[0].ticket == first);
    CHECK(results[1].ticket == second);
    CHECK(results[2].ticket == third);
    CHECK(results[0].owner == 7);
    CHECK(results[1].owner == 8);
    CHECK(results[2].owner == 9);
    for (const auto& result : results) {
        CHECK(result.failure == Failure::None);
        CHECK(result.outcome.written);
        REQUIRE(result.outcome.update);
        REQUIRE(result.stamp);
        CHECK(result.write.photo == rig.photo);
    }
    CHECK(results[0].outcome.update->collection.revision == 1);
    CHECK(results[1].outcome.update->collection.find(SnapshotId{1})->name == "A renamed");
    CHECK(results[2].outcome.update->id == SnapshotId{2});
    CHECK(rig.collection() == results[2].outcome.update->collection);
    // The stamp is the sidecar's right after the commit; the last one is the file's now.
    CHECK(results[2].stamp == app::fileStamp(sidecarPath(rig.photo)));
}

TEST_CASE("Adjacent thumbnail patches of one photograph share one lock and one commit",
          "[snapshots][writer][thumbnail]") {
    const Rig rig;
    std::vector<Snapshot> made;
    for (int i = 0; i < 3; ++i) {
        const SnapshotUpdate update =
            addSnapshot(rig.photo, static_cast<std::uint64_t>(i),
                        rig.draft("S" + std::to_string(i), static_cast<float>(i) + 1.0F));
        made.push_back(*update.collection.find(update.id));
    }
    // The patches are rendered before the writer exists, so that it does only its own work.
    const SnapshotWrite p0 = rig.patch(made[0]);
    const SnapshotWrite p1 = rig.patch(made[1]);
    const SnapshotWrite p2 = rig.patch(made[2]);

    Collector collector;
    app::SnapshotWriter writer(collector.callback());
    Gate gate;
    const GateGuard guard(gate);
    std::atomic<int> calls = 0;
    writer.setBeforeWriteHook([&] {
        if (++calls == 1) {
            gate.wait();
        }
    });
    const CountedLocks locks;

    // The writer is held in front of the rename, so the patches queue up behind it.
    writer.submit(rig.rename(3, made[0].id, "S0 renamed"), 1);
    REQUIRE(waitUntil([&] { return gate.entered(); }));
    writer.submit(p0, 1);
    writer.submit(p1, 1);
    writer.submit(p2, 1);
    gate.release();

    REQUIRE(waitUntil([&] { return collector.count() == 4 && writer.pending() == 0; }));
    CHECK(calls == 2);       // the rename, and the three patches as one
    CHECK(locks.taken == 2); // so one lock for them
    const auto results = collector.results();
    for (std::size_t i = 1; i < 4; ++i) {
        CHECK(results[i].failure == Failure::None);
        CHECK(results[i].outcome.written);
        CHECK_FALSE(results[i].outcome.update);
        CHECK(results[i].stamp == results[1].stamp);
    }
    CHECK(results[0].ticket < results[1].ticket);
    CHECK(results[1].ticket < results[2].ticket);
    CHECK(results[2].ticket < results[3].ticket);
    CHECK(results[3].stamp == app::fileStamp(sidecarPath(rig.photo)));
    const SnapshotCollection collection = rig.collection();
    CHECK(collection.revision == 4); // the patches did not change it
    for (const Snapshot& snapshot : collection.entries) {
        CHECK(snapshot.thumbnail);
    }
    CHECK(collection.find(made[0].id)->name == "S0 renamed");

    SECTION("a patch that does not match is reported unwritten and the others still land") {
        // A recipe that is not the snapshot's has another key.
        SnapshotWrite wrong = p0;
        std::get<SnapshotThumbnailPatch>(wrong.change).state = exposed(2.5F);
        std::get<SnapshotThumbnailPatch>(wrong.change).thumbnail.renderKey = "stale";
        const std::size_t seen = collector.count();
        writer.submit(wrong, 1);
        writer.submit(p1, 1);
        REQUIRE(waitUntil([&] { return collector.count() == seen + 2 && writer.pending() == 0; }));
        INFO(collector.results()[seen].error);
        CHECK_FALSE(collector.results()[seen].outcome.written);
        CHECK(collector.results()[seen].failure == Failure::None);
        CHECK(collector.results()[seen + 1].outcome.written);
    }
}

TEST_CASE("Patches for two photographs are not mixed in one commit", "[snapshots][writer]") {
    const Rig one;
    const Rig two;
    const SnapshotUpdate a = addSnapshot(one.photo, 0, one.draft("A"));
    const SnapshotUpdate b = addSnapshot(two.photo, 0, two.draft("B"));
    const SnapshotWrite pa = one.patch(*a.collection.find(a.id));
    const SnapshotWrite pb = two.patch(*b.collection.find(b.id));

    Collector collector;
    app::SnapshotWriter writer(collector.callback());
    Gate gate;
    const GateGuard guard(gate);
    std::atomic<int> calls = 0;
    writer.setBeforeWriteHook([&] {
        if (++calls == 1) {
            gate.wait();
        }
    });
    writer.submit(one.rename(1, a.id, "A renamed"), 1);
    REQUIRE(waitUntil([&] { return gate.entered(); }));
    writer.submit(pa, 1);
    writer.submit(pb, 1);
    gate.release();
    REQUIRE(waitUntil([&] { return collector.count() == 3 && writer.pending() == 0; }));
    CHECK(calls == 3);
    CHECK(one.collection().find(a.id)->thumbnail);
    CHECK(two.collection().find(b.id)->thumbnail);
}

TEST_CASE("A conflict comes back with the collection read right after it", "[snapshots][writer]") {
    const Rig rig;
    addSnapshot(rig.photo, 0, rig.draft("Theirs"));
    const std::string before = slurp(sidecarPath(rig.photo));
    Collector collector;
    app::SnapshotWriter writer(collector.callback());

    writer.submit(rig.create(0, "Mine"), 3); // the revision it saw is already gone
    REQUIRE(waitUntil([&] { return collector.count() == 1 && writer.pending() == 0; }));

    const auto result = collector.results().front();
    CHECK(result.failure == Failure::Conflict);
    CHECK_FALSE(result.outcome.written);
    CHECK_FALSE(result.stamp);
    CHECK_THAT(result.error, ContainsSubstring("revision"));
    REQUIRE(result.fresh);
    CHECK(result.fresh->collection == rig.collection());
    CHECK(result.fresh->collection.revision == 1);
    CHECK(result.fresh->unreadable == 0);
    CHECK(slurp(sidecarPath(rig.photo)) == before);
}

TEST_CASE("A refused write is reported and changes nothing", "[snapshots][writer]") {
    const Rig rig;
    addSnapshot(rig.photo, 0, rig.draft("A"));
    const std::string before = slurp(sidecarPath(rig.photo));
    Collector collector;
    app::SnapshotWriter writer(collector.callback());

    writer.submit(rig.rename(1, SnapshotId{1}, ""), 1);        // an empty name
    writer.submit(rig.rename(1, SnapshotId{99}, "Nobody"), 1); // not a snapshot
    SnapshotWrite foreign = rig.create(1, "x");
    foreign.photo = rig.directory.file("IMG_404.dng"); // not a photograph at all
    writer.submit(foreign, 1);
    writer.submit(rig.create(1, "Fine"), 1); // and the writer goes on
    REQUIRE(waitUntil([&] { return collector.count() == 4 && writer.pending() == 0; }));

    const auto results = collector.results();
    CHECK(results[0].failure == Failure::Refused);
    CHECK(results[1].failure == Failure::Refused);
    CHECK(results[0].error.size() > 0);
    CHECK_FALSE(results[0].stamp);
    CHECK(results[2].failure != Failure::None);
    CHECK(results[3].failure == Failure::None);
    CHECK(rig.collection().entries.size() == 2);
    CHECK(slurp(sidecarPath(rig.photo)) != before); // only the last write changed it
}

TEST_CASE("A locked sidecar gives a lock timeout and no write", "[snapshots][writer][slow]") {
    const ShortLockTimeout shortTimeout;
    const Rig rig;
    addSnapshot(rig.photo, 0, rig.draft("A"));
    const std::string before = slurp(sidecarPath(rig.photo));
    Collector collector;
    app::SnapshotWriter writer(collector.callback());

    QLockFile lock(QString::fromStdString(sidecarPath(rig.photo).string() + ".lock"));
    REQUIRE(lock.tryLock(1000));
    writer.submit(rig.create(1, "B"), 1);
    REQUIRE(waitUntil([&] { return collector.count() == 1 && writer.pending() == 0; }));
    const auto result = collector.results().front();
    CHECK(result.failure == Failure::LockTimeout);
    CHECK_FALSE(result.outcome.written);
    CHECK_FALSE(result.stamp);
    CHECK(slurp(sidecarPath(rig.photo)) == before);

    lock.unlock();
    writer.submit(rig.create(1, "B"), 1);
    REQUIRE(waitUntil([&] { return collector.count() == 2 && writer.pending() == 0; }));
    CHECK(collector.results()[1].failure == Failure::None);
}

TEST_CASE("Destroying the writer carries out structural writes, drops patches and delivers nothing",
          "[snapshots][writer]") {
    const Rig rig;
    const SnapshotUpdate a = addSnapshot(rig.photo, 0, rig.draft("A"));
    const SnapshotWrite patch = rig.patch(*a.collection.find(a.id));

    Collector collector;
    Gate gate;
    const GateGuard guard(gate);
    std::jthread closer;
    {
        auto writer = std::make_unique<app::SnapshotWriter>(collector.callback());
        std::atomic<int> calls = 0;
        writer->setBeforeWriteHook([&] {
            if (++calls == 1) {
                gate.wait();
            }
        });
        writer->submit(rig.create(1, "First"), 1);
        REQUIRE(waitUntil([&] { return gate.entered(); }));
        writer->submit(patch, 1);
        writer->submit(patch, 1);
        writer->submit(rig.rename(2, SnapshotId{1}, "A renamed"), 1);
        REQUIRE(writer->pending() == 4);

        // The destructor drops the patches and waits for the rest; the gate opens meanwhile.
        app::SnapshotWriter* raw = writer.release();
        closer = std::jthread([raw] { delete raw; });
        // pending() is not callable once the object may be gone, so wait on the file instead
        // after a pause long enough for the destructor to have begun.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        gate.release();
    }
    closer.join();

    // The creation and the rename were carried out, the patches were not.
    const SnapshotCollection collection = rig.collection();
    REQUIRE(collection.entries.size() == 2);
    CHECK(collection.find(SnapshotId{1})->name == "A renamed");
    CHECK(collection.find(SnapshotId{2})->name == "First");
    CHECK_FALSE(collection.find(SnapshotId{1})->thumbnail);
    CHECK(collection.revision == 3);
    // Nothing was delivered after destruction began, the write in flight included.
    CHECK(collector.count() == 0);
}

TEST_CASE("A Save on another thread waits for the writer's lock and both changes land",
          "[snapshots][writer][lock]") {
    const Rig rig;
    addSnapshot(rig.photo, 0, rig.draft("A"));
    Collector collector;
    app::SnapshotWriter writer(collector.callback());
    std::atomic<std::thread::id> writerThread{};
    writer.setBeforeWriteHook([&] { writerThread = std::this_thread::get_id(); });
    Gate gate;
    const GateGuard guard(gate);
    // Held inside the writer's lock: called by every writer right after it has the lock.
    detail::sidecarLockHeldHook = [&] {
        if (std::this_thread::get_id() == writerThread.load()) {
            gate.wait();
        }
    };
    struct Reset {
        ~Reset() {
            detail::sidecarLockHeldHook = nullptr;
        }
    } reset;

    writer.submit(rig.rename(1, SnapshotId{1}, "Renamed"), 1);
    REQUIRE(waitUntil([&] { return gate.entered(); }));

    std::atomic<bool> saved = false;
    std::jthread saver([&] {
        writeSidecar(openPhoto(rig.photo).with(exposed(1.5F)));
        saved = true;
    });
    // The Save cannot get the lock while the writer holds it.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(saved);

    gate.release();
    REQUIRE(waitUntil([&] { return saved.load() && collector.count() == 1; }));
    saver.join();
    CHECK(collector.results().front().failure == Failure::None);
    const auto contents = readSidecar(rig.photo);
    REQUIRE(contents);
    REQUIRE(contents->state);
    CHECK(*contents->state == exposed(1.5F));
    CHECK(contents->snapshots.find(SnapshotId{1})->name == "Renamed");
}
