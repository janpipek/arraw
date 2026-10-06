#include "ProcessingPlan.h"

#include "SampleConversion.h"
#include "TimingTrace.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

using namespace arraw;

namespace {

/// @brief Whether every alpha sample of one layout is exactly one once converted.
///
/// Converted as development converts it, so that what is tested is what the
/// developed pixels hold. NaN is not one.
template <typename Sample> bool alphaIsOne(const ImageBuffer& source) {
    const auto samples = source.samples<Sample>();
    for (std::size_t index = 3; index < samples.size(); index += 4) {
        if (toUnit(samples[index]) != 1.0F) {
            return false;
        }
    }
    return true;
}

/// @brief Whether a source has no transparency at all.
///
/// Free for a layout with no alpha channel; otherwise one early-exiting read
/// of the alpha samples.
bool isOpaque(const ImageBuffer& source) {
    switch (source.format()) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbU16:
    case PixelFormat::RgbF32:
        return true;
    case PixelFormat::RgbaU8:
        return alphaIsOne<std::uint8_t>(source);
    case PixelFormat::RgbaU16:
        return alphaIsOne<std::uint16_t>(source);
    case PixelFormat::RgbaF32:
        return alphaIsOne<float>(source);
    }
    return false;
}

/// @brief Resolves a request against the cropped size into the resize block.
/// @param source Pixels to scan for opacity, if there are any at hand.
ResizePlan resizePlanFor(const RenderRequest& request, ImageSize cropped,
                         const ImageBuffer* source) {
    ResizePlan resize;
    resize.region = regionOf(request, cropped);
    resize.outputSize = resolvedSize(request, resize.region.size());
    if (resize.isIdentity(cropped)) {
        return resize;
    }
    if (resize.resamples()) {
        resize.filter = request.filter;
    }
    // Also for a region that is only cut: the device runs it through the resize
    // passes, whose opaque path is the exact one.
    resize.opaque = source != nullptr && isOpaque(*source);
    return resize;
}

} // namespace

PixelRegion arraw::regionOf(const RenderRequest& request, ImageSize frame) {
    if (frame.empty()) {
        throw std::invalid_argument("Cannot resolve a region of an empty photograph");
    }
    if (!request.region) {
        return {0, 0, frame.width, frame.height};
    }
    const RenderRequest::Region& region = *request.region;
    const bool finite = std::isfinite(region.left) && std::isfinite(region.top) &&
                        std::isfinite(region.right) && std::isfinite(region.bottom);
    if (!finite || region.left < 0.0 || region.top < 0.0 || region.right > 1.0 ||
        region.bottom > 1.0 || !(region.left < region.right) || !(region.top < region.bottom)) {
        throw std::invalid_argument(
            "A region needs finite edges with 0 <= left < right <= 1 and 0 <= top < bottom <= 1");
    }

    // Outward, and at least a pixel. left < right <= 1 keeps the first pixel
    // inside the frame, so only the far edge ever needs pushing out. An edge
    // within a millionth of a pixel of a pixel boundary is on it: a region
    // given as fractions of whole pixels lands on exactly those pixels,
    // whatever the rounding of the division that made it.
    constexpr double onBoundary = 1e-6;
    const auto edges = [](double from, double to, std::uint32_t length) {
        const auto first = static_cast<std::uint32_t>(
            std::min(std::floor(from * length + onBoundary), static_cast<double>(length - 1)));
        const auto last = static_cast<std::uint32_t>(
            std::min(std::ceil(to * length - onBoundary), static_cast<double>(length)));
        return std::pair{first, std::max(last, first + 1)};
    };
    const auto [x0, x1] = edges(region.left, region.right, frame.width);
    const auto [y0, y1] = edges(region.top, region.bottom, frame.height);
    return {x0, y0, x1 - x0, y1 - y0};
}

FrameMapping arraw::frameMappingOf(const ProcessingPlan& plan) {
    const ImageSize cropped = plan.geometry->outputSize;
    const ResizePlan& resize = *plan.resize;
    const auto width = static_cast<double>(cropped.width);
    const auto height = static_cast<double>(cropped.height);
    FrameMapping mapping;
    mapping.origin = {resize.region.x / width, resize.region.y / height};
    mapping.step = {static_cast<double>(resize.region.width) / resize.outputSize.width / width,
                    static_cast<double>(resize.region.height) / resize.outputSize.height / height};
    mapping.aspect = width / height;
    return mapping;
}

ProcessingPlan arraw::planFor(const ColorEncoding& encoding, const DevelopState& state,
                              double pixelScale) {
    const DevelopSettings& settings = state.settings;
    ProcessingPlan plan = tonePlanFor(settings.tone);
    plan.toWorking = colorMatrixFor(encoding, settings.color);
    plan.denoise = denoisePlanFor(settings.noiseReduction, encoding, pixelScale);
    plan.toneCurves = toneCurvePlanFor(settings.toneCurve);
    plan.colorAdjustments = colorAdjustmentPlanFor(settings.color, settings.hsl,
                                                   settings.blackAndWhite, settings.colorGrading);
    plan.effects = effectsPlanFor(settings.effects);
    return plan;
}

ProcessingPlan arraw::planFor(const Photo& photo, const RenderRequest& request) {
    // What the file declares is its full resolution: one sensor pixel a pixel.
    auto plan = planFor(photo.metadata().encoding, photo.state());
    plan.presence = presencePlanFor(photo.state().settings.presence, photo.metadata().encoding, 1.0,
                                    photo.metadata().size);
    plan.geometry = geometryPlanFor(photo.metadata().size, photo.metadata().orientation,
                                    photo.state().settings.geometry);
    plan.resize = resizePlanFor(request, plan.geometry->outputSize, nullptr);
    return plan;
}

ProcessingPlan arraw::planFor(const ImageBuffer& source, const DevelopState& state,
                              const RenderRequest& request) {
    const detail::TimingSpan timing("develop.plan");
    auto plan = planFor(source.encoding(), state, source.pixelScale());
    plan.presence = presencePlanFor(state.settings.presence, source.encoding(), source.pixelScale(),
                                    source.size());
    plan.geometry = geometryPlanFor(source.size(), source.orientation(), state.settings.geometry);
    plan.resize = resizePlanFor(request, plan.geometry->outputSize, &source);
    return plan;
}
