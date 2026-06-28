#pragma once

#include "ExportMetadata.h"
#include "ExportOptions.h"
#include "develop/UserMetadata.h"

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

// The off-GUI-thread tail of an export (docs/adr/0045). Given the rendered
// linear working-space image, run the output transform + sharpen, encode and
// write the file, then best-effort embed metadata (only when the write
// succeeded). Pure CPU/IO over value inputs — no RHI, no DevelopSession — so it
// runs on a worker thread and is unit-testable.
struct ExportTailResult {
    bool saved = false;
    ExportMetadataStatus metadata = ExportMetadataStatus::SkippedNoBackend;
};

ExportTailResult runExportTail(
    QImage linearWorkingImage,
    const ExportOptions& options,
    const QString& outputPath,
    const QString& sourcePath,
    const UserMetadata& metadata);
