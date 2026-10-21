#include "OutputHistogramCount.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace arraw;

namespace {

/// @brief Decoding and luminance of one output encoding, from which luma is counted.
struct LumaTable {
    /// Linear value of each code.
    std::array<float, outputHistogramBins> decoded{};

    /// Linear luminance above which a pixel's luma is code `i + 1`, for `i` in 0..254: the
    /// linear value of the half-way point between codes `i` and `i + 1`.
    std::array<float, outputHistogramBins - 1> boundaries{};

    /// Y row of the encoding's RGB-to-XYZ matrix, normalised to sum to one.
    std::array<float, 3> weights{};

    /// Luma code of the start of each cell of `lumaCells` equal steps of linear luminance: where
    /// the search over `boundaries` would land for that value. A value in the cell lands at this
    /// code or above, so a short forward walk over the boundaries makes the lookup exact.
    std::vector<std::uint8_t> cellStart;
};

/// Cells of the luma lookup over linear luminance 0..1.
constexpr std::size_t lumaCells = 65536;

/// @brief Decodes an sRGB (and Display P3) transfer function value to linear.
double decodeSrgb(double encoded) {
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

/// @brief Decodes an Adobe RGB (1998) value to linear.
double decodeAdobe(double encoded) {
    return std::pow(encoded, 563.0 / 256.0);
}

LumaTable buildTable(NamedEncoding encoding) {
    LumaTable table;
    std::array<double, 3> y{};
    double (*decode)(double) = decodeSrgb;
    switch (encoding) {
    case NamedEncoding::Srgb:
        y = {0.2126729, 0.7151522, 0.0721750};
        break;
    case NamedEncoding::DisplayP3:
        y = {0.2289746, 0.6917385, 0.0792869};
        break;
    case NamedEncoding::AdobeRgb:
        y = {0.2973769, 0.6273491, 0.0752741};
        decode = decodeAdobe;
        break;
    default:
        throw std::invalid_argument(
            "An output histogram counts sRGB, Display P3 or Adobe RGB pictures");
    }
    const double sum = y[0] + y[1] + y[2];
    for (std::size_t i = 0; i < 3; ++i) {
        table.weights[i] = static_cast<float>(y[i] / sum);
    }
    for (std::size_t code = 0; code < outputHistogramBins; ++code) {
        table.decoded[code] = static_cast<float>(decode(static_cast<double>(code) / 255.0));
    }
    for (std::size_t code = 0; code + 1 < outputHistogramBins; ++code) {
        table.boundaries[code] =
            static_cast<float>(decode((static_cast<double>(code) + 0.5) / 255.0));
    }
    table.cellStart.resize(lumaCells);
    for (std::size_t cell = 0; cell < lumaCells; ++cell) {
        const float start = static_cast<float>(cell) / static_cast<float>(lumaCells);
        table.cellStart[cell] = static_cast<std::uint8_t>(
            std::ranges::upper_bound(table.boundaries, start) - table.boundaries.begin());
    }
    return table;
}

/// @brief Gives the luma code of a linear luminance: the number of boundaries at or below it.
/// @details Equal to a binary search over the boundaries, without its mispredicted branches.
std::size_t lumaCode(const LumaTable& table, float linear) {
    const auto cell = static_cast<std::size_t>(std::min(
        std::max(linear, 0.0F) * static_cast<float>(lumaCells), static_cast<float>(lumaCells - 1)));
    std::size_t code = table.cellStart[cell];
    // Boundaries inside the cell (two lie within one cell at the dark end of Adobe RGB), and
    // luminance that rounds a little above one.
    while (code < table.boundaries.size() && linear >= table.boundaries[code]) {
        ++code;
    }
    return code;
}

/// @brief Gives the tables of an output encoding, built once.
const LumaTable& tableFor(NamedEncoding encoding) {
    static const LumaTable srgb = buildTable(NamedEncoding::Srgb);
    static const LumaTable p3 = buildTable(NamedEncoding::DisplayP3);
    static const LumaTable adobe = buildTable(NamedEncoding::AdobeRgb);
    switch (encoding) {
    case NamedEncoding::Srgb:
        return srgb;
    case NamedEncoding::DisplayP3:
        return p3;
    case NamedEncoding::AdobeRgb:
        return adobe;
    default:
        break;
    }
    throw std::invalid_argument(
        "An output histogram counts sRGB, Display P3 or Adobe RGB pictures");
}

/// @brief Reads a sample of a row without assuming alignment.
template <typename Sample> Sample load(const std::byte* row, std::size_t index) {
    Sample value;
    std::memcpy(&value, row + index * sizeof(Sample), sizeof(Sample));
    return value;
}

/// @brief Rounds a sample to its 8-bit code.
template <typename Sample> std::uint8_t toCode(Sample sample) {
    if constexpr (std::is_same_v<Sample, std::uint8_t>) {
        return sample;
    } else if constexpr (std::is_same_v<Sample, std::uint16_t>) {
        return static_cast<std::uint8_t>((static_cast<std::uint32_t>(sample) * 255U + 32767U) /
                                         65535U);
    } else {
        // NaN fails the first test and counts as 0.
        if (!(sample > 0.0F)) {
            return 0;
        }
        if (!(sample < 1.0F)) {
            return 255;
        }
        return static_cast<std::uint8_t>(sample * 255.0F + 0.5F);
    }
}

/// @brief Tells whether a sample of alpha leaves the pixel invisible.
template <typename Sample> bool transparent(Sample alpha) {
    if constexpr (std::is_same_v<Sample, float>) {
        return !(alpha > 0.0F);
    } else {
        return alpha == 0;
    }
}

template <typename Sample>
void countRows(const std::byte* rows, ImageSize size, std::size_t stride, std::size_t channels,
               const LumaTable& table, std::span<const RowSpan> spans,
               const std::atomic<bool>* abandon, std::uint32_t step, OutputHistogram& histogram) {
    std::uint32_t rowsDone = 0;
    for (std::uint32_t y = 0; y < size.height; y += step, ++rowsDone) {
        if (rowsDone % outputCountBandRows == 0 && abandon != nullptr &&
            abandon->load(std::memory_order_relaxed)) {
            return;
        }
        const std::byte* row = rows + static_cast<std::size_t>(y) * stride;
        const std::uint32_t first = spans.empty() ? 0 : spans[y].begin;
        const std::uint32_t begin = (first + step - 1) / step * step;
        const std::uint32_t end = spans.empty() ? size.width : spans[y].end;
        for (std::uint32_t x = begin; x < end; x += step) {
            const std::size_t at = static_cast<std::size_t>(x) * channels;
            if (channels == 4 && transparent(load<Sample>(row, at + 3))) {
                continue;
            }
            const std::uint8_t red = toCode(load<Sample>(row, at));
            const std::uint8_t green = toCode(load<Sample>(row, at + 1));
            const std::uint8_t blue = toCode(load<Sample>(row, at + 2));
            const float linear = table.weights[0] * table.decoded[red] +
                                 table.weights[1] * table.decoded[green] +
                                 table.weights[2] * table.decoded[blue];
            ++histogram.luma[lumaCode(table, linear)];
            ++histogram.red[red];
            ++histogram.green[green];
            ++histogram.blue[blue];
            ++histogram.pixels;
            if (red == 0 || green == 0 || blue == 0) {
                ++histogram.shadowsClipped;
            }
            if (red == 255 || green == 255 || blue == 255) {
                ++histogram.highlightsClipped;
            }
        }
    }
}

} // namespace

std::uint32_t arraw::outputHistogramStep(ImageSize size) {
    std::uint32_t step = 1;
    const auto counted = [&](std::uint64_t n) {
        return (size.width + n - 1) / n * ((size.height + n - 1) / n);
    };
    while (counted(step) > outputHistogramMaxPixels) {
        ++step;
    }
    return step;
}

OutputHistogram arraw::countOutputCodes(const std::byte* rows, ImageSize size, std::size_t stride,
                                        PixelFormat format, NamedEncoding encoding,
                                        std::span<const RowSpan> spans,
                                        const std::atomic<bool>* abandon, std::uint32_t step) {
    if (step == 0) {
        throw std::invalid_argument("A count takes a step of at least 1");
    }
    const LumaTable& table = tableFor(encoding);
    if (!spans.empty()) {
        if (spans.size() != size.height) {
            throw std::invalid_argument("A count takes one span per row, or none");
        }
        for (const RowSpan& span : spans) {
            if (span.end > size.width) {
                throw std::invalid_argument("A span of a count leaves its row");
            }
        }
    }
    OutputHistogram histogram;
    histogram.encoding = encoding;
    const std::size_t channels = channelCount(format);
    switch (format) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbaU8:
        countRows<std::uint8_t>(rows, size, stride, channels, table, spans, abandon, step,
                                histogram);
        break;
    case PixelFormat::RgbU16:
    case PixelFormat::RgbaU16:
        countRows<std::uint16_t>(rows, size, stride, channels, table, spans, abandon, step,
                                 histogram);
        break;
    case PixelFormat::RgbF32:
    case PixelFormat::RgbaF32:
        countRows<float>(rows, size, stride, channels, table, spans, abandon, step, histogram);
        break;
    }
    return histogram;
}
