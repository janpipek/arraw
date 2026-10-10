#include "RenderProgress.h"

#include "Denoise.h"
#include "Presence.h"
#include "StageTable.h"

#include <algorithm>
#include <exception>
#include <numeric>

using namespace arraw;

namespace {

// Nanoseconds of wall time per pixel on a release build at 24 MP, the banded
// loops on eight threads and the others on one (ADR 042). Remeasure when a pass
// gets much faster or slower; only the ratios matter. The pointwise chain, the
// geometry and the resize were remeasured once their zeroing and their loops
// were banded (ADR 043): a 6000x4000 source turned by 3 degrees and resized to
// 1500 pixels, the resize's share split between its passes as before.

/// @brief Cost of the pointwise chain per pixel of the source, the zeroing of its result included.
constexpr double pointwiseCost = 7.0;

/// @brief What reading the Presence context adds to the chain per pixel.
constexpr double presenceSampleCost = 30.0;

/// @brief Cost of drawing a brush's coverage, in nanoseconds per unit of swept area times the
/// long edge in pixels squared (the time model `t = 1.0 s * A * (L / 6000)^2` of the prototype's
/// report, B7).
///
/// Release build, 24 MP, eight threads, B8 of the brush bench, sixteen realistic masks: 27.3
/// through the cache's `drawn` and 28.9 banded; 28 is taken for both.
constexpr double coverageCost = 28.0;

/// @brief Cost of quantising one brush's coverage into the packed planes, per pixel (B8: 1.8).
constexpr double packCost = 1.8;

/// @brief Cost of the geometry resample per pixel of its result.
constexpr double geometryCost = 12.0;

/// @brief Cost of the horizontal resize per pixel it reads or writes, whichever are more.
constexpr double resizeAcrossCost = 10.0;

/// @brief Cost of the vertical resize per pixel of the result's width and the larger height.
constexpr double resizeDownCost = 3.0;

/// @brief Cost of the vignette per pixel of the result.
constexpr double vignetteCost = 25.0;

/// @brief Cost of the grain per pixel of the result.
constexpr double grainCost = 156.0;

/// @brief Gives the number of pixels of a size, as a weight.
double pixelsOf(ImageSize size) {
    return static_cast<double>(size.pixelCount());
}

/// @brief Gives the sum of some weights.
template <typename Weights> double sumOf(const Weights& weights) {
    return std::accumulate(weights.begin(), weights.end(), 0.0);
}

} // namespace

std::array<double, 2> detail::resizeUnitWeights(ImageSize input, ImageSize output) noexcept {
    // A downscale's taps grow with the reduction, so the work follows the larger side.
    const auto inputHeight = static_cast<double>(input.height);
    const auto widest = static_cast<double>(std::max(input.width, output.width));
    const auto tallest = static_cast<double>(std::max(input.height, output.height));
    return {resizeAcrossCost * widest * inputHeight, resizeDownCost * tallest * output.width};
}

detail::CoverageWork detail::coverageWorkOf(const CoverageStatus& status,
                                            const BrushCoverageRef& brush) noexcept {
    CoverageWork work;
    if (status.readiness == CoverageReadiness::Held) {
        return work;
    }
    work.pack = packCost * pixelsOf(brush.raster);
    if (status.readiness == CoverageReadiness::Cached) {
        return work;
    }
    const double edge = std::max(brush.raster.width, brush.raster.height);
    double area = 0.0;
    const auto strokes = brush.strokes->strokes();
    for (std::size_t n = std::min(status.cachedStrokes, strokes.size()); n < strokes.size(); ++n) {
        area += budgetOf(*strokes[n]).sweptArea;
    }
    work.draw = coverageCost * area * edge * edge;
    return work;
}

std::vector<double> detail::coverageUnitWeights(const LocalPlan& local,
                                                const CoverageResidency* residency) {
    std::vector<double> weights;
    for (const LocalMaskPlan& mask : local.masks) {
        if (mask.kind != LocalMaskKind::Brush) {
            continue;
        }
        const CoverageStatus status = statusOf(mask.brush, residency);
        if (status.readiness == CoverageReadiness::Held) {
            continue;
        }
        const CoverageWork work = coverageWorkOf(status, mask.brush);
        weights.push_back(work.draw + work.pack);
    }
    return weights;
}

detail::StepWeights detail::renderStepWeights(const ProcessingPlan& plan,
                                              const RenderRequest& request,
                                              const CoverageResidency* residency) noexcept {
    StepWeights weights{};
    if (!plan.geometry) {
        return weights;
    }
    const GeometryPlan& geometry = *plan.geometry;
    const ImageSize source = geometry.sourceSize;
    const auto at = [&weights](ProgressStep step) -> double& {
        return weights[static_cast<std::size_t>(step)];
    };
    try {
        at(ProgressStep::Denoise) = sumOf(denoiseLoopWeights(plan.denoise, source));
        at(ProgressStep::Context) = sumOf(presenceLoopWeights(plan.pointwise.presence, source));
        at(ProgressStep::Coverage) = sumOf(coverageUnitWeights(plan.pointwise.local, residency));
    } catch (const std::exception&) {
        // Only an allocation can fail here; the pass itself would say so.
    }
    at(ProgressStep::Pointwise) =
        (pointwiseCost + (plan.pointwise.presence.active() ? presenceSampleCost : 0.0)) *
        pixelsOf(source);
    if (!geometry.isIdentity()) {
        at(ProgressStep::Geometry) = geometryCost * pixelsOf(geometry.outputSize);
    }
    // From the request, as the whole render resolves it, not from the plan's
    // resize, which a call that stops before it plans as none.
    ImageSize output = geometry.outputSize;
    try {
        const PixelRegion region = regionOf(request, geometry.outputSize);
        output = resolvedSize(request, region.size());
        if (region.size() != geometry.outputSize || output != geometry.outputSize) {
            at(ProgressStep::Resize) = sumOf(resizeUnitWeights(region.size(), output));
        }
    } catch (const std::exception&) {
        // A request the resize would refuse: it refuses it, and nothing is resized.
    }
    if (plan.effects.vignette.active) {
        at(ProgressStep::Effects) += vignetteCost * pixelsOf(output);
    }
    if (plan.effects.grain.active) {
        at(ProgressStep::Effects) += grainCost * pixelsOf(output);
    }
    return weights;
}

ProgressStep detail::stepAfter(Stage boundary) noexcept {
    if (boundary == Stage::Effects) {
        return rowOf(Stage::Effects).firstStep;
    }
    return rowOf(following(boundary)).firstStep;
}

ProgressStep detail::stepThrough(Stage boundary) noexcept {
    return rowOf(boundary).lastStep;
}
