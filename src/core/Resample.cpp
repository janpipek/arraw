#include "Resample.h"

#include "ResampleWeights.h"
#include "TimingTrace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

using namespace arraw;

namespace {

constexpr std::size_t channels = 4;

/// @brief Premultiplied pixels after one pass, with the range of colours that fed each of them.
///
/// For an opaque source only `pixels` is filled, with alpha one throughout; the
/// rest is what an opaque source never needs.
struct Pass {
    std::vector<float> pixels; ///< Premultiplied RGBA.
    std::vector<float> low;    ///< Lowest unpremultiplied RGB of the contributing visible pixels.
    std::vector<float> high;   ///< Highest unpremultiplied RGB of the contributing visible pixels.
    std::vector<char> translucent; ///< Per pixel: whether any contributing pixel was not opaque.
};

/// @brief Clamps a source index into the image, which extends its edge pixels.
std::size_t clampIndex(std::int64_t index, std::uint32_t length) {
    return static_cast<std::size_t>(std::clamp<std::int64_t>(index, 0, length - 1));
}

/// @brief Stores an accumulated sample, applying the rule that ringing never goes below black.
float store(double value, bool sawNegative) {
    return (value < 0.0 && !sawNegative) ? 0.0F : static_cast<float>(value);
}

/// @brief Resizes the rows of premultiplied pixels, one output column at a time.
///
/// @tparam Opaque Whether every alpha is one: then there is no premultiplying to
/// do, no range of colours or translucency to track, and alpha comes out one.
template <bool Opaque>
Pass horizontalPass(const ImageBuffer& source, std::uint32_t outWidth, ResizeFilter filter) {
    constexpr float inf = std::numeric_limits<float>::infinity();
    const ImageSize size = source.size();
    const AxisWeights axis = axisWeights(size.width, outWidth, filter);
    const auto input = source.samples<float>();
    const std::size_t outSamples = static_cast<std::size_t>(outWidth) * size.height * channels;
    Pass output;
    output.pixels.resize(outSamples);
    if constexpr (!Opaque) {
        output.low.assign(outSamples, inf);
        output.high.assign(outSamples, -inf);
        output.translucent.assign(static_cast<std::size_t>(outWidth) * size.height, 0);
    }
    std::vector<float> row(static_cast<std::size_t>(size.width) * channels);

    for (std::uint32_t y = 0; y < size.height; ++y) {
        const auto* in = &input[static_cast<std::size_t>(y) * size.width * channels];
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const float alpha = Opaque ? 1.0F : in[x * channels + 3];
            for (std::size_t c = 0; c < 3; ++c) {
                row[x * channels + c] =
                    Opaque ? in[x * channels + c] : in[x * channels + c] * alpha;
            }
            row[x * channels + 3] = alpha;
        }
        const std::size_t rowStart = static_cast<std::size_t>(y) * outWidth * channels;
        for (std::uint32_t x = 0; x < outWidth; ++x) {
            const Taps& taps = axis.taps[x];
            std::array<double, channels> sum{};
            std::array<bool, channels> negative{};
            float* low = Opaque ? nullptr : &output.low[rowStart + x * channels];
            float* high = Opaque ? nullptr : &output.high[rowStart + x * channels];
            for (std::size_t k = 0; k < taps.count; ++k) {
                const double weight = axis.weights[taps.offset + k];
                const std::size_t index =
                    clampIndex(taps.first + static_cast<std::int64_t>(k), size.width);
                const float* pixel = &row[index * channels];
                for (std::size_t c = 0; c < (Opaque ? 3 : channels); ++c) {
                    sum[c] += weight * pixel[c];
                    negative[c] = negative[c] || pixel[c] < 0.0F;
                }
                if constexpr (!Opaque) {
                    if (pixel[3] < 1.0F) {
                        output.translucent[rowStart / channels + x] = 1;
                    }
                    if (pixel[3] >= transparentBelow) {
                        for (std::size_t c = 0; c < 3; ++c) {
                            const float colour = in[index * channels + c];
                            low[c] = std::min(low[c], colour);
                            high[c] = std::max(high[c], colour);
                        }
                    }
                }
            }
            for (std::size_t c = 0; c < 3; ++c) {
                output.pixels[rowStart + x * channels + c] = store(sum[c], negative[c]);
            }
            output.pixels[rowStart + x * channels + 3] = Opaque ? 1.0F : store(sum[3], negative[3]);
        }
    }
    return output;
}

