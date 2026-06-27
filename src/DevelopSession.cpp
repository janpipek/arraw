#include "DevelopSession.h"

#include "LensCorrection.h"
#include "Spot.h"

#include <utility>

namespace {
// The lens-corrected and spotted buffers are a pure function of the source
// buffers, the lens profile, the three lens-correction toggles, and the spots
// list — never of tone/colour/curve/WB/crop, which the GPU shader applies live.
// Re-warping them is expensive (a full-resolution CPU resample), so it must run
// only when one of these inputs actually changes, not on every slider tick.
bool derivedBufferInputsDiffer(const GlobalAdjustment& a, const GlobalAdjustment& b) {
    return a.lensCorrectDistortion != b.lensCorrectDistortion
           || a.lensCorrectVignetting != b.lensCorrectVignetting
           || a.lensCorrectCA != b.lensCorrectCA || a.spots != b.spots;
}

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
    : QObject(parent) {}

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
    ensureFullResDerived();
    if (spottedFullResBuffer.valid())
        return spottedFullResBuffer;
    if (correctedFullResBuffer.valid())
        return correctedFullResBuffer;
    return fullResBuffer;
}

void DevelopSession::setParams(const GlobalAdjustment& params) {
    if (params == adjustments)
        return; // no-op: avoids re-warping buffers when the undo command replays the same state
    const bool rebuild = derivedBufferInputsDiffer(adjustments, params);
    adjustments = params;
    if (rebuild)
        rebuildDerivedBuffers(); // skip the costly warp when only shader-side params changed
    isDevelopDirty = adjustments != savedAdjustments;
}

void DevelopSession::setLocalAdjustments(std::vector<LocalAdjustment> localAdjustments) {
    adjustments.localAdjustments = std::move(localAdjustments);
    isDevelopDirty = adjustments != savedAdjustments;
}

void DevelopSession::setSpots(std::vector<Spot> spots) {
    adjustments.spots = std::move(spots);
    rebuildDerivedBuffers();
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

namespace {
LensCorrectionToggles togglesOf(const GlobalAdjustment& a) {
    return {.distortion = a.lensCorrectDistortion,
            .vignetting = a.lensCorrectVignetting,
            .ca = a.lensCorrectCA};
}
bool correctionActive(const LensCorrectionModel& m, const LensCorrectionToggles& t) {
    return (t.distortion && m.hasDistortion) || (t.vignetting && m.hasVignetting)
           || (t.ca && m.hasTCA);
}
} // namespace

void DevelopSession::rebuildDerivedBuffers() {
    rebuildPreviewDerived();
    fullResDerivedDirty = true; // full-res is recomputed lazily on next access
}

void DevelopSession::rebuildPreviewDerived() {
    const LensCorrectionToggles toggles = togglesOf(adjustments);
    correctedPreviewBuffer = (correctionActive(lensModel, toggles) && previewBuffer.valid())
        ? applyLensCorrection(previewBuffer, lensModel, toggles)
        : ImageBuffer{};

    // Spots clone on the lens-corrected base when present, else on the clean buffer.
    const ImageBuffer& base =
        correctedPreviewBuffer.valid() ? correctedPreviewBuffer : previewBuffer;
    if (!adjustments.spots.empty() && base.valid() && fullResBuffer.width > 0) {
        const double sx = double(base.width) / fullResBuffer.width;
        const double sy = double(base.height) / fullResBuffer.height;
        spottedPreviewBuffer = applySpots(base, scaleSpots(adjustments.spots, sx, sy));
    } else {
        spottedPreviewBuffer = {};
    }
}

void DevelopSession::ensureFullResDerived() const {
    if (!fullResDerivedDirty)
        return;
    fullResDerivedDirty = false;

    const LensCorrectionToggles toggles = togglesOf(adjustments);
    correctedFullResBuffer = (correctionActive(lensModel, toggles) && fullResBuffer.valid())
        ? applyLensCorrection(fullResBuffer, lensModel, toggles)
        : ImageBuffer{};
    const ImageBuffer& base =
        correctedFullResBuffer.valid() ? correctedFullResBuffer : fullResBuffer;
    spottedFullResBuffer =
        (!adjustments.spots.empty() && base.valid()) ? applySpots(base, adjustments.spots)
                                                     : ImageBuffer{};
}
