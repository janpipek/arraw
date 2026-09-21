#include "ImageImport.h"

#include <QFile>
#include <QImage>
#include <QImageReader>

#include <cstdint>
#include <cstring>
#include <stdexcept>

using namespace arraw;
using namespace std;

namespace {

/// @brief A buffer layout together with the QImage layout matching it byte for byte.
struct LayoutPair {
    PixelFormat buffer;
    QImage::Format qt;
};

/// @brief Chooses the buffer layout that preserves a decoded image's precision.
///
/// Only layouts that ::exportImage can write back are produced, so anything
/// that loads can also be saved. Sixteen-bit and float sources therefore gain
/// an opaque alpha channel, QImage having no three-channel wide layout.
/// @param image Decoded image to inspect.
/// @return The chosen pair of layouts.
LayoutPair chooseLayout(const QImage& image) {
    switch (image.format()) {
    case QImage::Format_RGBX64:
    case QImage::Format_RGBA64:
    case QImage::Format_RGBA64_Premultiplied:
    case QImage::Format_Grayscale16:
        return {PixelFormat::RgbaU16, QImage::Format_RGBA64};

    case QImage::Format_RGBX32FPx4:
    case QImage::Format_RGBA32FPx4:
    case QImage::Format_RGBA32FPx4_Premultiplied:
        return {PixelFormat::RgbaF32, QImage::Format_RGBA32FPx4};

    default:
        return image.hasAlphaChannel() ? LayoutPair{PixelFormat::RgbaU8, QImage::Format_RGBA8888}
                                       : LayoutPair{PixelFormat::RgbU8, QImage::Format_RGB888};
    }
}

/// @brief Copies a decoded image into a tightly packed buffer.
/// @param image Decoded image; any QImage layout is accepted.
/// @return A buffer holding the same pixels, tagged as sRGB.
/// @throws std::runtime_error if the image cannot be converted or is empty.
ImageBuffer qimageToBuffer(const QImage& image) {
    const LayoutPair layout = chooseLayout(image);
    const QImage source = image.convertToFormat(layout.qt);
    if (source.isNull()) {
        throw std::runtime_error("decoded image could not be converted to a supported layout");
    }

    // Any embedded ICC profile is ignored for now; sRGB is assumed.
    ImageBuffer buffer({static_cast<std::uint32_t>(source.width()),
                           static_cast<std::uint32_t>(source.height())},
        layout.buffer, ColorEncoding::Srgb);

    // QImage pads rows to a four-byte boundary, ImageBuffer does not, so the
    // two strides differ and the copy has to go row by row.
    const std::size_t rowBytes = buffer.rowStride();
    std::byte* destination = buffer.bytes().data();
    for (int y = 0; y < source.height(); ++y) {
        std::memcpy(destination + static_cast<std::size_t>(y) * rowBytes, source.constScanLine(y),
            rowBytes);
    }

    return buffer;
}

} // namespace

ImageBuffer arraw::loadImage(const std::filesystem::path& path) {
    QFile sourceFile(path);
    if (!sourceFile.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(
            sourceFile.fileName().toStdString() + ": " + sourceFile.errorString().toStdString());
    }

    // Reading from the device rather than a file name lets the reader detect
    // the format from the content instead of trusting the extension.
    QImageReader reader(&sourceFile);
    const QImage image = reader.read();
    if (image.isNull()) {
        throw std::runtime_error(
            sourceFile.fileName().toStdString() + ": " + reader.errorString().toStdString());
    }

    // TODO: choose the target PixelFormat, convert the QImage into the matching
    // layout, and copy it row by row into an ImageBuffer.
    return qimageToBuffer(image);
}