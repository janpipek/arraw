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

struct ResolvedUserMetadata {
    UserMetadata metadata;
    UserMetadataPresence presence;
};

ResolvedUserMetadata resolveUserMetadata(const SidecarData& sidecar, const LoadResult& result) {
    UserMetadata resolved;
    resolved.caption = metadataRowValue(result.metadata, QStringLiteral("Description"));
    resolved.creator = metadataRowValue(result.metadata, QStringLiteral("Artist"));

    auto overlay = [](QString& target, const QString& value, bool present) {
        if (present)
            target = value;
    };

    overlay(resolved.title, result.embeddedMetadata.title, result.embeddedMetadataPresence.title);
    overlay(resolved.caption, result.embeddedMetadata.caption, result.embeddedMetadataPresence.caption);
    if (result.embeddedMetadataPresence.keywords)
        resolved.keywords = result.embeddedMetadata.keywords;
    overlay(resolved.creator, result.embeddedMetadata.creator, result.embeddedMetadataPresence.creator);
    overlay(
        resolved.copyright,
        result.embeddedMetadata.copyright,
        result.embeddedMetadataPresence.copyright);

    resolved.rating = sidecar.metadata.rating;
    resolved.label = sidecar.metadata.label;
    overlay(resolved.title, sidecar.metadata.title, sidecar.metadataPresence.title);
    overlay(resolved.caption, sidecar.metadata.caption, sidecar.metadataPresence.caption);
    if (sidecar.metadataPresence.keywords)
        resolved.keywords = sidecar.metadata.keywords;
    overlay(resolved.creator, sidecar.metadata.creator, sidecar.metadataPresence.creator);
    overlay(resolved.copyright, sidecar.metadata.copyright, sidecar.metadataPresence.copyright);
    return {resolved, sidecar.metadataPresence};
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
    const ResolvedUserMetadata metadata = resolveUserMetadata(sidecar.data, result);
    return {
        sidecar.data.adjustments,
        metadata.metadata,
        metadata.presence,
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
