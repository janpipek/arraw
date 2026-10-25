#include "SettingCodec.h"
#include "SidecarLimits.h"
#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <DevelopState.h>
#include <Diagnostics.h>
#include <NegativeSettings.h>
#include <Photo.h>
#include <SettingsJson.h>
#include <Sidecar.h>
#include <Snapshots.h>

#include <QBuffer>
#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QList>
#include <QLockFile>
#include <QString>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace arraw;
using Catch::Matchers::ContainsSubstring;

/// Snapshots in the sidecar (ADR 056): the value types, sidecar version 2, targeted writes and
/// the tolerance of readers.

namespace {

namespace fs = std::filesystem;

constexpr std::string_view rawFixture = "linear-32x24-neutral.dng";

std::string slurp(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

void spit(const fs::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

bool contains(const std::string& text, std::string_view piece) {
    return text.find(piece) != std::string::npos;
}

std::size_t count(const CollectedDiagnostics& log, Notice notice) {
    std::size_t total = 0;
    for (const Diagnostic& entry : log.entries()) {
        total += entry.notice == notice ? 1 : 0;
    }
    return total;
}

/// A RAW fixture beside which a sidecar can sit.
struct Bench {
    test::TempDir directory;
    fs::path photo = directory.file("IMG_1.dng");
    fs::path sidecar = directory.file("IMG_1.xmp");

    Bench() {
        fs::copy_file(test::fixture(rawFixture), photo);
    }

    void start(const fs::path& from) const {
        fs::copy_file(from, sidecar);
    }

    [[nodiscard]] SidecarContents read(DiagnosticLog& log = discardedDiagnostics()) const {
        const auto contents = readSidecar(photo, log);
        REQUIRE(contents);
        return *contents;
    }
};

/// A JPEG of a flat colour.
SnapshotThumbnail thumbnailOf(int width, int height, std::string key = "key-1") {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    REQUIRE(image.save(&buffer, "JPEG", 80));
    SnapshotThumbnail thumbnail;
    thumbnail.jpeg.resize(static_cast<std::size_t>(bytes.size()));
    std::memcpy(thumbnail.jpeg.data(), bytes.constData(), thumbnail.jpeg.size());
    thumbnail.renderKey = std::move(key);
    return thumbnail;
}

/// A snapshot to add: an exposure of its own, so recipes differ.
Snapshot draft(std::string name, float exposure = 0.5F, std::optional<SnapshotId> parent = {}) {
    Snapshot snapshot;
    snapshot.parent = parent;
    snapshot.name = std::move(name);
    snapshot.created = std::chrono::sys_seconds{std::chrono::seconds{1'790'000'000}};
    snapshot.state.settings.tone.exposure = exposure;
    snapshot.negative.demosaic = Demosaic::Vng;
    return snapshot;
}

/// A sidecar of one description: attributes, then child elements.
std::string sidecarOf(const std::string& attributes, const std::string& body = {}) {
    return R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about="" xmlns:arraw="http://ns.arraw.org/develop/"
    xmlns:xmp="http://ns.adobe.com/xap/1.0/" )" +
           attributes + ">" + body + "</rdf:Description></rdf:RDF></x:xmpmeta>";
}

/// One list item with the given field elements.
std::string item(const std::string& fields) {
    return "<rdf:li rdf:parseType=\"Resource\">" + fields + "</rdf:li>";
}

std::string field(std::string_view key, const std::string& value) {
    return "<arraw:" + std::string(key) + ">" + value + "</arraw:" + std::string(key) + ">";
}

std::string escaped(const std::string& text) {
    return QString::fromStdString(text).toHtmlEscaped().toStdString();
}

/// A usable node, by hand.
std::string node(std::uint64_t id, std::optional<std::uint64_t> parent, const std::string& name,
                 const std::string& stateJson = {}) {
    DevelopState state;
    state.settings.tone.exposure = 0.25F;
    return item(field("id", std::to_string(id)) +
                (parent ? field("parent", std::to_string(*parent)) : "") + field("name", name) +
                field("created", "2026-10-23T12:00:00Z") +
                field("state", escaped(stateJson.empty() ? stateToJson(state) : stateJson)) +
                field("negative", escaped(negativeToJson(NegativeSettings{}))));
}

std::string listOf(const std::string& items) {
    return "<arraw:snapshots><rdf:Seq>" + items + "</rdf:Seq></arraw:snapshots>";
}

} // namespace

TEST_CASE("A snapshot is checked on its own", "[snapshots]") {
    Snapshot good = draft("Warm");
    good.id = SnapshotId{3};
    CHECK_NOTHROW(validate(good));

    SECTION("an id is not zero and a parent is not itself") {
        good.id = SnapshotId{};
        CHECK_THROWS_AS(validate(good), std::invalid_argument);
        good.id = SnapshotId{3};
        good.parent = SnapshotId{3};
        CHECK_THROWS_AS(validate(good), std::invalid_argument);
    }
    SECTION("a name has 1 to 256 bytes of storable text") {
        good.name = "";
        CHECK_THROWS_AS(validate(good), std::invalid_argument);
        good.name = std::string(256, 'a');
        CHECK_NOTHROW(validate(good));
        good.name = std::string(257, 'a');
        CHECK_THROWS_AS(validate(good), std::invalid_argument);
        good.name = std::string(127, 'e') + "\xC3\xA9" + std::string(127, 'e'); // 256 bytes.
        CHECK_NOTHROW(validate(good));
        good.name += "\xC3\xA9";
        CHECK_THROWS_AS(validate(good), std::invalid_argument);
        good.name = "line\nbreak";
        CHECK_THROWS_AS(validate(good), std::invalid_argument);
        good.name = "bad \xFF utf8";
        CHECK_THROWS_AS(validate(good), std::invalid_argument);
    }
    SECTION("a time lies between 1970 and the year 9999") {
        good.created = std::chrono::sys_seconds{std::chrono::seconds{-1}};
        CHECK_THROWS_AS(validate(good), std::invalid_argument);
        good.created = std::chrono::sys_seconds{std::chrono::seconds{253402300800}};
        CHECK_THROWS_AS(validate(good), std::invalid_argument);
    }
    SECTION("the recipe is validated") {
        good.state.settings.tone.exposure = 99.0F;
        CHECK_THROWS_AS(validate(good), std::invalid_argument);
    }
}

TEST_CASE("A thumbnail is a small JPEG with a key", "[snapshots][thumbnail]") {
    CHECK_NOTHROW(validate(thumbnailOf(256, 171)));
    CHECK_NOTHROW(validate(thumbnailOf(1, 1)));
    CHECK_THROWS_AS(validate(thumbnailOf(257, 10)), std::invalid_argument);
    CHECK_THROWS_AS(validate(thumbnailOf(10, 257)), std::invalid_argument);

    SnapshotThumbnail keyless = thumbnailOf(8, 8);
    keyless.renderKey.clear();
    CHECK_THROWS_AS(validate(keyless), std::invalid_argument);

    SnapshotThumbnail empty = thumbnailOf(8, 8);
    empty.jpeg.clear();
    CHECK_THROWS_AS(validate(empty), std::invalid_argument);

    SnapshotThumbnail notJpeg = thumbnailOf(8, 8);
    notJpeg.jpeg.assign(100, std::byte{7});
    CHECK_THROWS_AS(validate(notJpeg), std::invalid_argument);

    SnapshotThumbnail png;
    QImage image(8, 8, QImage::Format_RGB32);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    REQUIRE(image.save(&buffer, "PNG"));
    png.jpeg.resize(static_cast<std::size_t>(bytes.size()));
    std::memcpy(png.jpeg.data(), bytes.constData(), png.jpeg.size());
    png.renderKey = "k";
    CHECK_THROWS_AS(validate(png), std::invalid_argument);

    SnapshotThumbnail heavy = thumbnailOf(8, 8);
    heavy.jpeg.resize(maximumSnapshotThumbnailBytes + 1);
    CHECK_THROWS_AS(validate(heavy), std::invalid_argument);
}

TEST_CASE("A thumbnail's limits are accepted at the edge and refused beyond it",
          "[snapshots][thumbnail]") {
    CHECK_NOTHROW(validate(thumbnailOf(256, 256)));

    SECTION("exactly the byte limit passes the size check") {
        // Padding after the image data is ignored by the decoder, so the header still reads.
        SnapshotThumbnail padded = thumbnailOf(8, 8);
        REQUIRE(padded.jpeg.size() < maximumSnapshotThumbnailBytes);
        padded.jpeg.resize(maximumSnapshotThumbnailBytes, std::byte{0});
        CHECK_NOTHROW(validate(padded));
        padded.jpeg.push_back(std::byte{0});
        CHECK_THROWS_AS(validate(padded), std::invalid_argument);
    }
    SECTION("a small file declaring huge dimensions is refused by its header") {
        SnapshotThumbnail bomb = thumbnailOf(8, 8);
        // The start-of-frame marker is FF C0 (baseline); then length, precision, height, width.
        bool patched = false;
        for (std::size_t i = 0; i + 9 < bomb.jpeg.size(); ++i) {
            if (bomb.jpeg[i] == std::byte{0xFF} && bomb.jpeg[i + 1] == std::byte{0xC0}) {
                bomb.jpeg[i + 5] = std::byte{0x1F}; // height 8000
                bomb.jpeg[i + 6] = std::byte{0x40};
                bomb.jpeg[i + 7] = std::byte{0x1F}; // width 8000
                bomb.jpeg[i + 8] = std::byte{0x40};
                patched = true;
                break;
            }
        }
        REQUIRE(patched);
        CHECK_THROWS_AS(validate(bomb), std::invalid_argument);
    }
    SECTION("a render key is 1 to 256 bytes of storable text") {
        SnapshotThumbnail thumbnail = thumbnailOf(8, 8, std::string(256, 'k'));
        CHECK_NOTHROW(validate(thumbnail));
        thumbnail.renderKey = std::string(257, 'k');
        CHECK_THROWS_AS(validate(thumbnail), std::invalid_argument);
        for (const std::string& key : {std::string("a\x01"
                                                   "b"),
                                       std::string("a\rb"), std::string("a\nb"),
                                       std::string("a\xFF"
                                                   "b")}) {
            thumbnail.renderKey = key;
            INFO(key.size());
            CHECK_THROWS_AS(validate(thumbnail), std::invalid_argument);
        }
        thumbnail.renderKey = "caf\xC3\xA9 \xE2\x98\x80";
        CHECK_NOTHROW(validate(thumbnail));
    }
}

TEST_CASE("A thumbnail with a bad render key never reaches the sidecar", "[snapshots][thumbnail]") {
    const Bench bench;
    Snapshot snapshot = draft("Look");
    snapshot.thumbnail = thumbnailOf(8, 8, "a\x01 key");
    CHECK_THROWS_AS(addSnapshot(bench.photo, 0, snapshot), std::invalid_argument);
    CHECK_FALSE(fs::exists(bench.sidecar));

    const SnapshotId id = addSnapshot(bench.photo, 0, draft("Look")).id;
    const std::string before = slurp(bench.sidecar);
    CHECK_THROWS_AS(setSnapshotThumbnail(bench.photo, id, thumbnailOf(8, 8, "a\rb"), "a\rb"),
                    std::invalid_argument);
    CHECK(slurp(bench.sidecar) == before);
    CHECK_NOTHROW(bench.read());
}

TEST_CASE("The first and last spellable seconds round-trip through the sidecar",
          "[snapshots][crud]") {
    const Bench bench;
    std::uint64_t revision = 0;
    for (const std::int64_t seconds : {std::int64_t{0}, std::int64_t{253402300799}}) {
        Snapshot edge = draft("Edge " + std::to_string(seconds));
        edge.created = std::chrono::sys_seconds{std::chrono::seconds{seconds}};
        CHECK_NOTHROW(validate([&] {
            Snapshot valid = edge;
            valid.id = SnapshotId{1};
            return valid;
        }()));
        const SnapshotId id = addSnapshot(bench.photo, revision++, edge).id;
        const SnapshotCollection collection = openDocument(bench.photo).snapshots;
        REQUIRE(collection.find(id) != nullptr);
        CHECK(collection.find(id)->created.time_since_epoch().count() == seconds);
    }
}

TEST_CASE("Names keep their whitespace through the sidecar", "[snapshots][crud]") {
    const Bench bench;
    std::uint64_t revision = 0;
    for (const std::string& name :
         {std::string(" leading"), std::string("trailing "), std::string("   "),
          std::string("two  spaces"), std::string("\xE2\x80\xA8 separator")}) {
        INFO("name of " << name.size() << " bytes");
        const SnapshotId id = addSnapshot(bench.photo, revision++, draft(name)).id;
        const SnapshotCollection collection = openDocument(bench.photo).snapshots;
        REQUIRE(collection.find(id) != nullptr);
        CHECK(collection.find(id)->name == name);
    }
}

TEST_CASE("A collection is checked as a graph", "[snapshots]") {
    const auto entry = [](std::uint64_t id, std::optional<std::uint64_t> parent) {
        Snapshot snapshot = draft("S" + std::to_string(id));
        snapshot.id = SnapshotId{id};
        if (parent) {
            snapshot.parent = SnapshotId{*parent};
        }
        return snapshot;
    };
    SnapshotCollection collection;
    collection.entries = {entry(1, {}), entry(2, 1), entry(5, 2)};
    collection.nextId = 6;
    CHECK_NOTHROW(validate(collection));
    CHECK(collection.find(SnapshotId{2}) == &collection.entries[1]);
    CHECK(collection.find(SnapshotId{3}) == nullptr);

    SECTION("an id is not repeated") {
        collection.entries.push_back(entry(2, {}));
        CHECK_THROWS_AS(validate(collection), std::invalid_argument);
    }
    SECTION("the counter is above every id") {
        collection.nextId = 5;
        CHECK_THROWS_AS(validate(collection), std::invalid_argument);
    }
    SECTION("a parent exists") {
        collection.entries[1].parent = SnapshotId{9};
        CHECK_THROWS_AS(validate(collection), std::invalid_argument);
    }
    SECTION("there is no cycle") {
        collection.entries[0].parent = SnapshotId{5};
        CHECK_THROWS_AS(validate(collection), std::invalid_argument);
    }
    SECTION("there are at most 32 snapshots") {
        collection.entries.clear();
        for (std::uint64_t id = 1; id <= 33; ++id) {
            collection.entries.push_back(entry(id, {}));
        }
        collection.nextId = 40;
        CHECK_THROWS_AS(validate(collection), std::invalid_argument);
        collection.entries.pop_back();
        CHECK_NOTHROW(validate(collection));
    }
}

TEST_CASE("A photograph opens as a document without a sidecar", "[snapshots][document]") {
    const Bench bench;
    const OpenDocument document = openDocument(bench.photo);
    CHECK(document.snapshots.entries.empty());
    CHECK(document.snapshots.nextId == 1);
    CHECK(document.snapshots.revision == 0);
    CHECK_FALSE(document.savedBase);
    CHECK(document.photo == openPhoto(bench.photo));
}

TEST_CASE("Adding, renaming and deleting snapshots round-trips through the sidecar",
          "[snapshots][crud]") {
    const Bench bench;
    SnapshotUpdate update = addSnapshot(bench.photo, 0, draft("First", 0.5F));
    const SnapshotId first = update.id;
    CHECK(first.value == 1);
    CHECK(update.collection.revision == 1);
    CHECK(update.collection.nextId == 2);

    update = addSnapshot(bench.photo, 1, draft("Second", 1.0F, first));
    const SnapshotId second = update.id;
    update = addSnapshot(bench.photo, 2, draft("Third", 1.5F, second));
    const SnapshotId third = update.id;
    CHECK(std::vector{first.value, second.value, third.value} ==
          std::vector<std::uint64_t>{1, 2, 3});

    const OpenDocument document = openDocument(bench.photo);
    REQUIRE(document.snapshots.entries.size() == 3);
    CHECK(document.snapshots == update.collection);
    CHECK_NOTHROW(validate(document.snapshots));
    const Snapshot& read = document.snapshots.entries[1];
    CHECK(read.name == "Second");
    CHECK(read.parent == first);
    CHECK(read.created == draft("").created);
    CHECK(read.state.settings.tone.exposure == 1.0F);
    CHECK(read.negative.demosaic == Demosaic::Vng);
    CHECK_FALSE(read.thumbnail);

    update = renameSnapshot(bench.photo, 3, second, "Middle \xC3\xA9 & <b>");
    CHECK(update.collection.revision == 4);
    CHECK(openDocument(bench.photo).snapshots.find(second)->name == "Middle \xC3\xA9 & <b>");

    // Deleting reparents children; the id is never reused.
    update = deleteSnapshot(bench.photo, 4, second);
    CHECK(update.collection.revision == 5);
    REQUIRE(update.collection.entries.size() == 2);
    CHECK(update.collection.find(third)->parent == first);
    update = deleteSnapshot(bench.photo, 5, first);
    CHECK_FALSE(update.collection.find(third)->parent);
    update = addSnapshot(bench.photo, 6, draft("Fourth"));
    CHECK(update.id.value == 4);
    CHECK(update.collection.nextId == 5);
}

TEST_CASE("Snapshot edits refuse what is invalid, and leave the file as it was",
          "[snapshots][crud]") {
    const Bench bench;
    const SnapshotId first = addSnapshot(bench.photo, 0, draft("First")).id;
    const std::string before = slurp(bench.sidecar);

    CHECK_THROWS_AS(addSnapshot(bench.photo, 1, draft("")), std::invalid_argument);
    CHECK_THROWS_AS(addSnapshot(bench.photo, 1, draft(std::string(257, 'x'))),
                    std::invalid_argument);
    CHECK_THROWS_AS(addSnapshot(bench.photo, 1, draft("Orphan", 0.0F, SnapshotId{7})),
                    std::invalid_argument);
    Snapshot bad = draft("Bad");
    bad.thumbnail = thumbnailOf(300, 10);
    CHECK_THROWS_AS(addSnapshot(bench.photo, 1, bad), std::invalid_argument);
    CHECK_THROWS_AS(renameSnapshot(bench.photo, 1, SnapshotId{9}, "x"), std::invalid_argument);
    CHECK_THROWS_AS(renameSnapshot(bench.photo, 1, first, ""), std::invalid_argument);
    CHECK_THROWS_AS(deleteSnapshot(bench.photo, 1, SnapshotId{9}), std::invalid_argument);
    CHECK(slurp(bench.sidecar) == before);

    SECTION("the list holds 32") {
        std::uint64_t revision = 1;
        for (int i = 1; i < 32; ++i) {
            addSnapshot(bench.photo, revision++, draft("S" + std::to_string(i)));
        }
        const std::string full = slurp(bench.sidecar);
        CHECK_THROWS_AS(addSnapshot(bench.photo, revision, draft("One too many")),
                        std::invalid_argument);
        CHECK(slurp(bench.sidecar) == full);
        CHECK(openDocument(bench.photo).snapshots.entries.size() == 32);
    }
}

TEST_CASE("A stale revision is refused under the lock", "[snapshots][revision]") {
    const Bench bench;
    const SnapshotId first = addSnapshot(bench.photo, 0, draft("First")).id;
    const std::string before = slurp(bench.sidecar);

    try {
        addSnapshot(bench.photo, 0, draft("Late"));
        FAIL("a stale add was accepted");
    } catch (const SnapshotRevisionConflict& conflict) {
        CHECK(conflict.expected == 0);
        CHECK(conflict.actual == 1);
    }
    CHECK_THROWS_AS(renameSnapshot(bench.photo, 0, first, "Late"), SnapshotRevisionConflict);
    CHECK_THROWS_AS(deleteSnapshot(bench.photo, 2, first), SnapshotRevisionConflict);
    CHECK(slurp(bench.sidecar) == before);

    // Another process's rename is not lost by a stale session.
    renameSnapshot(bench.photo, 1, first, "Renamed elsewhere");
    CHECK_THROWS_AS(deleteSnapshot(bench.photo, 1, first), SnapshotRevisionConflict);
    CHECK(openDocument(bench.photo).snapshots.find(first)->name == "Renamed elsewhere");
}

TEST_CASE("A writer rereads the sidecar after it has the lock", "[snapshots][lock]") {
    const Bench bench;
    const SnapshotId first = addSnapshot(bench.photo, 0, draft("First")).id;
    const auto meanwhile = [&] {
        // What another process commits while this one waits: a second snapshot, revision 2.
        spit(bench.sidecar, sidecarOf("arraw:version=\"2\" arraw:activeStatePresent=\"False\" "
                                      "arraw:snapshotRevision=\"2\" arraw:snapshotNextId=\"3\"",
                                      listOf(node(1, {}, "First") + node(2, {}, "Meanwhile"))));
    };
    QLockFile held(QString::fromStdString(bench.sidecar.string() + ".lock"));
    REQUIRE(held.tryLock(1000));
    std::exception_ptr failure;
    std::jthread writer;
    const auto wait = [] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); };

    SECTION("a structural edit built on the old revision is refused") {
        writer = std::jthread([&] {
            try {
                renameSnapshot(bench.photo, 1, first, "Stale rename");
            } catch (...) {
                failure = std::current_exception();
            }
        });
        wait();
        meanwhile();
        held.unlock();
        writer.join();
        REQUIRE(failure);
        try {
            std::rethrow_exception(failure);
        } catch (const SnapshotRevisionConflict& conflict) {
            CHECK(conflict.expected == 1);
            CHECK(conflict.actual == 2);
        }
        const SnapshotCollection collection = openDocument(bench.photo).snapshots;
        CHECK(collection.entries.size() == 2);
        CHECK(collection.find(first)->name == "First");
    }
    SECTION("Save keeps a snapshot that was added while it waited") {
        const Photo edited =
            openPhoto(bench.photo).with(DevelopState{.settings = {.tone = {.exposure = 2.0F}}});
        writer = std::jthread([&] {
            try {
                writeSidecar(edited);
            } catch (...) {
                failure = std::current_exception();
            }
        });
        wait();
        meanwhile();
        held.unlock();
        writer.join();
        CHECK_FALSE(failure);
        const OpenDocument document = openDocument(bench.photo);
        CHECK(document.photo.state().settings.tone.exposure == 2.0F);
        REQUIRE(document.snapshots.entries.size() == 2);
        CHECK(document.snapshots.entries[1].name == "Meanwhile");
        CHECK(document.snapshots.revision == 2);
    }
    SECTION("a marks write keeps it too") {
        writer = std::jthread([&] {
            try {
                writeSidecarMarks(bench.photo, {.rating = 4});
            } catch (...) {
                failure = std::current_exception();
            }
        });
        wait();
        meanwhile();
        held.unlock();
        writer.join();
        CHECK_FALSE(failure);
        CHECK(openDocument(bench.photo).snapshots.entries.size() == 2);
        CHECK(bench.read().marks.rating == 4);
    }
}

TEST_CASE("A thumbnail is attached by id and key, and is not structural",
          "[snapshots][thumbnail]") {
    const Bench bench;
    const SnapshotId first = addSnapshot(bench.photo, 0, draft("First")).id;
    const SnapshotId second = addSnapshot(bench.photo, 1, draft("Second")).id;

    REQUIRE(
        setSnapshotThumbnail(bench.photo, first, thumbnailOf(32, 24, "key-First"), "key-First"));
    OpenDocument document = openDocument(bench.photo);
    CHECK(document.snapshots.revision == 2);
    REQUIRE(document.snapshots.find(first)->thumbnail);
    CHECK(document.snapshots.find(first)->thumbnail->renderKey == "key-First");
    CHECK_FALSE(document.snapshots.find(second)->thumbnail);

    // It is replaceable.
    REQUIRE(
        setSnapshotThumbnail(bench.photo, first, thumbnailOf(16, 12, "key-First"), "key-First"));
    CHECK(openDocument(bench.photo).snapshots.find(first)->thumbnail->jpeg.size() !=
          document.snapshots.find(first)->thumbnail->jpeg.size());

    // A key that is no longer the node's, or an id that is gone, writes nothing.
    const std::string before = slurp(bench.sidecar);
    CHECK_FALSE(
        setSnapshotThumbnail(bench.photo, second, thumbnailOf(8, 8, "key-Old"), "key-Second"));
    CHECK_FALSE(
        setSnapshotThumbnail(bench.photo, SnapshotId{9}, thumbnailOf(8, 8, "key-Z"), "key-Z"));
    CHECK(slurp(bench.sidecar) == before);
    CHECK_THROWS_AS(
        setSnapshotThumbnail(bench.photo, second, thumbnailOf(300, 8, "key-Second"), "key-Second"),
        std::invalid_argument);

    // The caller's expected key is compared with the key the thumbnail carries: one made for
    // another key is dropped. The file does not recompute keys; that rests on recipes being
    // immutable and IDs unique. A rename in between is kept ...
    renameSnapshot(bench.photo, 2, first, "Renamed");
    const std::string renamed = slurp(bench.sidecar);
    CHECK_FALSE(
        setSnapshotThumbnail(bench.photo, first, thumbnailOf(8, 8, "key-First"), "key-Renamed"));
    CHECK(slurp(bench.sidecar) == renamed);
    CHECK(openDocument(bench.photo).snapshots.find(first)->name == "Renamed");
    // ... while a sibling's rename does not disturb a patch of this node, nor the revision.
    renameSnapshot(bench.photo, 3, second, "Sibling");
    REQUIRE(
        setSnapshotThumbnail(bench.photo, first, thumbnailOf(8, 8, "key-Renamed"), "key-Renamed"));
    document = openDocument(bench.photo);
    CHECK(document.snapshots.find(second)->name == "Sibling");
    CHECK(document.snapshots.find(first)->name == "Renamed");
    CHECK(document.snapshots.revision == 4);

    // A deleted node's late thumbnail does not come back.
    deleteSnapshot(bench.photo, 4, second);
    CHECK_FALSE(
        setSnapshotThumbnail(bench.photo, second, thumbnailOf(8, 8, "key-Sibling"), "key-Sibling"));
    document = openDocument(bench.photo);
    CHECK(document.snapshots.entries.size() == 1);
    CHECK(document.snapshots.revision == 5);
}

TEST_CASE("Snapshot writes leave the saved settings alone, and Save keeps the snapshots",
          "[snapshots][save]") {
    const Bench bench;
    const Photo opened = openPhoto(bench.photo);
    const Photo edited = opened.with(DevelopState{.settings = {.tone = {.exposure = 2.0F}}});
    writeSidecar(edited);
    const SnapshotId first = addSnapshot(bench.photo, 0, draft("First")).id;

    // Snapshot writes leave the saved active recipe alone.
    addSnapshot(bench.photo, 1, draft("Second"));
    CHECK(openPhoto(bench.photo).state().settings.tone.exposure == 2.0F);

    // Save writes its recipe and leaves the list alone.
    writeSidecar(edited.with(DevelopState{.settings = {.tone = {.exposure = 3.0F}}}));
    OpenDocument document = openDocument(bench.photo);
    CHECK(document.photo.state().settings.tone.exposure == 3.0F);
    CHECK(document.snapshots.entries.size() == 2);
    CHECK(document.snapshots.revision == 2);
    CHECK_FALSE(document.savedBase);

    // Save with a base stores it; it must exist; clearing it is a Save too.
    writeSidecar(edited, first);
    CHECK(openDocument(bench.photo).savedBase == first);
    const std::string before = slurp(bench.sidecar);
    CHECK_THROWS_AS(writeSidecar(edited, SnapshotId{42}), std::invalid_argument);
    CHECK(slurp(bench.sidecar) == before);
    writeSidecar(edited);
    CHECK(openDocument(bench.photo).savedBase == first);
    writeSidecar(edited, std::nullopt);
    CHECK_FALSE(openDocument(bench.photo).savedBase);

    // Deleting the saved base moves it to its parent, or clears it for a root; settings stay.
    const SnapshotId child = addSnapshot(bench.photo, 2, draft("Child", 0.0F, first)).id;
    writeSidecar(edited, child);
    deleteSnapshot(bench.photo, 3, child);
    CHECK(openDocument(bench.photo).savedBase == first);
    writeSidecar(edited, first);
    deleteSnapshot(bench.photo, 4, first);
    document = openDocument(bench.photo);
    CHECK_FALSE(document.savedBase);
    CHECK(document.snapshots.entries.size() == 1);
    CHECK(document.photo.state().settings.tone.exposure == 2.0F);
}

TEST_CASE("A RAW with no saved settings stays at its defaults through every v2 write",
          "[snapshots][presence]") {
    const Bench bench;
    const DevelopState rawDefault = openPhoto(bench.photo).state();
    // Otherwise "the state equals the RAW default" would hold for any state at all.
    REQUIRE(rawDefault != DevelopState{});
    const auto requireDefaults = [&] {
        CHECK_FALSE(bench.read().state);
        CHECK(openPhoto(bench.photo).state() == rawDefault);
    };

    SECTION("from no sidecar: rating, then snapshot, then thumbnail") {
        writeSidecarMarks(bench.photo, {.rating = 3});
        std::string text = slurp(bench.sidecar);
        CHECK(contains(text, "arraw:version=\"2\""));
        CHECK(contains(text, "arraw:activeStatePresent=\"False\""));
        requireDefaults();

        const SnapshotId id = addSnapshot(bench.photo, 0, draft("Look")).id;
        requireDefaults();
        CHECK(bench.read().marks.rating == 3);

        REQUIRE(setSnapshotThumbnail(bench.photo, id, thumbnailOf(16, 16, "key-Look"), "key-Look"));
        requireDefaults();
        CHECK(bench.read().marks.rating == 3);
        CHECK(bench.read().snapshots.entries.size() == 1);
        CHECK(contains(slurp(bench.sidecar), "arraw:activeStatePresent=\"False\""));

        renameSnapshot(bench.photo, 1, id, "Look 2");
        requireDefaults();
        deleteSnapshot(bench.photo, 2, id);
        requireDefaults();
        CHECK(bench.read().marks.rating == 3);
        CHECK(contains(slurp(bench.sidecar), "arraw:activeStatePresent=\"False\""));
    }
    SECTION("from an unversioned sidecar of marks alone") {
        spit(bench.sidecar, sidecarOf("xmp:Rating=\"2\""));
        CHECK_FALSE(bench.read().state);
        writeSidecarMarks(bench.photo, {.rating = 5});
        requireDefaults();
        CHECK(contains(slurp(bench.sidecar), "arraw:activeStatePresent=\"False\""));
        addSnapshot(bench.photo, 0, draft("Look"));
        requireDefaults();
        CHECK(bench.read().marks.rating == 5);
    }
    SECTION("an untouched RAW with only a negative key records no state") {
        spit(bench.sidecar, sidecarOf("arraw:demosaic=\"dcb\""));
        writeSidecarMarks(bench.photo, {.rating = 1});
        requireDefaults();
        CHECK(bench.read().negative->demosaic == Demosaic::Dcb);
    }
    SECTION("a version 1 sidecar that records settings keeps them as explicit") {
        spit(bench.sidecar, sidecarOf("arraw:version=\"1\" arraw:exposure=\"1.5\""));
        writeSidecarMarks(bench.photo, {.rating = 1});
        CHECK(contains(slurp(bench.sidecar), "arraw:activeStatePresent=\"True\""));
        REQUIRE(bench.read().state);
        CHECK(bench.read().state->settings.tone.exposure == 1.5F);
        const SnapshotId id = addSnapshot(bench.photo, 0, draft("Look")).id;
        CHECK(bench.read().state->settings.tone.exposure == 1.5F);
        REQUIRE(setSnapshotThumbnail(bench.photo, id, thumbnailOf(8, 8, "key-Look"), "key-Look"));
        REQUIRE(bench.read().state);
        CHECK(bench.read().state->settings.tone.exposure == 1.5F);
        CHECK(contains(slurp(bench.sidecar), "arraw:activeStatePresent=\"True\""));
    }
    SECTION("a version 1 sidecar with only a version is an explicit neutral state") {
        spit(bench.sidecar, sidecarOf("arraw:version=\"1\""));
        REQUIRE(bench.read().state);
        writeSidecarMarks(bench.photo, {.rating = 1});
        CHECK(contains(slurp(bench.sidecar), "arraw:activeStatePresent=\"True\""));
        CHECK(bench.read().state);
    }
    SECTION("an explicit Save is explicit even when it equals the defaults") {
        writeSidecar(openPhoto(bench.photo));
        CHECK(contains(slurp(bench.sidecar), "arraw:activeStatePresent=\"True\""));
        CHECK(bench.read().state);
        writeSidecarMarks(bench.photo, {.rating = 2});
        CHECK(bench.read().state);
    }
}

TEST_CASE("A version 2 file whose presence marker is damaged is inferred from active keys only",
          "[snapshots][presence]") {
    const Bench bench;

    SECTION("a missing marker with active keys records a state, with a warning") {
        spit(bench.sidecar, sidecarOf("arraw:version=\"2\" arraw:exposure=\"1.5\""));
        CollectedDiagnostics log;
        const SidecarContents contents = bench.read(log);
        REQUIRE(contents.state);
        CHECK(contents.state->settings.tone.exposure == 1.5F);
        CHECK(count(log, Notice::SettingMalformed) == 1);
    }
    SECTION("a malformed marker is no better") {
        spit(bench.sidecar, sidecarOf("arraw:version=\"2\" arraw:activeStatePresent=\"maybe\" "
                                      "arraw:exposure=\"1.5\""));
        CollectedDiagnostics log;
        CHECK(bench.read(log).state);
        CHECK(count(log, Notice::SettingMalformed) == 1);
    }
    SECTION("version, snapshots, marks and negative do not count as active keys") {
        spit(bench.sidecar, sidecarOf("arraw:version=\"2\" arraw:savedBase=\"1\" xmp:Rating=\"3\" "
                                      "arraw:demosaic=\"vng\"",
                                      listOf(node(1, {}, "Look"))));
        CollectedDiagnostics log;
        const SidecarContents contents = bench.read(log);
        CHECK_FALSE(contents.state);
        CHECK(contents.snapshots.entries.size() == 1);
        CHECK(count(log, Notice::SettingMalformed) == 1);
    }
    SECTION("a non-active write keeps the damaged marker and the active fields verbatim") {
        spit(bench.sidecar, sidecarOf("arraw:version=\"2\" arraw:activeStatePresent=\"maybe\" "
                                      "arraw:exposure=\"1.5\""));
        writeSidecarMarks(bench.photo, {.rating = 4});
        std::string text = slurp(bench.sidecar);
        CHECK(contains(text, "arraw:activeStatePresent=\"maybe\""));
        CHECK(contains(text, "arraw:exposure=\"1.5\""));
        addSnapshot(bench.photo, 0, draft("Look"));
        text = slurp(bench.sidecar);
        CHECK(contains(text, "arraw:activeStatePresent=\"maybe\""));
        CHECK(contains(text, "arraw:exposure=\"1.5\""));
        CHECK(bench.read().marks.rating == 4);

        // An explicit Save repairs the marker.
        writeSidecar(openPhoto(bench.photo));
        text = slurp(bench.sidecar);
        CHECK_FALSE(contains(text, "maybe"));
        CHECK(contains(text, "arraw:activeStatePresent=\"True\""));
        CollectedDiagnostics log;
        CHECK(bench.read(log).state);
        CHECK(count(log, Notice::SettingMalformed) == 0);
    }
    SECTION("a missing marker on a file without active keys stays missing under marks writes") {
        spit(bench.sidecar, sidecarOf("arraw:version=\"2\" xmp:Rating=\"1\""));
        writeSidecarMarks(bench.photo, {.rating = 2});
        CHECK_FALSE(contains(slurp(bench.sidecar), "activeStatePresent"));
        CHECK_FALSE(bench.read().state);
    }
}

TEST_CASE("A marker of False wins over leftover active keys, and True over none",
          "[snapshots][presence]") {
    const Bench bench;
    spit(bench.sidecar, sidecarOf("arraw:version=\"2\" arraw:activeStatePresent=\"False\" "
                                  "arraw:exposure=\"1.5\""));
    CHECK_FALSE(bench.read().state);
    spit(bench.sidecar, sidecarOf("arraw:version=\"2\" arraw:activeStatePresent=\"True\""));
    REQUIRE(bench.read().state);
    CHECK(bench.read().state->settings == DevelopSettings{});
}

TEST_CASE("Frozen version 2 sidecars are read, and Save keeps their snapshots",
          "[snapshots][versions]") {
    const Bench bench;
    SECTION("complete.xmp") {
        bench.start(test::fixture("sidecars/v2/complete.xmp"));
        CollectedDiagnostics log;
        const OpenDocument document = openDocument(bench.photo, log);
        CHECK(log.entries().empty());
        CHECK(document.photo.state().settings.tone.exposure == 1.25F);
        CHECK(document.photo.marks() == PhotoMarks{.rating = 4, .label = ColorLabel::Blue});
        REQUIRE(document.snapshots.entries.size() == 2);
        CHECK(document.snapshots.nextId == 4);
        CHECK(document.snapshots.revision == 3);
        CHECK(document.savedBase == SnapshotId{2});
        const Snapshot& warm = document.snapshots.entries[0];
        CHECK(warm.id == SnapshotId{1});
        CHECK_FALSE(warm.parent);
        CHECK(warm.name == "Warm \xE2\x98\x80");
        CHECK(warm.state.settings.tone.exposure == 0.5F);
        CHECK(warm.negative.demosaic == Demosaic::Vng);
        REQUIRE(warm.thumbnail);
        CHECK(warm.thumbnail->renderKey == "frozen-key-1");
        const Snapshot& cool = document.snapshots.entries[1];
        CHECK(cool.parent == SnapshotId{1});
        CHECK_FALSE(cool.thumbnail);
        REQUIRE(cool.state.localAdjustments.size() == 1);
        CHECK_NOTHROW(validate(document.snapshots));
        CHECK(bench.read().others.size() == 1);

        // Save and marks writes keep the list and the foreign property.
        writeSidecar(document.photo.with(PhotoMarks{.rating = 2}));
        writeSidecarMarks(bench.photo, {.rating = 3});
        const OpenDocument again = openDocument(bench.photo);
        CHECK(again.snapshots == document.snapshots);
        CHECK(again.savedBase == document.savedBase);
        CHECK(bench.read().others.size() == 1);
        CHECK(contains(slurp(bench.sidecar), "acme:Owner"));
    }
    SECTION("marks-only.xmp") {
        bench.start(test::fixture("sidecars/v2/marks-only.xmp"));
        CollectedDiagnostics log;
        const OpenDocument document = openDocument(bench.photo, log);
        CHECK(log.entries().empty());
        CHECK_FALSE(bench.read().state);
        CHECK(document.photo.marks().rating == 2);
        CHECK(document.photo.state() == openPhoto(bench.photo).state());
        CHECK(document.snapshots.entries.empty());
    }
}

TEST_CASE("Version 1 sidecars are read as before and upgraded by the first write",
          "[snapshots][versions]") {
    const Bench bench;
    bench.start(test::fixture("sidecars/v1/complete.xmp"));
    CollectedDiagnostics log;
    const OpenDocument document = openDocument(bench.photo, log);
    CHECK(log.entries().empty());
    CHECK(document.snapshots.entries.empty());
    CHECK(document.snapshots.nextId == 1);
    CHECK(document.photo.state().settings.tone.exposure == 1.25F);

    writeSidecarMarks(bench.photo, {.rating = 1});
    const std::string text = slurp(bench.sidecar);
    CHECK(contains(text, "arraw:version=\"2\""));
    CHECK(contains(text, "arraw:activeStatePresent=\"True\""));
    CHECK(contains(text, "arraw:exposure=\"1.25\""));
    CHECK(contains(text, "acme:Owner"));
}

TEST_CASE("A newer sidecar is refused by every snapshot writer", "[snapshots][versions]") {
    const Bench bench;
    const std::string newer = sidecarOf("arraw:version=\"3\" arraw:activeStatePresent=\"True\"",
                                        listOf(node(1, {}, "Look")));
    spit(bench.sidecar, newer);
    CHECK_THROWS_AS(addSnapshot(bench.photo, 0, draft("x")), std::runtime_error);
    CHECK_THROWS_AS(renameSnapshot(bench.photo, 0, SnapshotId{1}, "x"), std::runtime_error);
    CHECK_THROWS_AS(deleteSnapshot(bench.photo, 0, SnapshotId{1}), std::runtime_error);
    CHECK_THROWS_AS(
        setSnapshotThumbnail(bench.photo, SnapshotId{1}, thumbnailOf(8, 8, "key-Look"), "key-Look"),
        std::runtime_error);
    CHECK_THROWS_AS(writeSidecarMarks(bench.photo, {.rating = 1}), std::runtime_error);
    CHECK(slurp(bench.sidecar) == newer);
}

TEST_CASE("Foreign XMP survives every snapshot write", "[snapshots][foreign]") {
    const Bench bench;
    bench.start(test::fixture("sidecar-foreign.xmp"));
    const SidecarContents before = bench.read();
    REQUIRE_FALSE(before.others.empty());

    const SnapshotId first = addSnapshot(bench.photo, 0, draft("First")).id;
    renameSnapshot(bench.photo, 1, first, "Renamed");
    REQUIRE(
        setSnapshotThumbnail(bench.photo, first, thumbnailOf(8, 8, "key-Renamed"), "key-Renamed"));
    const SnapshotId second = addSnapshot(bench.photo, 2, draft("Second")).id;
    deleteSnapshot(bench.photo, 3, second);
    writeSidecarMarks(bench.photo, {.rating = 2});

    const SidecarContents after = bench.read();
    CHECK(after.others == before.others);
    CHECK(after.creatorTool == before.creatorTool);
    CHECK(after.negative == before.negative);
    CHECK(after.state == before.state);
    const std::string text = slurp(bench.sidecar);
    for (const ForeignNamespace& other : before.others) {
        CHECK(contains(text, other.uri));
    }
    CHECK(contains(text, "arraw:futureKnob"));
    CHECK(contains(text, "arraw:version=\"2\""));
    // The namespace of arraw is the frozen one.
    CHECK(contains(text, "xmlns:arraw=\"http://ns.arraw.org/develop/\""));
    CHECK_FALSE(contains(text, "develop/1.0/"));
}

TEST_CASE("A failed snapshot write leaves the sidecar byte for byte", "[snapshots][atomic]") {
    const Bench bench;
    addSnapshot(bench.photo, 0, draft("First"));
    const std::string before = slurp(bench.sidecar);
    const auto listing = [&] {
        std::set<std::string> names;
        for (const auto& entry : fs::directory_iterator(bench.directory.path())) {
            names.insert(entry.path().filename().string());
        }
        return names;
    };
    const std::set<std::string> files = listing();

    // A directory nobody can add to: the lock and the replacement cannot be made.
    fs::permissions(bench.directory.path(), fs::perms::owner_read | fs::perms::owner_exec);
    struct Restore {
        fs::path directory;
        ~Restore() {
            fs::permissions(directory, fs::perms::owner_all);
        }
    } restore{bench.directory.path()};
    {
        std::ofstream probe(bench.directory.file("probe"));
        if (probe.is_open()) {
            // Do not let a run that proves nothing pass for one that does: say so loudly.
            WARN("permissions are not enforced here (running as root?), so atomicity of a failed "
                 "write is NOT exercised");
            SKIP("permissions are not enforced here, so a write cannot be made to fail");
        }
    }
    CHECK_THROWS_WITH(addSnapshot(bench.photo, 1, draft("Second")), ContainsSubstring("IMG_1.xmp"));
    CHECK_THROWS_WITH(writeSidecarMarks(bench.photo, {.rating = 1}),
                      ContainsSubstring("IMG_1.xmp"));
    CHECK(slurp(bench.sidecar) == before);
    // Neither a lock file nor a half-written replacement is left behind.
    CHECK(listing() == files);
}

namespace {

/// Lowers the time a writer waits for the lock, for the life of the object.
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

} // namespace

TEST_CASE("A writer gives up after the lock timeout and leaves the file alone",
          "[snapshots][lock]") {
    const ShortLockTimeout shortTimeout;
    const Bench bench;
    const SnapshotId first = addSnapshot(bench.photo, 0, draft("First")).id;
    const Photo opened = openPhoto(bench.photo);
    const SnapshotThumbnail thumbnail = thumbnailOf(8, 8, "key-First");
    const std::string before = slurp(bench.sidecar);

    const std::string lockPath = bench.sidecar.string() + ".lock";
    QLockFile lock(QString::fromStdString(lockPath));
    REQUIRE(lock.tryLock(1000));

    const auto timesOut = [&](const auto& write) {
        const auto started = std::chrono::steady_clock::now();
        try {
            write();
            FAIL("the write got through a held lock");
        } catch (const SidecarLockTimeout& error) {
            CHECK_THAT(error.what(), ContainsSubstring(lockPath));
            CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(90));
        }
        CHECK(slurp(bench.sidecar) == before);
    };
    SECTION("add") {
        timesOut([&] { addSnapshot(bench.photo, 1, draft("Second")); });
    }
    SECTION("rename") {
        timesOut([&] { renameSnapshot(bench.photo, 1, first, "Renamed"); });
    }
    SECTION("delete") {
        timesOut([&] { deleteSnapshot(bench.photo, 1, first); });
    }
    SECTION("thumbnail") {
        timesOut([&] { setSnapshotThumbnail(bench.photo, first, thumbnail, thumbnail.renderKey); });
    }
    SECTION("Save") {
        timesOut([&] { writeSidecar(opened); });
    }
    SECTION("Save with a base") {
        timesOut([&] { writeSidecar(opened, first); });
    }
    SECTION("marks") {
        timesOut([&] { writeSidecarMarks(bench.photo, {.rating = 1}); });
    }

