#include <ImageExport.h>

#include <QFile>
#include <QImage>
#include <QImageWriter>

#include <algorithm>
#include <cctype>
#include <string>

using namespace arraw;
using namespace std;

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
static QImage::Format pixelFormatToQImageFormat(PixelFormat format) {
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
    const uchar* data = reinterpret_cast<const uchar*>(imageBuffer.bytes().data());
    QImage result(data, imageBuffer.size().width, imageBuffer.size().height,
                  imageBuffer.rowStride(), pixelFormatToQImageFormat(imageBuffer.format()));
    return result;
}

void arraw::exportImage(const ImageBuffer& image, const std::filesystem::path& path,
                        const ExportOptions& options) {
    const ImageFileFormat format = extractImageFileFormat(path, options);
    QFile outputFile(path);
    if (!outputFile.open(QIODevice::WriteOnly)) {
        exit(-127); // TODO: Rewrite to proper exceptions!
    }
    QImageWriter writer(&outputFile, fileFormatToString(format));
    QImage qImage = imageBufferToQImage(image);
    writer.write(qImage);
}
