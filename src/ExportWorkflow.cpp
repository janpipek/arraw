#include "ExportWorkflow.h"

#include "pipeline/ColorManagement.h"

#include <algorithm>
#include <QFileInfo>

ExportFormatSpec exportFormatSpec(ExportOptions::Format format) {
    switch (format) {
    case ExportOptions::Format::JPEG:
        return {"jpg", "JPEG (*.jpg *.jpeg)", "JPEG"};
    case ExportOptions::Format::PNG:
        return {"png", "PNG (*.png)", nullptr};
    case ExportOptions::Format::TIFF:
        return {"tif", "TIFF (*.tif *.tiff)", nullptr};
    }
    return {"jpg", "JPEG (*.jpg *.jpeg)", "JPEG"};
}

QString withExportSuffix(QString path, ExportOptions::Format format) {
    if (QFileInfo(path).suffix().isEmpty())
        path += "." + exportFormatSpec(format).suffix;
    return path;
}

QString batchExportPath(
    const QString& outputDir, const QString& sourcePath, ExportOptions::Format format) {
    return outputDir + "/" + QFileInfo(sourcePath).completeBaseName() + "."
           + exportFormatSpec(format).suffix;
}

// Simple unsharp mask: radius is roughly 1% of image width, amount 0..100.
// The blur is approximated by a smooth downscale + upscale round-trip,
// which is much cheaper than a real Gaussian at these radii.
QImage applyUnsharpMask(QImage image, int amount) {
    if (amount == 0 || image.isNull())
        return image;

    const float strength = amount / 100.0f;
    const int radius = std::max(1, image.width() / 100);
    const int blurWidth = std::max(1, image.width() / (radius + 1));
    const int blurHeight = std::max(1, image.height() / (radius + 1));
    const QImage blurred
        = image.scaled(blurWidth, blurHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
              .scaled(image.width(), image.height(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (image.format() == QImage::Format_RGBA64) {
        for (int y = 0; y < image.height(); ++y) {
            const auto* src = reinterpret_cast<const quint16*>(image.constScanLine(y));
            const auto* blur = reinterpret_cast<const quint16*>(blurred.constScanLine(y));
            auto* dst = reinterpret_cast<quint16*>(image.scanLine(y));
            for (int x = 0; x < image.width() * 4; ++x) {
                if ((x & 3) == 3)
                    continue; // leave alpha alone
                const int value = int(src[x]) + int(strength * (int(src[x]) - int(blur[x])));
                dst[x] = quint16(std::clamp(value, 0, 65535));
            }
        }
        return image;
    }

    for (int y = 0; y < image.height(); ++y) {
        const uchar* src = image.constScanLine(y);
        const uchar* blur = blurred.constScanLine(y);
        uchar* dst = image.scanLine(y);
        for (int x = 0; x < image.width() * 3; ++x) {
            const int value = int(src[x]) + int(strength * (int(src[x]) - int(blur[x])));
            dst[x] = uchar(std::clamp(value, 0, 255));
        }
    }
    return image;
}

QImage prepareExportImage(QImage linearWorkingImage, const ExportOptions& options) {
    QImage output = toOutputImage(linearWorkingImage, options.profile, options.bitDepth == 16);
    return applyUnsharpMask(std::move(output), options.sharpening);
}

bool saveExportImage(const QImage& image, const QString& path, const ExportOptions& options) {
    const ExportFormatSpec spec = exportFormatSpec(options.format);
    if (spec.saveFormat)
        return image.save(path, spec.saveFormat, options.quality);
    return image.save(path);
}
