#include "Resample.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

using namespace arraw;

namespace {

constexpr std::size_t channels = 4;

/// @brief Weights of the source pixels that make up one output coordinate.
struct Taps {
    std::int64_t first = 0; ///< Index of the first source pixel; may lie outside the image.
    std::size_t offset = 0; ///< Position of its weight in the shared weight array.
    std::size_t count = 0;  ///< Number of consecutive source pixels.
};

/// @brief Precomputed taps for every output coordinate along one axis.
struct AxisWeights {
    std::vector<Taps> taps;      ///< One entry per output coordinate.
    std::vector<double> weights; ///< Normalised weights of all taps, back to back.
};

/// @brief Alpha below which a resampled pixel counts as fully transparent (2^-16).
constexpr float transparentBelow = 1.0F / 65536.0F;

/// @brief Premultiplied pixels after one pass, with the range of colours that fed each of them.
struct Pass {
    std::vector<float> pixels; ///< Premultiplied RGBA.
    std::vector<float> low;    ///< Lowest unpremultiplied RGB of the contributing visible pixels.
    std::vector<float> high;   ///< Highest unpremultiplied RGB of the contributing visible pixels.
    std::vector<char> translucent; ///< Per pixel: whether any contributing pixel was not opaque.
};

/// @brief Evaluates a kernel at a distance in kernel units.
double kernel(ResizeFilter filter, double x) {
    const double distance = std::abs(x);
    if (filter == ResizeFilter::Bilinear) {
        return distance < 1.0 ? 1.0 - distance : 0.0;
    }
    if (distance >= 3.0) {
        return 0.0;
    }
    if (distance < 1e-12) {
        return 1.0;
    }
    const double pix = std::numbers::pi * distance;
    return 3.0 * std::sin(pix) * std::sin(pix / 3.0) / (pix * pix);
}

/// @brief Computes the taps and normalised weights for resizing one axis.
AxisWeights axisWeights(std::uint32_t in, std::uint32_t out, ResizeFilter filter) {
    const double scale = static_cast<double>(out) / in;
    const double stretch = std::max(1.0, 1.0 / scale);
    const double radius = (filter == ResizeFilter::Bilinear ? 1.0 : 3.0) * stretch;

    AxisWeights result;
    result.taps.reserve(out);
    if (in == out) {
        /// Lanczos at scale 1 is only nearly an identity, so make it exact.
        result.weights.assign(out, 1.0);
        for (std::uint32_t x = 0; x < out; ++x) {
            result.taps.push_back({x, x, 1});
        }
        return result;
    }
    for (std::uint32_t x = 0; x < out; ++x) {
        const double centre = (x + 0.5) / scale - 0.5;
        const auto first = static_cast<std::int64_t>(std::ceil(centre - radius));
        const auto last = static_cast<std::int64_t>(std::floor(centre + radius));
        const std::size_t offset = result.weights.size();
        double sum = 0.0;
        for (std::int64_t i = first; i <= last; ++i) {
            const double weight = kernel(filter, (static_cast<double>(i) - centre) / stretch);
            result.weights.push_back(weight);
            sum += weight;
        }
        for (std::size_t k = offset; k < result.weights.size(); ++k) {
            result.weights[k] /= sum;
        }
        result.taps.push_back({first, offset, result.weights.size() - offset});
    }
    return result;
}

/// @brief Clamps a source index into the image, which extends its edge pixels.
std::size_t clampIndex(std::int64_t index, std::uint32_t length) {
    return static_cast<std::size_t>(std::clamp<std::int64_t>(index, 0, length - 1));
}

/// @brief Stores an accumulated sample, applying the rule that ringing never goes below black.
float store(double value, bool sawNegative) {
    return (value < 0.0 && !sawNegative) ? 0.0F : static_cast<float>(value);
}

/// @brief Resizes the rows of premultiplied pixels, one output column at a time.
Pass horizontalPass(const ImageBuffer& source, std::uint32_t outWidth, ResizeFilter filter) {
    constexpr float inf = std::numeric_limits<float>::infinity();
    const ImageSize size = source.size();
    const AxisWeights axis = axisWeights(size.width, outWidth, filter);
    const auto input = source.samples<float>();
    const std::size_t outSamples = static_cast<std::size_t>(outWidth) * size.height * channels;
    Pass output{std::vector<float>(outSamples), std::vector<float>(outSamples, inf),
                std::vector<float>(outSamples, -inf),
                std::vector<char>(static_cast<std::size_t>(outWidth) * size.height)};
    std::vector<float> row(static_cast<std::size_t>(size.width) * channels);

    for (std::uint32_t y = 0; y < size.height; ++y) {
        const auto* in = &input[static_cast<std::size_t>(y) * size.width * channels];
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const float alpha = in[x * channels + 3];
            for (std::size_t c = 0; c < 3; ++c) {
                row[x * channels + c] = in[x * channels + c] * alpha;
            }
            row[x * channels + 3] = alpha;
        }
        const std::size_t rowStart = static_cast<std::size_t>(y) * outWidth * channels;
        for (std::uint32_t x = 0; x < outWidth; ++x) {
            const Taps& taps = axis.taps[x];
            std::array<double, channels> sum{};
            std::array<bool, channels> negative{};
            float* low = &output.low[rowStart + x * channels];
            float* high = &output.high[rowStart + x * channels];
            for (std::size_t k = 0; k < taps.count; ++k) {
                const double weight = axis.weights[taps.offset + k];
                const std::size_t index =
                    clampIndex(taps.first + static_cast<std::int64_t>(k), size.width);
                const float* pixel = &row[index * channels];
                for (std::size_t c = 0; c < channels; ++c) {
                    sum[c] += weight * pixel[c];
                    negative[c] = negative[c] || pixel[c] < 0.0F;
                }
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
            for (std::size_t c = 0; c < channels; ++c) {
                output.pixels[rowStart + x * channels + c] = store(sum[c], negative[c]);
            }
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
void verticalPass(const Pass& input, std::uint32_t width, std::uint32_t inHeight,
                  std::span<float> result, std::uint32_t outHeight, ResizeFilter filter) {
    constexpr float inf = std::numeric_limits<float>::infinity();
    const AxisWeights axis = axisWeights(inHeight, outHeight, filter);
    const std::size_t rowSamples = static_cast<std::size_t>(width) * channels;
    std::vector<double> sum(rowSamples);
    std::vector<char> negative(rowSamples);
    std::vector<float> low(rowSamples);
    std::vector<float> high(rowSamples);
    std::vector<char> translucent(width);

    for (std::uint32_t y = 0; y < outHeight; ++y) {
        const Taps& taps = axis.taps[y];
        std::fill(sum.begin(), sum.end(), 0.0);
        std::fill(negative.begin(), negative.end(), char{0});
        std::fill(low.begin(), low.end(), inf);
        std::fill(high.begin(), high.end(), -inf);
        std::fill(translucent.begin(), translucent.end(), char{0});
        for (std::size_t k = 0; k < taps.count; ++k) {
            const double weight = axis.weights[taps.offset + k];
            const std::size_t start =
                clampIndex(taps.first + static_cast<std::int64_t>(k), inHeight) * rowSamples;
            const float* in = &input.pixels[start];
            for (std::size_t i = 0; i < rowSamples; ++i) {
                sum[i] += weight * in[i];
                negative[i] |= static_cast<char>(in[i] < 0.0F);
                low[i] = std::min(low[i], input.low[start + i]);
                high[i] = std::max(high[i], input.high[start + i]);
            }
            const std::size_t startPixel = start / channels;
            for (std::uint32_t x = 0; x < width; ++x) {
                translucent[x] |= input.translucent[startPixel + x];
            }
        }
        float* out = &result[static_cast<std::size_t>(y) * rowSamples];
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t p = x * channels;
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

} // namespace

ImageBuffer arraw::resample(ImageBuffer source, ImageSize size, ResizeFilter filter) {
    if (source.format() != workingFormat) {
        throw std::invalid_argument("Resampling requires developed float pixels");
    }
    if (size.empty()) {
        throw std::invalid_argument("Resampling needs a result of at least one pixel");
    }
    if (source.size() == size) {
        return source;
    }

    const Pass across = horizontalPass(source, size.width, filter);
    ImageBuffer result(size, workingFormat, source.encoding(), source.orientation());
    verticalPass(across, size.width, source.size().height, result.samples<float>(), size.height,
                 filter);
    return result;
}
