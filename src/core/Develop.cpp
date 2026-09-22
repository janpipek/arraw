#include "Develop.h"

#include "ProcessingPlan.h"

#include <WhiteBalance.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <variant>

using namespace arraw;

namespace {

/// @brief Converts one stored sample to the unit range development works in.
template <typename Sample> constexpr float toUnit(Sample value) {
    if constexpr (std::is_same_v<Sample, float>) {
        return value;
    } else {
        return static_cast<float>(value) / static_cast<float>(std::numeric_limits<Sample>::max());
    }
}

/// @brief Runs the pointwise chain over every pixel of one layout.
///
/// The order itself is in ::arraw::developPixel; this is only the traversal,
/// deliberately small enough that nothing can hide in it. Alpha is copied
/// rather than developed: no setting produces transparency, and a source that
/// carried some keeps exactly what it had.
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

ImageBuffer arraw::develop(const ImageBuffer& source, const DevelopSettings& settings,
                           const RenderRequest& request) {
    if (request.targetSize.has_value()) {
        throw std::invalid_argument("Rendering to a requested size is not implemented yet");
    }

    const ProcessingPlan plan = planFor(source, settings);

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
    return applyGeometry(std::move(result), *plan.geometry);
}
