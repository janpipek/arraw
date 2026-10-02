#include "Develop.h"

#include "ProcessingPlan.h"
#include "Resample.h"
#include "SampleConversion.h"

#include <WhiteBalance.h>

#include <algorithm>
#include <array>
#include <cmath>
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

} // namespace

ImageBuffer arraw::develop(const ImageBuffer& source, const DevelopState& state,
                           const RenderRequest& request) {
    const ProcessingPlan plan = planFor(source, state, request);

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
    const ResizePlan& resize = *plan.resize;
    return resample(applyGeometry(std::move(result), *plan.geometry), resize.outputSize,
                    resize.filter, resize.opaque);
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
