#pragma once
#include "develop/UserMetadata.h"

#include "develop/DemosaicAlgorithm.h"
#include "DevelopSession.h"
#include "develop/GlobalAdjustment.h"
#include "pipeline/LoadResult.h"
#include "io/XmpSidecar.h"

#include <atomic>
#include <functional>
#include <memory>
#include <QString>

/**
 * Image-load decisions that do not need MainWindow ownership.
 *
 * MainWindow still owns QFutureWatcher, queued GUI callbacks, and widget sync.
 * This module owns path-based cache keys, raw-vs-standard decoding, and the
 * conversion from sidecar load results into DevelopSession-ready state.
 */
struct ResolvedLoadedImage {
    GlobalAdjustment adjustments;
    UserMetadata metadata;
    UserMetadataPresence metadataPresence;
    DevelopSession::SidecarState sidecarState = DevelopSession::SidecarState::Unknown;
    std::vector<Snapshot> snapshots; // named A/B develop states (docs/adr/0033)
};

using EmbeddedPreviewCallback = std::function<void(ImageBuffer)>;

// The demosaic algorithm is part of the key: each algorithm's decode caches
// independently, so only a never-tried algorithm pays the decode cost while
// switching among tried ones (or undo/redo) is instant (ADR 0033).
QString decodeCacheKey(const QString& path, DemosaicAlgorithm algo = kDefaultDemosaic);
DevelopSession::SidecarState toSessionSidecarState(SidecarLoadStatus status);
GlobalAdjustment resolvePendingPreviewParams(const QString& path);
GlobalAdjustment resolveImageAdjustments(const QString& path, const QRectF& defaultCrop);
ResolvedLoadedImage resolveLoadedImage(const QString& path, const LoadResult& result);
bool shouldConfirmLeavingImage(const DevelopSession& session);
LoadResult decodeImage(
    const QString& path,
    EmbeddedPreviewCallback onPreview,
    const std::shared_ptr<std::atomic<bool>>& cancel,
    DemosaicAlgorithm algo = kDefaultDemosaic);