    lock.unlock();
    CHECK_NOTHROW(addSnapshot(bench.photo, 1, draft("Second")));
    CHECK_FALSE(fs::exists(lockPath));
}

#ifdef __linux__
TEST_CASE("A lock left by a writer that is gone is broken", "[snapshots][lock]") {
    const Bench bench;
    const SnapshotId first = addSnapshot(bench.photo, 0, draft("First")).id;
    const std::string lockPath = bench.sidecar.string() + ".lock";

    // The same host, a pid that does not exist, and old.
    QByteArray content;
    {
        QLockFile held(QString::fromStdString(lockPath));
        REQUIRE(held.tryLock(100));
        QFile file(QString::fromStdString(lockPath));
        REQUIRE(file.open(QIODevice::ReadOnly));
        content = file.readAll();
    }
    REQUIRE_FALSE(fs::exists(lockPath));
    QList<QByteArray> lines = content.split('\n');
    REQUIRE(lines.size() >= 3);
    lines[0] = "2147483646";
    spit(lockPath, lines.join('\n').toStdString());
    fs::last_write_time(lockPath, fs::file_time_type::clock::now() - std::chrono::hours(1));

    CHECK_NOTHROW(renameSnapshot(bench.photo, 1, first, "Despite stale lock"));
    CHECK(openDocument(bench.photo).snapshots.find(first)->name == "Despite stale lock");
}
#endif

