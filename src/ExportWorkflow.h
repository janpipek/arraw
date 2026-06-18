#pragma once

#include "ExportOptions.h"

#include <QImage>
#include <QString>

/**
 * Export-specific decisions that do not need MainWindow ownership.
 *
 * This module keeps file-format mapping, output filename construction,
 * output-profile conversion, sharpening, and image saving close together. The
 * GUI still owns dialogs, progress, and renderer calls; these functions own the
 * pure export rules around those interactions.
 */
struct ExportFormatSpec {
    QString suffix;
    QString nameFilter;
    const char* saveFormat = nullptr;
};

ExportFormatSpec exportFormatSpec(ExportOptions::Format format);
QString withExportSuffix(QString path, ExportOptions::Format format);
QString batchExportPath(
    const QString& outputDir, const QString& sourcePath, ExportOptions::Format format);

QImage applyUnsharpMask(QImage image, int amount);
QImage prepareExportImage(QImage linearWorkingImage, const ExportOptions& options);
bool saveExportImage(const QImage& image, const QString& path, const ExportOptions& options);
