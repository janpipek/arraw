#include "QtImage.h"

#include <cstring>
#include <stdexcept>
#include <utility>

QImage::Format arraw::qtimage::toImageFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::RgbU8:
        return QImage::Format_RGB888;
    case PixelFormat::RgbaU8:
        return QImage::Format_RGBA8888;
    case PixelFormat::RgbaU16:
        return QImage::Format_RGBA64;
    case PixelFormat::RgbaF32:
        return QImage::Format_RGBA32FPx4;

    // QImage has no three-channel 16-bit or 32-bit float format; these must be
    // widened to four channels before they can be wrapped.
    case PixelFormat::RgbU16:
    case PixelFormat::RgbF32:
        return QImage::Format_Invalid;
    }
    return QImage::Format_Invalid;
}

std::optional<arraw::PixelFormat> arraw::qtimage::toPixelFormat(QImage::Format format) {
    switch (format) {
    case QImage::Format_RGB888:
        return PixelFormat::RgbU8;
    case QImage::Format_RGBA8888:
        return PixelFormat::RgbaU8;
    case QImage::Format_RGBA64:
        return PixelFormat::RgbaU16;
    case QImage::Format_RGBA32FPx4:
        return PixelFormat::RgbaF32;
    default:
        return std::nullopt;
    }
}

QColorSpace arraw::qtimage::toColorSpace(NamedEncoding encoding) {
    switch (encoding) {
    case NamedEncoding::LinearRec2020:
        // Real primaries containing both output gamuts, and a linear transfer
        // so exposure and blending are physically meaningful (ADR 003).
        return QColorSpace(QColorSpace::Primaries::Bt2020, QColorSpace::TransferFunction::Linear);
    case NamedEncoding::Srgb:
        return QColorSpace::SRgb;
    case NamedEncoding::DisplayP3:
        return QColorSpace::DisplayP3;
    case NamedEncoding::AdobeRgb:
        return QColorSpace::AdobeRgb;
    }
    throw std::invalid_argument("Unknown colour encoding");
}

QImage arraw::qtimage::toImage(const ImageBuffer& buffer) {
    if (!std::in_range<int>(buffer.size().width) || !std::in_range<int>(buffer.size().height) ||
        !std::in_range<qsizetype>(buffer.rowStride())) {
        throw std::invalid_argument("Image dimensions exceed the encoder's limits");
    }

    const auto* samples = reinterpret_cast<const uchar*>(buffer.bytes().data());
    return QImage(samples, static_cast<int>(buffer.size().width),
                  static_cast<int>(buffer.size().height),
                  static_cast<qsizetype>(buffer.rowStride()), toImageFormat(buffer.format()));
}

arraw::ImageBuffer arraw::qtimage::toBuffer(const QImage& image, ColorEncoding encoding) {
    if (image.isNull()) {
        throw std::invalid_argument("Cannot copy a null image into a buffer");
    }

    const std::optional<PixelFormat> format = toPixelFormat(image.format());
    if (!format.has_value()) {
        throw std::invalid_argument("Image layout has no arraw equivalent");
    }

    ImageBuffer buffer(
        {static_cast<std::uint32_t>(image.width()), static_cast<std::uint32_t>(image.height())},
        *format, encoding);

    // QImage pads rows to a four-byte boundary and ImageBuffer does not, so the
    // strides differ and the copy has to go row by row.
    const std::size_t rowBytes = buffer.rowStride();
    std::byte* destination = buffer.bytes().data();
    for (int y = 0; y < image.height(); ++y) {
        std::memcpy(destination + static_cast<std::size_t>(y) * rowBytes, image.constScanLine(y),
                    rowBytes);
    }

    return buffer;
}
