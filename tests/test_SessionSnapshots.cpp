#include "SidecarLimits.h"
#include "support/Fixtures.h"
#include "support/LensFixtures.h"
#include "support/Lensfun.h"
#include "support/TempDir.h"

#include <DevelopState.h>
#include <EditSession.h>
#include <NegativeSettings.h>
#include <Photo.h>
#include <Sidecar.h>
#include <Snapshots.h>

#include <QLockFile>
#include <QString>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace arraw;
using Catch::Matchers::ContainsSubstring;

/// Snapshots in an edit session (ADR 056, decision 3): the working and saved base, history,
/// Create, Restore, Rename, Delete, Save and Discard.

namespace {

namespace fs = std::filesystem;

/// A RAW beside which the sidecar lives.
struct Rig {
    test::TempDir directory;
    fs::path photo = directory.file("IMG_1.dng");

    Rig() {
        fs::copy_file(test::fixture("linear-32x24-neutral.dng"), photo);
    }

    [[nodiscard]] EditSession open() const {
        return EditSession(openDocument(photo));
    }

    [[nodiscard]] OpenDocument disk() const {
        return openDocument(photo);
    }
};

DevelopState exposed(float exposure) {
    DevelopState state;
    state.settings.tone.exposure = exposure;
    return state;
}

std::optional<SnapshotId> id(std::uint64_t value) {
    return SnapshotId{value};
}

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

/// Bases of every history step.
std::vector<std::optional<SnapshotId>> basesOf(const EditSession& session) {
    std::vector<std::optional<SnapshotId>> bases;
    for (const HistoryStep& step : session.history()) {
        bases.push_back(step.baseSnapshot);
    }
    return bases;
}

/// Holds the sidecar's lock, as a slow writer would.
struct HeldLock {
    QLockFile lock;