TEST_CASE("A lock held by a live writer is respected whatever its age", "[snapshots][lock]") {
    // Qt judges a lock whose owner still runs (same host, pid and application) by that alone, so
    // a slow writer is never overtaken; only a dead owner's lock is broken (see above).
    const Bench bench;
    const SnapshotId first = addSnapshot(bench.photo, 0, draft("First")).id;
    const int savedStale = detail::sidecarStaleLock;
    const int savedTimeout = detail::sidecarLockTimeout;
    detail::sidecarStaleLock = 150;
    detail::sidecarLockTimeout = 100;
    QLockFile lock(QString::fromStdString(bench.sidecar.string() + ".lock"));
    lock.setStaleLockTime(0);
    REQUIRE(lock.tryLock(100));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    CHECK_THROWS_AS(renameSnapshot(bench.photo, 1, first, "Overtaken"), SidecarLockTimeout);

    detail::sidecarStaleLock = savedStale;
    detail::sidecarLockTimeout = savedTimeout;
}

TEST_CASE("Concurrent writers of every kind all land", "[snapshots][concurrent]") {
    const Bench bench;
    // Three snapshots to patch while others are added.
    std::vector<SnapshotId> seeded;
    std::vector<SnapshotThumbnail> patches;
    for (std::uint64_t i = 0; i < 3; ++i) {
        const std::string name = "Seed" + std::to_string(i);
        seeded.push_back(addSnapshot(bench.photo, i, draft(name)).id);
        patches.push_back(
            thumbnailOf(8, 8, "key-" + name)); // Built here: Catch is not thread-safe.
    }
    const Photo saved =
        openPhoto(bench.photo).with(DevelopState{.settings = {.tone = {.exposure = 1.5F}}});

    constexpr int adders = 3;
    constexpr int perAdder = 4;
    constexpr int attempts = 200;
    std::atomic<int> added = 0;
    std::atomic<int> patched = 0;
    std::atomic<int> saves = 0;
    std::atomic<int> conflicts = 0;
    std::mutex problemsMutex;
    std::vector<std::string> problems;
    const auto problem = [&](const std::string& what) {
        const std::scoped_lock lock(problemsMutex);
        problems.push_back(what);
    };
    // Retries what is only contention; anything else is a problem for the main thread to judge.
    const auto persist = [&](const auto& step, const std::string& what) {
        for (int attempt = 0; attempt < attempts; ++attempt) {
            try {
                if (step()) {
                    return true;
                }
            } catch (const SnapshotRevisionConflict&) {
                ++conflicts;
            } catch (const SidecarLockTimeout&) {
            } catch (const std::exception& error) {
                problem(what + ": " + error.what());
                return false;
            }
        }
        problem(what + ": gave up");
        return false;
    };

    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < adders; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < perAdder; ++i) {
                    const std::string name = "T" + std::to_string(t) + "-" + std::to_string(i);
                    if (persist(
                            [&] {
                                const auto contents = readSidecar(bench.photo);
                                if (!contents) {
                                    throw std::runtime_error("the sidecar vanished");
                                }
                                addSnapshot(bench.photo, contents->snapshots.revision, draft(name));
                                return true;
                            },
                            "add " + name)) {
                        ++added;
                    }
                }
            });
        }
        threads.emplace_back([&] {
            for (std::size_t i = 0; i < seeded.size(); ++i) {
                persist(
                    [&] {
                        patched += setSnapshotThumbnail(bench.photo, seeded[i], patches[i],
                                                        patches[i].renderKey)
                                       ? 1
                                       : 0;
                        return true;
                    },
                    "thumbnail " + std::to_string(i));
            }
        });
        threads.emplace_back([&] {
            for (int i = 0; i < 3; ++i) {
                if (persist(
                        [&] {
                            writeSidecar(saved);
                            return true;
                        },
                        "save")) {
                    ++saves;
                }
            }
        });
    }
    INFO("conflicts seen: " << conflicts.load());
    for (const std::string& what : problems) {
        FAIL_CHECK(what);
    }
    CHECK(added == adders * perAdder);
    CHECK(patched == 3);
    CHECK(saves == 3);

    const OpenDocument document = openDocument(bench.photo);
    const SnapshotCollection& collection = document.snapshots;
    CHECK(document.photo.state().settings.tone.exposure == 1.5F);
    CHECK_NOTHROW(validate(collection));
    REQUIRE(collection.entries.size() == 3 + adders * perAdder);
    std::set<std::uint64_t> ids;
    for (const Snapshot& snapshot : collection.entries) {
        ids.insert(snapshot.id.value);
    }
    CHECK(ids.size() == collection.entries.size());
    CHECK(collection.revision == 3 + adders * perAdder);
    CHECK(collection.nextId == 3 + adders * perAdder + 1);
    for (const SnapshotId id : seeded) {
        CHECK(collection.find(id)->thumbnail);
    }
}

