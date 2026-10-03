#include "Exiv2Support.h"
#include "ImageImport.h"
#include "QtImage.h"

#include <QBuffer>
#include <QByteArray>
#include <QColorSpace>
#include <QImage>
#include <QImageIOHandler>
#include <QImageReader>
#include <QSize>
#include <Qt>

#include <exiv2/exiv2.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace arraw;

namespace {

/// @brief Reads the file's own orientation tag.
/// @return The orientation, Normal when the file records none or an invalid one.
ImageOrientation fileOrientation(const Exiv2::ExifData& data) {
    const auto found = data.findKey(Exiv2::ExifKey("Exif.Image.Orientation"));
    if (found == data.end() || found->count() == 0) {
        return ImageOrientation::Normal;
    }
    const auto value = found->toInt64();
    return value >= 1 && value <= 8 ? static_cast<ImageOrientation>(value)
                                    : ImageOrientation::Normal;
}

/// @brief Rearranges the pixels of an image so that it is upright.
///
/// Uses EXIF's own definition of the eight values: each says where the stored
/// image's first row and first column lie in the upright one.
/// @param image 32-bit image to turn.
/// @param orientation What the image's storage needs undoing.
/// @return A new image, with width and height swapped for the transposing values.
QImage upright(const QImage& image, ImageOrientation orientation) {
    if (orientation == ImageOrientation::Normal) {
        return image;
    }
    const int width = image.width();
    const int height = image.height();
    const bool swaps =
        orientation == ImageOrientation::Transpose || orientation == ImageOrientation::Rotate90 ||
        orientation == ImageOrientation::Transverse || orientation == ImageOrientation::Rotate270;
    QImage result(swaps ? height : width, swaps ? width : height, image.format());
    for (int y = 0; y < result.height(); ++y) {
        auto* out = reinterpret_cast<std::uint32_t*>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            int sourceX = x;
            int sourceY = y;
            switch (orientation) {
            case ImageOrientation::MirrorHorizontal:
                sourceX = width - 1 - x;
                break;
            case ImageOrientation::Rotate180:
                sourceX = width - 1 - x;
                sourceY = height - 1 - y;
                break;
            case ImageOrientation::MirrorVertical:
                sourceY = height - 1 - y;
                break;
            case ImageOrientation::Transpose:
                sourceX = y;
                sourceY = x;
                break;
            case ImageOrientation::Rotate90:
                sourceX = y;
                sourceY = height - 1 - x;
                break;
            case ImageOrientation::Transverse:
                sourceX = width - 1 - y;
                sourceY = height - 1 - x;
                break;
            case ImageOrientation::Rotate270:
                sourceX = width - 1 - y;
                sourceY = x;
                break;
            case ImageOrientation::Normal:
                break;
            }
            out[x] = reinterpret_cast<const std::uint32_t*>(image.constScanLine(sourceY))[sourceX];
        }
    }
    return result;
}

/// @brief Decodes one preview's bytes, upright, scaled down and in sRGB.
/// @param bytes The preview as the file stores it.
/// @param maxEdge Longest edge to keep; zero keeps all of it.
/// @param fallback Orientation to apply when the preview does not carry its own.
/// @return The image, or a null image if Qt cannot decode the bytes.
QImage decoded(const Exiv2::PreviewImage& preview, std::uint32_t maxEdge,
               ImageOrientation fallback) {
    QByteArray bytes(reinterpret_cast<const char*>(preview.pData()),
                     static_cast<qsizetype>(preview.size()));
    QBuffer device(&bytes);
    device.open(QIODevice::ReadOnly);
    QImageReader reader(&device);
    // A preview that says which way up it is has been turned already: applying
    // the file's orientation as well would turn it twice.
    const bool declaresOwn = reader.transformation() != QImageIOHandler::TransformationNone;
    reader.setAutoTransform(declaresOwn);
    QImage image = reader.read();
    if (image.isNull()) {
        return image;
    }

    if (image.colorSpace().isValid() && image.colorSpace() != QColorSpace::SRgb) {
        image.convertToColorSpace(QColorSpace::SRgb);
    }
    image.setColorSpace(QColorSpace::SRgb);
    image.convertTo(qtimage::toImageFormat(PixelFormat::RgbaU8));

    // Reduced before turning, which is the cheaper order and gives the same pixels.
    if (maxEdge > 0 && std::max(image.width(), image.height()) > static_cast<int>(maxEdge)) {
        image = image.scaled(static_cast<int>(maxEdge), static_cast<int>(maxEdge),
                             Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return declaresOwn ? image : upright(image, fallback);
}

/// @brief Orders the previews by how well they suit a size.
///
/// The smallest one that is at least @p maxEdge on its long edge comes first,
/// then the larger ones, then the smaller ones, nearest first, so that a decode
/// failure falls back to the next best.
std::vector<Exiv2::PreviewProperties> byPreference(std::vector<Exiv2::PreviewProperties> previews,
                                                   std::uint32_t maxEdge) {
    const auto edge = [](const Exiv2::PreviewProperties& p) {
        return std::max(p.width_, p.height_);
    };
    std::ranges::stable_sort(previews, [&](const auto& left, const auto& right) {
        const bool leftEnough = edge(left) >= maxEdge;
        const bool rightEnough = edge(right) >= maxEdge;
        if (leftEnough != rightEnough) {
            return leftEnough;
        }
        return leftEnough ? edge(left) < edge(right) : edge(left) > edge(right);
    });
    return previews;
}

} // namespace

std::optional<ImageBuffer> arraw::readEmbeddedPreview(const std::filesystem::path& path,
                                                      std::uint32_t maxEdge, DiagnosticLog& log) {
    exiv2support::prepare();
    try {
        std::error_code error;
        if (!std::filesystem::exists(path, error)) {
            throw std::runtime_error("the file cannot be opened");
        }
        const auto image = Exiv2::ImageFactory::open(exiv2support::path(path), false);
        image->readMetadata();
        const ImageOrientation orientation = fileOrientation(image->exifData());

        Exiv2::PreviewManager manager(*image);
        for (const auto& properties : byPreference(manager.getPreviewProperties(), maxEdge)) {
            const QImage result =
                decoded(manager.getPreviewImage(properties), maxEdge, orientation);
            if (!result.isNull()) {
                return qtimage::toBuffer(result, NamedEncoding::Srgb, ImageOrientation::Normal);
            }
        }
    } catch (const std::exception& problem) {
        log.record({.notice = Notice::PreviewUnreadable,
                    .severity = Severity::Warning,
                    .subject = path,
                    .values = {std::string(problem.what())}});
    }
    return std::nullopt;
}
