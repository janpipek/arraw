#include "core/ImageMetadata.h"
#include "develop/DemosaicAlgorithm.h"
#include "develop/UserMetadata.h"
#include "develop/GlobalAdjustment.h"
#include "pipeline/LoadResult.h"
#include "ImageLoadWorkflow.h"

#include "pipeline/RawProcessor.h"
#include "pipeline/StandardImageLoader.h"

#include <QFileInfo>

namespace {

QString metadataRowValue(const ImageMetadata& metadata, const QString& label) {
    for (const auto& [rowLabel, value] : metadata.rows) {
        if (rowLabel == label)
            return value;
    }
    return {};
}

} // namespace

ResolvedUserMetadata resolveUserMetadata(
    const SidecarData& sidecar, const ImageMetadata& exif, const XmpPacketMetadata& embedded) {
    UserMetadata resolved;
    resolved.caption = metadataRowValue(exif, QStringLiteral("Description"));
    resolved.creator = metadataRowValue(exif, QStringLiteral("Artist"));

    auto overlay = [](QString& target, const QString& value, bool present) {
        if (present)
            target = value;
    };

    overlay(resolved.title, embedded.metadata.title, embedded.presence.title);
    overlay(resolved.caption, embedded.metadata.caption, embedded.presence.caption);
    if (embedded.presence.keywords)
        resolved.keywords = embedded.metadata.keywords;
    overlay(resolved.creator, embedded.metadata.creator, embedded.presence.creator);
    overlay(resolved.copyright, embedded.metadata.copyright, embedded.presence.copyright);

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

QString decodeCacheKey(const QString& path, DemosaicAlgorithm algo) {
    const QFileInfo file(path);
    return file.canonicalFilePath() + '|' + QString::number(file.size()) + '|'
           + QString::number(file.lastModified().toMSecsSinceEpoch()) + '|' + demosaicToken(algo);
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
    // Decode emitted raw XMP bytes; parsing them is the shell's job (docs/adr/0041).
    const XmpPacketMetadata embedded = XmpSidecar::metadataPacketFromPacket(result.embeddedXmpPacket);
    const ResolvedUserMetadata metadata = resolveUserMetadata(sidecar.data, result.metadata, embedded);
    return {
        sidecar.data.adjustments,
        metadata.metadata,
        metadata.presence,
        toSessionSidecarState(sidecar.status),
        sidecar.data.snapshots,
    };
}

bool shouldConfirmLeavingImage(const DevelopSession& session) {
    return session.hasImage() && (session.developDirty() || session.metadataDirty());
}

LoadResult decodeImage(
    const QString& path,
    EmbeddedPreviewCallback onPreview,
    const std::shared_ptr<std::atomic<bool>>& cancel,
    DemosaicAlgorithm algo) {
    if (StandardImageLoader::canLoad(path))
        return StandardImageLoader::load(path, cancel);
    return RawProcessor::load(path, std::move(onPreview), cancel, algo);
}
