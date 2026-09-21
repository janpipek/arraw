#include "ImageImport.h"

#include "QtImage.h"

#include <QColorSpace>
#include <QFile>
#include <QImage>
#include <QImageReader>

#include <stdexcept>

using namespace arraw;
using namespace std;

namespace {

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

} // namespace

ImageBuffer arraw::loadImage(const std::filesystem::path& path) {
    QFile sourceFile(path);
    if (!sourceFile.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(sourceFile.fileName().toStdString() + ": " +
                                 sourceFile.errorString().toStdString());
    }

    // Reading from the device rather than a file name lets the reader detect
    // the format from the content instead of trusting the extension.
    QImageReader reader(&sourceFile);
    QImage decoded = reader.read();
    if (decoded.isNull()) {
        throw std::runtime_error(sourceFile.fileName().toStdString() + ": " +
                                 reader.errorString().toStdString());
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

    return qtimage::toBuffer(working, workingEncoding);
}