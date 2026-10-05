#include "DisplayImage.h"
#include "PreviewRenderer.h"
#include "support/TestImages.h"

#include <CurveHistogram.h>
#include <Develop.h>
#include <DevelopSettings.h>
#include <ImagePyramid.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
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
                if (result.curveHistogram) {
                    histograms_.push_back(std::move(result));
                } else if (!result.image && result.background) {
                    backgrounds_.push_back(std::move(result));
                } else {
                    results_.push_back(std::move(result));
                }
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

    /// Waits until a refreshed fallback for @p id, or a later one, has arrived.
    [[nodiscard]] bool waitForBackground(std::uint64_t id) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] {
            return !backgrounds_.empty() && backgrounds_.back().request >= id;
        });
    }

    /// Waits until a recounted curve histogram for @p id, or a later one, has arrived.
    [[nodiscard]] bool waitForHistogram(std::uint64_t id) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] {
            return !histograms_.empty() && histograms_.back().request >= id;
        });
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

    /// Refreshed fallbacks, apart from the renders.
    [[nodiscard]] std::vector<app::PreviewResult> backgrounds() {
        const std::scoped_lock lock(mutex_);
        return backgrounds_;
    }

    /// Recounted curve histograms, apart from the renders.
    [[nodiscard]] std::vector<app::PreviewResult> histograms() {
        const std::scoped_lock lock(mutex_);
        return histograms_;
    }

    [[nodiscard]] int lateCalls() {
        const std::scoped_lock lock(mutex_);
        return lateCalls_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<app::PreviewResult> results_;
    std::vector<app::PreviewResult> backgrounds_;
    std::vector<app::PreviewResult> histograms_;
    bool closed_ = false;
    int lateCalls_ = 0;
};

std::shared_ptr<const ImageBuffer> makeSource() {
    return std::make_shared<const ImageBuffer>(
        test::rainbow({256, 128}, PixelFormat::RgbaF32, workingEncoding));
}

/// A source far larger than a preview, as a photograph is.
std::shared_ptr<const ImageBuffer> makeLargeSource() {
    return std::make_shared<const ImageBuffer>(
        test::rainbow({2048, 1024}, PixelFormat::RgbaF32, workingEncoding));
}

} // namespace

TEST_CASE("A large source in a small viewport is developed from a reduced level",
          "[app][preview][pyramid]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeLargeSource());

    const std::uint64_t id = renderer.request({}, app::PreviewView::wholeFrame({512, 512}));

    REQUIRE(collector.waitFor(id));
    const auto results = collector.results();
    REQUIRE(results.back().error.empty());
    REQUIRE(results.back().image.has_value());
    // 2048x1024 into 512x512 is 512x256, which level 2 is exactly.
    REQUIRE(results.back().level == 2);
    REQUIRE(results.back().image->width() <= 512);
    REQUIRE(results.back().image->height() <= 512);
}

TEST_CASE("A reduced level is denoised with radii divided by its scale",
          "[app][preview][pyramid][denoise]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    const auto source = makeLargeSource();
    renderer.setSource(source);
    DevelopState state;
    state.settings.noiseReduction.luminance = 80.0F;
    state.settings.noiseReduction.color = 60.0F;

    const std::uint64_t id = renderer.request(state, app::PreviewView::wholeFrame({512, 512}));
    REQUIRE(collector.waitFor(id));
    const app::PreviewResult result = collector.results().back();
    REQUIRE(result.error.empty());
    REQUIRE(result.level == 2);

    // The same level developed by hand, whose pixels know their scale, and a
    // copy that claims to be full resolution.
    const ImageBuffer level2 = halved(halved(*source));
    REQUIRE(level2.pixelScale() == 4.0 * source->pixelScale());
    const RenderRequest request = app::previewRequest({512, 512});
    const QImage expected = app::toDisplayImage(develop(level2, state, request));
    ImageBuffer claimsFull = level2.clone();
    claimsFull.setPixelScale(1.0);
    const QImage unscaled = app::toDisplayImage(develop(claimsFull, state, request));
    REQUIRE(*result.image == expected);
    REQUIRE(*result.image != unscaled);
}

