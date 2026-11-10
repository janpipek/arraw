#include "EncodedForOutput.h"

#include "ColorSpaces.h"
#include "QtImage.h"
#include "RowBands.h"
#include "TransferFunctions.h"

#include <QColorSpace>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <variant>

using namespace arraw;

namespace {

/// Straight-alpha colour in double: red, green, blue, alpha.
using Pixel = std::array<double, 4>;

/// @brief Per-image constants of a conversion, resolved once before any pixel is touched.
struct Plan {
    /// Source encoding's curve to linear.
    transfer::Curve decode;
    /// Linear to the target encoding's curve.
    transfer::Curve encode;
    /// Source linear RGB to target linear RGB.
    colorspaces::Matrix3d matrix;
    /// Whether the two encodings share primaries, so that the matrix is skipped.
    bool samePrimaries;
    /// Whether the result is clamped to [0, 1], as every integer layout is.
    bool clamp;
};

/// @brief Clamps to [0, 1]; not-a-number gives zero.
double saturate(double value) {
    return value > 0.0 ? (value < 1.0 ? value : 1.0) : 0.0;
}

/// @brief Rounds a value in [0, 1] to the nearest code of a depth.
/// @tparam Code Unsigned type of the code.
/// @tparam Max Largest code.
template <typename Code, unsigned Max> Code quantise(double value) {
    return static_cast<Code>(saturate(value) * Max + 0.5);
}

/// @brief Converts one pixel from source encoding to target encoding.
Pixel convert(const Plan& plan, Pixel pixel) {
    for (std::size_t c = 0; c < 3; ++c) {
        pixel[c] = plan.decode(pixel[c]);
    }
    if (!plan.samePrimaries) {
        const auto& m = plan.matrix;
        const double r = pixel[0];
        const double g = pixel[1];
        const double b = pixel[2];
        pixel[0] = m[0] * r + m[1] * g + m[2] * b;
        pixel[1] = m[3] * r + m[4] * g + m[5] * b;
        pixel[2] = m[6] * r + m[7] * g + m[8] * b;
    }
    for (std::size_t c = 0; c < 3; ++c) {
        // Before the curve, so that a value beyond the gamut is clipped where it is linear.
        pixel[c] = plan.encode(plan.clamp ? saturate(pixel[c]) : pixel[c]);
    }
    return pixel;
}

/// @brief Reader of pixels from a source row of 8-bit samples.
template <std::size_t Channels> struct ReadU8 {
    Pixel operator()(const uchar* row, int x) const {
        const uchar* p = row + Channels * static_cast<std::size_t>(x);
        return {p[0] / 255.0, p[1] / 255.0, p[2] / 255.0, Channels == 4 ? p[3] / 255.0 : 1.0};
    }
};

/// @brief Reader of pixels from a source row of RGBA 16-bit samples.
struct ReadU16 {
    Pixel operator()(const uchar* row, int x) const {
        std::array<std::uint16_t, 4> p{};
        std::memcpy(p.data(), row + 8 * static_cast<std::size_t>(x), sizeof p);
        return {p[0] / 65535.0, p[1] / 65535.0, p[2] / 65535.0, p[3] / 65535.0};
    }
};

/// @brief Reader of pixels from a source row of RGBA float samples.
struct ReadF32 {
    Pixel operator()(const uchar* row, int x) const {
        std::array<float, 4> p{};
        std::memcpy(p.data(), row + 16 * static_cast<std::size_t>(x), sizeof p);
        return {p[0], p[1], p[2], p[3]};
    }
};

/// @brief Writer of pixels to an 8-bit target row, with or without alpha.
template <std::size_t Channels> struct WriteU8 {
    void operator()(uchar* row, int x, const Pixel& pixel) const {
        uchar* p = row + Channels * static_cast<std::size_t>(x);
        for (std::size_t c = 0; c < 3; ++c) {
            p[c] = quantise<uchar, 255>(pixel[c]);
        }
        if constexpr (Channels == 4) {
            p[3] = quantise<uchar, 255>(pixel[3]);
        }
    }
};

/// @brief Writer of pixels to a 16-bit target row; without alpha the fourth sample is full.
template <bool HasAlpha> struct WriteU16 {
    void operator()(uchar* row, int x, const Pixel& pixel) const {
        const std::array<std::uint16_t, 4> p{
            quantise<std::uint16_t, 65535>(pixel[0]), quantise<std::uint16_t, 65535>(pixel[1]),
            quantise<std::uint16_t, 65535>(pixel[2]),
            HasAlpha ? quantise<std::uint16_t, 65535>(pixel[3]) : std::uint16_t{65535}};
        std::memcpy(row + 8 * static_cast<std::size_t>(x), p.data(), sizeof p);
    }
};

/// @brief Writer of pixels to a float target row, as they are.
struct WriteF32 {
    void operator()(uchar* row, int x, const Pixel& pixel) const {
        const std::array<float, 4> p{static_cast<float>(pixel[0]), static_cast<float>(pixel[1]),
                                     static_cast<float>(pixel[2]), static_cast<float>(pixel[3])};
        std::memcpy(row + 16 * static_cast<std::size_t>(x), p.data(), sizeof p);
    }
};

/// @brief Converts every pixel of @p source into @p target, in bands of rows.
template <typename Read, typename Write>
void convertImage(const QImage& source, QImage& target, const Plan& plan, Read read, Write write) {
    const int width = source.width();
    const int height = source.height();
    const uchar* const input = source.constBits();
    const auto inputStride = static_cast<std::size_t>(source.bytesPerLine());
    // Taken once, here: a scanLine call from a band would detach the image there.
    uchar* const output = target.bits();
    const auto outputStride = static_cast<std::size_t>(target.bytesPerLine());
    // Rows are independent, so any split gives the single-threaded bits. Not counted as progress.
    detail::splitRowBands(static_cast<std::uint32_t>(height), static_cast<std::uint32_t>(width),
                          [&](std::uint32_t first, std::uint32_t last, bool /*onCaller*/) {
                              for (std::size_t y = first; y < last; ++y) {
                                  const uchar* in = input + y * inputStride;
                                  uchar* out = output + y * outputStride;
                                  for (int x = 0; x < width; ++x) {
                                      write(out, x, convert(plan, read(in, x)));
                                  }
                              }
                          });
}

/// @brief Calls @p body with the reader of a source layout.
/// @throws std::invalid_argument if the layout is not one ::arraw::qtimage::toImage gives.
template <typename Body> void withReader(QImage::Format format, Body&& body) {
    switch (format) {
    case QImage::Format_RGB888:
        return body(ReadU8<3>{});
    case QImage::Format_RGBA8888:
        return body(ReadU8<4>{});
    case QImage::Format_RGBA64:
        return body(ReadU16{});
    case QImage::Format_RGBA32FPx4:
        return body(ReadF32{});
    default:
        throw std::invalid_argument("Unsupported source layout");
    }
}

/// @brief Calls @p body with the writer of a target layout, if it is one.
/// @return Whether @p format is a layout the conversion writes. The one list of them.
template <typename Body> bool withWriter(QImage::Format format, Body&& body) {
    switch (format) {
    case QImage::Format_RGB888:
        body(WriteU8<3>{});
        return true;
    case QImage::Format_RGBA8888:
        body(WriteU8<4>{});
        return true;
    case QImage::Format_RGBX64:
        body(WriteU16<false>{});
        return true;
    case QImage::Format_RGBA64:
        body(WriteU16<true>{});
        return true;
    case QImage::Format_RGBA32FPx4:
        body(WriteF32{});
        return true;
    default:
        return false;
    }
}

/// @brief Converts @p source into a new image of a layout, by a plan.
/// @throws std::invalid_argument if @p format is not a layout that is written.
/// @throws std::runtime_error if the result cannot be allocated.
QImage convertedBy(const QImage& source, const Plan& plan, QImage::Format format) {
    if (!withWriter(format, [](auto) {})) {
        throw std::invalid_argument("Unsupported output layout");
    }
    QImage target(source.size(), format);
    if (target.isNull()) {
        throw std::runtime_error("Cannot prepare image for export");
    }
    withReader(source.format(), [&](auto read) {
        withWriter(format, [&](auto write) { convertImage(source, target, plan, read, write); });
    });
    return target;
}

} // namespace

