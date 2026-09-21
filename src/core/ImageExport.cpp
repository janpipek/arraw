#include <ImageExport.h>

#include "ColorSpaces.h"

#include <QColorSpace>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QSaveFile>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

using namespace arraw;

namespace {

/// @brief Resolves the format to encode, preferring an explicit request over
/// the destination's extension.
/// @param path Destination path, whose extension is the fallback.
/// @param options Export options, which may name a format outright.
/// @return The resolved file format.
/// @throws std::invalid_argument if no format was requested and the extension
/// is not one arraw writes.
ImageFileFormat extractImageFileFormat(const std::filesystem::path& path,
                                       const ExportOptions& options) {
    if (options.format.has_value()) {
        return *options.format;
    }

    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });

    if (extension == ".jpg" || extension == ".jpeg") {
        return ImageFileFormat::Jpeg;
    }
    if (extension == ".png") {
        return ImageFileFormat::Png;
    }
    if (extension == ".tif" || extension == ".tiff") {
        return ImageFileFormat::Tiff;
    }

    throw std::invalid_argument(
        extension.empty() ? "no image format requested and the destination has no extension"
                          : "no image format requested and '" + extension + "' names none");
}

QByteArray fileFormatToString(ImageFileFormat format) {
    switch (format) {
    case ImageFileFormat::Png:
        return "png";
    case ImageFileFormat::Jpeg:
        return "jpg";
    case ImageFileFormat::Tiff:
        return "tiff";
    }
    throw std::invalid_argument("Unknown image file format");
}

/// @brief Maps a sample layout onto the QImage format with identical memory
/// layout, so a buffer can be wrapped without converting it.
/// @param format Sample layout to map.
/// @return The matching QImage format, or `QImage::Format_Invalid` when QImage
/// has no layout-compatible equivalent.
QImage::Format pixelFormatToQImageFormat(PixelFormat format) {
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

QImage imageBufferToQImage(const ImageBuffer& imageBuffer) {
    if (!std::in_range<int>(imageBuffer.size().width) ||
        !std::in_range<int>(imageBuffer.size().height) ||
        !std::in_range<qsizetype>(imageBuffer.rowStride())) {
        throw std::invalid_argument("Image dimensions exceed the encoder's limits");
    }
    const uchar* data = reinterpret_cast<const uchar*>(imageBuffer.bytes().data());
    QImage result(data, imageBuffer.size().width, imageBuffer.size().height,
                  imageBuffer.rowStride(), pixelFormatToQImageFormat(imageBuffer.format()));
    return result;
}

/// @brief Validates the options applicable to the destination format.
void validateExportOptions(ImageFileFormat format, const ExportOptions& options) {
    if (options.encoding == workingEncoding) {
        throw std::invalid_argument("The working encoding is internal, not an output one");
    }
    if (options.bitDepth != 8 && options.bitDepth != 16) {
        throw std::invalid_argument("Export bit depth must be 8 or 16");
    }
    if (format == ImageFileFormat::Jpeg) {
        if (options.bitDepth != 8) {
            throw std::invalid_argument("JPEG export requires 8-bit samples");
        }
        if (options.quality < 0 || options.quality > 100) {
            throw std::invalid_argument("JPEG quality must be between 0 and 100");
        }
    }
}

/// @brief Checks whether every alpha sample in an RGBA buffer is opaque.
template <typename Sample> bool hasOpaqueAlpha(std::span<const Sample> samples, Sample opaque) {
    for (std::size_t index = 3; index < samples.size(); index += 4) {
        if (samples[index] != opaque) {
            return false;
        }
    }
    return true;
}

/// @brief Checks opacity before any conversion can quantise away transparency.
bool isOpaque(const ImageBuffer& image) {
    switch (image.format()) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbU16:
    case PixelFormat::RgbF32:
        return true;
    case PixelFormat::RgbaU8:
        return hasOpaqueAlpha(image.samples<std::uint8_t>(), std::uint8_t{255});
    case PixelFormat::RgbaU16:
        return hasOpaqueAlpha(image.samples<std::uint16_t>(), std::uint16_t{65535});
    case PixelFormat::RgbaF32:
        return hasOpaqueAlpha(image.samples<float>(), 1.0F);
    }
    throw std::invalid_argument("Unknown pixel format");
}

/// @brief Selects the encoder's sample layout, preserving alpha for PNG and TIFF.
QImage::Format outputPixelFormat(ImageFileFormat format, int bitDepth, bool hasAlpha) {
    if (format == ImageFileFormat::Jpeg) {
        return QImage::Format_RGB888;
    }
    if (bitDepth == 16) {
        return hasAlpha ? QImage::Format_RGBA64 : QImage::Format_RGBX64;
    }
    return hasAlpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888;
}

/// @brief Converts colour and sample depth, and selects profile metadata for export.
QImage prepareExportImage(const ImageBuffer& image, ImageFileFormat format,
                          const ExportOptions& options) {
    const auto sourceSpace = colorSpaceFor(image.encoding());
    const auto targetSpace = colorSpaceFor(options.encoding);
    QImage source = imageBufferToQImage(image);
    if (source.isNull()) {
        throw std::invalid_argument("Unsupported or invalid image buffer");
    }
    if (format == ImageFileFormat::Jpeg && !isOpaque(image)) {
        throw std::invalid_argument("JPEG export requires opaque pixels");
    }

    source.setColorSpace(sourceSpace);
    QImage prepared = source.convertedToColorSpace(
        targetSpace, outputPixelFormat(format, options.bitDepth, source.hasAlphaChannel()));
    if (prepared.isNull()) {
        throw std::runtime_error("Cannot prepare image for export");
    }
    if (!options.embedProfile) {
        prepared.setColorSpace(QColorSpace{});
    }
    return prepared;
}

} // namespace

void arraw::exportImage(const ImageBuffer& image, const std::filesystem::path& path,
                        const ExportOptions& options) {
    const auto format = extractImageFileFormat(path, options);
    const auto fileFormat = fileFormatToString(format);
    validateExportOptions(format, options);
    const QImage qImage = prepareExportImage(image, format, options);

    /// Use QFile for path conversion on Qt 6.10; QSaveFile's std::filesystem::path
    /// constructor is available from Qt 6.11.
    const auto fileName = QFile(path).fileName();
    QSaveFile output(fileName);
    if (!output.open(QIODevice::WriteOnly)) {
        throw std::runtime_error("Cannot open export destination: " +
                                 output.errorString().toStdString());
    }

    {
        QImageWriter writer(&output, fileFormat);
        if (format == ImageFileFormat::Jpeg) {
            writer.setQuality(options.quality);
        }

        if (!writer.write(qImage)) {
            throw std::runtime_error("Cannot encode image: " + writer.errorString().toStdString());
        }
    }

    if (!output.commit()) {
        throw std::runtime_error("Cannot commit export: " + output.errorString().toStdString());
    }
}