TEST_CASE("A viewport as large as the source is developed from the source itself",
          "[app][preview][pyramid]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeLargeSource());

    const std::uint64_t id = renderer.request({}, app::PreviewView::wholeFrame({2048, 1024}));

    REQUIRE(collector.waitFor(id));
    const auto results = collector.results();
    REQUIRE(results.back().error.empty());
    REQUIRE(results.back().level == 0);
    REQUIRE(results.back().image->width() == 2048);
}

TEST_CASE("The pyramid stops before its levels become too small to be of use",
          "[app][preview][pyramid]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeLargeSource());

    const std::uint64_t id = renderer.request({}, app::PreviewView::wholeFrame({16, 16}));

    REQUIRE(collector.waitFor(id));
    const auto results = collector.results();
    REQUIRE(results.back().error.empty());
    // Level 3 is 256x128; the one below would have a long edge under 256.
    REQUIRE(results.back().level == 3);
    REQUIRE(results.back().image->width() <= 16);
}

TEST_CASE("A new source starts a new pyramid", "[app][preview][pyramid]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeLargeSource());
    const std::uint64_t first = renderer.request({}, app::PreviewView::wholeFrame({512, 512}));
    REQUIRE(collector.waitFor(first));

    renderer.setSource(makeSource()); // 256x128: nothing to reduce.
    const std::uint64_t second = renderer.request({}, app::PreviewView::wholeFrame({128, 128}));

    REQUIRE(collector.waitFor(second));
    const auto results = collector.results();
    REQUIRE(results.back().request == second);
    REQUIRE(results.back().error.empty());
    REQUIRE(results.back().level == 0);
    REQUIRE(results.back().image->width() == 128);
}

TEST_CASE("A renderer told to use the CPU says so and needs no fallback reason", "[app][preview]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeSource());

    const std::uint64_t id = renderer.request({}, app::PreviewView::wholeFrame({100, 100}));

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

    const std::uint64_t first = renderer.request({}, app::PreviewView::wholeFrame({100, 100}));
    REQUIRE(collector.waitFor(first));
    const std::uint64_t second = renderer.request({}, app::PreviewView::wholeFrame({64, 64}));
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

    const std::uint64_t id = renderer.request({}, app::PreviewView::wholeFrame({100, 100}));

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
        last = renderer.request(state, app::PreviewView::wholeFrame({128, 64}));
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

    const std::uint64_t bad = renderer.request({}, app::PreviewView::wholeFrame({0, 0}));
    REQUIRE(collector.waitFor(bad));
    {
        const auto results = collector.results();
        REQUIRE(results.back().request == bad);
        REQUIRE(!results.back().image.has_value());
        REQUIRE(!results.back().error.empty());
    }

    const std::uint64_t good = renderer.request({}, app::PreviewView::wholeFrame({64, 64}));
    REQUIRE(collector.waitFor(good));
    const auto results = collector.results();
    REQUIRE(results.back().request == good);
    REQUIRE(results.back().image.has_value());
    REQUIRE(results.back().error.empty());
}

TEST_CASE("A request before any source yields an error result", "[app][preview]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback());

    const std::uint64_t id = renderer.request({}, app::PreviewView::wholeFrame({64, 64}));

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
    const std::uint64_t first = renderer.request({}, app::PreviewView::wholeFrame({128, 64}));
    const std::uint64_t dropped = renderer.request({}, app::PreviewView::wholeFrame({128, 64}));
    renderer.setSource(makeSource());
    const std::uint64_t kept = renderer.request({}, app::PreviewView::wholeFrame({128, 64}));

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
            (void)renderer.request({}, app::PreviewView::wholeFrame({128, 64}));
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

namespace {

/// @brief Renders one request and returns its result, requiring it to succeed.
app::PreviewResult renderOne(app::PreviewRenderer& renderer, Collector& collector,
                             const DevelopState& state, QSize viewport) {
    const std::uint64_t id = renderer.request(state, app::PreviewView::wholeFrame(viewport));
    REQUIRE(collector.waitFor(id));
    app::PreviewResult result = collector.results().back();
    REQUIRE(result.request == id);
    REQUIRE(result.error.empty());
    REQUIRE(result.image.has_value());
    return result;
}

/// @brief Renders a request on a renderer that has never seen another.
QImage freshImage(const DevelopState& state, QSize viewport) {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeSource());
    return *renderOne(renderer, collector, state, viewport).image;
}

DevelopState stateWith(float exposure, double straighten, float vignette = 0.0F,
                       float colorNoise = 0.0F) {
    DevelopSettings settings;
    settings.tone.exposure = exposure;
    settings.geometry.straighten = straighten;
    settings.effects.vignette.amount = vignette;
    settings.noiseReduction.color = colorNoise;
    return DevelopState{settings};
}

} // namespace

