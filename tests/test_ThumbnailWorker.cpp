#include "ThumbnailCache.h"
#include "ThumbnailWorker.h"
#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <Photo.h>
#include <Sidecar.h>

#include <QCoreApplication>
#include <QImage>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The thumbnail worker over a temporary folder of the preview fixture (src/app/ThumbnailWorker.h).

namespace {

namespace fs = std::filesystem;

/// Collects what the worker delivers, from its thread.
class Collector {
public:
    [[nodiscard]] std::function<void(ThumbnailResult)> callback() {
        return [this](ThumbnailResult result) {
            const std::scoped_lock lock(mutex_);
            results_.push_back(std::move(result));
        };
    }

    [[nodiscard]] std::vector<ThumbnailResult> results() const {
        const std::scoped_lock lock(mutex_);
        return results_;
    }

    [[nodiscard]] std::size_t count() const {
        const std::scoped_lock lock(mutex_);
        return results_.size();
    }

    void clear() {
        const std::scoped_lock lock(mutex_);
        results_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::vector<ThumbnailResult> results_;
};

/// Waits for a condition, running the event loop as the suite's other tests do.
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

/// Puts the preview fixture in a folder under a name.
fs::path addShot(const test::TempDir& folder, const std::string& name) {
    const fs::path path = folder.file(name);
    fs::copy_file(test::fixture("preview-32x24.dng"), path);
    return path;
}

/// Saves a sidecar for a photograph with a stated exposure.
void saveExposure(const fs::path& path, float exposure) {
    Photo photo = openPhoto(path);
    DevelopState state = photo.state();
    state.settings.tone.exposure = exposure;
    writeSidecar(photo.with(state));
}

/// Mean brightness of an image, 0 to 255.
double brightness(const QImage& image) {
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

/// Finds a result of a shot and kind.
const ThumbnailResult* find(const std::vector<ThumbnailResult>& results, const fs::path& primary,
                            ThumbnailKind kind) {
    const auto found = std::ranges::find_if(
        results, [&](const ThumbnailResult& r) { return r.primary == primary && r.kind == kind; });
    return found == results.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("The worker delivers the embedded preview, then the developed saved state",
          "[thumbnailworker]") {
    const test::TempDir folder;
    const test::TempDir cacheRoot;
    const fs::path plainShot = addShot(folder, "IMG_1.dng");
    const fs::path brightShot = addShot(folder, "IMG_2.dng");
    saveExposure(brightShot, 2.0F);

    Collector collector;
    ThumbnailWorker worker(ThumbnailCache(cacheRoot.path()), collector.callback());
    worker.setShots({plainShot, brightShot});
    REQUIRE(waitUntil([&] { return collector.count() == 4 && worker.pendingJobs() == 0; }));

    const auto results = collector.results();
    // Embedded for everything before any developed one.
    REQUIRE(results[0].kind == ThumbnailKind::Embedded);
    REQUIRE(results[1].kind == ThumbnailKind::Embedded);
    REQUIRE(results[2].kind == ThumbnailKind::Developed);
    REQUIRE(results[3].kind == ThumbnailKind::Developed);

    const auto* plainDeveloped = find(results, plainShot, ThumbnailKind::Developed);
    const auto* brightDeveloped = find(results, brightShot, ThumbnailKind::Developed);
    const auto* embedded = find(results, plainShot, ThumbnailKind::Embedded);
    REQUIRE(plainDeveloped != nullptr);
    REQUIRE(brightDeveloped != nullptr);
    REQUIRE(embedded != nullptr);
    for (const auto& result : results) {
        REQUIRE_FALSE(result.image.isNull());
        REQUIRE(std::max(result.image.width(), result.image.height()) <= ThumbnailCache::maxEdge);
    }
    // The saved exposure shows in the developed thumbnail and in no other.
    REQUIRE(brightness(brightDeveloped->image) > brightness(plainDeveloped->image) + 5.0);
    // Developed is arraw's rendering, not the camera's picture.
    REQUIRE(brightness(plainDeveloped->image) != brightness(embedded->image));
}

TEST_CASE("The second run is served from the cache", "[thumbnailworker]") {
    const test::TempDir folder;
    const test::TempDir cacheRoot;
    const fs::path shot = addShot(folder, "IMG_1.dng");
    saveExposure(shot, 1.0F);
    {
        Collector first;
        ThumbnailWorker worker(ThumbnailCache(cacheRoot.path()), first.callback());
        worker.setShots({shot});
        REQUIRE(waitUntil([&] { return first.count() == 2 && worker.pendingJobs() == 0; }));
    }
    const ThumbnailCache cache(cacheRoot.path());
    REQUIRE(cache.sizeBytes() > 0);
    REQUIRE_FALSE(cache.load(*ThumbnailCache::embeddedKey(shot)).isNull());
    const auto state = readSidecar(shot)->state;
    REQUIRE_FALSE(cache.load(*ThumbnailCache::developedKey(shot, state)).isNull());

    // Same file, same state: both come from the cache entries made above.
    Collector second;
    ThumbnailWorker worker(cache, second.callback());
    worker.setShots({shot});
    REQUIRE(waitUntil([&] { return second.count() == 2 && worker.pendingJobs() == 0; }));
}

TEST_CASE("Visible shots are served first", "[thumbnailworker]") {
    const test::TempDir folder;
    const test::TempDir cacheRoot;
    std::vector<fs::path> shots;
    for (int i = 0; i < 6; ++i) {
        shots.push_back(addShot(folder, "IMG_" + std::to_string(i) + ".dng"));
    }
    Collector collector;
    ThumbnailWorker worker(ThumbnailCache(cacheRoot.path()), collector.callback());
    // Everything is queued while the thread is busy with the first job, then the last two are
    // named visible; they must come before the shots between.
    worker.setVisible({shots[4], shots[5]});
    worker.setShots(shots);
    REQUIRE(waitUntil([&] { return collector.count() == 12 && worker.pendingJobs() == 0; }));
    const auto results = collector.results();
    const auto position = [&](const fs::path& shot, ThumbnailKind kind) {
        for (std::size_t i = 0; i < results.size(); ++i) {
            if (results[i].primary == shot && results[i].kind == kind) {
                return i;
            }
        }
        return results.size();
    };
    // The thread may have taken shots[0] before anything else; the visible ones precede 1 to 3.
    for (const auto kind : {ThumbnailKind::Embedded, ThumbnailKind::Developed}) {
        for (const int visible : {4, 5}) {
            for (const int other : {1, 2, 3}) {
                REQUIRE(position(shots[visible], kind) < position(shots[other], kind));
            }
        }
    }
}

TEST_CASE("A new folder drops the old jobs and their results", "[thumbnailworker]") {
    const test::TempDir oldFolder;
    const test::TempDir newFolder;
    const test::TempDir cacheRoot;
    std::vector<fs::path> oldShots;
    for (int i = 0; i < 60; ++i) {
        oldShots.push_back(addShot(oldFolder, "OLD_" + std::to_string(i) + ".dng"));
    }
    const fs::path newShot = addShot(newFolder, "NEW_1.dng");

    Collector collector;
    ThumbnailWorker worker(ThumbnailCache(cacheRoot.path()), collector.callback());
    const std::uint64_t first = worker.setShots(oldShots);
    const std::uint64_t second = worker.setShots({newShot});
    REQUIRE(second != first);
    REQUIRE(worker.generation() == second);
    REQUIRE(waitUntil([&] { return worker.pendingJobs() == 0; }));

    const auto results = collector.results();
    // Whatever of the old folder got through was delivered before the switch, as its own
    // generation; most of it never ran.
    REQUIRE(find(results, newShot, ThumbnailKind::Embedded) != nullptr);
    REQUIRE(find(results, newShot, ThumbnailKind::Developed) != nullptr);
    std::size_t oldDelivered = 0;
    for (const auto& result : results) {
        if (result.primary != newShot) {
            ++oldDelivered;
            REQUIRE(result.generation == first);
        }
    }
    REQUIRE(oldDelivered < 2 * oldShots.size());
}

TEST_CASE("Invalidating a shot develops its saved state afresh", "[thumbnailworker]") {
    const test::TempDir folder;
    const test::TempDir cacheRoot;
    const fs::path shot = addShot(folder, "IMG_1.dng");
    const fs::path other = addShot(folder, "IMG_2.dng");

    Collector collector;
    ThumbnailWorker worker(ThumbnailCache(cacheRoot.path()), collector.callback());
    worker.setShots({shot, other});
    REQUIRE(waitUntil([&] { return collector.count() == 4 && worker.pendingJobs() == 0; }));
    const double before =
        brightness(find(collector.results(), shot, ThumbnailKind::Developed)->image);

    // Another program, or this one's Save, writes the sidecar; the strip says so.
    saveExposure(shot, 2.0F);
    collector.clear();
    worker.invalidate(shot);
    REQUIRE(waitUntil([&] { return collector.count() == 1 && worker.pendingJobs() == 0; }));
    const auto results = collector.results();
    REQUIRE(results[0].primary == shot);
    REQUIRE(results[0].kind == ThumbnailKind::Developed);
    REQUIRE(brightness(results[0].image) > before + 5.0);

    // A shot the worker does not know is nothing to do.
    worker.invalidate(folder.file("stranger.dng"));
    QThread::msleep(50);
    REQUIRE(worker.pendingJobs() == 0);
    REQUIRE(collector.count() == 1);
}

TEST_CASE("A file that cannot be read gives no thumbnail and no failure", "[thumbnailworker]") {
    const test::TempDir folder;
    const test::TempDir cacheRoot;
    const fs::path broken = folder.file("IMG_broken.dng");
    {
        std::ofstream(broken) << "not a raw file";
    }
    const fs::path good = addShot(folder, "IMG_good.dng");
    Collector collector;
    ThumbnailWorker worker(ThumbnailCache(cacheRoot.path()), collector.callback());
    worker.setShots({broken, good});
    REQUIRE(waitUntil([&] { return worker.pendingJobs() == 0; }));
    const auto results = collector.results();
    REQUIRE(find(results, broken, ThumbnailKind::Developed) == nullptr);
    REQUIRE(find(results, good, ThumbnailKind::Developed) != nullptr);
}
