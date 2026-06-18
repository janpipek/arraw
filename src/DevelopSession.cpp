#include "DevelopSession.h"

#include "Spot.h"

#include <utility>

namespace {
std::vector<Spot> scaleSpots(const std::vector<Spot>& spots, double sx, double sy) {
    std::vector<Spot> out = spots;
    for (Spot& s : out) {
        s.destination = {s.destination.x() * sx, s.destination.y() * sy};
        s.source = {s.source.x() * sx, s.source.y() * sy};
        s.radius *= sx;
    }
    return out;
}
} // namespace

DevelopSession::DevelopSession(QObject* parent)
    : QObject(parent) {
}

void DevelopSession::beginLoading(QString path) {
    currentPath = std::move(path);
    state = LoadState::Loading;
    sidecar = SidecarState::Unknown;
}

void DevelopSession::setLoadedImage(
    QString path,
    const LoadResult& result,
    const GlobalAdjustment& params,
    SidecarState sidecarState,
    const UserMetadata& metadata) {
    currentPath = std::move(path);
    previewBuffer = result.preview;
    fullResBuffer = result.fullRes;
    imageMetadata = result.metadata;
    metadata_ = metadata;
    savedMetadata = metadata;
    imageDefaultCrop = result.defaultCrop;
    adjustments = params;
    savedAdjustments = params;
    isDevelopDirty = false;
    isMetadataDirty = false;
    useBaseLook = false;
    rebuildSpotBuffers();
    sidecar = sidecarState;
    state = LoadState::Loaded;
}

const ImageBuffer& DevelopSession::previewForDisplay() const {
    return spottedPreviewBuffer.valid() ? spottedPreviewBuffer : previewBuffer;
}

const ImageBuffer& DevelopSession::fullResForExport() const {
    return spottedFullResBuffer.valid() ? spottedFullResBuffer : fullResBuffer;
}

void DevelopSession::setParams(const GlobalAdjustment& params) {
    adjustments = params;
    rebuildSpotBuffers();
    isDevelopDirty = adjustments != savedAdjustments;
}

void DevelopSession::setSpots(std::vector<Spot> spots) {
    adjustments.spots = std::move(spots);
    rebuildSpotBuffers();
    isDevelopDirty = adjustments != savedAdjustments;
}

void DevelopSession::setUserMetadata(const UserMetadata& metadata) {
    metadata_ = metadata;
    isMetadataDirty = metadata_ != savedMetadata;
}

void DevelopSession::setBaseLook(bool on) {
    useBaseLook = on;
}

void DevelopSession::markDevelopSaved() {
    savedAdjustments = adjustments;
    isDevelopDirty = false;
}

void DevelopSession::markMetadataSaved() {
    savedMetadata = metadata_;
    isMetadataDirty = false;
}

void DevelopSession::rebuildSpotBuffers() {
    if (adjustments.spots.empty()) {
        spottedPreviewBuffer = {};
        spottedFullResBuffer = {};
        return;
    }
    if (previewBuffer.valid()) {
        const double sx = (fullResBuffer.valid() && fullResBuffer.width > 0)
            ? double(previewBuffer.width) / fullResBuffer.width
            : 1.0;
        const double sy = (fullResBuffer.valid() && fullResBuffer.height > 0)
            ? double(previewBuffer.height) / fullResBuffer.height
            : 1.0;
        spottedPreviewBuffer = applySpots(previewBuffer, scaleSpots(adjustments.spots, sx, sy));
    } else {
        spottedPreviewBuffer = {};
    }
    spottedFullResBuffer = fullResBuffer.valid() ? applySpots(fullResBuffer, adjustments.spots)
                                                 : ImageBuffer{};
}
