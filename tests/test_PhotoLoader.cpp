#include "PhotoLoader.h"
#include "ThumbnailCache.h"
#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <ColorEncoding.h>
#include <ImageBuffer.h>
#include <Progress.h>

#include <QColor>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The window's decode and camera preview threads (src/app/PhotoLoader.h, ADR 043).

namespace {

using namespace std::chrono_literals;

/// Collects what the loader delivers, from its threads.
struct Collector {
    std::mutex mutex;
    std::vector<DecodedPhoto> decoded;
    std::vector<CameraPreview> previews;
    std::vector<std::uint64_t> progressRequests;

    [[nodiscard]] PhotoLoader::DecodedCallback onDecoded() {
        return [this](DecodedPhoto result) {
            const std::scoped_lock lock(mutex);
            decoded.push_back(std::move(result));
        };
    }

    [[nodiscard]] PhotoLoader::PreviewCallback onPreview() {
        return [this](CameraPreview preview) {
            const std::scoped_lock lock(mutex);
            previews.push_back(std::move(preview));
        };
    }

    [[nodiscard]] PhotoLoader::ProgressCallback onProgress() {
        return [this](std::uint64_t request, const Progress& /*progress*/) {
            const std::scoped_lock lock(mutex);
            progressRequests.push_back(request);
        };
    }

    [[nodiscard]] std::size_t decodedCount() {
        const std::scoped_lock lock(mutex);
        return decoded.size();
    }
};

/// Waits for a condition, without an event loop: the loader needs none.
bool waitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 20s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        QThread::msleep(2);
    }
    return true;
}

/// A decoder that holds every file named "slow" until its channel is cancelled, and makes a
/// one-pixel image of any other.
struct ControlledDecoder {
    std::atomic<int> slowStarted{0};
    std::atomic<int> slowCancelled{0};

    [[nodiscard]] PhotoLoader::Decoder decoder() {
        return [this](const std::filesystem::path& path, ProgressChannel& channel) {
            if (path.filename() == "slow") {
                ++slowStarted;
                while (!channel.cancelled()) {
                    QThread::msleep(1);
                }
                ++slowCancelled;
                throw Cancelled();
            }
            if (path.filename() == "broken") {
                throw std::runtime_error("not a photograph");
            }
            return ImageBuffer({1, 1}, PixelFormat::RgbaF32, workingEncoding);
        };
    }
};

} // namespace

TEST_CASE("The loader decodes a photograph on its thread and reports its progress",
          "[app][loader]") {
    const test::TempDir cacheRoot;
    Collector collector;
    PhotoLoader loader(ThumbnailCache(cacheRoot.path()), collector.onDecoded(),
                       collector.onPreview(), collector.onProgress());
    const std::uint64_t request = loader.decode(test::fixture("bayer-32x24.dng"));
    REQUIRE(waitUntil([&] { return collector.decodedCount() == 1; }));
    const std::scoped_lock lock(collector.mutex);
    const DecodedPhoto& result = collector.decoded.front();
    CHECK(result.request == request);
    CHECK(result.path == test::fixture("bayer-32x24.dng"));
    REQUIRE(result.decoded != nullptr);
    CHECK(result.decoded->size() == ImageSize{32, 24});
    CHECK(result.error.empty());
    REQUIRE_FALSE(collector.progressRequests.empty());
    for (const std::uint64_t reported : collector.progressRequests) {
        CHECK(reported == request);
    }
}

TEST_CASE("A decode that fails delivers why, and no pixels", "[app][loader]") {
    const test::TempDir cacheRoot;
    Collector collector;
    ControlledDecoder control;
    PhotoLoader loader(ThumbnailCache(cacheRoot.path()), collector.onDecoded(), {}, {},
                       control.decoder());
    const std::uint64_t request = loader.decode("broken");
    REQUIRE(waitUntil([&] { return collector.decodedCount() == 1; }));
    const std::scoped_lock lock(collector.mutex);
    CHECK(collector.decoded.front().request == request);
    CHECK(collector.decoded.front().decoded == nullptr);
    CHECK(collector.decoded.front().error == "not a photograph");
}

TEST_CASE("A newer decode cancels the one in progress, which delivers nothing", "[app][loader]") {
    const test::TempDir cacheRoot;
    Collector collector;
    ControlledDecoder control;
    PhotoLoader loader(ThumbnailCache(cacheRoot.path()), collector.onDecoded(), {}, {},
                       control.decoder());
    loader.decode("slow");
    REQUIRE(waitUntil([&] { return control.slowStarted == 1; }));
    // Queued behind the slow one, then replaced before it could start: never decoded.
    loader.decode("replaced");
    const std::uint64_t newest = loader.decode("newest");
    REQUIRE(waitUntil([&] { return collector.decodedCount() == 1; }));
    CHECK(control.slowCancelled == 1);
    std::this_thread::sleep_for(50ms);
    const std::scoped_lock lock(collector.mutex);
    REQUIRE(collector.decoded.size() == 1);
    CHECK(collector.decoded.front().request == newest);
    CHECK(collector.decoded.front().path == "newest");
}

TEST_CASE("Cancelling the decode drops it, and closing the loader stops it", "[app][loader]") {
    const test::TempDir cacheRoot;
    Collector collector;
    ControlledDecoder control;
    {
        PhotoLoader loader(ThumbnailCache(cacheRoot.path()), collector.onDecoded(), {}, {},
                           control.decoder());
        loader.decode("slow");
        REQUIRE(waitUntil([&] { return control.slowStarted == 1; }));
        loader.cancelDecode();
        REQUIRE(waitUntil([&] { return control.slowCancelled == 1; }));

        loader.decode("slow");
        REQUIRE(waitUntil([&] { return control.slowStarted == 2; }));
        // The destructor cancels the decode rather than waiting for it to finish.
    }
    CHECK(control.slowCancelled == 2);
    CHECK(collector.decodedCount() == 0);
}

TEST_CASE("The loader reads a camera preview without holding up decodes", "[app][loader]") {
    const test::TempDir cacheRoot;
    Collector collector;
    ControlledDecoder control;
    PhotoLoader loader(ThumbnailCache(cacheRoot.path()), collector.onDecoded(),
                       collector.onPreview(), {}, control.decoder());
    loader.decode("slow");
    REQUIRE(waitUntil([&] { return control.slowStarted == 1; }));
    // On its own thread: the decode in progress does not hold it up.
    const std::uint64_t request = loader.readCameraPreview(test::fixture("preview-32x24.dng"));
    REQUIRE(waitUntil([&] {
        const std::scoped_lock lock(collector.mutex);
        return !collector.previews.empty();
    }));
    loader.cancelDecode();
    const std::scoped_lock lock(collector.mutex);
    const CameraPreview& preview = collector.previews.front();
    CHECK(preview.request == request);
    REQUIRE_FALSE(preview.image.isNull());
    // The fixture's preview is flat magenta.
    CHECK(preview.image.pixelColor(0, 0) == QColor(255, 0, 255));
}
