#include "SidecarLimits.h"
#include "ThumbnailCache.h"
#include "ThumbnailWorker.h"
#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <DevelopState.h>
#include <EditSession.h>
#include <NegativeSettings.h>
#include <Photo.h>
#include <Sidecar.h>
#include <SnapshotThumbnails.h>
#include <SnapshotWrites.h>
#include <Snapshots.h>

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QImage>
#include <QLockFile>
#include <QRandomGenerator>
#include <QString>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace arraw;

/// Snapshot thumbnails (ADR 056, decision 9): the render key, the bounded JPEG, the explicit-recipe
/// job of the worker, and the session applying a result to its matching node only.

namespace {

namespace fs = std::filesystem;

constexpr const char* fixtureName = "preview-32x24.dng";

DevelopState exposed(float exposure) {
    DevelopState state;
    state.settings.tone.exposure = exposure;
    return state;
}

std::string slurp(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

void spit(const fs::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

/// A photograph in a folder of its own.
struct Rig {
    test::TempDir directory;
    fs::path photo = directory.file("IMG_1.dng");

    Rig() {
        fs::copy_file(test::fixture(fixtureName), photo);
    }

    [[nodiscard]] EditSession open() const {
        return EditSession(openDocument(photo));
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

/// Mean brightness of a thumbnail, 0 to 255.
double brightness(const SnapshotThumbnail& thumbnail) {
    const QImage image = QImage::fromData(reinterpret_cast<const uchar*>(thumbnail.jpeg.data()),
                                          static_cast<qsizetype>(thumbnail.jpeg.size()), "JPEG");
    REQUIRE_FALSE(image.isNull());
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    double sum = 0;
    for (int y = 0; y < rgb.height(); ++y) {
        const uchar* line = rgb.constScanLine(y);
        for (int x = 0; x < rgb.width() * 3; ++x) {
            sum += line[x];
        }
    }
    return sum / (static_cast<double>(rgb.width()) * rgb.height() * 3.0);
}

/// Pixel size of an encoded thumbnail.
QSize sizeOf(const SnapshotThumbnail& thumbnail) {
    return QImage::fromData(reinterpret_cast<const uchar*>(thumbnail.jpeg.data()),
                            static_cast<qsizetype>(thumbnail.jpeg.size()), "JPEG")
        .size();
}

/// Thumbnail of a recipe, as the worker would render it.
SnapshotThumbnail renderOf(const fs::path& photo, const DevelopState& state,
                           const NegativeSettings& negative = {}) {
    const auto rendered = renderSnapshotThumbnail(photo, {SnapshotId{1}, state, negative});
    REQUIRE(rendered);
    return *rendered;
}

/// Image of noise, which JPEG cannot compress.
QImage noise(int width, int height) {
    QRandomGenerator generator(12345);
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixel(x, y, generator.generate() | 0xFF000000U);
        }
    }
    return image;
}

/// Opens a gate when it goes out of scope, so that a failed check cannot leave the worker
/// blocked and the suite hanging in the worker's destructor. Declare it after the worker.
struct GateGuard {
    explicit GateGuard(std::function<void()> release) : release(std::move(release)) {}
    ~GateGuard() {
        release();
    }
    GateGuard(const GateGuard&) = delete;
    GateGuard& operator=(const GateGuard&) = delete;

    std::function<void()> release;
};

bool waitUntil(const std::function<bool()>& condition,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(20000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return true;
}

/// Collects what the worker delivers, from its thread.
class Collector {
public:
    [[nodiscard]] std::function<void(app::SnapshotThumbnailResult)> callback() {
        return [this](app::SnapshotThumbnailResult result) {
            const std::scoped_lock lock(mutex_);
            results_.push_back(std::move(result));
        };
    }

    [[nodiscard]] std::vector<app::SnapshotThumbnailResult> results() const {
        const std::scoped_lock lock(mutex_);
        return results_;
    }

    [[nodiscard]] std::size_t count() const {
        const std::scoped_lock lock(mutex_);
        return results_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<app::SnapshotThumbnailResult> results_;
};

/// Blocks the worker thread inside a callback until released.
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

} // namespace

TEST_CASE("The render key follows the recipe and the file, and nothing machine-specific",
          "[snapshotthumbnails]") {
    const Rig rig;
    const DevelopState state = exposed(1.0F);
    const auto key = snapshotThumbnailKey(rig.photo, state, {});
    REQUIRE(key);
    CHECK(key->size() == 64);
    CHECK(key->size() <= maximumSnapshotThumbnailKeyBytes);

    SECTION("is stable") {
        CHECK(snapshotThumbnailKey(rig.photo, exposed(1.0F), {}) == key);
    }
    SECTION("changes with the state") {
        CHECK(snapshotThumbnailKey(rig.photo, exposed(1.5F), {}) != key);
    }
    SECTION("changes with the negative settings") {
        NegativeSettings negative;
        negative.demosaic = Demosaic::Vng;
        CHECK(snapshotThumbnailKey(rig.photo, state, negative) != key);
    }
    SECTION("changes with the lens settings of the negative") {
        NegativeSettings negative;
        negative.lens.distortion = true;
        CHECK(snapshotThumbnailKey(rig.photo, state, negative) != key);
    }
    SECTION("changes with the file's size") {
        const test::TempDir other;
        const fs::path same = other.file("IMG_1.dng");
        fs::copy_file(test::fixture("exif-32x24.dng"), same);
        REQUIRE(fs::file_size(same) != fs::file_size(rig.photo));
        CHECK(snapshotThumbnailKey(same, state, {}) != key);
    }
    SECTION("changes with the file's name") {
        const test::TempDir other;
        const fs::path renamed = other.file("IMG_2.dng");
        fs::copy_file(rig.photo, renamed);
        CHECK(snapshotThumbnailKey(renamed, state, {}) != key);
    }
    SECTION("changes with the file's content when name and size are the same") {
        const test::TempDir other;
        const fs::path same = other.file("IMG_1.dng");
        std::string bytes = slurp(rig.photo);
        bytes.front() = static_cast<char>(bytes.front() ^ 0x55);
        spit(same, bytes);
        REQUIRE(fs::file_size(same) == fs::file_size(rig.photo));
        CHECK(snapshotThumbnailKey(same, state, {}) != key);
    }
    SECTION("does not change with the folder or the modification time") {
        // A folder synced to another machine: same name and size, another path and time.
        const test::TempDir other;
        const fs::path copy = other.file("IMG_1.dng");
        fs::copy_file(rig.photo, copy);
        fs::last_write_time(copy, fs::last_write_time(copy) - std::chrono::hours(48));
        CHECK(snapshotThumbnailKey(copy, state, {}) == key);
    }
    SECTION("is nothing for a file that is not there") {
        CHECK_FALSE(snapshotThumbnailKey(rig.directory.file("none.dng"), state, {}));
    }
}

TEST_CASE("A thumbnail is current only for the recipe and file it was made for",
          "[snapshotthumbnails]") {
    const Rig rig;
    Snapshot snapshot;
    snapshot.id = SnapshotId{1};
    snapshot.name = "A";
    snapshot.state = exposed(1.0F);
    CHECK_FALSE(hasCurrentThumbnail(rig.photo, snapshot)); // none
    snapshot.thumbnail = encodeSnapshotThumbnail(QImage(8, 8, QImage::Format_RGB32), "other");
    CHECK_FALSE(hasCurrentThumbnail(rig.photo, snapshot)); // stale
    snapshot.thumbnail->renderKey = *snapshotThumbnailKey(rig.photo, snapshot.state, {});
    CHECK(hasCurrentThumbnail(rig.photo, snapshot));
    snapshot.state = exposed(2.0F); // the recipe moved on
    CHECK_FALSE(hasCurrentThumbnail(rig.photo, snapshot));
}

TEST_CASE("Encoding keeps a thumbnail within 256 pixels and 64 KiB", "[snapshotthumbnails]") {
    SECTION("a large picture is reduced") {
        QImage image(1000, 500, QImage::Format_RGBA8888);
        image.fill(Qt::darkCyan);
        const SnapshotThumbnail thumbnail = encodeSnapshotThumbnail(image, "key");
        CHECK(sizeOf(thumbnail) == QSize(256, 128));
        CHECK(thumbnail.renderKey == "key");
        CHECK_NOTHROW(validate(thumbnail));
    }
    SECTION("a small picture is not enlarged") {
        QImage image(32, 24, QImage::Format_RGB32);
        image.fill(Qt::red);
        CHECK(sizeOf(encodeSnapshotThumbnail(image, "key")) == QSize(32, 24));
    }
    SECTION("a picture over the byte limit is encoded at a lower quality until it fits") {
        // Noise at 256x256 is about 50 KB at the best quality, so the limit is lowered to make it
        // not fit.
        const QImage image = noise(256, 256);
        const SnapshotThumbnail best = encodeSnapshotThumbnail(image, "key");
        constexpr std::size_t limit = 30 * 1024;
        REQUIRE(best.jpeg.size() > limit);

        const SnapshotThumbnail thumbnail = encodeSnapshotThumbnail(image, "key", limit);
        CHECK(thumbnail.jpeg.size() <= limit);
        CHECK(thumbnail.jpeg.size() < best.jpeg.size());
        CHECK(sizeOf(thumbnail) == QSize(256, 256));
        CHECK_NOTHROW(validate(thumbnail));
        // No quality fits: an error, not an oversized thumbnail.
        CHECK_THROWS_AS(encodeSnapshotThumbnail(image, "key", 100), std::runtime_error);
    }
    SECTION("the limit never exceeds 64 KiB") {
        const SnapshotThumbnail thumbnail =
            encodeSnapshotThumbnail(noise(256, 256), "key", 10 * maximumSnapshotThumbnailBytes);
        CHECK(thumbnail.jpeg.size() <= maximumSnapshotThumbnailBytes);
    }
    SECTION("refuses what it cannot make") {
        CHECK_THROWS_AS(encodeSnapshotThumbnail(QImage{}, "key"), std::invalid_argument);
        CHECK_THROWS_AS(encodeSnapshotThumbnail(QImage(8, 8, QImage::Format_RGB32), ""),
                        std::invalid_argument);
    }
}

TEST_CASE("Rendering develops the explicit recipe, fitted and bounded, and reads no sidecar",
          "[snapshotthumbnails]") {
    const Rig rig;
    const SnapshotThumbnail dark = renderOf(rig.photo, exposed(-2.0F));
    const SnapshotThumbnail bright = renderOf(rig.photo, exposed(2.0F));
    CHECK(brightness(bright) > brightness(dark) + 10.0);
    CHECK(dark.renderKey == *snapshotThumbnailKey(rig.photo, exposed(-2.0F), {}));
    CHECK(dark.jpeg.size() <= maximumSnapshotThumbnailBytes);
    CHECK(sizeOf(dark).width() <= maximumSnapshotThumbnailSide);
    CHECK(sizeOf(dark).height() <= maximumSnapshotThumbnailSide);
    CHECK_NOTHROW(validate(dark));

    SECTION("a sidecar with other settings changes nothing") {
        Photo photo = openPhoto(rig.photo);
        writeSidecar(photo.with(exposed(3.0F)));
        const SnapshotThumbnail again = renderOf(rig.photo, exposed(-2.0F));
        CHECK(again == dark);
    }
    SECTION("a cancelled render gives nothing") {
        int asked = 0;
        const auto result = renderSnapshotThumbnail(rig.photo, {SnapshotId{1}, exposed(1.0F), {}},
                                                    {}, [&] { return ++asked > 1; });
        CHECK_FALSE(result);
        CHECK(asked > 1);
        CHECK_FALSE(renderSnapshotThumbnail(rig.photo, {SnapshotId{1}, exposed(1.0F), {}}, {},
                                            [] { return true; }));
    }
    SECTION("a file that cannot be read is an error") {
        CHECK_THROWS(renderSnapshotThumbnail(rig.directory.file("none.dng"),
                                             {SnapshotId{1}, exposed(1.0F), {}}));
    }
}

TEST_CASE("The cache keeps bounded snapshot thumbnails and drops damaged ones",
          "[snapshotthumbnails]") {
    const Rig rig;
    const test::TempDir root;
    const app::ThumbnailCache cache(root.path());
    const SnapshotThumbnail thumbnail = renderOf(rig.photo, exposed(1.0F));
    REQUIRE_FALSE(cache.loadSnapshot(thumbnail.renderKey));
    REQUIRE(cache.storeSnapshot(thumbnail));
    CHECK(cache.loadSnapshot(thumbnail.renderKey) == thumbnail);

    SECTION("an invalid thumbnail is not stored") {
        SnapshotThumbnail bad = thumbnail;
        bad.renderKey = "ff" + std::string(8, 'a');
        bad.jpeg.assign(10, std::byte{1});
        CHECK_FALSE(cache.storeSnapshot(bad));
        CHECK_FALSE(cache.loadSnapshot(bad.renderKey));
    }
    SECTION("an oversized entry is deleted when read") {
        const fs::path entry =
            root.path() / thumbnail.renderKey.substr(0, 2) / (thumbnail.renderKey + ".jpg");
        REQUIRE(fs::exists(entry));
        spit(entry, std::string(maximumSnapshotThumbnailBytes + 100, 'x'));
        CHECK_FALSE(cache.loadSnapshot(thumbnail.renderKey));
        CHECK_FALSE(fs::exists(entry));
    }
    SECTION("an entry cut off after a good header is deleted when read") {
        const fs::path entry =
            root.path() / thumbnail.renderKey.substr(0, 2) / (thumbnail.renderKey + ".jpg");
        std::string bytes = slurp(entry);
        REQUIRE(bytes.size() > 400);
        bytes.resize(bytes.size() / 2);
        spit(entry, bytes);
        CHECK_FALSE(cache.loadSnapshot(thumbnail.renderKey));
        CHECK_FALSE(fs::exists(entry));
    }
    SECTION("a picture larger than 256 pixels is deleted when read") {
        const fs::path entry =
            root.path() / thumbnail.renderKey.substr(0, 2) / (thumbnail.renderKey + ".jpg");
        QImage image(300, 300, QImage::Format_RGB32);
        image.fill(Qt::blue);
        REQUIRE(image.save(QString::fromStdU16String(entry.u16string()), "JPEG", 50));
        CHECK_FALSE(cache.loadSnapshot(thumbnail.renderKey));
        CHECK_FALSE(fs::exists(entry));
    }
}

TEST_CASE("The session lists the thumbnails it wants", "[snapshotthumbnails][session]") {
    Rig rig;
    EditSession session = rig.open();
    const SnapshotId a = session.createSnapshot("A");
    CHECK(session.snapshotsNeedingThumbnails() == std::vector<SnapshotId>{a});

    const Snapshot recipe = *session.snapshots().find(a);
    REQUIRE(session.applySnapshotThumbnail(a, renderOf(rig.photo, recipe.state, recipe.negative)));
    CHECK(session.snapshotsNeedingThumbnails().empty());

    SECTION("a copy of the folder on another machine keeps it") {
        const test::TempDir other;
        fs::copy_file(rig.photo, other.file("IMG_1.dng"));
        fs::copy_file(fs::path(rig.photo).replace_extension(".xmp"), other.file("IMG_1.xmp"));
        fs::last_write_time(other.file("IMG_1.dng"),
                            fs::file_time_type::clock::now() - std::chrono::hours(100));
        const OpenDocument document = openDocument(other.file("IMG_1.dng"));
        REQUIRE(document.snapshots.find(a)->thumbnail);
        CHECK(hasCurrentThumbnail(other.file("IMG_1.dng"), *document.snapshots.find(a)));
    }
    SECTION("a file of another size makes it stale") {
        std::ofstream(rig.photo, std::ios::binary | std::ios::app) << "x";
        CHECK(session.snapshotsNeedingThumbnails() == std::vector<SnapshotId>{a});
    }
    SECTION("a thumbnail the reader drops is missing") {
        std::string text = slurp(fs::path(rig.photo).replace_extension(".xmp"));
        const auto at = text.find("/9j/"); // base64 of a JPEG's first bytes
        REQUIRE(at != std::string::npos);
        text.replace(at, 4, "AAAA");
        spit(fs::path(rig.photo).replace_extension(".xmp"), text);
        const OpenDocument document = openDocument(rig.photo);
        REQUIRE(document.snapshots.find(a));
        CHECK_FALSE(document.snapshots.find(a)->thumbnail);
        EditSession fresh{openDocument(rig.photo)};
        CHECK(fresh.snapshotsNeedingThumbnails() == std::vector<SnapshotId>{a});
    }
}

namespace {

/// Everything about a session that a thumbnail must not change.
struct Observed {
    Photo photo;
    std::vector<HistoryStep> history;
    std::size_t position;
    bool editing;
    bool dirty;
    std::optional<SnapshotId> base;
    std::optional<SnapshotId> savedBase;
    std::uint64_t revision;

    friend bool operator==(const Observed&, const Observed&) = default;
};

Observed observe(const EditSession& session) {
    return {session.photo(),
            session.history(),
            session.position(),
            session.editing(),
            session.hasUnsavedChanges(),
            session.baseSnapshot(),
            session.savedBaseSnapshot(),
            session.snapshots().revision};
}

/// What the sidecar holds apart from the snapshots' thumbnails.
struct OnDisk {
    std::optional<DevelopState> state;
    std::optional<NegativeSettings> negative;
    std::optional<SnapshotId> savedBase;
    std::uint64_t revision;
    std::vector<std::string> names;

    friend bool operator==(const OnDisk&, const OnDisk&) = default;
};

OnDisk diskOf(const fs::path& photo) {
    const auto contents = readSidecar(photo);
    REQUIRE(contents);
    OnDisk disk{
        contents->state, contents->negative, contents->savedBase, contents->snapshots.revision, {}};
    for (const Snapshot& snapshot : contents->snapshots.entries) {
        disk.names.push_back(snapshot.name);
    }
    return disk;
}

} // namespace

TEST_CASE("Applying a thumbnail patches the matching node and nothing else",
          "[snapshotthumbnails][session]") {
    Rig rig;
    EditSession session = rig.open();
    session.setState(exposed(1.0F));
    const SnapshotId a = session.createSnapshot("A");
    session.setState(exposed(2.0F));
    const SnapshotId b = session.createSnapshot("B");
    session.begin();
    session.update(exposed(3.0F)); // an open gesture, unsaved
    REQUIRE(session.editing());
    REQUIRE(session.hasUnsavedChanges());

    const Observed before = observe(session);
    const OnDisk diskBefore = diskOf(rig.photo);
    const Snapshot recipeA = *session.snapshots().find(a);
    const SnapshotThumbnail thumbnail = renderOf(rig.photo, recipeA.state, recipeA.negative);

    REQUIRE(session.applySnapshotThumbnail(a, thumbnail));

    CHECK(observe(session) == before);
    CHECK(diskOf(rig.photo) == diskBefore);
    CHECK(session.snapshots().find(a)->thumbnail == thumbnail);
    CHECK_FALSE(session.snapshots().find(b)->thumbnail);
    const OpenDocument disk = openDocument(rig.photo);
    CHECK(disk.snapshots.find(a)->thumbnail == thumbnail);
    CHECK_FALSE(disk.snapshots.find(b)->thumbnail);
    // The session's copy equals the file, so a reader of the sidecar sees no outside change.
    CHECK(disk.snapshots == session.snapshots());
    // The pending gesture is still there, and still unsaved.
    CHECK(session.photo().state() == exposed(3.0F));
}

TEST_CASE("A thumbnail that is not the one wanted is refused and writes nothing",
          "[snapshotthumbnails][session]") {
    Rig rig;
    EditSession session = rig.open();
    session.setState(exposed(1.0F));
    const SnapshotId a = session.createSnapshot("A");
    const std::string sidecarBefore = slurp(fs::path(rig.photo).replace_extension(".xmp"));
    const Observed before = observe(session);
    const SnapshotCollection collection = session.snapshots();
    const Snapshot recipe = *session.snapshots().find(a);
    const SnapshotThumbnail good = renderOf(rig.photo, recipe.state, recipe.negative);

    const auto unchanged = [&] {
        CHECK(slurp(fs::path(rig.photo).replace_extension(".xmp")) == sidecarBefore);
        CHECK(session.snapshots() == collection);
        CHECK(observe(session) == before);
    };

    SECTION("made for another recipe") {
        CHECK_FALSE(session.applySnapshotThumbnail(a, renderOf(rig.photo, exposed(5.0F))));
        unchanged();
    }
    SECTION("made for the same recipe but another file") {
        std::ofstream(rig.photo, std::ios::binary | std::ios::app) << "x";
        CHECK_FALSE(session.applySnapshotThumbnail(a, good));
        unchanged();
    }
    SECTION("for a snapshot the session does not know") {
        CHECK_FALSE(session.applySnapshotThumbnail(SnapshotId{99}, good));
        unchanged();
    }
    SECTION("for a snapshot deleted by someone else") {
        const SnapshotUpdate gone = deleteSnapshot(rig.photo, session.snapshots().revision, a);
        const std::string afterDelete = slurp(fs::path(rig.photo).replace_extension(".xmp"));
        CHECK_FALSE(session.applySnapshotThumbnail(a, good));
        CHECK(slurp(fs::path(rig.photo).replace_extension(".xmp")) == afterDelete);
        CHECK_FALSE(session.snapshots().find(a)->thumbnail);
        CHECK(observe(session) == before);
        CHECK(readSidecar(rig.photo)->snapshots == gone.collection);
    }
    SECTION("that is not a valid thumbnail") {
        SnapshotThumbnail bad = good;
        bad.jpeg.assign(5, std::byte{0});
        CHECK_THROWS_AS(session.applySnapshotThumbnail(a, bad), std::invalid_argument);
        unchanged();
    }
    SECTION("while the sidecar is locked") {
        const ShortLockTimeout shortTimeout;
        QLockFile lock(QString::fromStdString(sidecarPath(rig.photo).string() + ".lock"));
        REQUIRE(lock.tryLock(1000));
        CHECK_THROWS_AS(session.applySnapshotThumbnail(a, good), SidecarLockTimeout);
        lock.unlock();
        unchanged();
        // The failure is not final: the same result applies once the lock is free.
        CHECK(session.applySnapshotThumbnail(a, good));
    }
}

TEST_CASE("The worker renders the recipe it is given, not the saved settings",
          "[snapshotthumbnails][worker]") {
    const Rig rig;
    const test::TempDir cacheRoot;
    // Saved settings that differ from the recipe.
    writeSidecar(openPhoto(rig.photo).with(exposed(-3.0F)));

    Collector collector;
    app::ThumbnailWorker worker(
        app::ThumbnailCache(cacheRoot.path()), [](app::ThumbnailResult) {}, false, {},
        collector.callback());
    const std::uint64_t generation = worker.beginSnapshots();
    const SnapshotRecipe recipe{SnapshotId{7}, exposed(2.0F), {}};
    REQUIRE(worker.requestSnapshotThumbnail(rig.photo, recipe, generation));
    REQUIRE(waitUntil([&] { return collector.count() == 1 && worker.pendingJobs() == 0; }));

    const app::SnapshotThumbnailResult result = collector.results().front();
    CHECK(result.primary == rig.photo);
    CHECK(result.id == SnapshotId{7});
    CHECK(result.generation == generation);
    CHECK(result.thumbnail.renderKey == *snapshotThumbnailKey(rig.photo, recipe.state, {}));
    CHECK_NOTHROW(validate(result.thumbnail));
    CHECK(result.thumbnail.jpeg.size() <= maximumSnapshotThumbnailBytes);
    CHECK(brightness(result.thumbnail) > brightness(renderOf(rig.photo, exposed(-3.0F))) + 10.0);
    // The saved settings were not touched.
    CHECK(openPhoto(rig.photo).state() == exposed(-3.0F));

    SECTION("the cache serves the second run, and replaces a damaged entry") {
        const app::ThumbnailCache cache(cacheRoot.path());
        const auto cached = cache.loadSnapshot(result.thumbnail.renderKey);
        REQUIRE(cached);
        CHECK(*cached == result.thumbnail);

        const fs::path entry = cacheRoot.path() / result.thumbnail.renderKey.substr(0, 2) /
                               (result.thumbnail.renderKey + ".jpg");
        spit(entry, "not a jpeg");
        const std::size_t seen = collector.count();
        REQUIRE(worker.requestSnapshotThumbnail(rig.photo, recipe, generation));
        REQUIRE(
            waitUntil([&] { return collector.count() == seen + 1 && worker.pendingJobs() == 0; }));
        CHECK(collector.results().back().thumbnail == result.thumbnail);
        CHECK(cache.loadSnapshot(result.thumbnail.renderKey) == result.thumbnail);
    }
}

TEST_CASE("Results of an obsolete snapshot generation are not delivered",
          "[snapshotthumbnails][worker]") {
    const Rig rig;
    const test::TempDir folder;
    const test::TempDir cacheRoot;
    const fs::path shot = folder.file("IMG_9.dng");
    fs::copy_file(test::fixture(fixtureName), shot);

    Collector collector;
    Gate gate;
    // The worker is held inside the embedded result of a shot, so that what is queued behind it
    // can be changed while it waits.
    app::ThumbnailWorker worker(
        app::ThumbnailCache(cacheRoot.path()),
        [&](app::ThumbnailResult result) {
            if (result.kind == app::ThumbnailKind::Embedded) {
                gate.wait();
            }
        },
        false, {}, collector.callback());
    worker.setShots({shot});
    const GateGuard guard([&] { gate.release(); });
    REQUIRE(waitUntil([&] { return gate.entered(); }));

    const std::uint64_t first = worker.beginSnapshots();
    CHECK(worker.requestSnapshotThumbnail(rig.photo, {SnapshotId{1}, exposed(1.0F), {}}, first));
    CHECK(worker.requestSnapshotThumbnail(rig.photo, {SnapshotId{2}, exposed(2.0F), {}}, first));
    const std::uint64_t second = worker.beginSnapshots(); // another photograph opened
    CHECK(second != first);
    CHECK(worker.snapshotGeneration() == second);
    // A late request of the old generation is refused outright.
    CHECK_FALSE(
        worker.requestSnapshotThumbnail(rig.photo, {SnapshotId{3}, exposed(3.0F), {}}, first));
    CHECK(worker.requestSnapshotThumbnail(rig.photo, {SnapshotId{4}, exposed(4.0F), {}}, second));
    gate.release();

    REQUIRE(waitUntil([&] { return collector.count() >= 1 && worker.pendingJobs() == 0; }));
    const auto results = collector.results();
    REQUIRE(results.size() == 1);
    CHECK(results[0].id == SnapshotId{4});
    CHECK(results[0].generation == second);
}

TEST_CASE("A new folder leaves snapshot jobs alone, and a snapshot job that fails delivers nothing",
          "[snapshotthumbnails][worker]") {
    const Rig rig;
    const test::TempDir cacheRoot;
    Collector collector;
    app::ThumbnailWorker worker(
        app::ThumbnailCache(cacheRoot.path()), [](app::ThumbnailResult) {}, false, {},
        collector.callback());
    const std::uint64_t generation = worker.beginSnapshots();
    // The first is not a decodable photograph; the worker goes on to the next.
    const fs::path garbage = rig.directory.file("garbage.dng");
    spit(garbage, "this is not a raw file");
    REQUIRE(
        worker.requestSnapshotThumbnail(garbage, {SnapshotId{1}, exposed(1.0F), {}}, generation));
    worker.setShots({});
    REQUIRE(
        worker.requestSnapshotThumbnail(rig.photo, {SnapshotId{2}, exposed(1.0F), {}}, generation));
    REQUIRE(waitUntil([&] { return worker.pendingJobs() == 0 && collector.count() >= 1; }));
    const auto results = collector.results();
    REQUIRE(results.size() == 1);
    CHECK(results[0].id == SnapshotId{2});
}

TEST_CASE("Asking again for a queued snapshot replaces the earlier request",
          "[snapshotthumbnails][worker]") {
    const Rig rig;
    const test::TempDir folder;
    const test::TempDir cacheRoot;
    const fs::path shot = folder.file("IMG_9.dng");
    fs::copy_file(test::fixture(fixtureName), shot);
    Collector collector;
    Gate gate;
    app::ThumbnailWorker worker(
        app::ThumbnailCache(cacheRoot.path()),
        [&](app::ThumbnailResult result) {
            if (result.kind == app::ThumbnailKind::Embedded) {
                gate.wait();
            }
        },
        false, {}, collector.callback());
    worker.setShots({shot});
    const GateGuard guard([&] { gate.release(); });
    REQUIRE(waitUntil([&] { return gate.entered(); }));
    const std::uint64_t generation = worker.beginSnapshots();
    CHECK(
        worker.requestSnapshotThumbnail(rig.photo, {SnapshotId{1}, exposed(1.0F), {}}, generation));
    CHECK(
        worker.requestSnapshotThumbnail(rig.photo, {SnapshotId{1}, exposed(2.0F), {}}, generation));
    gate.release();
    REQUIRE(waitUntil([&] { return collector.count() >= 1 && worker.pendingJobs() == 0; }));
    REQUIRE(collector.count() == 1);
    CHECK(collector.results()[0].thumbnail.renderKey ==
          *snapshotThumbnailKey(rig.photo, exposed(2.0F), {}));
}

TEST_CASE("A new folder leaves queued snapshot jobs alone", "[snapshotthumbnails][worker]") {
    const Rig rig;
    const test::TempDir folder;
    const test::TempDir cacheRoot;
    const fs::path shot = folder.file("IMG_9.dng");
    fs::copy_file(test::fixture(fixtureName), shot);
    Collector collector;
    Gate gate;
    app::ThumbnailWorker worker(
        app::ThumbnailCache(cacheRoot.path()),
        [&](app::ThumbnailResult result) {
            if (result.kind == app::ThumbnailKind::Embedded) {
                gate.wait();
            }
        },
        false, {}, collector.callback());
    worker.setShots({shot});
    const GateGuard guard([&] { gate.release(); });
    REQUIRE(waitUntil([&] { return gate.entered(); }));
    const std::uint64_t generation = worker.beginSnapshots();
    REQUIRE(
        worker.requestSnapshotThumbnail(rig.photo, {SnapshotId{1}, exposed(1.0F), {}}, generation));
    worker.setShots({});
    gate.release();
    REQUIRE(waitUntil([&] { return collector.count() == 1 && worker.pendingJobs() == 0; }));
    CHECK(collector.results()[0].id == SnapshotId{1});
}

TEST_CASE("Stopping the worker abandons the snapshot jobs still queued",
          "[snapshotthumbnails][worker]") {
    const Rig rig;
    const test::TempDir folder;
    const test::TempDir cacheRoot;
    const fs::path shot = folder.file("IMG_9.dng");
    fs::copy_file(test::fixture(fixtureName), shot);
    Collector collector;
    Gate gate;
    std::jthread opener;
    {
        app::ThumbnailWorker worker(
            app::ThumbnailCache(cacheRoot.path()),
            [&](app::ThumbnailResult result) {
                if (result.kind == app::ThumbnailKind::Embedded) {
                    gate.wait();
                }
            },
            false, {}, collector.callback());
        worker.setShots({shot});
        const GateGuard guard([&] { gate.release(); });
        REQUIRE(waitUntil([&] { return gate.entered(); }));
        const std::uint64_t generation = worker.beginSnapshots();
        for (std::uint32_t id = 1; id <= 3; ++id) {
            REQUIRE(worker.requestSnapshotThumbnail(
                rig.photo, {SnapshotId{id}, exposed(static_cast<float>(id)), {}}, generation));
        }
        // The destructor asks to stop while the worker is held; the gate opens a little later.
        opener = std::jthread([&] {
            QThread::msleep(100);
            gate.release();
        });
    }
    opener.join();
    CHECK(collector.count() == 0);
}

TEST_CASE("Preparing a thumbnail write reads no file and ignores a snapshot the session lacks",
          "[snapshotthumbnails][session][prepare]") {
    Rig rig;
    EditSession session = rig.open();
    session.setState(exposed(1.0F));
    const SnapshotId a = session.createSnapshot("A");
    const Snapshot recipe = *session.snapshots().find(a);
    const SnapshotThumbnail thumbnail = renderOf(rig.photo, recipe.state, recipe.negative);
    const Observed before = observe(session);

    // The photograph is gone: preparing must not need it, as the key is worked out by the write.
    fs::remove(rig.photo);
    const auto write = session.prepareSnapshotThumbnail(a, thumbnail);
    REQUIRE(write);
    const auto& patch = std::get<SnapshotThumbnailPatch>(write->change);
    CHECK(patch.id == a);
    CHECK(patch.state == recipe.state);
    CHECK(patch.negative == recipe.negative);
    CHECK(patch.thumbnail == thumbnail);
    CHECK_FALSE(session.prepareSnapshotThumbnail(SnapshotId{99}, thumbnail));
    CHECK(observe(session) == before);

    SnapshotThumbnail bad = thumbnail;
    bad.jpeg.assign(5, std::byte{0});
    CHECK_THROWS_AS(session.prepareSnapshotThumbnail(a, bad), std::invalid_argument);
    CHECK_THROWS_AS(session.prepareSnapshotThumbnail(SnapshotId{99}, bad), std::invalid_argument);

    // And the write, without the photograph to work the key out from, writes nothing.
    const SnapshotWriteOutcome outcome = writeSnapshotChange(*write);
    CHECK_FALSE(outcome.written);
    CHECK(session.finishSnapshotWrite(*write, outcome));
    CHECK_FALSE(session.snapshots().find(a)->thumbnail);
}

TEST_CASE("Finishing a thumbnail write changes no history, base, dirty state or revision",
          "[snapshotthumbnails][session][prepare]") {
    Rig rig;
    EditSession session = rig.open();
    session.setState(exposed(1.0F));
    const SnapshotId a = session.createSnapshot("A");
    session.setState(exposed(2.0F));
    session.begin();
    session.update(exposed(3.0F)); // a gesture, unsaved
    const Observed before = observe(session);
    const OnDisk diskBefore = diskOf(rig.photo);
    const Snapshot recipe = *session.snapshots().find(a);
    const SnapshotThumbnail thumbnail = renderOf(rig.photo, recipe.state, recipe.negative);

    const auto write = session.prepareSnapshotThumbnail(a, thumbnail);
    REQUIRE(write);
    const SnapshotWriteOutcome outcome = writeSnapshotChange(*write);
    REQUIRE(outcome.written);
    CHECK_FALSE(outcome.update);
    CHECK(session.finishSnapshotWrite(*write, outcome));

    CHECK(observe(session) == before);
    CHECK(diskOf(rig.photo) == diskBefore);
    CHECK(session.snapshots().find(a)->thumbnail == thumbnail);
    CHECK(session.snapshots() == openDocument(rig.photo).snapshots);

    SECTION("a snapshot deleted meanwhile is not brought back") {
        const SnapshotWrite another = *session.prepareSnapshotThumbnail(
            a, renderOf(rig.photo, recipe.state, recipe.negative));
        session.deleteSnapshot(a);
        const Observed afterDelete = observe(session);
        session.finishSnapshotWrite(another, SnapshotWriteOutcome{true, std::nullopt});
        CHECK(observe(session) == afterDelete);
        CHECK(session.snapshots().find(a) == nullptr);
    }
}

TEST_CASE("The worker serves snapshots after the visible developed thumbnails and before the rest",
          "[snapshotthumbnails][worker]") {
    const Rig rig;
    const test::TempDir folder;
    const test::TempDir cacheRoot;
    const auto shot = [&](const std::string& name) {
        const fs::path path = folder.file(name);
        fs::copy_file(test::fixture(fixtureName), path);
        return path;
    };
    const fs::path gateShot = shot("IMG_gate.dng");
    const fs::path visibleShot = shot("IMG_visible.dng");
    const fs::path offShot = shot("IMG_off.dng");
    const fs::path urgentShot = shot("IMG_urgent.dng");

    std::mutex mutex;
    std::vector<std::string> served;
    const auto note = [&](std::string entry) {
        const std::scoped_lock lock(mutex);
        served.push_back(std::move(entry));
    };
    Gate gate;
    app::ThumbnailWorker worker(
        app::ThumbnailCache(cacheRoot.path()),
        [&](app::ThumbnailResult result) {
            if (result.kind == app::ThumbnailKind::Embedded && result.primary == gateShot) {
                gate.wait();
            }
            note(std::string(result.kind == app::ThumbnailKind::Embedded ? "embedded "
                                                                         : "developed ") +
                 result.primary.stem().string());
        },
        false, {}, [&](app::SnapshotThumbnailResult) { note("snapshot"); });
    worker.setShots({gateShot});
    const GateGuard guard([&] { gate.release(); });
    REQUIRE(waitUntil([&] { return gate.entered(); }));

    // The worker is held inside the first result; everything else queues behind it.
    worker.addShots({visibleShot, offShot, urgentShot});
    worker.setVisible({visibleShot});
    worker.invalidate(urgentShot);
    const std::uint64_t generation = worker.beginSnapshots();
    REQUIRE(
        worker.requestSnapshotThumbnail(rig.photo, {SnapshotId{1}, exposed(1.0F), {}}, generation));
    gate.release();

    REQUIRE(waitUntil([&] { return worker.pendingJobs() == 0; }));
    const std::scoped_lock lock(mutex);
    CHECK(served == std::vector<std::string>{"embedded IMG_gate", "developed IMG_urgent",
                                             "embedded IMG_visible", "embedded IMG_off",
                                             "developed IMG_visible", "snapshot",
                                             "developed IMG_gate", "developed IMG_off"});
}

TEST_CASE("A snapshot request whose stored key is current is dropped without a render",
          "[snapshotthumbnails][worker]") {
    const Rig rig;
    const test::TempDir cacheRoot;
    Collector collector;
    app::ThumbnailWorker worker(
        app::ThumbnailCache(cacheRoot.path()), [](app::ThumbnailResult) {}, false, {},
        collector.callback());
    const std::uint64_t generation = worker.beginSnapshots();
    const SnapshotRecipe recipe{SnapshotId{1}, exposed(1.0F), {}};
    const std::string key = *snapshotThumbnailKey(rig.photo, recipe.state, recipe.negative);

    REQUIRE(worker.requestSnapshotThumbnail(rig.photo, recipe, generation, key));
    REQUIRE(waitUntil([&] { return worker.pendingJobs() == 0; }));
    CHECK(collector.count() == 0);
    // Not rendered, so not cached either.
    CHECK_FALSE(app::ThumbnailCache(cacheRoot.path()).loadSnapshot(key));

    SECTION("another stored key is stale and is made again") {
        REQUIRE(worker.requestSnapshotThumbnail(rig.photo, recipe, generation, "stale-key"));
        REQUIRE(waitUntil([&] { return collector.count() == 1 && worker.pendingJobs() == 0; }));
        CHECK(collector.results()[0].thumbnail.renderKey == key);
    }
    SECTION("no stored key means there is none to match") {
        REQUIRE(worker.requestSnapshotThumbnail(rig.photo, recipe, generation));
        REQUIRE(waitUntil([&] { return collector.count() == 1 && worker.pendingJobs() == 0; }));
        CHECK(collector.results()[0].thumbnail.renderKey == key);
    }
}
