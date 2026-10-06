#include "ImagePyramid.h"

#include "GeometryPlan.h"
#include "ProcessingPlan.h"
#include "RowBands.h"
#include "SampleConversion.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace arraw {

namespace {

/// @brief Gives the size of the next level of a pyramid.
ImageSize halfOf(ImageSize size) {
    return {(size.width + 1) / 2, (size.height + 1) / 2};
}

/// @brief Halves an image of one sample type into a float one.
///
/// Reads the stored samples through ::arraw::toUnit, as ::arraw::toRgbaF32
/// would, so no full-size float copy is made first (ADR 043): the same numbers,
/// without the allocation that cost most of a 24 MP halving. Each output row
/// reads two input rows and writes only itself, so banded it gives the same bits
/// on any number of threads (ADR 039).
template <typename Sample> void halveInto(const ImageBuffer& image, ImageBuffer& result) {
    const ImageSize in = image.size();
    const ImageSize out = result.size();
    const std::size_t channels = channelCount(image.format());
    const auto input = image.samples<Sample>();
    auto output = result.samples<float>();
    // Not forEachRowBand: halving is no unit of any operation's progress (ADR 042).
    detail::splitRowBands(
        out.height, out.width, [&](std::uint32_t first, std::uint32_t last, bool) {
            for (std::uint32_t y = first; y < last; ++y) {
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
                            const Sample* pixel =
                                &input[(static_cast<std::size_t>(rows[r]) * in.width + columns[c]) *
                                       channels];
                            const double a = channels == 4 ? toUnit(pixel[3]) : 1.0F;
                            colour[0] += toUnit(pixel[0]) * a;
                            colour[1] += toUnit(pixel[1]) * a;
                            colour[2] += toUnit(pixel[2]) * a;
                            alpha += a;
                        }
                    }
                    float* target = &output[(static_cast<std::size_t>(y) * out.width + x) * 4];
                    if (alpha > 0.0) {
                        target[0] = static_cast<float>(colour[0] / alpha);
                        target[1] = static_cast<float>(colour[1] / alpha);
                        target[2] = static_cast<float>(colour[2] / alpha);
                        target[3] =
                            static_cast<float>(alpha / static_cast<double>(rowCount * columnCount));
                    } else {
                        target[0] = target[1] = target[2] = target[3] = 0.0F;
                    }
                }
            }
        });
}

} // namespace

ImageBuffer halved(const ImageBuffer& image) {
    const ImageSize in = image.size();
    if (in.width == 1 && in.height == 1) {
        throw std::invalid_argument("Cannot halve an image of one pixel");
    }
    ImageBuffer result(halfOf(in), PixelFormat::RgbaF32, image.encoding(), image.orientation());
    result.setPixelScale(2.0 * image.pixelScale());
    switch (image.format()) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbaU8:
        halveInto<std::uint8_t>(image, result);
        break;
    case PixelFormat::RgbU16:
    case PixelFormat::RgbaU16:
        halveInto<std::uint16_t>(image, result);
        break;
    case PixelFormat::RgbF32:
    case PixelFormat::RgbaF32:
        halveInto<float>(image, result);
        break;
    }
    return result;
}

int pyramidLevelFor(ImageSize sourceSize, ImageOrientation orientation, const DevelopState& state,
                    const RenderRequest& request) {
    const ImageSize cropped =
        geometryPlanFor(sourceSize, orientation, state.settings.geometry).outputSize;
    // What is resized is the region, so what must be covered is its size.
    const ImageSize wanted = resolvedSize(request, regionOf(request, cropped).size());
    if (request.upscale == Upscale::Allowed) {
        return 0;
    }
    int level = 0;
    ImageSize size = cropped;
    while (true) {
        const ImageSize next = halfOf(size);
        // Stops at one pixel, where halving no longer shrinks.
        if (next == size) {
            return level;
        }
        const ImageSize region = regionOf(request, next).size();
        if (region.width < wanted.width || region.height < wanted.height) {
            return level;
        }
        size = next;
        ++level;
    }
}

} // namespace arraw