TEST_CASE("Damaged snapshot data is reported and skipped, and the photograph opens",
          "[snapshots][damaged]") {
    const Bench bench;
    const std::string badJson = "{ not json";
    const SnapshotThumbnail good = thumbnailOf(8, 8, "k");
    const QByteArray goodBase64 = QByteArray(reinterpret_cast<const char*>(good.jpeg.data()),
                                             static_cast<qsizetype>(good.jpeg.size()))
                                      .toBase64();
    const std::string items =
        node(1, {}, "Fine") +                                 // usable root
        node(2, 1, "Bad state", badJson) +                    // unusable: state
        node(3, {}, "Dup") + node(3, {}, "Dup again") +       // duplicate id
        node(4, 99, "Orphan") +                               // parent missing
        node(5, 6, "Cycle A") + node(6, 5, "Cycle B") +       // cycle
        item(field("id", "zero?") + field("name", "No id")) + // unusable: id
        node(7, {}, "") +                                     // unusable: name
        item(field("id", "8") + field("name", "Thumb") + field("created", "2026-10-23T12:00:00Z") +
             field("state", escaped(stateToJson(DevelopState{}))) +
             field("negative", escaped(negativeToJson(NegativeSettings{}))) +
             field("thumbnail", goodBase64.toStdString()) + field("thumbnailKey", "")) + // key
        node(12, {}, "Highest but bad", badJson) + // unusable, and the highest id of all
        item(field("id", "9") + field("name", "Thumb2") + field("created", "2026-10-23T12:00:00Z") +
             field("state", escaped(stateToJson(DevelopState{}))) +
             field("negative", escaped(negativeToJson(NegativeSettings{}))) +
             field("thumbnail", "@@@not-base64@@@") + field("thumbnailKey", "k"));
    spit(bench.sidecar,
         sidecarOf("arraw:version=\"2\" arraw:activeStatePresent=\"False\" arraw:savedBase=\"77\" "
                   "arraw:snapshotNextId=\"2\" arraw:snapshotRevision=\"5\" xmp:Rating=\"3\"",
                   listOf(items)));

    CollectedDiagnostics log;
    const OpenDocument document = openDocument(bench.photo, log);
    CHECK(document.photo.marks().rating == 3);
    const SnapshotCollection& collection = document.snapshots;
    std::set<std::uint64_t> ids;
    for (const Snapshot& snapshot : collection.entries) {
        ids.insert(snapshot.id.value);
    }
    CHECK(ids == std::set<std::uint64_t>{1, 3, 4, 5, 6, 8, 9});
    CHECK(collection.find(SnapshotId{4})->parent == std::nullopt); // root
    // Exactly one edge of the cycle is cut.
    const bool aRoot = !collection.find(SnapshotId{5})->parent;
    const bool bRoot = !collection.find(SnapshotId{6})->parent;
    CHECK(aRoot != bRoot);
    CHECK_FALSE(collection.find(SnapshotId{8})->thumbnail);
    CHECK_FALSE(collection.find(SnapshotId{9})->thumbnail);
    CHECK_FALSE(document.savedBase); // dangling
    CHECK(collection.nextId == 13);  // raised above the unusable node 12, not only usable ones
    CHECK(collection.revision == 5);
    CHECK_NOTHROW(validate(collection));
    // One notice for each defect: bad state (2), duplicate (3), orphan (4), cycle (5/6), no id,
    // empty name (7), empty key (8), bad base64 (9), bad state (12), dangling saved base.
    CHECK(count(log, Notice::SnapshotDamaged) == 10);
    for (const Diagnostic& entry : log.entries()) {
        CHECK(entry.notice == Notice::SnapshotDamaged);
        CHECK(entry.values.size() == 3);
    }
    CHECK_FALSE(bench.read().state);

    SECTION("an edit keeps the raw content it could not read") {
        const SnapshotId extra = addSnapshot(bench.photo, 5, draft("Extra")).id;
        CHECK(extra.value == 13);
        renameSnapshot(bench.photo, 6, SnapshotId{1}, "Renamed");
        deleteSnapshot(bench.photo, 7, SnapshotId{4});
        setSnapshotThumbnail(bench.photo, SnapshotId{1}, thumbnailOf(8, 8, "key-Renamed"),
                             "key-Renamed");
        const std::string text = slurp(bench.sidecar);
        CHECK(contains(text, "{ not json"));
        CHECK(contains(text, "Bad state"));
        CHECK(contains(text, "zero?"));
        CHECK(contains(text, "@@@not-base64@@@"));
        CHECK(contains(text, "Dup again"));
        CHECK(contains(text, "arraw:savedBase=\"77\""));
        CHECK(contains(text, "Highest but bad"));
        const SnapshotCollection after = openDocument(bench.photo).snapshots;
        CHECK(after.find(SnapshotId{1})->name == "Renamed");
        CHECK(after.find(SnapshotId{13}) != nullptr);
        CHECK(after.find(SnapshotId{4}) == nullptr);
        CHECK(after.nextId == 14);
        CHECK(after.revision == 8);
    }
}

