#include "ImageLoadWorkflow.h"

#include "RawProcessor.h"
#include "StandardImageLoader.h"

#include <QFileInfo>

namespace {

QString metadataRowValue(const ImageMetadata& metadata, const QString& label) {
    for (const auto& [rowLabel, value] : metadata.rows) {
        if (rowLabel == label)
            return value;
    }
    return {};
}

UserMetadata resolveUserMetadata(const UserMetadata& sidecar, const LoadResult& result) {
    UserMetadata resolved;
    resolved.caption = metadataRowValue(result.metadata, QStringLiteral("Description"));
    resolved.creator = metadataRowValue(result.metadata, QStringLiteral("Artist"));

    auto overlay = [](QString& target, const QString& value) {
        if (!value.isEmpty())
            target = value;
    };

    overlay(resolved.title, result.embeddedMetadata.title);
    overlay(resolved.caption, result.embeddedMetadata.caption);
    if (!result.embeddedMetadata.keywords.isEmpty())
        resolved.keywords = result.embeddedMetadata.keywords;
    overlay(resolved.creator, result.embeddedMetadata.creator);
    overlay(resolved.copyright, result.embeddedMetadata.copyright);

    resolved.rating = sidecar.rating;
    resolved.label = sidecar.label;
    overlay(resolved.title, sidecar.title);
    overlay(resolved.caption, sidecar.caption);
    if (!sidecar.keywords.isEmpty())
        resolved.keywords = sidecar.keywords;
    overlay(resolved.creator, sidecar.creator);
    overlay(resolved.copyright, sidecar.copyright);
    return resolved;
}

} // namespace

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
    const SidecarLoadResult sidecar
        = XmpSidecar::resolveForImage(path, result.defaultCrop, result.seededOrientation);
    return {
        sidecar.data.adjustments,
        resolveUserMetadata(sidecar.data.metadata, result),
        toSessionSidecarState(sidecar.status),
    };
}

bool shouldConfirmLeavingImage(const DevelopSession& session) {
    return session.hasImage() && (session.developDirty() || session.metadataDirty());
}

LoadResult decodeImage(
    const QString& path,
    EmbeddedPreviewCallback onPreview,
    const std::shared_ptr<std::atomic<bool>>& cancel) {
    if (StandardImageLoader::canLoad(path))
        return StandardImageLoader::load(path, cancel);
    return RawProcessor::load(path, std::move(onPreview), cancel);
}
