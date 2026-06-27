#pragma once

#include "develop/DevelopSession.h"
#include "pipeline/ImagePipeline.h"

#include <QString>

/**
 * User-facing status text for the main editor shell.
 *
 * These helpers keep status-bar wording and sidecar error text out of
 * MainWindow's orchestration code. They do not inspect widgets or mutate state.
 */
QString sidecarWriteErrorText(const QString& path);
QString loadedImageStatusText(
    const QString& path, const ImageBuffer& fullRes, DevelopSession::SidecarState sidecarState);
QString batchExportStatusText(int exported, int total);
