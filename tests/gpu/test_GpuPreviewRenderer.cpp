#include "BrushCoverage.h"
#include "DisplayImage.h"
#include "GpuContext.h"
#include "GpuDevelop.h"
#include "GpuTesting.h"
#include "PreviewRenderer.h"
#include "support/BrushGenerators.h"
#include "support/LensTables.h"
#include "support/ProgressGate.h"
#include "support/TestImages.h"

#include <Develop.h>
#include <DevelopState.h>
#include <ImagePyramid.h>
#include <LocalAdjustmentEdits.h>
#include <LocalAdjustments.h>
#include <Progress.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace arraw;
using namespace arraw::test;
using namespace std::chrono_literals;

/// The window's renderer on a device the application refuses (a software one), so that its GPU
/// path for brushes runs in a test: which ladder is asked, the stand-in, level 0.

namespace {

/// @brief Results of a renderer, collected for the test thread.
class Collector {
public:
    /// @brief Gives the callback to hand the renderer.
    [[nodiscard]] std::function<void(app::PreviewResult)> callback() {
        return [this](app::PreviewResult result) {
            {
                const std::scoped_lock lock(mutex_);
                if (result.image) {
                    results_.push_back(std::move(result));
                }
            }
            changed_.notify_all();
        };
    }

    /// @brief Waits for a result of a render at its own level, for @p id or a later request.
    [[nodiscard]] bool waitForFinal(std::uint64_t id) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 60s, [&] {
            return std::ranges::any_of(results_, [id](const app::PreviewResult& result) {
                return result.request >= id && !result.provisional;
            });
        });
    }

    /// @brief Gives the results of a request, in delivery order.
    [[nodiscard]] std::vector<app::PreviewResult> resultsOf(std::uint64_t id) {
        const std::scoped_lock lock(mutex_);
        std::vector<app::PreviewResult> found;
        std::ranges::copy_if(
            results_, std::back_inserter(found),
            [id](const app::PreviewResult& result) { return result.request == id; });
        return found;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<app::PreviewResult> results_;
};

/// @brief Makes a software-capable context, as the application would not.
std::unique_ptr<GpuContext> makeTestContext(std::string&) {
    return std::make_unique<GpuContext>(gpuTestBackend());
}

} // namespace

TEST_CASE("The window renders a brush on the GPU, the stand-in included, without falling back",
          "[gpu][app][preview][brush][provisional]") {
    // Skips when this machine has no device for the backend.
    static_cast<void>(gpuContext());
    detail::brushCoverageCache().clear();
    const auto source = std::make_shared<const ImageBuffer>(
        test::rainbow({1024, 512}, PixelFormat::RgbaF32, workingEncoding));
    LocalAdjustment adjustment;
    adjustment.shape = BrushMask{test::paintedMask(9101, 4, test::everydayStyle, 0.5)};
    adjustment.deltas.exposure = 1.0F;
    const DevelopState state = withLocalAdjustmentAdded(DevelopState{}, adjustment);
    const QSize fullSize{1024, 512};

    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Auto, {}, {},
                                  makeTestContext);
    renderer.setStandInThreshold(0.0);
    renderer.setSource(source);
    const std::uint64_t id =
        renderer.request(state, LensCorrection{}, app::PreviewView::wholeFrame(fullSize));
    REQUIRE(collector.waitForFinal(id));
    const auto results = collector.resultsOf(id);
    REQUIRE(results.size() == 2);
    const app::PreviewResult& coarse = results[0];
    const app::PreviewResult& own = results[1];
    for (const app::PreviewResult& result : results) {
        INFO(result.fallbackReason);
        REQUIRE(result.error.empty());
        REQUIRE(result.onGpu);
        REQUIRE(result.fallbackReason.empty());
        REQUIRE_FALSE(result.deviceName.empty());
    }
    REQUIRE(coarse.provisional);
    REQUIRE(coarse.level == 1);
    REQUIRE_FALSE(own.provisional);
    REQUIRE(own.level == 0);

    // Level 0 is a direct GPU render of the source; the stand-in one of level 1.
    GpuContext& context = gpuContext();
    const RenderRequest request = app::previewRequest(fullSize);
    const ImageBuffer level1 = halved(*source);
    REQUIRE(*own.image ==
            app::toDisplayImage(
                developOnGpu(context, *source, state, LensCorrection{}, Stage::Effects, request)
                    .readBack()));
    REQUIRE(*coarse.image ==
            app::toDisplayImage(
                developOnGpu(context, level1, state, LensCorrection{}, Stage::Effects, request)
                    .readBack()));
}

