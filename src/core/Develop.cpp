#include "Develop.h"

#include "CheckpointState.h"
#include "ProcessingPlan.h"
#include "Resample.h"
#include "SampleConversion.h"
#include "TimingTrace.h"

#include <WhiteBalance.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <variant>

using namespace arraw;

namespace {

/// @brief Runs the pointwise chain over every pixel of one layout.
///
/// The order itself is in ::arraw::developPixel; this is only the traversal,
/// deliberately small enough that nothing can hide in it. Alpha is copied
/// rather than developed: no setting produces transparency, and a source that
/// carried some keeps exactly what it had. ::arraw::ResizePlan::opaque, scanned
/// from the source, relies on this: were alpha ever changed here, it would no
/// longer describe the developed pixels.
template <typename Sample>
void developSamples(const ImageBuffer& source, ImageBuffer& result, const ProcessingPlan& plan) {
    const auto input = source.samples<Sample>();
    const auto output = result.samples<float>();
    const std::size_t channels = channelCount(source.format());
    const auto pixels = static_cast<std::size_t>(source.size().pixelCount());

    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        const auto* in = &input[pixel * channels];
        auto* out = &output[pixel * 4];

        const Colour developed = developPixel(plan, {toUnit(in[0]), toUnit(in[1]), toUnit(in[2])});
        out[0] = developed[0];
        out[1] = developed[1];
        out[2] = developed[2];
        out[3] = channels == 4 ? toUnit(in[3]) : 1.0F;
    }
}

/// @brief Runs the pointwise chain over a source, into the working format.
ImageBuffer developPointwise(const ImageBuffer& source, const ProcessingPlan& plan) {
    const detail::TimingSpan timing("cpu.pointwise");
    ImageBuffer result(source.size(), workingFormat, workingEncoding);
    switch (source.format()) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbaU8:
        developSamples<std::uint8_t>(source, result, plan);
        break;
    case PixelFormat::RgbU16:
    case PixelFormat::RgbaU16:
        developSamples<std::uint16_t>(source, result, plan);
        break;
    case PixelFormat::RgbF32:
    case PixelFormat::RgbaF32:
        developSamples<float>(source, result, plan);
        break;
    }
    return result;
}

/// @brief Copies a rectangle of working-format pixels out of a frame.
ImageBuffer cutOut(const ImageBuffer& frame, const PixelRegion& region) {
    ImageBuffer cut(region.size(), workingFormat, frame.encoding(), frame.orientation());
    const auto in = frame.samples<float>();
    const auto out = cut.samples<float>();
    const std::size_t rowSamples = static_cast<std::size_t>(region.width) * 4;
    for (std::uint32_t row = 0; row < region.height; ++row) {
        const std::size_t start =
            (static_cast<std::size_t>(region.y + row) * frame.size().width + region.x) * 4;
        std::copy_n(in.begin() + static_cast<std::ptrdiff_t>(start), rowSamples,
                    out.begin() + static_cast<std::ptrdiff_t>(row * rowSamples));
    }
    return cut;
}

/// @brief Runs the resize that a plan resolved: cut the region, then resample it.
ImageBuffer resizeBy(ImageBuffer framed, const ProcessingPlan& plan) {
    const ResizePlan& resize = *plan.resize;
    if (!resize.region.covers(framed.size())) {
        framed = cutOut(framed, resize.region);
    }
    return resample(std::move(framed), resize.outputSize, resize.filter, resize.opaque);
}

/// @brief Checks that a pass boundary is one that exists.
void requireBoundary(Stage stopAfter) {
    if (static_cast<std::size_t>(stopAfter) >= stageCount) {
        throw std::invalid_argument("A render needs a recognised pass boundary to stop after");
    }
}

/// @brief Runs the stages after a boundary, up to another, on pixels taken at the first.
/// @param done Boundary @p pixels were taken at.
/// @param pixels Developed pixels at @p done; consumed.
/// @param plan The plan that made them and makes the rest.
/// @param stopAfter Last boundary to run, not before @p done.
RenderCheckpoint runStages(Stage done, ImageBuffer pixels, ProcessingPlan plan, Stage stopAfter) {
    Stage at = done;
    if (at == Stage::Pointwise && stopAfter != Stage::Pointwise) {
        pixels = applyGeometry(std::move(pixels), *plan.geometry);
        at = Stage::Geometry;
    }
    if (at == Stage::Geometry && stopAfter == Stage::Resize) {
        pixels = resizeBy(std::move(pixels), plan);
        at = Stage::Resize;
    }
    return makeCheckpoint(at, std::move(plan), std::move(pixels));
}

} // namespace

