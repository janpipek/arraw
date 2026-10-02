#include "ProcessingPlan.h"

#include "SampleConversion.h"

#include <cstdint>

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
    resize.outputSize = resolvedSize(request, cropped);
    if (resize.isIdentity(cropped)) {
        return resize;
    }
    resize.filter = request.filter;
    resize.opaque = source != nullptr && isOpaque(*source);
    return resize;
}

} // namespace

ProcessingPlan arraw::planFor(const ColorEncoding& encoding, const DevelopState& state) {
    const DevelopSettings& settings = state.settings;
    ProcessingPlan plan = tonePlanFor(settings.tone);
    plan.toWorking = colorMatrixFor(encoding, settings.color);
    return plan;
}

ProcessingPlan arraw::planFor(const Photo& photo, const RenderRequest& request) {
    auto plan = planFor(photo.metadata().encoding, photo.state());
    plan.geometry = geometryPlanFor(photo.metadata().size, photo.metadata().orientation,
                                    photo.state().settings.geometry);
    plan.resize = resizePlanFor(request, plan.geometry->outputSize, nullptr);
    return plan;
}

ProcessingPlan arraw::planFor(const ImageBuffer& source, const DevelopState& state,
                              const RenderRequest& request) {
    auto plan = planFor(source.encoding(), state);
    plan.geometry = geometryPlanFor(source.size(), source.orientation(), state.settings.geometry);
    plan.resize = resizePlanFor(request, plan.geometry->outputSize, &source);
    return plan;
}