TEST_CASE("An edit resumes from the newest checkpoint it can still use", "[app][preview][resume]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeSource());
    const QSize wide{200, 200};
    const QSize narrow{150, 150};

    struct Step {
        const char* label;
        DevelopState state;
        QSize viewport;
        std::optional<Stage> resumed;
    };
    const std::vector<Step> steps{
        {"the first render develops from the level", stateWith(0.0F, 0.0), wide, std::nullopt},
        {"a tone change resumes from nothing", stateWith(0.5F, 0.0), wide, std::nullopt},
        {"a viewport change resumes from the geometry", stateWith(0.5F, 0.0), narrow,
         Stage::Geometry},
        {"a straighten change resumes from the pointwise result", stateWith(0.5F, 5.0), narrow,
         Stage::Pointwise},
        {"the same request again resumes from the resize", stateWith(0.5F, 5.0), narrow,
         Stage::Resize},
        {"a tone change after that resumes from nothing", stateWith(-0.5F, 5.0), narrow,
         std::nullopt},
        {"a viewport change after that resumes from the geometry", stateWith(-0.5F, 5.0), wide,
         Stage::Geometry},
        {"a vignette change resumes from the resize", stateWith(-0.5F, 5.0, -40.0F), wide,
         Stage::Resize},
        {"a viewport change with a vignette resumes from the geometry",
         stateWith(-0.5F, 5.0, -40.0F), narrow, Stage::Geometry},
        {"a vignette turned off resumes from the resize", stateWith(-0.5F, 5.0), narrow,
         Stage::Resize},
        {"noise reduction turned on develops from the level", stateWith(-0.5F, 5.0, 0.0F, 40.0F),
         narrow, std::nullopt},
        {"a tone change with noise reduction resumes from the denoise result",
         stateWith(0.25F, 5.0, 0.0F, 40.0F), narrow, Stage::Denoise},
        {"a stronger noise reduction develops from the level again",
         stateWith(0.25F, 5.0, 0.0F, 70.0F), narrow, std::nullopt},
        {"noise reduction turned off develops from the level, keeping no denoise result",
         stateWith(0.25F, 5.0), narrow, std::nullopt},
        {"a tone change then resumes from nothing again", stateWith(0.5F, 5.0), narrow,
         std::nullopt},
    };
    for (const Step& step : steps) {
        INFO(step.label);
        const auto result = renderOne(renderer, collector, step.state, step.viewport);
        REQUIRE(result.resumedFrom == step.resumed);
        REQUIRE_FALSE(result.onGpu);
        // Whatever it resumed from, the picture is what a renderer with no
        // history makes of the same request, bit for bit.
        REQUIRE(*result.image == freshImage(step.state, step.viewport));
    }
}

TEST_CASE("A new source starts with no checkpoints", "[app][preview][resume]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeSource());
    const DevelopState state = stateWith(0.5F, 3.0);
    REQUIRE_FALSE(renderOne(renderer, collector, state, {200, 200}).resumedFrom.has_value());

    // Same size and settings, other pixels: only the renderer knows to start again.
    ImageBuffer changed = test::rainbow({256, 128}, PixelFormat::RgbaF32, workingEncoding);
    changed.samples<float>()[0] += 0.5F;
    renderer.setSource(std::make_shared<const ImageBuffer>(std::move(changed)));
    REQUIRE_FALSE(renderOne(renderer, collector, state, {200, 200}).resumedFrom.has_value());
}