/// @brief Resizes the columns, one output row at a time, and unpremultiplies each row into place.
///
/// Lanczos weights are signed, so premultiplied colour and alpha are different
/// signed sums and their quotient is unbounded near transparency. Alpha is
/// clamped to [0, 1], pixels below ::transparentBelow become transparent, and
/// where the window holds any pixel that is not opaque, colour is clamped to
/// the range of the visible pixels in it. An opaque window is left to the
/// filter and the rule against ringing below zero alone: clamping it to its
/// neighbourhood would make the resize non-linear everywhere, which is not
/// what the plan chose.
///
/// @tparam Opaque Whether every alpha is one: then each row is the filtered
/// colour with alpha one, which is what the general path computes for it, with
/// none of the tracking and no final clamp.
template <bool Opaque>
void verticalPass(const Pass& input, std::uint32_t width, std::uint32_t inHeight,
                  std::span<float> result, std::uint32_t outHeight, ResizeFilter filter) {
    constexpr float inf = std::numeric_limits<float>::infinity();
    const AxisWeights axis = axisWeights(inHeight, outHeight, filter);
    const std::size_t rowSamples = static_cast<std::size_t>(width) * channels;
    std::vector<double> sum(rowSamples);
    std::vector<char> negative(rowSamples);
    std::vector<float> low(Opaque ? 0 : rowSamples);
    std::vector<float> high(Opaque ? 0 : rowSamples);
    std::vector<char> translucent(Opaque ? 0 : width);

    for (std::uint32_t y = 0; y < outHeight; ++y) {
        const Taps& taps = axis.taps[y];
        std::fill(sum.begin(), sum.end(), 0.0);
        std::fill(negative.begin(), negative.end(), char{0});
        if constexpr (!Opaque) {
            std::fill(low.begin(), low.end(), inf);
            std::fill(high.begin(), high.end(), -inf);
            std::fill(translucent.begin(), translucent.end(), char{0});
        }
        for (std::size_t k = 0; k < taps.count; ++k) {
            const double weight = axis.weights[taps.offset + k];
            const std::size_t start =
                clampIndex(taps.first + static_cast<std::int64_t>(k), inHeight) * rowSamples;
            const float* in = &input.pixels[start];
            for (std::size_t i = 0; i < rowSamples; ++i) {
                sum[i] += weight * in[i];
                negative[i] |= static_cast<char>(in[i] < 0.0F);
                if constexpr (!Opaque) {
                    low[i] = std::min(low[i], input.low[start + i]);
                    high[i] = std::max(high[i], input.high[start + i]);
                }
            }
            if constexpr (!Opaque) {
                const std::size_t startPixel = start / channels;
                for (std::uint32_t x = 0; x < width; ++x) {
                    translucent[x] |= input.translucent[startPixel + x];
                }
            }
        }
        float* out = &result[static_cast<std::size_t>(y) * rowSamples];
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t p = x * channels;
            if constexpr (Opaque) {
                for (std::size_t c = 0; c < 3; ++c) {
                    out[p + c] = store(sum[p + c], negative[p + c] != 0);
                }
                out[p + 3] = 1.0F;
            } else {
                const float alpha = std::clamp(store(sum[p + 3], negative[p + 3] != 0), 0.0F, 1.0F);
                if (alpha < transparentBelow) {
                    std::fill(out + p, out + p + channels, 0.0F);
                    continue;
                }
                for (std::size_t c = 0; c < 3; ++c) {
                    const float colour = store(sum[p + c], negative[p + c] != 0) / alpha;
                    if (translucent[x] == 0) {
                        out[p + c] = colour;
                    } else {
                        out[p + c] = low[p + c] <= high[p + c]
                                         ? std::clamp(colour, low[p + c], high[p + c])
                                         : 0.0F;
                    }
                }
                out[p + 3] = alpha;
            }
        }
    }
}

} // namespace

double arraw::resampledPixelScale(double scale, std::uint32_t from, std::uint32_t to) {
    return scale * static_cast<double>(from) / static_cast<double>(to);
}

ImageBuffer arraw::resample(ImageBuffer source, ImageSize size, ResizeFilter filter, bool opaque) {
    const detail::TimingSpan timing("cpu.resize");
    if (source.format() != workingFormat) {
        throw std::invalid_argument("Resampling requires developed float pixels");
    }
    if (size.empty()) {
        throw std::invalid_argument("Resampling needs a result of at least one pixel");
    }
    if (source.size() == size) {
        return source;
    }

    ImageBuffer result(size, workingFormat, source.encoding(), source.orientation());
    result.setPixelScale(resampledPixelScale(source.pixelScale(), source.size().width, size.width));
    const auto out = result.samples<float>();
    if (opaque) {
        const Pass across = horizontalPass<true>(source, size.width, filter);
        verticalPass<true>(across, size.width, source.size().height, out, size.height, filter);
    } else {
        const Pass across = horizontalPass<false>(source, size.width, filter);
        verticalPass<false>(across, size.width, source.size().height, out, size.height, filter);
    }
    return result;
}