QImage arraw::encodedForOutput(const ImageBuffer& image, NamedEncoding encoding,
                               QImage::Format format) {
    // A camera-native buffer has no colour space Qt could name, and no output
    // profile could describe one sensor's primaries to a viewer. Refusing here
    // says which stage is missing rather than which enumerator is unknown.
    const auto* sourceEncoding = std::get_if<NamedEncoding>(&image.encoding());
    if (sourceEncoding == nullptr) {
        throw std::invalid_argument(
            "A camera-native buffer must pass white balance before it can be exported");
    }
    const QImage source = qtimage::toImage(image);
    if (source.isNull()) {
        throw std::invalid_argument("Unsupported or invalid image buffer");
    }
    const Plan plan{.decode = transfer::decoderFor(*sourceEncoding),
                    .encode = transfer::encoderFor(encoding),
                    .matrix = colorspaces::between(*sourceEncoding, encoding),
                    .samePrimaries = colorspaces::primariesOf(*sourceEncoding) ==
                                     colorspaces::primariesOf(encoding),
                    .clamp = format != QImage::Format_RGBA32FPx4};
    QImage target = convertedBy(source, plan, format);
    // Qt only labels: the numbers are ours (ADR 057), the profile says what they mean.
    target.setColorSpace(qtimage::toColorSpace(encoding));
    return target;
}

QImage arraw::quantisedForOutput(const QImage& floats, QImage::Format format) {
    if (floats.format() != QImage::Format_RGBA32FPx4) {
        throw std::invalid_argument("Only a float image can be quantised for output");
    }
    // Both curves and both primaries are the image's own, so only the clamp and the rounding act.
    const Plan plan{.decode = transfer::identity,
                    .encode = transfer::identity,
                    .matrix = {},
                    .samePrimaries = true,
                    .clamp = format != QImage::Format_RGBA32FPx4};
    QImage target = convertedBy(floats, plan, format);
    target.setColorSpace(floats.colorSpace());
    return target;
}