TEST_CASE("A region request renders that part of the frame at the size asked for",
          "[app][preview][region]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeLargeSource());

    const QRect region(512, 256, 256, 128);
    const std::uint64_t id =
        renderer.request({}, {.region = region, .outputSize = {128, 64}, .devicePixelRatio = 2.0});

    REQUIRE(collector.waitFor(id));
    const auto result = collector.results().back();
    REQUIRE(result.error.empty());
    REQUIRE(result.image.has_value());
    REQUIRE(result.image->size() == QSize(128, 64));
    REQUIRE(result.image->devicePixelRatio() == 2.0);
    // Level 1 halves the frame; the region falls on its pixel boundaries, so
    // what was rendered is exactly what was asked for.
    REQUIRE(result.region == QRectF(512.0 / 2048, 256.0 / 1024, 256.0 / 2048, 128.0 / 1024));
    REQUIRE(result.frame == QSize(2048, 1024));
    // The region is 256 pixels wide at level 0 and 128 at level 1, which still
    // covers the 128 wanted; level 2 would not.
    REQUIRE(result.level == 1);
}

TEST_CASE("A region off the level's pixels reports where it was actually rendered",
          "[app][preview][region]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeLargeSource());

    // Odd edges at full resolution; level 1 has pixels twice as wide, so the
    // engine snaps outward to even ones: 513 -> 512, 513 + 255 = 768 stays.
    const std::uint64_t id = renderer.request(
        {},
        {.region = QRect(513, 257, 255, 127), .outputSize = {128, 64}, .devicePixelRatio = 1.0});

    REQUIRE(collector.waitFor(id));
    const auto result = collector.results().back();
    REQUIRE(result.error.empty());
    REQUIRE(result.level == 1);
    REQUIRE(result.region.left() == Catch::Approx(512.0 / 2048));
    REQUIRE(result.region.top() == Catch::Approx(256.0 / 1024));
    REQUIRE(result.region.right() == Catch::Approx(768.0 / 2048));
    REQUIRE(result.region.bottom() == Catch::Approx(384.0 / 1024));
}

TEST_CASE("A small region at one image pixel per output pixel is developed from level 0",
          "[app][preview][region][pyramid]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeLargeSource());

    const std::uint64_t id = renderer.request(
        {},
        {.region = QRect(100, 100, 200, 100), .outputSize = {200, 100}, .devicePixelRatio = 1.0});

    REQUIRE(collector.waitFor(id));
    const auto result = collector.results().back();
    REQUIRE(result.error.empty());
    REQUIRE(result.level == 0);
    REQUIRE(result.image->size() == QSize(200, 100));
}

TEST_CASE("A request without a region reports the whole frame", "[app][preview][region]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeLargeSource());

    const std::uint64_t id = renderer.request({}, app::PreviewView::wholeFrame({512, 512}));

    REQUIRE(collector.waitFor(id));
    const auto result = collector.results().back();
    REQUIRE(result.region == QRectF(0.0, 0.0, 1.0, 1.0));
    REQUIRE(result.frame == QSize(2048, 1024));
}

TEST_CASE("A region outside the frame is reported as a failure", "[app][preview][region]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeLargeSource());

    const std::uint64_t id = renderer.request(
        {},
        {.region = QRect(2000, 0, 100, 100), .outputSize = {100, 100}, .devicePixelRatio = 1.0});

    REQUIRE(collector.waitFor(id));
    const auto result = collector.results().back();
    REQUIRE_FALSE(result.error.empty());
    REQUIRE_FALSE(result.image.has_value());
}

TEST_CASE("A region carries the last whole frame beneath it", "[app][preview][region]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeLargeSource());
    auto id = renderer.request({}, app::PreviewView::wholeFrame({1024, 1024}));
    REQUIRE(collector.waitFor(id));
    const QImage whole = *collector.results().back().image;

    app::PreviewView view{.region = QRect(100, 100, 200, 100), .outputSize = {200, 100}};
    id = renderer.request({}, view);
    REQUIRE(collector.waitFor(id));
    const auto zoomed = collector.results().back();
    REQUIRE(zoomed.background.has_value());
    REQUIRE(zoomed.background->cacheKey() == whole.cacheKey());

    view.region = QRect(200, 100, 200, 100);
    id = renderer.request({}, view);
    REQUIRE(collector.waitFor(id));
    const auto panned = collector.results().back();
    REQUIRE(panned.background->cacheKey() == whole.cacheKey());
    REQUIRE(panned.resumedFrom == Stage::Geometry);
    // Nothing was out of date, so nothing followed.
    REQUIRE(collector.backgrounds().empty());
}