    explicit HeldLock(const fs::path& photo)
        : lock(QString::fromStdString(sidecarPath(photo).string() + ".lock")) {
        REQUIRE(lock.tryLock(1000));
    }
};

/// A snapshot as another process would add one.
Snapshot theirs(const std::string& name) {
    Snapshot snapshot;
    snapshot.name = name;
    snapshot.created = std::chrono::sys_seconds{std::chrono::seconds{1'790'000'000}};
    return snapshot;
}

} // namespace

TEST_CASE("A session opened from a document starts with its saved base", "[session][snapshots]") {
    const Rig rig;
    {
        EditSession first = rig.open();
        first.createSnapshot("A");
        first.save();
    }
    const EditSession session = rig.open();

    CHECK(session.snapshots().entries.size() == 1);
    CHECK(session.baseSnapshot() == id(1));
    CHECK(session.savedBaseSnapshot() == id(1));
    CHECK(session.history().size() == 1);
    CHECK(session.history()[0].baseSnapshot == id(1));
    CHECK_FALSE(session.hasUnsavedChanges());
}

TEST_CASE("Create writes at once, takes the current base as parent and adds no step",
          "[session][snapshots][create]") {
    const Rig rig;
    EditSession session = rig.open();
    session.setState(exposed(1.0F));

    const SnapshotId a = session.createSnapshot("A");

    CHECK(a.value == 1);
    CHECK(session.history().size() == 2);
    CHECK(session.position() == 1);
    CHECK(session.baseSnapshot() == a);
    CHECK(session.history()[1].baseSnapshot == a);
    CHECK(session.history()[0].baseSnapshot == std::nullopt);
    // Written to the sidecar, with the look as it was; the saved recipe and base are untouched.
    const OpenDocument disk = rig.disk();
    REQUIRE(disk.snapshots.entries.size() == 1);
    CHECK(disk.snapshots.entries[0].name == "A");
    CHECK(disk.snapshots.entries[0].state == exposed(1.0F));
    CHECK_FALSE(disk.savedBase);
    CHECK(disk.photo.state() == defaultStateFor(disk.photo.metadata().encoding));
    CHECK(session.snapshots() == disk.snapshots);
    // The new base is unsaved, whatever the pixels.
    CHECK(session.hasUnsavedChanges());

    session.setState(exposed(2.0F));
    const SnapshotId b = session.createSnapshot("B");
    CHECK(session.snapshots().find(b)->parent == a);
}

TEST_CASE("Create settles a gesture and anchors the step it makes",
          "[session][snapshots][create]") {
    const Rig rig;
    EditSession session = rig.open();
    session.begin();
    session.update(exposed(1.5F));

    const SnapshotId a = session.createSnapshot("A");

    CHECK_FALSE(session.editing());
    CHECK(session.history().size() == 2);
    CHECK(session.history()[1].state == exposed(1.5F));
    CHECK(session.history()[1].baseSnapshot == a);
    CHECK(session.snapshots().find(a)->state == exposed(1.5F));
}

TEST_CASE("A failed Create changes nothing, an open gesture included",
          "[session][snapshots][create]") {
    const Rig rig;
    EditSession session = rig.open();
    session.setState(exposed(1.0F));
    session.begin();
    session.update(exposed(2.0F));
    const std::vector<HistoryStep> history = session.history();
    const std::size_t position = session.position();

    {
        const ShortLockTimeout shortTimeout;
        QLockFile lock(QString::fromStdString(sidecarPath(rig.photo).string() + ".lock"));
        REQUIRE(lock.tryLock(1000));
        CHECK_THROWS_AS(session.createSnapshot("A"), SidecarLockTimeout);
        CHECK_THROWS_AS(session.createSnapshot(""), std::invalid_argument);
    }

    CHECK(session.editing());
    CHECK(session.photo().state() == exposed(2.0F));
    CHECK(session.history() == history);
    CHECK(session.position() == position);
    CHECK(session.snapshots().entries.empty());
    CHECK(session.baseSnapshot() == std::nullopt);
    CHECK(rig.disk().snapshots.entries.empty());

    // The same call goes through once the lock is free.
    CHECK_NOTHROW(session.createSnapshot("A"));
    CHECK_FALSE(session.editing());
    CHECK(session.history().size() == history.size() + 1);
}

TEST_CASE("Create keeps what could be redone, and undo reaches the earlier base",
          "[session][snapshots][create]") {
    const Rig rig;
    EditSession session = rig.open();
    session.setState(exposed(1.0F));
    session.setState(exposed(2.0F));
    session.setState(exposed(3.0F));
    session.undo();
    session.undo();
    REQUIRE(session.position() == 1);
    REQUIRE(session.canRedo());

    const SnapshotId x = session.createSnapshot("X");

    CHECK(session.history().size() == 4);
    CHECK(session.position() == 1);
    CHECK(session.canRedo());
    CHECK(basesOf(session) ==
          std::vector<std::optional<SnapshotId>>{std::nullopt, x, std::nullopt, std::nullopt});
    session.redo();
    CHECK(session.baseSnapshot() == std::nullopt);
    CHECK(session.photo().state() == exposed(2.0F));
    session.undo();
    CHECK(session.baseSnapshot() == x);
    CHECK(session.photo().state() == exposed(1.0F));
    // Undo reaches the preceding edit and its earlier base.
    session.undo();
    CHECK(session.baseSnapshot() == std::nullopt);
    CHECK_FALSE(session.canUndo());

    // A later edit drops the redo entries, by the ordinary rule.
    session.setState(exposed(4.0F));
    CHECK_FALSE(session.canRedo());
}

TEST_CASE("Undo never removes a durable snapshot", "[session][snapshots][create]") {
    const Rig rig;
    EditSession session = rig.open();
    session.setState(exposed(1.0F));
    const SnapshotId a = session.createSnapshot("A");
    session.setState(exposed(2.0F));

    while (session.canUndo()) {
        session.undo();
    }

    CHECK(session.snapshots().find(a) != nullptr);
    CHECK(rig.disk().snapshots.find(a) != nullptr);
    while (session.canRedo()) {
        session.redo();
    }
    CHECK(rig.disk().snapshots.entries.size() == 1);
}

TEST_CASE("Restore is an exact undoable step", "[session][snapshots][restore]") {
    const Rig rig;
    EditSession session = rig.open();
    NegativeSettings negative;
    negative.demosaic = Demosaic::Vng;
    session.setState(negative, exposed(1.0F));
    const SnapshotId a = session.createSnapshot("Look A");
    session.setState(NegativeSettings{}, exposed(2.0F));
    REQUIRE(session.canUndo());

    session.restoreSnapshot(a);

    CHECK(session.history().size() == 4);
    const HistoryStep& step = session.history().back();
    CHECK(step.origin == EditOrigin::Restore);
    CHECK(step.detail == "Look A");
    CHECK(step.baseSnapshot == a);
    CHECK(step.state == exposed(1.0F));
    CHECK(step.negative == negative);
    CHECK(session.photo().state() == exposed(1.0F));
    CHECK(session.photo().negative() == negative);
    CHECK(session.baseSnapshot() == a);

    session.undo();
    CHECK(session.photo().state() == exposed(2.0F));
    CHECK(session.baseSnapshot() == a); // The step before it carries A, which Create made.
    session.redo();
    CHECK(session.baseSnapshot() == a);
    CHECK(session.photo().negative() == negative);
}

TEST_CASE("A base change is a step even with equal pixels", "[session][snapshots][restore]") {
    const Rig rig;
    EditSession session = rig.open();
    const SnapshotId s = session.createSnapshot("S");
    session.save();
    const SnapshotId t = session.createSnapshot("T");
    REQUIRE(session.baseSnapshot() == t);
    REQUIRE(session.hasUnsavedChanges());

    session.restoreSnapshot(s);

    CHECK(session.history().size() == 2);
    CHECK(session.history()[1].baseSnapshot == s);
    CHECK(session.photo().state() == session.history()[0].state);
    CHECK_FALSE(session.hasUnsavedChanges()); // Back at the saved base, and the saved pixels.
    // Nothing to restore when both the look and the base already are the snapshot's.
    session.restoreSnapshot(s);
    CHECK(session.history().size() == 2);
    // An unknown snapshot changes nothing.
    CHECK_THROWS_AS(session.restoreSnapshot(SnapshotId{99}), std::invalid_argument);
    CHECK(session.history().size() == 2);
}

TEST_CASE("Restore commits an open edit first and drops the redo steps",
          "[session][snapshots][restore]") {
    const Rig rig;
    EditSession session = rig.open();
    session.setState(exposed(1.0F));
    const SnapshotId a = session.createSnapshot("A");
    session.setState(exposed(2.0F));
    session.undo();
    REQUIRE(session.canRedo());
    session.begin();
    session.update(exposed(3.0F));

    session.restoreSnapshot(a);

    CHECK_FALSE(session.editing());
    // Opened, edit 1 (anchored), the gesture, then the restore; edit 2 was dropped.
    CHECK(session.history().size() == 4);
    CHECK(session.history()[2].state == exposed(3.0F));
    CHECK(session.history()[3].origin == EditOrigin::Restore);
    CHECK_FALSE(session.canRedo());
}

TEST_CASE("The worked example: A, B and C", "[session][snapshots]") {
    const Rig rig;
    EditSession session = rig.open();
    const SnapshotId a = session.createSnapshot("A"); // Base A at the default look.
    session.setState(exposed(1.0F));
    const SnapshotId b = session.createSnapshot("B"); // Child of A, the look of exposure 1.
    session.restoreSnapshot(a);
    REQUIRE(session.baseSnapshot() == a);

    session.setState(exposed(2.0F)); // Edit on A.
    session.restoreSnapshot(b);      // Restore B.
    session.setState(exposed(3.0F)); // Edit on B.
    const SnapshotId c = session.createSnapshot("C");

    CHECK(session.snapshots().find(c)->parent == b);
    CHECK(session.snapshots().find(b)->parent == a);
    CHECK(session.snapshots().find(a)->parent == std::nullopt);
    const std::size_t last = session.history().size() - 1;
    CHECK(session.position() == last);
    CHECK(session.history()[last].baseSnapshot == c);

    // Undo goes to the Restore-B position, then across the Restore to the A-based work.
    session.undo();
    CHECK(session.history()[session.position()].origin == EditOrigin::Restore);
    CHECK(session.baseSnapshot() == b);
    CHECK(session.photo().state() == exposed(1.0F));
    session.undo();
    CHECK(session.baseSnapshot() == a);
    CHECK(session.photo().state() == exposed(2.0F));
    // Redo returns through the same positions to the captured one, with the new base.
    session.redo();
    CHECK(session.history()[session.position()].origin == EditOrigin::Restore);
    CHECK(session.baseSnapshot() == b);
    session.redo();
    CHECK(session.position() == last);
    CHECK(session.baseSnapshot() == c);
    CHECK(session.photo().state() == exposed(3.0F));
    CHECK(session.snapshots().entries.size() == 3);
}

TEST_CASE("goTo restores the base of the step", "[session][snapshots]") {
    const Rig rig;
    EditSession session = rig.open();
    const SnapshotId a = session.createSnapshot("A");
    session.setState(exposed(1.0F));
    session.restoreSnapshot(a);
    session.setState(exposed(2.0F));
    const SnapshotId b = session.createSnapshot("B");

    session.goTo(1);
    CHECK(session.baseSnapshot() == a);
    session.goTo(3);
    CHECK(session.baseSnapshot() == b);
    session.goTo(0);
    CHECK(session.baseSnapshot() == a);
}

TEST_CASE("goTo reaches a step that has no base", "[session][snapshots]") {
    const Rig rig;
    EditSession session = rig.open();
    session.setState(exposed(1.0F));
    const SnapshotId a = session.createSnapshot("A");
    session.setState(exposed(2.0F));
    REQUIRE(session.baseSnapshot() == a);

    session.goTo(0);
    CHECK(session.baseSnapshot() == std::nullopt);
    session.goTo(1);
    CHECK(session.baseSnapshot() == a);
    session.goTo(0);
    CHECK(session.baseSnapshot() == std::nullopt);
}

TEST_CASE("Dirty state compares the base too, and Save writes it", "[session][snapshots][save]") {
    const Rig rig;
    EditSession session = rig.open();
    CHECK_FALSE(session.hasUnsavedChanges());
    session.createSnapshot("A");
    CHECK(session.hasUnsavedChanges()); // Equal pixels, another base.
    CHECK_FALSE(session.canUndo());     // Create added no step to take back.
    session.discardChanges();
    CHECK_FALSE(session.hasUnsavedChanges());
    CHECK(session.baseSnapshot() == std::nullopt);
}

TEST_CASE("Saving writes the base and cleans the session", "[session][snapshots][save]") {
    const Rig rig;
    EditSession session = rig.open();
    session.createSnapshot("A");
    session.setState(exposed(1.0F));
    const SnapshotId b = session.createSnapshot("B");
    REQUIRE(session.hasUnsavedChanges());

    session.save();

    CHECK_FALSE(session.hasUnsavedChanges());
    CHECK(session.savedBaseSnapshot() == b);
    const OpenDocument disk = rig.disk();
    CHECK(disk.savedBase == b);
    CHECK(disk.photo.state() == exposed(1.0F));
    CHECK(disk.snapshots.entries.size() == 2);
    // History is kept; undoing takes back the exposure, so the state differs and it is dirty.
    session.undo();
    CHECK(session.hasUnsavedChanges());
}

TEST_CASE("Save retries once when another process deleted the base", "[session][snapshots][save]") {
    const Rig rig;
    EditSession session = rig.open();
    const SnapshotId a = session.createSnapshot("A");
    session.save();
    const SnapshotId b = session.createSnapshot("B");
    session.setState(exposed(1.0F));
    REQUIRE(session.baseSnapshot() == b);
    // Another process deletes B: its session's revision is the file's.
    deleteSnapshot(rig.photo, rig.disk().snapshots.revision, b);

    CHECK_NOTHROW(session.save());

    CHECK(session.baseSnapshot() == a);
    CHECK(session.savedBaseSnapshot() == a);
    CHECK(session.snapshots().find(b) == nullptr);
    CHECK(basesOf(session) == std::vector<std::optional<SnapshotId>>{a, a});
    CHECK_FALSE(session.hasUnsavedChanges());
    const OpenDocument disk = rig.disk();
    CHECK(disk.savedBase == a);
    CHECK(disk.photo.state() == exposed(1.0F));
}

TEST_CASE("Save writes only the marks the session changed", "[session][snapshots][save]") {
    const Rig rig;
    EditSession session = rig.open();
    // Another process rates the photograph after the session read the file.
    writeSidecarMarks(rig.photo, PhotoMarks{.rating = 5, .label = ColorLabel::Red});

    session.setState(exposed(1.0F));
    session.save();

    const auto contents = readSidecar(rig.photo);
    REQUIRE(contents);
    CHECK(contents->marks == PhotoMarks{.rating = 5, .label = ColorLabel::Red});
    CHECK(contents->state == exposed(1.0F));
}

TEST_CASE("Marks the sidecar API is told to change are the only ones written",
          "[session][snapshots][save]") {
    const Rig rig;
    writeSidecarMarks(rig.photo, PhotoMarks{.rating = 4, .label = ColorLabel::Blue});
    const Photo photo = openPhoto(rig.photo);

    CHECK(changedMarks({}, {}).empty());
    CHECK(changedMarks({.rating = 1}, {.rating = 2}).rating == 2);
    CHECK_FALSE(changedMarks({.rating = 1}, {.rating = 2}).label);
    const MarkChanges cleared = changedMarks({.label = ColorLabel::Red}, {});
    REQUIRE(cleared.label);
    CHECK_FALSE(*cleared.label);

    // The photograph's own marks are ignored; only the named label changes.
    writeSidecar(photo.with(PhotoMarks{.rating = 1}), std::nullopt,
                 MarkChanges{.label = std::optional<ColorLabel>(ColorLabel::Green)});
    auto contents = readSidecar(rig.photo);
    REQUIRE(contents);
    CHECK(contents->marks == PhotoMarks{.rating = 4, .label = ColorLabel::Green});

    writeSidecarMarkChanges(rig.photo, MarkChanges{.rating = 2});
    contents = readSidecar(rig.photo);
    CHECK(contents->marks == PhotoMarks{.rating = 2, .label = ColorLabel::Green});
    CHECK_THROWS_AS(writeSidecarMarkChanges(rig.photo, MarkChanges{.rating = 9}),
                    std::invalid_argument);
    CHECK_THROWS_AS(writeSidecar(photo, std::nullopt, MarkChanges{.rating = 9}),
                    std::invalid_argument);
}

TEST_CASE("Discard returns to the saved recipe and base, and keeps the snapshots",
          "[session][snapshots][save]") {
    const Rig rig;
    EditSession session = rig.open();
    session.setState(exposed(1.0F));
    const SnapshotId a = session.createSnapshot("A");
    session.save();
    session.setState(exposed(2.0F));
    const SnapshotId b = session.createSnapshot("B");
    session.restoreSnapshot(a);
    session.begin();
    session.update(exposed(5.0F));
    REQUIRE(session.hasUnsavedChanges());

    session.discardChanges();

    CHECK_FALSE(session.editing());
    CHECK(session.photo() == session.saved());
    CHECK(session.baseSnapshot() == a);
    CHECK(session.photo().state() == exposed(1.0F));
    CHECK(session.history().size() == 1);
    CHECK(session.history()[0].baseSnapshot == a);
    CHECK_FALSE(session.hasUnsavedChanges());
    CHECK(session.snapshots().entries.size() == 2);
    CHECK(rig.disk().snapshots.find(b) != nullptr);
}

TEST_CASE("Rename writes at once and changes neither history nor rendering",
          "[session][snapshots][rename]") {
    const Rig rig;
    EditSession session = rig.open();
    const SnapshotId a = session.createSnapshot("A");
    session.setState(exposed(1.0F));
    const std::vector<HistoryStep> history = session.history();
    const Photo photo = session.photo();
    const bool dirty = session.hasUnsavedChanges();

    session.renameSnapshot(a, "Renamed");

    CHECK(session.snapshots().find(a)->name == "Renamed");
    CHECK(rig.disk().snapshots.find(a)->name == "Renamed");
    CHECK(session.history() == history);
    CHECK(session.photo() == photo);
    CHECK(session.hasUnsavedChanges() == dirty);
    CHECK_THROWS_AS(session.renameSnapshot(a, ""), std::invalid_argument);
    CHECK_THROWS_AS(session.renameSnapshot(SnapshotId{9}, "x"), std::invalid_argument);
    CHECK(session.snapshots().find(a)->name == "Renamed");
}

TEST_CASE("Delete reparents and remaps every reference", "[session][snapshots][delete]") {
    const Rig rig;
    EditSession session = rig.open();
    const SnapshotId a = session.createSnapshot("A");
    session.setState(exposed(1.0F));
    const SnapshotId b = session.createSnapshot("B");
    session.setState(exposed(2.0F));
    const SnapshotId c = session.createSnapshot("C");
    session.save();
    session.begin(); // An open edit remembers its base too.
    session.update(exposed(3.0F));
    REQUIRE(session.snapshots().find(c)->parent == b);
    REQUIRE(session.baseSnapshot() == c);
    REQUIRE(session.savedBaseSnapshot() == c);

    session.deleteSnapshot(c);
    CHECK(session.baseSnapshot() == b);
    CHECK(session.savedBaseSnapshot() == b);
    CHECK(rig.disk().savedBase == b);
    for (const HistoryStep& step : session.history()) {
        CHECK(step.baseSnapshot != c);
    }
    CHECK(session.photo().state() == exposed(3.0F));
    session.commit();
    CHECK(session.history().back().baseSnapshot == b);
    CHECK(session.snapshots().find(c) == nullptr);

    session.deleteSnapshot(a); // B becomes a root; nothing referenced A but history.
    CHECK(session.snapshots().find(b)->parent == std::nullopt);
    CHECK(session.history()[0].baseSnapshot == std::nullopt);
    session.deleteSnapshot(b);
    CHECK(session.baseSnapshot() == std::nullopt);
    CHECK(session.savedBaseSnapshot() == std::nullopt);
    CHECK(session.snapshots().entries.empty());
    CHECK(session.snapshots() == rig.disk().snapshots);
    CHECK(basesOf(session) ==
          std::vector<std::optional<SnapshotId>>(session.history().size(), std::nullopt));

    // Undo does not bring a deleted snapshot back.
    while (session.canUndo()) {
        session.undo();
    }
    CHECK(rig.disk().snapshots.entries.empty());
    CHECK(session.baseSnapshot() == std::nullopt);
    CHECK_THROWS_AS(session.deleteSnapshot(b), std::invalid_argument);
}

TEST_CASE("Deleting children's parent keeps their looks", "[session][snapshots][delete]") {
    const Rig rig;
    EditSession session = rig.open();
    const SnapshotId a = session.createSnapshot("A");
    session.setState(exposed(1.0F));
    const SnapshotId b = session.createSnapshot("B");
    session.setState(exposed(2.0F));
    const SnapshotId c = session.createSnapshot("C");

    session.deleteSnapshot(b);

    CHECK(session.snapshots().find(c)->parent == a);
    CHECK(session.snapshots().find(c)->state == exposed(2.0F));
    CHECK(session.snapshots() == rig.disk().snapshots);
}

TEST_CASE("A revision conflict refreshes the collection and says so", "[session][snapshots]") {
    const Rig rig;
    EditSession session = rig.open();
    const SnapshotId a = session.createSnapshot("A");
    session.setState(exposed(1.0F));
    const SnapshotId b = session.createSnapshot("B");
    // Another process adds one, and deletes B.
    const OpenDocument seen = rig.disk();
    addSnapshot(rig.photo, seen.snapshots.revision, [] {
        Snapshot snapshot;
        snapshot.name = "Theirs";
        snapshot.created = std::chrono::sys_seconds{std::chrono::seconds{1'790'000'000}};
        return snapshot;
    }());
    deleteSnapshot(rig.photo, seen.snapshots.revision + 1, b);
    REQUIRE(session.baseSnapshot() == b);

    SECTION("Create") {
        CHECK_THROWS_WITH(session.createSnapshot("C"), ContainsSubstring("snapshots changed"));
    }
    SECTION("Rename") {
        CHECK_THROWS_AS(session.renameSnapshot(a, "Renamed"), SnapshotRevisionConflict);
    }
    SECTION("Delete") {
        CHECK_THROWS_AS(session.deleteSnapshot(a), SnapshotRevisionConflict);
    }

    // Refreshed: the session knows the file's list, and its base is B's nearest survivor.
    CHECK(session.snapshots() == rig.disk().snapshots);
    CHECK(session.snapshots().find(b) == nullptr);
    CHECK(session.baseSnapshot() == a);
    CHECK(basesOf(session).back() == a);
    CHECK(session.snapshots().entries.size() == 2);
    // The operation goes through now.
    CHECK_NOTHROW(session.renameSnapshot(a, "Renamed"));
    CHECK(rig.disk().snapshots.find(a)->name == "Renamed");
}

TEST_CASE("A session made from a bare photo meets the sidecar's snapshots as a conflict",
          "[session][snapshots]") {
    const Rig rig;
    addSnapshot(rig.photo, 0, [] {
        Snapshot snapshot;
        snapshot.name = "Existing";
        snapshot.created = std::chrono::sys_seconds{std::chrono::seconds{1'790'000'000}};
        return snapshot;
    }());
    EditSession session(openPhoto(rig.photo));
    CHECK(session.snapshots().entries.empty());

    CHECK_THROWS_AS(session.createSnapshot("New"), SnapshotRevisionConflict);

    CHECK(session.snapshots().entries.size() == 1);
    CHECK_NOTHROW(session.createSnapshot("New"));
    CHECK(rig.disk().snapshots.entries.size() == 2);
}

TEST_CASE("The session refreshes on request", "[session][snapshots]") {
    const Rig rig;
    EditSession session = rig.open();
    const SnapshotId a = session.createSnapshot("A");
    renameSnapshot(rig.photo, rig.disk().snapshots.revision, a, "Elsewhere");

    session.refreshSnapshots();

    CHECK(session.snapshots().find(a)->name == "Elsewhere");
    CHECK(session.baseSnapshot() == a);
}

TEST_CASE("Restore bypasses lens rematching, and Create records the lens of a settled gesture",
          "[session][snapshots][restore][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const test::TempDir directory;
    const fs::path path = directory.file("frame.dng");
    fs::copy_file(test::fixture("lens-canon-32x24.dng"), path);
    NegativeSettings on = openPhoto(path).negative();
    on.lens.distortion = true; // On, with no lens recorded: a rematch would record one.
    // Another process wrote a snapshot of exactly that.
    Snapshot snapshot = theirs("No lens recorded");
    snapshot.negative = on;
    snapshot.state = exposed(1.0F);
    const SnapshotId a = addSnapshot(path, 0, snapshot).id;
    EditSession session(openDocument(path), &test::fixtureDatabase());

    session.restoreSnapshot(a);

    const HistoryStep& step = session.history().back();
    CHECK(step.origin == EditOrigin::Restore);
    CHECK(step.negative == on);
    CHECK_FALSE(step.negative.lens.lens);
    CHECK(session.photo().negative() == on);
    // A later edit that leaves the lens settings alone records none either.
    session.setState(exposed(2.0F));
    CHECK_FALSE(session.photo().negative().lens.lens);

    // Create settles a gesture as an edit does, so the lens is recorded in the snapshot.
    const fs::path other = directory.file("frame2.dng");
    fs::copy_file(test::fixture("lens-canon-32x24.dng"), other);
    EditSession gesture(openPhoto(other), &test::fixtureDatabase());
    gesture.begin();
    gesture.updateNegative(on);
    const SnapshotId b = gesture.createSnapshot("Gesture");
    CHECK(gesture.photo().negative().lens.lens);
    CHECK(gesture.snapshots().find(b)->negative.lens.lens);
}

TEST_CASE("Deleting the base under an open edit remaps its baseline",
          "[session][snapshots][delete]") {
    const Rig rig;
    EditSession session = rig.open();
    session.setState(exposed(1.0F));
    const SnapshotId a = session.createSnapshot("A");
    session.setState(exposed(2.0F));
    const SnapshotId c = session.createSnapshot("C");
    const std::size_t steps = session.history().size();
    session.begin();

    SECTION("cancel") {
        session.update(exposed(3.0F));
        session.deleteSnapshot(c);
        session.cancel();
        CHECK(session.baseSnapshot() == a);
        CHECK(session.photo().state() == exposed(2.0F));
    }
    SECTION("commit with nothing changed adds no step") {
        session.deleteSnapshot(c);
        session.commit();
        CHECK(session.history().size() == steps);
        CHECK(session.baseSnapshot() == a);
    }
    SECTION("another process deletes it and the session refreshes") {
        session.update(exposed(3.0F));
        deleteSnapshot(rig.photo, rig.disk().snapshots.revision, c);
        session.refreshSnapshots();
        session.cancel();
        CHECK(session.baseSnapshot() == a);
        CHECK(session.photo().state() == exposed(2.0F));
    }
}

TEST_CASE("Rename and Delete that fail change nothing", "[session][snapshots][rename][delete]") {
    const Rig rig;
    EditSession session = rig.open();
    session.setState(exposed(1.0F));
    const SnapshotId a = session.createSnapshot("A");
    session.setState(exposed(2.0F));
    const SnapshotId b = session.createSnapshot("B");
    session.save();
    session.begin();
    session.update(exposed(3.0F));
    const SnapshotCollection collection = session.snapshots();
    const auto history = session.history();
    const OpenDocument disk = rig.disk();

    {
        const ShortLockTimeout shortTimeout;
        const HeldLock held(rig.photo);
        CHECK_THROWS_AS(session.renameSnapshot(a, "Renamed"), SidecarLockTimeout);
        CHECK_THROWS_AS(session.deleteSnapshot(b), SidecarLockTimeout);
    }

    CHECK(session.snapshots() == collection);
    CHECK(session.history() == history);
    CHECK(session.baseSnapshot() == b);
    CHECK(session.savedBaseSnapshot() == b);
    CHECK(session.editing());
    CHECK(rig.disk().snapshots == disk.snapshots);
    CHECK(rig.disk().savedBase == b);
}

TEST_CASE("A conflict keeps an open edit and remaps its baseline", "[session][snapshots]") {
    const Rig rig;
    EditSession session = rig.open();
    session.setState(exposed(1.0F));
    const SnapshotId a = session.createSnapshot("A");
    session.setState(exposed(2.0F));
    const SnapshotId b = session.createSnapshot("B");
    session.begin();
    session.update(exposed(3.0F));
    const std::uint64_t revision = rig.disk().snapshots.revision;
    deleteSnapshot(rig.photo, revision, b);

    CHECK_THROWS_AS(session.createSnapshot("C"), SnapshotRevisionConflict);

    CHECK(session.editing());
    CHECK(session.photo().state() == exposed(3.0F));
    CHECK(session.baseSnapshot() == a);
    session.cancel();
    CHECK(session.baseSnapshot() == a);
    CHECK(session.photo().state() == exposed(2.0F));
}

TEST_CASE("Refresh remaps through several deleted ancestors, or to no base",
          "[session][snapshots][refresh]") {
    const Rig rig;
    EditSession session = rig.open();
    const SnapshotId a = session.createSnapshot("A");
    session.setState(exposed(1.0F));
    const SnapshotId b = session.createSnapshot("B");
    session.setState(exposed(2.0F));
    const SnapshotId c = session.createSnapshot("C");
    session.save();
    session.setState(exposed(3.0F));

    SECTION("two ancestors") {
        deleteSnapshot(rig.photo, rig.disk().snapshots.revision, c);
        deleteSnapshot(rig.photo, rig.disk().snapshots.revision, b);
        session.refreshSnapshots();
        CHECK(session.baseSnapshot() == a);
        CHECK(session.savedBaseSnapshot() == a);
        // Not-last steps are remapped too.
        for (const HistoryStep& step : session.history()) {
            CHECK((step.baseSnapshot == a || step.baseSnapshot == std::nullopt));
        }
    }
    SECTION("no survivor") {
        deleteSnapshot(rig.photo, rig.disk().snapshots.revision, c);
        deleteSnapshot(rig.photo, rig.disk().snapshots.revision, b);
        deleteSnapshot(rig.photo, rig.disk().snapshots.revision, a);
        session.refreshSnapshots();
        CHECK(session.baseSnapshot() == std::nullopt);
        CHECK(session.savedBaseSnapshot() == std::nullopt);
        CHECK(basesOf(session) ==
              std::vector<std::optional<SnapshotId>>(session.history().size(), std::nullopt));
    }
    SECTION("the sidecar is gone") {
        fs::remove(sidecarPath(rig.photo));
        session.refreshSnapshots();
        CHECK(session.snapshots().entries.empty());
        CHECK(session.baseSnapshot() == std::nullopt);
        CHECK(session.savedBaseSnapshot() == std::nullopt);
    }
    SECTION("an unreadable sidecar changes nothing") {
        std::ofstream(sidecarPath(rig.photo), std::ios::binary) << "not xml at all";
        const SnapshotCollection collection = session.snapshots();
        const auto history = session.history();
        CHECK_THROWS_AS(session.refreshSnapshots(), std::runtime_error);
        CHECK(session.snapshots() == collection);
        CHECK(session.history() == history);
        CHECK(session.baseSnapshot() == c);
    }
    CHECK(session.photo().state() == exposed(3.0F));
}

TEST_CASE("Save keeps another process's snapshots, and the session list stays stale",
          "[session][snapshots][save]") {
    const Rig rig;
    EditSession session = rig.open();
    const SnapshotId a = session.createSnapshot("A");
    addSnapshot(rig.photo, rig.disk().snapshots.revision, theirs("Theirs"));
    session.setState(exposed(1.0F));

    session.save();

    CHECK(rig.disk().snapshots.entries.size() == 2);
    CHECK(rig.disk().savedBase == a);
    CHECK(session.snapshots().entries.size() == 1);
}

TEST_CASE("A failed Save retry leaves the refresh standing", "[session][snapshots][save]") {
    const Rig rig;
    EditSession session = rig.open();
    const SnapshotId a = session.createSnapshot("A");
    session.setState(exposed(1.0F));
    deleteSnapshot(rig.photo, rig.disk().snapshots.revision, a);
    {
        const ShortLockTimeout shortTimeout;
        const HeldLock held(rig.photo);
        // The first write times out before it can notice the missing base: nothing changes.
        CHECK_THROWS_AS(session.save(), SidecarLockTimeout);
    }
    CHECK(session.baseSnapshot() == a);
    CHECK(session.snapshots().find(a) != nullptr);
    CHECK(session.hasUnsavedChanges());
    CHECK_NOTHROW(session.save());
    CHECK(session.baseSnapshot() == std::nullopt);
}

TEST_CASE("Marks are written alone, and a stale session puts nothing back",
          "[session][snapshots][save]") {
    const Rig rig;
    EditSession session = rig.open();
    const SnapshotId a = session.createSnapshot("A");
    session.setState(exposed(1.0F));
    session.save();
    // Another process rates the photograph and saves another recipe.
    writeSidecar(session.photo().with(exposed(5.0F)).with(PhotoMarks{.rating = 5}), a);
    session.setState(exposed(2.0F));

    session.setMarks({.label = ColorLabel::Red});

    const OpenDocument disk = rig.disk();
    CHECK(disk.photo.marks() == PhotoMarks{.rating = 5, .label = ColorLabel::Red});
    CHECK(disk.photo.state() == exposed(5.0F));
    CHECK(disk.savedBase == a);
}

TEST_CASE("Mark changes at the edges", "[session][snapshots][save]") {
    const Rig rig;
    const auto marks = [&] { return readSidecar(rig.photo)->marks; };

    SECTION("nothing named writes nothing and makes no sidecar") {
        writeSidecarMarkChanges(rig.photo, {});
        CHECK_FALSE(fs::exists(sidecarPath(rig.photo)));
    }
    SECTION("a rating cleared to zero, rejected, and both at once") {
        writeSidecarMarks(rig.photo, {.rating = 4, .label = ColorLabel::Blue});
        writeSidecarMarkChanges(rig.photo, {.rating = 0});
        CHECK(marks() == PhotoMarks{.rating = 0, .label = ColorLabel::Blue});
        writeSidecarMarkChanges(rig.photo, {.rating = -1});
        CHECK(marks().rating == -1);
        writeSidecarMarkChanges(rig.photo,
                                {.rating = 2, .label = std::optional<std::optional<ColorLabel>>()});
        CHECK(marks() == PhotoMarks{.rating = 2, .label = ColorLabel::Blue});
        CHECK_THROWS_AS(writeSidecarMarkChanges(rig.photo, {.rating = 6}), std::invalid_argument);
        CHECK(marks().rating == 2);
    }
}

TEST_CASE("A session made from a bare photo leaves the saved base as the file has it",
          "[session][snapshots][save]") {
    const Rig rig;
    SnapshotId a;
    {
        EditSession first = rig.open();
        a = first.createSnapshot("A");
        first.save();
    }
    REQUIRE(rig.disk().savedBase == a);

    EditSession bare(openPhoto(rig.photo));
    bare.setState(exposed(1.0F));
    bare.save();

    CHECK(rig.disk().savedBase == a);
    CHECK(rig.disk().photo.state() == exposed(1.0F));
    CHECK(rig.disk().snapshots.entries.size() == 1);
}
