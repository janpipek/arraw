#include "BrushCoverage.h"
#include "DisplayImage.h"
#include "GpuContext.h"
#include "GpuDevelop.h"
#include "GpuTesting.h"
#include "PreviewRenderer.h"
#include "support/BrushGenerators.h"
#include "support/TestImages.h"

#include <Develop.h>
#include <DevelopState.h>
#include <ImagePyramid.h>
#include <LocalAdjustmentEdits.h>
#include <LocalAdjustments.h>

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
    const std::uint64_t id = renderer.request(state, app::PreviewView::wholeFrame(fullSize));
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
                developOnGpu(context, *source, state, Stage::Effects, request).readBack()));
    REQUIRE(*coarse.image ==
            app::toDisplayImage(
                developOnGpu(context, level1, state, Stage::Effects, request).readBack()));
}