TEST_CASE("A snapshot list that cannot be edited whole refuses the edit", "[snapshots][damaged]") {
    const Bench bench;
    const auto refuses = [&](const std::string& body, const std::string& attributes = {}) {
        const std::string original =
            sidecarOf("arraw:version=\"2\" arraw:activeStatePresent=\"False\" " + attributes, body);
        spit(bench.sidecar, original);
        CollectedDiagnostics log;
        CHECK_NOTHROW(static_cast<void>(openDocument(bench.photo, log)));
        CHECK(openDocument(bench.photo).snapshots.entries.empty());
        CHECK(count(log, Notice::SnapshotDamaged) >= 1);
        CHECK(bench.read().snapshotsDamaged);
        CHECK_THROWS_AS(addSnapshot(bench.photo, 0, draft("x")), std::runtime_error);
        CHECK_THROWS_AS(renameSnapshot(bench.photo, 0, SnapshotId{1}, "x"), std::runtime_error);
        CHECK_THROWS_AS(deleteSnapshot(bench.photo, 0, SnapshotId{1}), std::runtime_error);
        CHECK(slurp(bench.sidecar) == original);
        // Marks writes do not touch the list, so they are still allowed.
        CHECK_NOTHROW(writeSidecarMarks(bench.photo, {.rating = 2}));
        CHECK(contains(slurp(bench.sidecar), "not a sequence"));
    };
    SECTION("a list that is plain text") {
        refuses("<arraw:snapshots>not a sequence</arraw:snapshots>");
    }
    SECTION("a list that is an attribute") {
        refuses("", "arraw:snapshots=\"not a sequence\"");
    }
    SECTION("a list that appears twice, the good one first") {
        refuses(listOf(node(1, {}, "A")) + "<arraw:snapshots>not a sequence</arraw:snapshots>");
    }
    SECTION("a list that appears twice, the bad one first") {
        refuses("<arraw:snapshots>not a sequence</arraw:snapshots>" + listOf(node(1, {}, "A")));
    }
}

