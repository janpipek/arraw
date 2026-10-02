#include "ImagePyramid.h"

#include "GeometryPlan.h"
#include "SampleConversion.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace arraw {

namespace {

/// @brief Gives the size of the next level of a pyramid.
ImageSize halfOf(ImageSize size) {
    return {(size.width + 1) / 2, (size.height + 1) / 2};
}

} // namespace

ImageBuffer halved(const ImageBuffer& image) {
    const ImageSize in = image.size();
    if (in.width == 1 && in.height == 1) {
        throw std::invalid_argument("Cannot halve an image of one pixel");
    }
    // Converted only when it must be: a float image is read in place.
    std::optional<ImageBuffer> converted;
    if (image.format() != PixelFormat::RgbaF32) {
        converted.emplace(toRgbaF32(image));
    }
    const ImageBuffer& source = converted ? *converted : image;

    const ImageSize out = halfOf(in);
    ImageBuffer result(out, PixelFormat::RgbaF32, image.encoding(), image.orientation());
    const auto input = source.samples<float>();
    auto output = result.samples<float>();
    for (std::uint32_t y = 0; y < out.height; ++y) {
        const std::uint32_t y1 = std::min(2 * y + 1, in.height - 1);
        const std::uint32_t rows[2] = {2 * y, y1};
        const std::size_t rowCount = y1 == 2 * y ? 1 : 2;
        for (std::uint32_t x = 0; x < out.width; ++x) {
            const std::uint32_t x1 = std::min(2 * x + 1, in.width - 1);
            const std::uint32_t columns[2] = {2 * x, x1};
            const std::size_t columnCount = x1 == 2 * x ? 1 : 2;
            double colour[3] = {0.0, 0.0, 0.0};
            double alpha = 0.0;
            for (std::size_t r = 0; r < rowCount; ++r) {
                for (std::size_t c = 0; c < columnCount; ++c) {
                    const float* pixel =
                        &input[(static_cast<std::size_t>(rows[r]) * in.width + columns[c]) * 4];
                    const double a = pixel[3];
                    colour[0] += pixel[0] * a;
                    colour[1] += pixel[1] * a;
                    colour[2] += pixel[2] * a;
                    alpha += a;
                }
            }
            float* target = &output[(static_cast<std::size_t>(y) * out.width + x) * 4];
            if (alpha > 0.0) {
                target[0] = static_cast<float>(colour[0] / alpha);
                target[1] = static_cast<float>(colour[1] / alpha);
                target[2] = static_cast<float>(colour[2] / alpha);
                target[3] = static_cast<float>(alpha / static_cast<double>(rowCount * columnCount));
            } else {
                target[0] = target[1] = target[2] = target[3] = 0.0F;
            }
        }
    }
    return result;
}

int pyramidLevelFor(ImageSize sourceSize, ImageOrientation orientation, const DevelopState& state,
                    const RenderRequest& request) {
    const ImageSize cropped =
        geometryPlanFor(sourceSize, orientation, state.settings.geometry).outputSize;
    const ImageSize wanted = resolvedSize(request, cropped);
    if (request.upscale == Upscale::Allowed) {
        return 0;
    }
    int level = 0;
    ImageSize size = cropped;
    while (true) {
        const ImageSize next = halfOf(size);
        // Stops at one pixel, where halving no longer shrinks.
        if (next == size || next.width < wanted.width || next.height < wanted.height) {
            return level;
        }
        size = next;
        ++level;
    }
}

} // namespace arraw
