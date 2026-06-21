#include "DevelopSession.h"

#include "LensCorrection.h"
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
    lensModel = result.lensModel;
    adjustments = params;
    savedAdjustments = params;
    isDevelopDirty = false;
    isMetadataDirty = false;
    useBaseLook = false;
    rebuildDerivedBuffers();
    sidecar = sidecarState;
    state = LoadState::Loaded;
}

const ImageBuffer& DevelopSession::previewForDisplay() const {
    if (spottedPreviewBuffer.valid())
        return spottedPreviewBuffer;
    if (correctedPreviewBuffer.valid())
        return correctedPreviewBuffer;
    return previewBuffer;
}

const ImageBuffer& DevelopSession::fullResForExport() const {
    if (spottedFullResBuffer.valid())
        return spottedFullResBuffer;
    if (correctedFullResBuffer.valid())
        return correctedFullResBuffer;
    return fullResBuffer;
}

void DevelopSession::setParams(const GlobalAdjustment& params) {
    adjustments = params;
    rebuildDerivedBuffers();
    isDevelopDirty = adjustments != savedAdjustments;
}

void DevelopSession::setLocalAdjustments(std::vector<LocalAdjustment> localAdjustments) {
    adjustments.localAdjustments = std::move(localAdjustments);
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
    sidecar = SidecarState::Loaded;
}

void DevelopSession::markDevelopSaveFailed() {
    isDevelopDirty = adjustments != savedAdjustments;
    sidecar = SidecarState::WriteError;
}

void DevelopSession::markMetadataSaved() {
    savedMetadata = metadata_;
    isMetadataDirty = false;
    sidecar = SidecarState::Loaded;
}

void DevelopSession::markMetadataSaveFailed() {
    isMetadataDirty = metadata_ != savedMetadata;
    sidecar = SidecarState::WriteError;
}

void DevelopSession::rebuildDerivedBuffers() {
    rebuildCorrectionBuffers();
    rebuildSpotBuffers();
}

void DevelopSession::rebuildCorrectionBuffers() {
    const LensCorrectionToggles toggles{
        .distortion = adjustments.lensCorrectDistortion,
        .vignetting = adjustments.lensCorrectVignetting,
        .ca = adjustments.lensCorrectCA};
    const bool active = (toggles.distortion && lensModel.hasDistortion)
                        || (toggles.vignetting && lensModel.hasVignetting)
                        || (toggles.ca && lensModel.hasTCA);
    if (!active) {
        correctedPreviewBuffer = {};
        correctedFullResBuffer = {};
        return;
    }
    correctedPreviewBuffer =
        previewBuffer.valid() ? applyLensCorrection(previewBuffer, lensModel, toggles) : ImageBuffer{};
    correctedFullResBuffer =
        fullResBuffer.valid() ? applyLensCorrection(fullResBuffer, lensModel, toggles) : ImageBuffer{};
}

void DevelopSession::rebuildSpotBuffers() {
    // Spots clone on the lens-corrected base when present, else on the clean buffer.
    const ImageBuffer& basePreview =
        correctedPreviewBuffer.valid() ? correctedPreviewBuffer : previewBuffer;
    const ImageBuffer& baseFull =
        correctedFullResBuffer.valid() ? correctedFullResBuffer : fullResBuffer;

    if (adjustments.spots.empty()) {
        spottedPreviewBuffer = {};
        spottedFullResBuffer = {};
        return;
    }
    if (basePreview.valid()) {
        const double sx = (baseFull.valid() && baseFull.width > 0)
            ? double(basePreview.width) / baseFull.width
            : 1.0;
        const double sy = (baseFull.valid() && baseFull.height > 0)
            ? double(basePreview.height) / baseFull.height
            : 1.0;
        spottedPreviewBuffer = applySpots(basePreview, scaleSpots(adjustments.spots, sx, sy));
    } else {
        spottedPreviewBuffer = {};
    }
    spottedFullResBuffer = baseFull.valid() ? applySpots(baseFull, adjustments.spots) : ImageBuffer{};
}