TEST_CASE("On the GPU a Queue request lets the render in flight finish, then delivers its own",
          "[gpu][app][preview][queue]") {
    static_cast<void>(gpuContext());
    const auto source = std::make_shared<const ImageBuffer>(
        test::rainbow({256, 128}, PixelFormat::RgbaF32, workingEncoding));
    const QSize size{256, 128};

    test::ProgressGate gate;

    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Auto, {},
                                  gate.callback(), makeTestContext);
    renderer.setSource(source);
    DevelopState exposed;
    exposed.settings.tone.exposure = 0.5F;
    gate.close();
    const std::uint64_t first =
        renderer.request({}, LensCorrection{}, app::PreviewView::wholeFrame(size));
    REQUIRE(gate.waitHeld());
    const std::uint64_t second = renderer.request(
        exposed, LensCorrection{}, app::PreviewView::wholeFrame(size), app::Supersede::Queue);
    gate.open();
    REQUIRE(collector.waitForFinal(second));

    const auto firstResults = collector.resultsOf(first);
    REQUIRE(firstResults.size() == 1);
    REQUIRE(firstResults[0].onGpu);
    REQUIRE(collector.resultsOf(second).size() == 1);
    REQUIRE(collector.resultsOf(second)[0].onGpu);
}

TEST_CASE("A render with an active lens correction is made on the GPU, and matches the CPU's",
          "[gpu][app][preview][lens]") {
    GpuContext& context = gpuContext();
    const auto source = std::make_shared<const ImageBuffer>(
        test::rainbow({256, 128}, PixelFormat::RgbaF32, workingEncoding));
    const QSize size{256, 128};
    const LensCorrection barrel = test::barrelCorrection(-0.08F, 2.0);

    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Auto, {}, {},
                                  makeTestContext);
    renderer.setSource(source);

    const std::uint64_t plain =
        renderer.request({}, LensCorrection{}, app::PreviewView::wholeFrame(size));
    REQUIRE(collector.waitForFinal(plain));
    REQUIRE(collector.resultsOf(plain).back().onGpu);

    const std::uint64_t corrected =
        renderer.request({}, barrel, app::PreviewView::wholeFrame(size));
    REQUIRE(collector.waitForFinal(corrected));
    const app::PreviewResult result = collector.resultsOf(corrected).back();
    REQUIRE(result.error.empty());
    CHECK(result.onGpu);
    CHECK(result.fallbackReason.empty());
    // The picture is the device's own render of the correction, which is not the plain one.
    const RenderRequest request = app::previewRequest(size);
    CHECK(*result.image ==
          app::toDisplayImage(
              developOnGpu(context, *source, {}, barrel, Stage::Effects, request).readBack()));
    CHECK(*result.image != *collector.resultsOf(plain).back().image);
}

TEST_CASE("A lens render the device cannot make falls back to the CPU, which makes it",
          "[gpu][app][preview][lens]") {
    GpuContext& context = gpuContext();
    // Wider than the device's textures, so that the device cannot hold the photograph.
    const auto width = static_cast<std::uint32_t>(context.info().maxTextureSize) + 16;
    const auto source = std::make_shared<const ImageBuffer>(
        test::rainbow({width, 8}, PixelFormat::RgbaF32, workingEncoding));
    const QSize size{static_cast<int>(width), 8};
    const LensCorrection barrel = test::barrelCorrection(-0.08F, static_cast<double>(width) / 8.0);

    Collector collector;
    app::PreviewRenderer renderer(collector.callback(), app::PreviewRenderer::Device::Auto, {}, {},
                                  makeTestContext);
    renderer.setSource(source);
    const std::uint64_t id = renderer.request({}, barrel, app::PreviewView::wholeFrame(size));
    REQUIRE(collector.waitForFinal(id));
    const app::PreviewResult result = collector.resultsOf(id).back();
    REQUIRE(result.error.empty());
    CHECK_FALSE(result.onGpu);
    CHECK_FALSE(result.fallbackReason.empty());
    CHECK(*result.image ==
          app::toDisplayImage(develop(*source, {}, barrel, app::previewRequest(size))));
}