TEST_CASE("An empty snapshot list property is an empty list", "[snapshots][damaged]") {
    const Bench bench;
    spit(bench.sidecar,
         sidecarOf("arraw:version=\"2\" arraw:activeStatePresent=\"False\"", "<arraw:snapshots/>"));
    CollectedDiagnostics log;
    CHECK(openDocument(bench.photo, log).snapshots.entries.empty());
    CHECK(log.entries().empty());
    CHECK(addSnapshot(bench.photo, 0, draft("First")).collection.entries.size() == 1);
    CHECK(openDocument(bench.photo).snapshots.entries.size() == 1);
}

TEST_CASE("Snapshot names and recipes survive XML escaping", "[snapshots][crud]") {
    const Bench bench;
    Snapshot odd = draft("<a href=\"x\">&amp; 'quoted' \xF0\x9F\x93\xB7</a>");
    odd.state.settings.tone.exposure = 0.125F;
    odd.thumbnail = thumbnailOf(40, 30, "key-" + odd.name);
    const SnapshotId id = addSnapshot(bench.photo, 0, odd).id;
    odd.id = id;
    CHECK(openDocument(bench.photo).snapshots.entries == std::vector<Snapshot>{odd});
}

TEST_CASE("Snapshot nodes carry local adjustments", "[snapshots][crud]") {
    const Bench bench;
    Snapshot withMask = draft("Masked");
    LocalAdjustment mask;
    mask.id = LocalAdjustmentId{1};
    mask.name = "Sky";
    mask.shape = RadialMask{};
    withMask.state.localAdjustments = {mask};
    withMask.state.nextLocalAdjustmentId = LocalAdjustmentId{2};
    const SnapshotId id = addSnapshot(bench.photo, 0, withMask).id;
    const SnapshotCollection collection = openDocument(bench.photo).snapshots;
    REQUIRE(collection.find(id) != nullptr);
    CHECK(collection.find(id)->state.localAdjustments.size() == 1);
    CHECK(collection.find(id)->state.localAdjustments[0].name == "Sky");
    // The active recipe keeps its own masks, unaffected.
    CHECK(openPhoto(bench.photo).state().localAdjustments.empty());
}

TEST_CASE("A Save whose base was deleted meanwhile writes nothing and says why",
          "[snapshots][save]") {
    const Bench bench;
    const Photo edited =
        openPhoto(bench.photo).with(DevelopState{.settings = {.tone = {.exposure = 2.0F}}});
    const SnapshotId first = addSnapshot(bench.photo, 0, draft("First")).id;
    writeSidecar(edited, first);

    deleteSnapshot(bench.photo, 1, first); // Another process.
    const std::string before = slurp(bench.sidecar);
    try {
        writeSidecar(edited.with(DevelopState{.settings = {.tone = {.exposure = 3.0F}}}), first);
        FAIL("a base that is gone was accepted");
    } catch (const SnapshotBaseMissing& error) {
        CHECK(error.base == first);
    }
    CHECK(slurp(bench.sidecar) == before);
    // The session remaps its base and saves again.
    CHECK_NOTHROW(writeSidecar(edited.with(DevelopState{.settings = {.tone = {.exposure = 3.0F}}}),
                               std::nullopt));
    CHECK(openPhoto(bench.photo).state().settings.tone.exposure == 3.0F);
}

TEST_CASE("Unusable entries that fill the list are named in the refusal", "[snapshots][damaged]") {
    const Bench bench;
    std::string items;
    for (std::uint64_t id = 1; id <= 20; ++id) {
        items += node(id, {}, "Good " + std::to_string(id));
    }
    for (std::uint64_t id = 21; id <= 32; ++id) {
        items += node(id, {}, "Bad " + std::to_string(id), "{ not json");
    }
    spit(bench.sidecar,
         sidecarOf("arraw:version=\"2\" arraw:activeStatePresent=\"False\"", listOf(items)));
    CHECK(openDocument(bench.photo).snapshots.entries.size() == 20);
    CHECK_THROWS_WITH(addSnapshot(bench.photo, 0, draft("x")),
                      ContainsSubstring("20 of them usable"));
    CHECK_THROWS_WITH(addSnapshot(bench.photo, 0, draft("x")),
                      ContainsSubstring("12 unreadable entries"));
    // A snapshot that is not there says, once unreadable entries exist, that it may be one of them.
    CHECK_THROWS_WITH(renameSnapshot(bench.photo, 0, SnapshotId{25}, "x"),
                      ContainsSubstring("no usable snapshot 25") &&
                          ContainsSubstring("12 unreadable entries"));
}

TEST_CASE("A missing snapshot of a healthy list is a plain refusal", "[snapshots][damaged]") {
    const Bench bench;
    addSnapshot(bench.photo, 0, draft("First"));
    CHECK_THROWS_WITH(deleteSnapshot(bench.photo, 1, SnapshotId{7}),
                      ContainsSubstring("no usable snapshot 7"));
    try {
        deleteSnapshot(bench.photo, 1, SnapshotId{7});
    } catch (const std::invalid_argument& error) {
        CHECK_THAT(error.what(), !ContainsSubstring("unreadable"));
    }
}

namespace {

/// Lowers the largest snapshot list a snapshot edit may leave, for the life of the object.
struct SubtreeLimit {
    std::size_t before = detail::snapshotSubtreeLimit;

    explicit SubtreeLimit(std::size_t limit) {
        detail::snapshotSubtreeLimit = limit;
    }
    ~SubtreeLimit() {
        detail::snapshotSubtreeLimit = before;
    }
    SubtreeLimit(const SubtreeLimit&) = delete;
    SubtreeLimit& operator=(const SubtreeLimit&) = delete;
};

/// The serialized snapshot list of a sidecar, as bytes.
std::size_t listBytes(const std::string& sidecarText) {
    const std::string close = "</arraw:snapshots>";
    const auto begin = sidecarText.find("<arraw:snapshots");
    const auto end = sidecarText.find(close);
    REQUIRE(begin != std::string::npos);
    REQUIRE(end != std::string::npos);
    return end + close.size() - begin;
}

} // namespace

