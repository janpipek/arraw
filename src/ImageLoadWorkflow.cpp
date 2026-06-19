#include "ImageLoadWorkflow.h"

#include "RawProcessor.h"
#include "StandardImageLoader.h"

#include <QFileInfo>

QString decodeCacheKey(const QString& path) {
    const QFileInfo file(path);
    return file.canonicalFilePath() + '|' + QString::number(file.size()) + '|'
           + QString::number(file.lastModified().toMSecsSinceEpoch());
}

DevelopSession::SidecarState toSessionSidecarState(SidecarLoadStatus status) {
    switch (status) {
    case SidecarLoadStatus::Missing:
        return DevelopSession::SidecarState::Missing;
    case SidecarLoadStatus::Loaded:
        return DevelopSession::SidecarState::Loaded;
    case SidecarLoadStatus::ParseError:
        return DevelopSession::SidecarState::ParseError;
    }
    return DevelopSession::SidecarState::Unknown;
}

GlobalAdjustment resolvePendingPreviewParams(const QString& path) {
    return resolveImageAdjustments(path, QRectF(0.0, 0.0, 1.0, 1.0));
}

GlobalAdjustment resolveImageAdjustments(const QString& path, const QRectF& defaultCrop) {
    return XmpSidecar::resolveForImage(path, defaultCrop).data.adjustments;
}

ResolvedLoadedImage resolveLoadedImage(const QString& path, const LoadResult& result) {
    const SidecarLoadResult sidecar = XmpSidecar::resolveForImage(path, result.defaultCrop);
    return {
        sidecar.data.adjustments,
        sidecar.data.metadata,
        toSessionSidecarState(sidecar.status),
    };
}

LoadResult decodeImage(
    const QString& path,
    EmbeddedPreviewCallback onPreview,
    const std::shared_ptr<std::atomic<bool>>& cancel) {
    if (StandardImageLoader::canLoad(path))
        return StandardImageLoader::load(path, cancel);
    return RawProcessor::load(path, std::move(onPreview), cancel);
}