TEST_CASE("An edit shows the earlier fallback until a refreshed one follows",
          "[app][preview][region]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeLargeSource());
    const app::PreviewView view{.region = QRect(100, 100, 200, 100), .outputSize = {200, 100}};
    auto id = renderer.request({}, view);
    REQUIRE(collector.waitFor(id));
    // Nothing was rendered of the whole frame yet.
    REQUIRE_FALSE(collector.results().back().background.has_value());
    REQUIRE(collector.waitForBackground(id));
    const QImage first = *collector.backgrounds().back().background;
    REQUIRE(first.size() == QSize(1024, 512));

    DevelopState edited;
    edited.settings.tone.exposure = 1.0F;
    id = renderer.request(edited, view);
    REQUIRE(collector.waitFor(id));
    REQUIRE(collector.results().back().background->cacheKey() == first.cacheKey());
    REQUIRE(collector.waitForBackground(id));
    REQUIRE(*collector.backgrounds().back().background != first);

    // A geometry edit moves the frame under the fallback, which is dropped.
    edited.settings.geometry.straighten = 5.0;
    id = renderer.request(edited, view);
    REQUIRE(collector.waitFor(id));
    REQUIRE_FALSE(collector.results().back().background.has_value());
    REQUIRE(collector.waitForBackground(id));

    renderer.setSource(makeSource());
    id = renderer.request(
        {}, app::PreviewView{.region = QRect(0, 0, 100, 100), .outputSize = {100, 100}});
    REQUIRE(collector.waitFor(id));
    REQUIRE_FALSE(collector.results().back().background.has_value());
    REQUIRE(collector.waitForBackground(id));
    REQUIRE(collector.backgrounds().back().background->size() == QSize(256, 128));
}

TEST_CASE("Desktop CPU preference overrides automatic preview rendering",
          "[app][preview][settings]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Auto,
                                  {.cpuOnly = true, .gpu = std::nullopt});
    renderer.setSource(makeSource());
    const auto id = renderer.request({}, app::PreviewView::wholeFrame({128, 64}));
    REQUIRE(collector.waitFor(id));
    const auto result = collector.results().back();
    REQUIRE(result.image);
    REQUIRE(result.error.empty());
    REQUIRE_FALSE(result.onGpu);
    REQUIRE(result.fallbackReason.empty());
}

TEST_CASE("The curve histogram is counted once requests pause", "[app][preview][histogram]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setCurveHistogramWanted(true);
    const auto source = makeLargeSource();
    renderer.setSource(source);
    DevelopState state;
    state.settings.tone.exposure = 0.4F;
    const auto id = renderer.request(state, app::PreviewView::wholeFrame({300, 300}));
    REQUIRE(collector.waitForHistogram(id));

    const auto histograms = collector.histograms();
    REQUIRE(histograms.size() == 1);
    const app::PreviewResult& delivered = histograms.back();
    REQUIRE(delivered.request == id);
    REQUIRE_FALSE(delivered.image.has_value());
    REQUIRE_FALSE(delivered.background.has_value());
    // The level that covers the CPU's own histogram request, whatever the
    // view's: 2048x1024 fits 512 at level 2, not at level 1 as the GPU's 1024 would.
    const ImageBuffer level2 = halved(halved(*source));
    REQUIRE(*delivered.curveHistogram ==
            curveHistogram(level2, state, app::cpuCurveHistogramRequest));
    REQUIRE(*delivered.curveHistogram != curveHistogram(halved(*source), state));
}