TEST_CASE("The snapshot list may serialize to the limit and not a byte more",
          "[snapshots][limit]") {
    const Bench bench;
    const Snapshot wide = draft("Wide \xC3\xA9\xE2\x98\x80"); // Multi-byte: bytes, not characters.
    addSnapshot(bench.photo, 0, wide);
    const std::size_t size = listBytes(slurp(bench.sidecar));
    fs::remove(bench.sidecar);

    SECTION("exactly at the limit") {
        const SubtreeLimit limit(size);
        CHECK_NOTHROW(addSnapshot(bench.photo, 0, wide));
    }
    SECTION("one byte over") {
        const SubtreeLimit limit(size - 1);
        CHECK_THROWS_AS(addSnapshot(bench.photo, 0, wide), std::invalid_argument);
        CHECK_FALSE(fs::exists(bench.sidecar));
    }
    SECTION("foreign XMP outside the list is not counted") {
        spit(bench.sidecar, sidecarOf("xmlns:acme=\"http://acme.example/ns/\" arraw:version=\"2\" "
                                      "arraw:activeStatePresent=\"False\"",
                                      "<acme:Big>" + std::string(10'000, 'x') + "</acme:Big>"));
        const SubtreeLimit limit(size);
        CHECK_NOTHROW(addSnapshot(bench.photo, 0, wide));
    }
    SECTION("a thumbnail patch is held to it too") {
        const SnapshotId id = [&] {
            const SubtreeLimit open(size);
            return addSnapshot(bench.photo, 0, wide).id;
        }();
        const SubtreeLimit limit(size);
        const std::string before = slurp(bench.sidecar);
        CHECK_THROWS_AS(setSnapshotThumbnail(bench.photo, id, thumbnailOf(8, 8, "key-" + wide.name),
                                             "key-" + wide.name),
                        std::invalid_argument);
        CHECK(slurp(bench.sidecar) == before);
    }
}

namespace {

std::string thumbnailBase64(int width, int height) {
    const SnapshotThumbnail thumbnail = thumbnailOf(width, height);
    return QByteArray(reinterpret_cast<const char*>(thumbnail.jpeg.data()),
                      static_cast<qsizetype>(thumbnail.jpeg.size()))
        .toBase64()
        .toStdString();
}

/// A node with the given fields replaced: id, name and the rest are valid unless overridden.
std::string nodeWith(std::uint64_t id, const std::string& created, const std::string& extra = {},
                     const std::string& name = "Node") {
    return item(field("id", std::to_string(id)) + field("name", name) + field("created", created) +
                field("state", escaped(stateToJson(DevelopState{}))) +
                field("negative", escaped(negativeToJson(NegativeSettings{}))) + extra);
}

} // namespace

TEST_CASE("Each kind of read-side damage is skipped or repaired, and reported once",
          "[snapshots][damaged]") {
    const std::string when = "2026-10-23T12:00:00Z";
    struct Row {
        std::string label;
        std::string attributes;
        std::string body;
        std::set<std::uint64_t> ids; ///< Snapshots that read.
        std::size_t damaged;         ///< ::Notice::SnapshotDamaged entries.
        std::size_t malformed;       ///< ::Notice::SettingMalformed entries.
        bool listDamaged;            ///< SidecarContents::snapshotsDamaged.
    };
    std::string thirtyFour;
    for (std::uint64_t id = 1; id <= 34; ++id) {
        thirtyFour += nodeWith(id, when);
    }
    const std::string maximum = "18446744073709551615";
    const std::vector<Row> rows = {
        {"a bad time", "", nodeWith(1, "yesterday") + nodeWith(2, when), {2}, 1, 0, true},
        {"no time",
         "",
         item(field("id", "1") + field("name", "x") + field("state", "{}") +
              field("negative", "{}")) +
             nodeWith(2, when),
         {2},
         1,
         0,
         true},
        {"a parent that is itself",
         "",
         item(field("id", "1") + field("parent", "1") + field("name", "x") +
              field("created", when) + field("state", escaped(stateToJson(DevelopState{}))) +
              field("negative", escaped(negativeToJson({})))) +
             nodeWith(2, when),
         {1, 2},
         1,
         0,
         false},
        {"a parent of zero",
         "",
         item(field("id", "1") + field("parent", "0") + field("name", "x") +
              field("created", when) + field("state", escaped(stateToJson(DevelopState{}))) +
              field("negative", escaped(negativeToJson({})))) +
             nodeWith(2, when),
         {1, 2},
         1,
         0,
         false},
        {"a name over 256 bytes",
         "",
         nodeWith(1, when, "", std::string(257, 'n')) + nodeWith(2, when),
         {2},
         1,
         0,
         true},
        {"a name with a control character",
         "",
         nodeWith(1, when, "", "a&#9;b") + nodeWith(2, when),
         {2},
         1,
         0,
         true},
        {"a thumbnail over 256 pixels",
         "",
         nodeWith(1, when,
                  field("thumbnail", thumbnailBase64(300, 10)) + field("thumbnailKey", "k")) +
             nodeWith(2, when),
         {1, 2},
         1,
         0,
         false},
        {"a thumbnail that is no JPEG",
         "",
         nodeWith(1, when, field("thumbnail", "AAAAAAAAAAAAAAAA") + field("thumbnailKey", "k")) +
             nodeWith(2, when),
         {1, 2},
         1,
         0,
         false},
        {"a thumbnail without its key",
         "",
         nodeWith(1, when, field("thumbnail", thumbnailBase64(8, 8))) + nodeWith(2, when),
         {1, 2},
         1,
         0,
         false},
        {"34 entries",
         "",
         thirtyFour,
         {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
          17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32},
         2,
         0,
         true},
        {"the largest id",
         "",
         nodeWith(1, when) + node(18446744073709551615ULL, {}, "Last"),
         {1},
         1,
         0,
         true},
        {"a revision that is no number",
         "arraw:snapshotRevision=\"many\"",
         nodeWith(1, when),
         {1},
         0,
         1,
         false},
        {"a next id that is no number",
         "arraw:snapshotNextId=\"-3\"",
         nodeWith(1, when),
         {1},
         0,
         1,
         false},
        {"a saved base that is no number",
         "arraw:savedBase=\"first\"",
         nodeWith(1, when),
         {1},
         0,
         1,
         false},
    };
    for (const Row& row : rows) {
        DYNAMIC_SECTION(row.label) {
            const Bench bench;
            spit(bench.sidecar,
                 sidecarOf("arraw:version=\"2\" arraw:activeStatePresent=\"False\" " +
                               row.attributes,
                           listOf(row.body)));
            CollectedDiagnostics log;
            const OpenDocument document = openDocument(bench.photo, log);
            std::set<std::uint64_t> ids;
            for (const Snapshot& snapshot : document.snapshots.entries) {
                ids.insert(snapshot.id.value);
            }
            CHECK(ids == row.ids);
            CHECK(count(log, Notice::SnapshotDamaged) == row.damaged);
            CHECK(count(log, Notice::SettingMalformed) == row.malformed);
            CHECK(bench.read().snapshotsDamaged == row.listDamaged);
            CHECK_NOTHROW(validate(document.snapshots));
        }
    }
}

TEST_CASE("A list that has used up the ids cannot take another snapshot and is left alone",
          "[snapshots][damaged]") {
    const Bench bench;
    spit(bench.sidecar, sidecarOf("arraw:version=\"2\" arraw:activeStatePresent=\"False\"",
                                  listOf(node(18446744073709551615ULL, {}, "Last"))));
    const std::string before = slurp(bench.sidecar);
    CHECK(openDocument(bench.photo).snapshots.nextId == 18446744073709551615ULL);
    CHECK_THROWS_AS(addSnapshot(bench.photo, 0, draft("x")), std::invalid_argument);
    CHECK(slurp(bench.sidecar) == before);
}

TEST_CASE("Foreign and future content inside and around snapshot nodes survives every edit",
          "[snapshots][foreign]") {
    const Bench bench;
    const std::string nodeOne =
        "<rdf:li rdf:parseType=\"Resource\"><arraw:id>1</arraw:id><arraw:name>One</arraw:name>"
        "<arraw:created>2026-10-23T12:00:00Z</arraw:created>"
        "<arraw:state>" +
        escaped(stateToJson(DevelopState{})) +
        "</arraw:state>"
        "<arraw:negative>" +
        escaped(negativeToJson(NegativeSettings{})) +
        "</arraw:negative>"
        "<arraw:futureField>keep future</arraw:futureField>"
        "<acme:Note>keep note</acme:Note></rdf:li>";
    // The nested form some XMP writers produce: the fields sit in an rdf:Description.
    const std::string nodeTwo =
        "<rdf:li><rdf:Description><arraw:id>2</arraw:id><arraw:name>Two</arraw:name>"
        "<arraw:created>2026-10-23T12:00:00Z</arraw:created>"
        "<arraw:state>" +
        escaped(stateToJson(DevelopState{})) +
        "</arraw:state>"
        "<arraw:negative>" +
        escaped(negativeToJson(NegativeSettings{})) +
        "</arraw:negative>"
        "<acme:Note>keep nested</acme:Note></rdf:Description></rdf:li>";
    spit(bench.sidecar, R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about="" xmlns:acme="http://acme.example/ns/" acme:Owner="Jan"/>
  <rdf:Description rdf:about="" xmlns:arraw="http://ns.arraw.org/develop/"
    xmlns:acme="http://acme.example/ns/" arraw:version="2" arraw:activeStatePresent="False"
    arraw:snapshotRevision="3" arraw:snapshotNextId="3" acme:Other="keep other">
   <arraw:snapshots><rdf:Seq>)" +
                            nodeOne + nodeTwo + R"(</rdf:Seq></arraw:snapshots>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>)");
    REQUIRE(bench.read().snapshots.entries.size() == 2);

    const SnapshotId three = addSnapshot(bench.photo, 3, draft("Three")).id;
    renameSnapshot(bench.photo, 4, SnapshotId{1}, "One renamed");
    renameSnapshot(bench.photo, 5, SnapshotId{2}, "Two renamed");
    REQUIRE(setSnapshotThumbnail(bench.photo, SnapshotId{2}, thumbnailOf(8, 8, "key-Two renamed"),
                                 "key-Two renamed"));
    REQUIRE(setSnapshotThumbnail(bench.photo, SnapshotId{1}, thumbnailOf(8, 8, "key-One renamed"),
                                 "key-One renamed"));
    deleteSnapshot(bench.photo, 6, three);
    writeSidecarMarks(bench.photo, {.rating = 2});

    const std::string text = slurp(bench.sidecar);
    for (const char* kept :
         {"keep future", "keep note", "keep nested", "keep other", "acme:Owner"}) {
        INFO(kept);
        CHECK(contains(text, kept));
    }
    const SnapshotCollection collection = openDocument(bench.photo).snapshots;
    REQUIRE(collection.entries.size() == 2);
    CHECK(collection.find(SnapshotId{1})->name == "One renamed");
    CHECK(collection.find(SnapshotId{2})->name == "Two renamed");
    CHECK(collection.find(SnapshotId{1})->thumbnail);
    CHECK(collection.find(SnapshotId{2})->thumbnail);
    CHECK(collection.revision == 7);
}

namespace {

/// A usable node with a field of its own that arraw does not know.
std::string nodeWithForeignField(std::uint64_t id, std::optional<std::uint64_t> parent,
                                 const std::string& name) {
    std::string text = node(id, parent, name);
    const std::string close = "</rdf:li>";
    text.insert(text.size() - close.size(), "<arraw:keepMe>foreign " + name + "</arraw:keepMe>");
    return text;
}

/// A list of two usable nodes, one with a foreign field, then the damage a discard removes.
std::string damagedItems() {
    const std::string badJson = "{ not json";
    return nodeWithForeignField(1, {}, "Root") + node(2, 1, "Child") +
           node(3, 1, "Bad state", badJson) + node(4, {}, "Another bad", badJson) +
           node(1, {}, "Duplicate of one") + node(9, {}, "The highest, bad", badJson);
}

constexpr std::string_view markedBare = "arraw:version=\"2\" arraw:activeStatePresent=\"False\" ";

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

TEST_CASE("The reader counts the entries it cannot use", "[snapshots][discard][damaged]") {
    const Bench bench;
    CHECK(openDocument(bench.photo).unreadableSnapshots == 0);

    SECTION("a damaged node, a repeated id and a node past the limit") {
        spit(bench.sidecar, sidecarOf(std::string(markedBare), listOf(damagedItems())));
        const SidecarContents contents = bench.read();
        CHECK(contents.unreadableSnapshots == 4); // 3, 4, the duplicate 1 and 9
        CHECK(contents.snapshots.entries.size() == 2);
        CHECK(openDocument(bench.photo).unreadableSnapshots == 4);
    }
    SECTION("entries past the limit of 32") {
        std::string items;
        for (std::uint64_t id = 1; id <= 34; ++id) {
            items += node(id, {}, "Snapshot " + std::to_string(id));
        }
        spit(bench.sidecar, sidecarOf(std::string(markedBare), listOf(items)));
        CHECK(bench.read().snapshots.entries.size() == 32);
        CHECK(bench.read().unreadableSnapshots == 2);
        CHECK(openDocument(bench.photo).unreadableSnapshots == 2);
    }
    SECTION("a list that is not one sequence counts every item of every occurrence, at least one") {
        spit(bench.sidecar,
             sidecarOf(std::string(markedBare),
                       listOf(node(2, {}, "A")) + listOf(node(7, {}, "B") + node(8, {}, "C"))));
        CHECK(bench.read().unreadableSnapshots == 3);
        CHECK(bench.read().snapshots.entries.empty());
        spit(bench.sidecar, sidecarOf(std::string(markedBare),
                                      "<arraw:snapshots>not a sequence</arraw:snapshots>"));
        CHECK(bench.read().unreadableSnapshots == 1);
        CHECK(openDocument(bench.photo).unreadableSnapshots == 1);
    }
    SECTION("an edit reports the count it leaves") {
        spit(bench.sidecar, sidecarOf(std::string(markedBare), listOf(damagedItems())));
        const SnapshotUpdate update = addSnapshot(bench.photo, 0, draft("New"));
        CHECK(update.unreadable == 4);
        CHECK(renameSnapshot(bench.photo, 1, SnapshotId{1}, "R").unreadable == 4);
    }
}

TEST_CASE("Discarding removes the unusable entries and keeps every snapshot",
          "[snapshots][discard]") {
    const Bench bench;
    spit(bench.sidecar, sidecarOf(std::string(markedBare) +
                                      "arraw:snapshotNextId=\"3\" arraw:snapshotRevision=\"5\"",
                                  listOf(damagedItems())));
    const SnapshotCollection before = bench.read().snapshots;
    REQUIRE(before.entries.size() == 2);
    REQUIRE(bench.read().unreadableSnapshots == 4);

    const auto update = discardUnreadableSnapshots(bench.photo, 5);

    REQUIRE(update);
    CHECK(update->unreadable == 0);
    CHECK(update->id.value == 0);
    CHECK(update->collection.entries == before.entries);
    CHECK(update->collection.revision == 6);
    CHECK(update->collection.nextId == 10); // above the raw id 9 of an entry that is gone
    const SidecarContents after = bench.read();
    CHECK(after.snapshots == update->collection);
    CHECK(after.unreadableSnapshots == 0);
    CHECK_FALSE(after.snapshotsDamaged);
    const std::string text = slurp(bench.sidecar);
    CHECK(contains(text, "foreign Root"));
    CHECK_FALSE(contains(text, "Duplicate of one"));
    CHECK_FALSE(contains(text, "Bad state"));
    CHECK_FALSE(contains(text, "The highest, bad"));
    CollectedDiagnostics log;
    static_cast<void>(openDocument(bench.photo, log));
    CHECK(count(log, Notice::SnapshotDamaged) == 0);

    // The list takes new snapshots again, and no removed id comes back.
    CHECK(addSnapshot(bench.photo, 6, draft("After")).id.value == 10);
}

TEST_CASE("Discarding a list that is not one sequence removes every occurrence",
          "[snapshots][discard]") {
    const Bench bench;
    spit(bench.sidecar, sidecarOf(std::string(markedBare) +
                                      "arraw:snapshotNextId=\"2\" arraw:snapshotRevision=\"3\"",
                                  listOf(node(2, {}, "A")) + listOf(node(7, {}, "B"))));
    REQUIRE(bench.read().unreadableSnapshots == 2);

    const auto update = discardUnreadableSnapshots(bench.photo, 3);

    REQUIRE(update);
    CHECK(update->collection.entries.empty());
    CHECK(update->collection.revision == 4);
    CHECK(update->collection.nextId == 8); // above the raw ids of both occurrences
    const std::string text = slurp(bench.sidecar);
    CHECK_FALSE(contains(text, "arraw:snapshots"));
    CHECK(bench.read().unreadableSnapshots == 0);
    CHECK_FALSE(bench.read().snapshotsDamaged);
    CHECK(addSnapshot(bench.photo, 4, draft("After")).id.value == 8);

    SECTION("a list that is an attribute or plain text goes too") {
        spit(bench.sidecar, sidecarOf(std::string(markedBare) + "arraw:snapshots=\"text\""));
        REQUIRE(bench.read().unreadableSnapshots == 1);
        REQUIRE(discardUnreadableSnapshots(bench.photo, 0));
        CHECK_FALSE(contains(slurp(bench.sidecar), "arraw:snapshots"));
        spit(bench.sidecar,
             sidecarOf(std::string(markedBare), "<arraw:snapshots>text</arraw:snapshots>"));
        REQUIRE(discardUnreadableSnapshots(bench.photo, 0));
        CHECK_FALSE(contains(slurp(bench.sidecar), "arraw:snapshots"));
        CHECK(bench.read().unreadableSnapshots == 0);
    }
}

TEST_CASE("Discarding refuses a stale revision and leaves the file alone", "[snapshots][discard]") {
    const Bench bench;
    spit(bench.sidecar, sidecarOf(std::string(markedBare) + "arraw:snapshotRevision=\"5\"",
                                  listOf(damagedItems())));
    const std::string before = slurp(bench.sidecar);

    try {
        static_cast<void>(discardUnreadableSnapshots(bench.photo, 4));
        FAIL("a stale revision was accepted");
    } catch (const SnapshotRevisionConflict& error) {
        CHECK(error.expected == 4);
        CHECK(error.actual == 5);
    }
    CHECK(slurp(bench.sidecar) == before);
}

TEST_CASE("Discarding with nothing unusable writes nothing", "[snapshots][discard]") {
    const Bench bench;
    SECTION("no sidecar") {
        CHECK_FALSE(discardUnreadableSnapshots(bench.photo, 0));
        CHECK_FALSE(fs::exists(bench.sidecar));
    }
    SECTION("a clean list") {
        addSnapshot(bench.photo, 0, draft("One"));
        addSnapshot(bench.photo, 1, draft("Two"));
        const std::string before = slurp(bench.sidecar);
        const auto stamp = fs::last_write_time(bench.sidecar);
        CHECK_FALSE(discardUnreadableSnapshots(bench.photo, 2));
        CHECK(slurp(bench.sidecar) == before);
        CHECK(fs::last_write_time(bench.sidecar) == stamp);
        CHECK_FALSE(fs::exists(bench.sidecar.string() + ".lock"));
        // The revision is still checked first.
        CHECK_THROWS_AS(discardUnreadableSnapshots(bench.photo, 1), SnapshotRevisionConflict);
    }
}

TEST_CASE("Discarding writes the parents and the saved base as the reader repaired them",
          "[snapshots][discard]") {
    const Bench bench;
    const std::string badJson = "{ not json";
    const std::string items =
        node(1, {}, "Root") + node(2, 1, "Child") +
        node(3, 99, "Dangling parent") +                // missing parent
        node(4, {}, "Bad", badJson) +                   // unusable, and a parent ...
        node(5, 4, "Child of the bad one") +            // ... of this one
        node(6, 7, "Cycle A") + node(7, 6, "Cycle B") + // a cycle
        item(field("id", "8") + field("parent", "x?") + field("name", "Odd parent") +
             field("created", "2026-10-23T12:00:00Z") +
             field("state", escaped(stateToJson(DevelopState{}))) +
             field("negative", escaped(negativeToJson(NegativeSettings{}))));
    spit(bench.sidecar,
         sidecarOf(std::string(markedBare) + "arraw:savedBase=\"77\"", listOf(items)));
    CollectedDiagnostics log;
    const SnapshotCollection before = bench.read(log).snapshots;
    REQUIRE(count(log, Notice::SnapshotDamaged) > 4);
    REQUIRE(before.entries.size() == 7);

    const auto update = discardUnreadableSnapshots(bench.photo, 0);

    REQUIRE(update);
    CHECK(update->collection.entries == before.entries); // as the reader had repaired them
    CHECK(update->collection.find(SnapshotId{2})->parent == SnapshotId{1});
    CHECK(update->collection.find(SnapshotId{3})->parent == std::nullopt);
    CHECK(update->collection.find(SnapshotId{5})->parent == std::nullopt);
    CHECK(update->collection.find(SnapshotId{8})->parent == std::nullopt);
    CHECK(update->savedBase == std::nullopt);
    // Nothing is left to repair on the next read: no notice at all.
    CollectedDiagnostics again;
    const SidecarContents after = bench.read(again);
    CHECK(count(again, Notice::SnapshotDamaged) == 0);
    CHECK(after.savedBase == std::nullopt);
    CHECK_FALSE(contains(slurp(bench.sidecar), "arraw:savedBase"));
    CHECK(after.snapshots.entries == before.entries);
}

TEST_CASE("Discarding keeps a saved base that names a listed snapshot", "[snapshots][discard]") {
    const Bench bench;
    spit(bench.sidecar,
         sidecarOf(std::string(markedBare) + "arraw:savedBase=\"2\"",
                   listOf(node(1, {}, "A") + node(2, 1, "B") + node(3, {}, "Bad", "{ not json"))));
    const auto update = discardUnreadableSnapshots(bench.photo, 0);
    REQUIRE(update);
    CHECK(update->savedBase == SnapshotId{2});
    CHECK(bench.read().savedBase == SnapshotId{2});
}

TEST_CASE("Discarding times out on a locked sidecar and leaves the file alone",
          "[snapshots][discard][lock][slow]") {
    const ShortLockTimeout shortTimeout;
    const Bench bench;
    spit(bench.sidecar, sidecarOf(std::string(markedBare), listOf(damagedItems())));
    const std::string before = slurp(bench.sidecar);

    QLockFile lock(QString::fromStdString(bench.sidecar.string() + ".lock"));
    REQUIRE(lock.tryLock(1000));
    CHECK_THROWS_AS(discardUnreadableSnapshots(bench.photo, 0), SidecarLockTimeout);
    CHECK(slurp(bench.sidecar) == before);
    lock.unlock();
    CHECK(discardUnreadableSnapshots(bench.photo, 0));
}

TEST_CASE("Several thumbnails are written under one lock and one commit",
          "[snapshots][thumbnail]") {
    const Bench bench;
    const SnapshotId one = addSnapshot(bench.photo, 0, draft("One")).id;
    const SnapshotId two = addSnapshot(bench.photo, 1, draft("Two")).id;
    const SnapshotId three = addSnapshot(bench.photo, 2, draft("Three")).id;
    // A rename that lands after the patches were prepared is kept.
    renameSnapshot(bench.photo, 3, two, "Two renamed");

    const std::vector<ThumbnailPatchByKey> patches = {
        {one, thumbnailOf(8, 8, "key-one"), "key-one"},
        {two, thumbnailOf(8, 8, "key-two"), "key-other"}, // the wrong key
        {three, thumbnailOf(16, 12, "key-three"), "key-three"},
        {SnapshotId{9}, thumbnailOf(8, 8, "key-gone"), "key-gone"}, // not a snapshot
    };
    {
        const CountedLocks locks;
        CHECK(setSnapshotThumbnails(bench.photo, patches) ==
              std::vector<bool>{true, false, true, false});
        CHECK(locks.taken == 1);
    }
    const SnapshotCollection collection = bench.read().snapshots;
    CHECK(collection.revision == 4);
    CHECK(collection.find(one)->thumbnail->renderKey == "key-one");
    CHECK_FALSE(collection.find(two)->thumbnail);
    CHECK(collection.find(two)->name == "Two renamed");
    CHECK(collection.find(three)->thumbnail->renderKey == "key-three");

    SECTION("when none matches nothing is written") {
        const std::string before = slurp(bench.sidecar);
        const auto stamp = fs::last_write_time(bench.sidecar);
        const std::vector<ThumbnailPatchByKey> none = {
            {two, thumbnailOf(8, 8, "key-two"), "key-other"},
            {SnapshotId{9}, thumbnailOf(8, 8, "key-gone"), "key-gone"}};
        CHECK(setSnapshotThumbnails(bench.photo, none) == std::vector<bool>{false, false});
        CHECK(slurp(bench.sidecar) == before);
        CHECK(fs::last_write_time(bench.sidecar) == stamp);
    }
    SECTION("an empty batch takes no lock") {
        const CountedLocks locks;
        CHECK(setSnapshotThumbnails(bench.photo, {}).empty());
        CHECK(locks.taken == 0);
    }
    SECTION("an invalid thumbnail refuses the whole batch") {
        const std::string before = slurp(bench.sidecar);
        const std::vector<ThumbnailPatchByKey> bad = {
            {two, thumbnailOf(8, 8, "key-two"), "key-two"},
            {three, thumbnailOf(300, 8, "key-three"), "key-three"}};
        CHECK_THROWS_AS(setSnapshotThumbnails(bench.photo, bad), std::invalid_argument);
        CHECK(slurp(bench.sidecar) == before);
    }
    SECTION("the later of two patches for one snapshot wins") {
        const std::vector<ThumbnailPatchByKey> twice = {
            {one, thumbnailOf(8, 8, "key-new"), "key-new"},
            {one, thumbnailOf(24, 24, "key-new"), "key-new"}};
        CHECK(setSnapshotThumbnails(bench.photo, twice) == std::vector<bool>{true, true});
        const SnapshotThumbnail kept = *bench.read().snapshots.find(one)->thumbnail;
        CHECK(kept == twice.back().thumbnail);
        CHECK(bench.read().snapshots.revision == 4);
    }
}
