#include "SampleConversion.h"

#include <cstddef>
#include <cstdint>

namespace arraw {

namespace {

/// @brief Converts every pixel of one sample type.
template <typename Sample> void convert(const ImageBuffer& source, ImageBuffer& result) {
    const auto input = source.samples<Sample>();
    const auto output = result.samples<float>();
    const std::size_t channels = channelCount(source.format());
    const auto pixels = static_cast<std::size_t>(source.size().pixelCount());
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        const auto* in = &input[pixel * channels];
        auto* out = &output[pixel * 4];
        out[0] = toUnit(in[0]);
        out[1] = toUnit(in[1]);
        out[2] = toUnit(in[2]);
        out[3] = channels == 4 ? toUnit(in[3]) : 1.0F;
    }
}

} // namespace

ImageBuffer toRgbaF32(const ImageBuffer& source) {
    if (source.format() == PixelFormat::RgbaF32) {
        return source.clone();
    }
    ImageBuffer result(source.size(), PixelFormat::RgbaF32, source.encoding(),
                       source.orientation());
    result.setPixelScale(source.pixelScale());
    switch (source.format()) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbaU8:
        convert<std::uint8_t>(source, result);
        break;
    case PixelFormat::RgbU16:
    case PixelFormat::RgbaU16:
        convert<std::uint16_t>(source, result);
        break;
    case PixelFormat::RgbF32:
    case PixelFormat::RgbaF32:
        convert<float>(source, result);
        break;
    }
    return result;
}

} // namespace arraw
