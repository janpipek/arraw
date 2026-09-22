#include "ImageImport.h"

#include "QtImage.h"
#include "RawImport.h"

#include <QColorSpace>
#include <QFile>
#include <QImage>
#include <QImageIOHandler>
#include <QImageReader>

#include <cstdint>
#include <stdexcept>

using namespace arraw;
using namespace std;

namespace {

/// @brief Reads the orientation using Qt's mirror-before-rotation convention.
ImageOrientation orientationOf(const QImageReader& reader) {
    switch (reader.transformation()) {
    case QImageIOHandler::TransformationMirror:
        return ImageOrientation::MirrorHorizontal;
    case QImageIOHandler::TransformationFlip:
        return ImageOrientation::MirrorVertical;
    case QImageIOHandler::TransformationRotate180:
        return ImageOrientation::Rotate180;
    case QImageIOHandler::TransformationRotate90:
        return ImageOrientation::Rotate90;
    case QImageIOHandler::TransformationMirrorAndRotate90:
        return ImageOrientation::Transverse;
    case QImageIOHandler::TransformationFlipAndRotate90:
        return ImageOrientation::Transpose;
    case QImageIOHandler::TransformationRotate270:
        return ImageOrientation::Rotate270;
    default:
        return ImageOrientation::Normal;
    }
}

/// @brief Chooses the buffer layout that preserves a decoded image's precision.
///
/// Only layouts that ::exportImage can write back are produced, so anything
/// that loads can also be saved. Every result is at least sixteen bits per
/// channel: the samples are linearised on the way in, and eight linear bits
/// crush the shadows. Three-channel wide layouts do not exist in QImage, so
/// the alpha channel comes along, opaque where the source had none.
/// @param image Decoded image to inspect.
/// @return The chosen buffer layout.
PixelFormat chooseLayout(const QImage& image) {
    switch (image.format()) {
    case QImage::Format_RGBX32FPx4:
    case QImage::Format_RGBA32FPx4:
    case QImage::Format_RGBA32FPx4_Premultiplied:
    case QImage::Format_RGBX16FPx4:
    case QImage::Format_RGBA16FPx4:
    case QImage::Format_RGBA16FPx4_Premultiplied:
        return PixelFormat::RgbaF32;

    default:
        return PixelFormat::RgbaU16;
    }
}

/// @brief Decodes through Qt's codecs, or returns a null image if they decline.
///
/// Reading from the device rather than a file name lets the reader detect the
/// format from the content instead of trusting the extension.
///
/// A detected format of "raw" is treated as a decline. KDE's kimageformats
/// installs a LibRaw-backed plugin into Qt's plugin directory, and it claims
/// files by extension -- including RAW extensions arraw does not list, such as
/// .mrw, .x3f and .crw. Left to it, those would be decoded with its processing
/// choices rather than ours (it bakes in orientation, which arraw applies at
/// development), with the metadata discarded, and only on machines where that
/// package happens to be installed.
///
/// ::arraw::loadImage now offers the file to LibRaw before reaching here, so
/// the plugin no longer gets the chance; this stays as the second line of
/// defence, for the day the plugin's LibRaw recognises a file ours does not.
QImage decodeThroughQt(QFile& file, QString& error, ImageOrientation& orientation) {
    QImageReader reader(&file);
    reader.setAutoTransform(false);
    if (reader.format() == "raw") {
        return {};
    }
    orientation = orientationOf(reader);
    QImage decoded = reader.read();
    error = reader.errorString();
    return decoded;
}

/// @brief Decides whether a file belongs to the RAW decoder.
///
/// Content has its say before Qt is asked, rather than after it fails. A RAW
/// container is usually a TIFF carrying an ordinary RGB preview, and Qt's TIFF
/// reader decodes that preview perfectly happily -- so a RAW under a name
/// arraw does not recognise would arrive as a thumbnail of the photograph
/// instead of the photograph, with nothing failing.
///
/// Asking LibRaw first is safe because LibRaw claims camera files, not TIFFs:
/// measured against LibRaw 0.22.2, every multi-channel TIFF offered to it is
/// declined, arraw's own exports included. The extension is kept as the fast
/// path in front of it, so the common case opens the file once.
///
/// Reading a file and describing it answer this the same way, so they cannot
/// disagree about which decoder a photograph belongs to.
/// @param path File to route.
/// @return `true` when the RAW decoder takes it.
bool decodedAsRaw(const std::filesystem::path& path) {
    return rawimport::namesRawFormat(path) || rawimport::holdsRawImage(path);
}

} // namespace

ImageMetadata arraw::readImageMetadata(const std::filesystem::path& path, DiagnosticLog& log) {
    if (decodedAsRaw(path)) {
        return rawimport::readMetadata(path, log);
    }

    QFile sourceFile(path);
    if (!sourceFile.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(sourceFile.fileName().toStdString() + ": " +
                                 sourceFile.errorString().toStdString());
    }

    // The header alone, which is all a QImageReader needs for the dimensions.
    QImageReader reader(&sourceFile);
    const QSize size = reader.size();
    if (reader.format() == "raw" || !size.isValid()) {
        throw std::runtime_error(sourceFile.fileName().toStdString() + ": " +
                                 reader.errorString().toStdString());
    }

    // Not the file's own space: loadImage converts everything Qt decodes into
    // the working encoding, so that is what a plan starts from.
    return {.size = {static_cast<std::uint32_t>(size.width()),
                     static_cast<std::uint32_t>(size.height())},
            .encoding = workingEncoding,
            .orientation = orientationOf(reader)};
}

ImageBuffer arraw::loadImage(const std::filesystem::path& path, DiagnosticLog& log) {
    if (decodedAsRaw(path)) {
        return rawimport::load(path, log);
    }

    QFile sourceFile(path);
    if (!sourceFile.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(sourceFile.fileName().toStdString() + ": " +
                                 sourceFile.errorString().toStdString());
    }

    QString readerError;
    ImageOrientation orientation = ImageOrientation::Normal;
    QImage decoded = decodeThroughQt(sourceFile, readerError, orientation);
    if (decoded.isNull()) {
        // Neither decoder recognised it. LibRaw has already declined above, so
        // there is nothing left to fall back to.
        throw std::runtime_error(sourceFile.fileName().toStdString() + ": " +
                                 readerError.toStdString());
    }

    if (!decoded.colorSpace().isValid()) {
        // An untagged file is sRGB by convention; nothing else is decidable.
        decoded.setColorSpace(QColorSpace::SRgb);
    }

    // Colour conversion, not merely a layout change: the embedded profile is
    // honoured here and the samples become linear Rec.2020.
    const QImage working = decoded.convertedToColorSpace(
        qtimage::toColorSpace(workingEncoding), qtimage::toImageFormat(chooseLayout(decoded)));
    if (working.isNull()) {
        throw std::runtime_error(sourceFile.fileName().toStdString() +
                                 ": cannot convert to the working space");
    }

    return qtimage::toBuffer(working, workingEncoding, orientation);
}
