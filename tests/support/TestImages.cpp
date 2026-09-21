#include "TestImages.h"

#include <cmath>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace {

/// @brief One colour as unbounded linear components.
struct Rgb {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
};

/// @brief Converts a position on the hue circle to a fully saturated colour.
/// @param hue Position on the circle, in [0, 1].
/// @param value Brightness of the result, in [0, 1].
/// @return The colour at that hue and brightness.
Rgb hueToRgb(float hue, float value) {
    const float sector = hue * 6.0F;
    const float fraction = sector - std::floor(sector);
    const float rising = value * fraction;
    const float falling = value * (1.0F - fraction);

    switch (static_cast<int>(sector) % 6) {
    case 0:
        return {value, rising, 0.0F};
    case 1:
        return {falling, value, 0.0F};
    case 2:
        return {0.0F, value, rising};
    case 3:
        return {0.0F, falling, value};
    case 4:
        return {rising, 0.0F, value};
    default:
        return {value, 0.0F, falling};
    }
}

/// @brief Writes normalised components into a buffer's samples, quantising
/// them when the samples are integers.
template <typename Sample>
void store(arraw::ImageBuffer& image, const std::vector<float>& components, float fullScale) {
    const auto samples = image.samples<Sample>();
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const float scaled = components[index] * fullScale;
        if constexpr (std::is_floating_point_v<Sample>) {
            samples[index] = scaled;
        } else {
            samples[index] = static_cast<Sample>(std::lround(scaled));
        }
    }
}

} // namespace

arraw::ImageBuffer arraw::test::rainbow(ImageSize size, PixelFormat format,
                                        NamedEncoding encoding) {
    ImageBuffer image(size, format, encoding);

    const std::size_t channels = channelCount(format);
    std::vector<float> components(static_cast<std::size_t>(size.pixelCount()) * channels, 1.0F);

    for (std::uint32_t y = 0; y < size.height; ++y) {
        // Darkest row at the top, so a vertical flip is visible.
        const float value =
            size.height > 1 ? 0.25F + 0.75F * static_cast<float>(y) / (size.height - 1) : 1.0F;

        for (std::uint32_t x = 0; x < size.width; ++x) {
            const float hue = size.width > 1 ? static_cast<float>(x) / (size.width - 1) : 0.0F;
            const Rgb colour = hueToRgb(hue, value);

            const std::size_t base = (static_cast<std::size_t>(y) * size.width + x) * channels;
            components[base] = colour.red;
            components[base + 1] = colour.green;
            components[base + 2] = colour.blue;
            // A fourth channel, where present, keeps its opaque initial value.
        }
    }

    switch (format) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbaU8:
        store<std::uint8_t>(image, components, 255.0F);
        break;
    case PixelFormat::RgbU16:
    case PixelFormat::RgbaU16:
        store<std::uint16_t>(image, components, 65535.0F);
        break;
    case PixelFormat::RgbF32:
    case PixelFormat::RgbaF32:
        store<float>(image, components, 1.0F);
        break;
    }

    return image;
}
