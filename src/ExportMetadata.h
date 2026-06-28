#pragma once

#include "ExportOptions.h"
#include "develop/UserMetadata.h"

#include <QString>

enum class ExportMetadataStatus {
    Embedded,
    SkippedNoBackend,
    Failed,
};

struct ExportMetadataResult {
    ExportMetadataStatus status = ExportMetadataStatus::SkippedNoBackend;
    QString error;

    bool ok() const { return status != ExportMetadataStatus::Failed; }
};

ExportMetadataResult embedExportMetadata(
    const QString& outputPath,
    const QString& sourcePath,
    const UserMetadata& metadata,
    const ExportMetadataSelection& selection);