ImageBuffer arraw::develop(const ImageBuffer& source, const DevelopState& state,
                           const RenderRequest& request) {
    const detail::TimingSpan timing("cpu.develop");
    // The direct path: no checkpoint, so nothing is shared and nothing copied.
    const ProcessingPlan plan = planFor(source, state, request);
    return resizeBy(applyGeometry(developPointwise(source, plan), *plan.geometry), plan);
}

RenderCheckpoint arraw::developUntil(const ImageBuffer& source, const DevelopState& state,
                                     Stage stopAfter, const RenderRequest& request) {
    requireBoundary(stopAfter);
    // Only a render that reaches the resize plans one: stopping earlier ignores
    // the request, and has no use for the opacity scan.
    ProcessingPlan plan =
        planFor(source, state, stopAfter == Stage::Resize ? request : RenderRequest{});
    ImageBuffer developed = developPointwise(source, plan);
    return runStages(Stage::Pointwise, std::move(developed), std::move(plan), stopAfter);
}

RenderCheckpoint arraw::resumeFrom(const RenderCheckpoint& from, const ImageBuffer& source,
                                   const DevelopState& state, Stage stopAfter,
                                   const RenderRequest& request) {
    requireBoundary(stopAfter);
    const CheckpointState& held = stateOf(from);
    const auto* pixels = std::get_if<ImageBuffer>(&held.pixels);
    if (pixels == nullptr) {
        throw std::invalid_argument("A checkpoint on a device cannot be resumed on the CPU");
    }
    ProcessingPlan plan =
        planFor(source, state, stopAfter == Stage::Resize ? request : RenderRequest{});
    requireResumable(held, plan, source.size(), stopAfter);
    if (stopAfter == held.boundary) {
        return from;
    }
    return runStages(held.boundary, pixels->clone(), std::move(plan), stopAfter);
}

ImageSize arraw::croppedSize(ImageSize sourceSize, ImageOrientation orientation,
                             const DevelopState& state) {
    return geometryPlanFor(sourceSize, orientation, state.settings.geometry).outputSize;
}

RenderRequest::Region arraw::renderedRegion(const RenderRequest& request, ImageSize frame) {
    const PixelRegion pixels = regionOf(request, frame);
    const auto width = static_cast<double>(frame.width);
    const auto height = static_cast<double>(frame.height);
    return {.left = pixels.x / width,
            .top = pixels.y / height,
            .right = (pixels.x + pixels.width) / width,
            .bottom = (pixels.y + pixels.height) / height};
}

ImageSize arraw::resolvedSize(const RenderRequest& request, ImageSize cropped) {
    if (cropped.empty()) {
        throw std::invalid_argument("Cannot resolve a size against an empty photograph");
    }
    if (!request.size.has_value()) {
        return cropped;
    }

    double scale = 1.0;
    std::uint32_t maxWidth = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t maxHeight = maxWidth;
    if (const auto* box = std::get_if<RenderRequest::FitInside>(&*request.size)) {
        if (box->width == 0 || box->height == 0) {
            throw std::invalid_argument("A box to fit inside needs both sides above zero");
        }
        scale = std::min(static_cast<double>(box->width) / cropped.width,
                         static_cast<double>(box->height) / cropped.height);
        maxWidth = box->width;
        maxHeight = box->height;
    } else {
        scale = std::get<RenderRequest::Scale>(*request.size).factor;
        if (!std::isfinite(scale) || scale <= 0.0) {
            throw std::invalid_argument("A scale factor must be finite and above zero");
        }
    }
    if (request.upscale == Upscale::Never) {
        scale = std::min(scale, 1.0);
    }

    const auto side = [scale](std::uint32_t length, std::uint32_t limit) {
        const double rounded = std::round(length * scale);
        if (rounded >= static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::invalid_argument("The requested size does not fit an image side");
        }
        return std::min(std::max(static_cast<std::uint32_t>(rounded), std::uint32_t{1}), limit);
    };
    return {side(cropped.width, maxWidth), side(cropped.height, maxHeight)};
}
