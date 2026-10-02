#include "PreviewRenderer.h"
#include "support/TestImages.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

using namespace arraw;
using namespace std::chrono_literals;

namespace {

/// Bound on every wait, so a hang fails the test instead of the run.
constexpr auto timeout = 60s;

/// Results delivered by a renderer's callback, collected for the test thread.
class Collector {
public:
    /// Callback to give the renderer.
    [[nodiscard]] std::function<void(app::PreviewResult)> callback() {
        return [this](app::PreviewResult result) {
            {
                const std::scoped_lock lock(mutex_);
                if (closed_) {
                    ++lateCalls_;
                }
                results_.push_back(std::move(result));
            }
            changed_.notify_all();
        };
    }

    /// Waits until a result for @p id, or a later one, has arrived.
    [[nodiscard]] bool waitFor(std::uint64_t id) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(
            lock, timeout, [&] { return !results_.empty() && results_.back().request >= id; });
    }

    /// Makes any later callback count as late.
    void close() {
        const std::scoped_lock lock(mutex_);
        closed_ = true;
    }

    [[nodiscard]] std::vector<app::PreviewResult> results() {
        const std::scoped_lock lock(mutex_);
        return results_;
    }

    [[nodiscard]] int lateCalls() {
        const std::scoped_lock lock(mutex_);
        return lateCalls_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<app::PreviewResult> results_;
    bool closed_ = false;
    int lateCalls_ = 0;
};

std::shared_ptr<const ImageBuffer> makeSource() {
    return std::make_shared<const ImageBuffer>(
        test::rainbow({256, 128}, PixelFormat::RgbaF32, workingEncoding));
}

} // namespace

TEST_CASE("A renderer told to use the CPU says so and needs no fallback reason", "[app][preview]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeSource());

    const std::uint64_t id = renderer.request({}, {100, 100}, 1.0);

    REQUIRE(collector.waitFor(id));
    const auto results = collector.results();
    REQUIRE(results.back().image.has_value());
    REQUIRE_FALSE(results.back().onGpu);
    REQUIRE(results.back().fallbackReason.empty());
}

TEST_CASE("An automatic renderer without a usable GPU falls back and says why", "[app][preview]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback());
    renderer.setSource(makeSource());

    const std::uint64_t first = renderer.request({}, {100, 100}, 1.0);
    REQUIRE(collector.waitFor(first));
    const std::uint64_t second = renderer.request({}, {64, 64}, 1.0);
    REQUIRE(collector.waitFor(second));

    const auto results = collector.results();
    const app::PreviewResult& last = results.back();
    REQUIRE(last.request == second);
    REQUIRE(last.error.empty());
    REQUIRE(last.image.has_value());
    // This suite runs without a QGuiApplication, so there is never a device here;
    // where there is one the preview is on it and nothing is to be explained.
    if (last.onGpu) {
        REQUIRE(!last.deviceName.empty());
        REQUIRE(last.fallbackReason.empty());
    } else {
        REQUIRE(last.deviceName.empty());
        REQUIRE(!last.fallbackReason.empty());
    }
}

TEST_CASE("A request renders and is answered with its id", "[app][preview]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback());
    renderer.setSource(makeSource());

    const std::uint64_t id = renderer.request({}, {100, 100}, 1.0);

    REQUIRE(collector.waitFor(id));
    const auto results = collector.results();
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].request == id);
    REQUIRE(results[0].error.empty());
    REQUIRE(results[0].image.has_value());
    REQUIRE(results[0].image->width() <= 100);
    REQUIRE(results[0].image->height() <= 100);
}

TEST_CASE("The newest request wins, and results never arrive out of order", "[app][preview]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback());
    renderer.setSource(makeSource());

    std::uint64_t last = 0;
    for (int i = 0; i < 50; ++i) {
        DevelopState state;
        state.settings.tone.exposure = static_cast<float>(i) * 0.02F;
        last = renderer.request(state, {128, 64}, 1.0);
    }

    REQUIRE(collector.waitFor(last));
    const auto results = collector.results();
    REQUIRE(!results.empty());
    REQUIRE(results.size() <= 50);
    REQUIRE(results.back().request == last);
    for (std::size_t i = 1; i < results.size(); ++i) {
        REQUIRE(results[i].request > results[i - 1].request);
    }
}

TEST_CASE("A failed render yields an error and the worker carries on", "[app][preview]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback());
    renderer.setSource(makeSource());

    const std::uint64_t bad = renderer.request({}, {0, 0}, 1.0);
    REQUIRE(collector.waitFor(bad));
    {
        const auto results = collector.results();
        REQUIRE(results.back().request == bad);
        REQUIRE(!results.back().image.has_value());
        REQUIRE(!results.back().error.empty());
    }

    const std::uint64_t good = renderer.request({}, {64, 64}, 1.0);
    REQUIRE(collector.waitFor(good));
    const auto results = collector.results();
    REQUIRE(results.back().request == good);
    REQUIRE(results.back().image.has_value());
    REQUIRE(results.back().error.empty());
}

TEST_CASE("A request before any source yields an error result", "[app][preview]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback());

    const std::uint64_t id = renderer.request({}, {64, 64}, 1.0);

    REQUIRE(collector.waitFor(id));
    const auto results = collector.results();
    REQUIRE(results.back().request == id);
    REQUIRE(!results.back().image.has_value());
    REQUIRE(!results.back().error.empty());
}

TEST_CASE("Replacing the source drops requests not yet started", "[app][preview]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback());
    renderer.setSource(makeSource());
    // Occupy the worker, then queue a request and drop it with the source.
    const std::uint64_t first = renderer.request({}, {128, 64}, 1.0);
    const std::uint64_t dropped = renderer.request({}, {128, 64}, 1.0);
    renderer.setSource(makeSource());
    const std::uint64_t kept = renderer.request({}, {128, 64}, 1.0);

    REQUIRE(collector.waitFor(kept));
    for (const auto& result : collector.results()) {
        // The one in progress may be delivered; the dropped one only if it had started.
        REQUIRE((result.request == first || result.request == dropped || result.request == kept));
    }
    REQUIRE(collector.results().back().request == kept);
}

TEST_CASE("Destroying the renderer with work pending ends the worker for good", "[app][preview]") {
    Collector collector;
    const auto begin = std::chrono::steady_clock::now();
    {
        app::PreviewRenderer renderer(collector.callback());
        renderer.setSource(makeSource());
        for (int i = 0; i < 20; ++i) {
            (void)renderer.request({}, {128, 64}, 1.0);
        }
    }
    collector.close();
    const std::size_t atDestruction = collector.results().size();

    // The worker was joined: nothing can arrive any more. A bounded wait for
    // one more result must time out, which is the point of the test, so it is
    // checked through the count rather than waited for.
    REQUIRE(collector.lateCalls() == 0);
    REQUIRE(collector.results().size() == atDestruction);
    REQUIRE(std::chrono::steady_clock::now() - begin < timeout);
}