TEST_CASE("A curve edit keeps the histogram, an exposure edit recounts it",
          "[app][preview][histogram]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setCurveHistogramWanted(true);
    renderer.setSource(makeSource());
    const app::PreviewView view = app::PreviewView::wholeFrame({200, 200});
    DevelopState state;
    auto id = renderer.request(state, view);
    REQUIRE(collector.waitForHistogram(id));
    const CurveHistogram first = *collector.histograms().back().curveHistogram;

    // A curve drag: several renders, each followed by a pause.
    for (const float y : {0.55F, 0.6F, 0.7F}) {
        state.settings.toneCurve.luma.points = {{0.0F, 0.0F}, {0.5F, y}, {1.0F, 1.0F}};
        id = renderer.request(state, view);
        REQUIRE(collector.waitFor(id));
        // Well past the pause: a recount, had one been started, would come
        // before the next request is served.
        std::this_thread::sleep_for(400ms);
    }
    // A zoom changes the view, not the histogram.
    id = renderer.request(
        state, app::PreviewView{.region = QRect(10, 10, 100, 60), .outputSize = {100, 60}});
    REQUIRE(collector.waitFor(id));
    std::this_thread::sleep_for(400ms);

    state.settings.tone.exposure = 1.0F;
    id = renderer.request(state, view);
    REQUIRE(collector.waitForHistogram(id));
    const auto histograms = collector.histograms();
    REQUIRE(histograms.size() == 2);
    REQUIRE(histograms.back().request == id);
    REQUIRE(*histograms.back().curveHistogram != first);
}

TEST_CASE("A new source recounts the histogram", "[app][preview][histogram]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setCurveHistogramWanted(true);
    renderer.setSource(makeSource());
    auto id = renderer.request({}, app::PreviewView::wholeFrame({200, 200}));
    REQUIRE(collector.waitForHistogram(id));

    renderer.setSource(makeSource());
    id = renderer.request({}, app::PreviewView::wholeFrame({200, 200}));
    REQUIRE(collector.waitForHistogram(id));
    REQUIRE(collector.histograms().size() == 2);
}

TEST_CASE("No histogram is counted while none is wanted", "[app][preview][histogram]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeSource());
    const app::PreviewView view = app::PreviewView::wholeFrame({200, 200});
    auto id = renderer.request({}, view);
    REQUIRE(collector.waitFor(id));
    std::this_thread::sleep_for(400ms);
    REQUIRE(collector.histograms().empty());

    // Wanted, then not wanted again before an exposure edit: still nothing.
    renderer.setCurveHistogramWanted(true);
    REQUIRE(collector.waitForHistogram(id));
    renderer.setCurveHistogramWanted(false);
    DevelopState state;
    state.settings.tone.exposure = 1.0F;
    id = renderer.request(state, view);
    REQUIRE(collector.waitFor(id));
    std::this_thread::sleep_for(400ms);
    REQUIRE(collector.histograms().size() == 1);
}

TEST_CASE("Becoming wanted counts once for the state last rendered", "[app][preview][histogram]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    const auto source = makeSource();
    renderer.setSource(source);
    DevelopState state;
    state.settings.tone.exposure = 0.7F;
    const auto id = renderer.request(state, app::PreviewView::wholeFrame({200, 200}));
    REQUIRE(collector.waitFor(id));
    std::this_thread::sleep_for(400ms);
    REQUIRE(collector.histograms().empty());

    // No new request: the worker counts for the last one at the next pause.
    renderer.setCurveHistogramWanted(true);
    REQUIRE(collector.waitForHistogram(id));
    const auto histograms = collector.histograms();
    REQUIRE(histograms.size() == 1);
    REQUIRE(histograms.back().request == id);
    REQUIRE(*histograms.back().curveHistogram ==
            curveHistogram(*source, state, app::cpuCurveHistogramRequest));

    // Hidden and shown again with nothing changed: the histogram is still current.
    renderer.setCurveHistogramWanted(false);
    renderer.setCurveHistogramWanted(true);
    std::this_thread::sleep_for(400ms);
    REQUIRE(collector.histograms().size() == 1);
    REQUIRE(collector.results().size() == 1);
}

TEST_CASE("Becoming wanted with no render yet counts nothing", "[app][preview][histogram]") {
    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Cpu);
    renderer.setSource(makeSource());
    renderer.setCurveHistogramWanted(true);
    std::this_thread::sleep_for(300ms);
    REQUIRE(collector.histograms().empty());
    REQUIRE(collector.results().empty());
}
