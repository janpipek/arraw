#include "develop/LocalAdjustment.h"
#include "develop/UserMetadata.h"
#include "develop/GlobalAdjustment.h"
#include "pipeline/LoadResult.h"
#include "develop/DevelopSession.h"

#include "pipeline/LensCorrection.h"
#include "develop/Spot.h"

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

void mergePresence(UserMetadataPresence& target, const UserMetadataPresence& changedFields) {
    target.title = target.title || changedFields.title;
    target.caption = target.caption || changedFields.caption;
    target.keywords = target.keywords || changedFields.keywords;
    target.creator = target.creator || changedFields.creator;
    target.copyright = target.copyright || changedFields.copyright;
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
    const UserMetadata& metadata,
    const UserMetadataPresence& presence,
    std::vector<Snapshot> snapshots) {
    currentPath = std::move(path);
    previewBuffer = result.preview;
    fullResBuffer = result.fullRes;
    sensorClipPreviewBuffer = result.sensorClipPreview;
    sensorClipFullResBuffer = result.sensorClipFullRes;
    imageMetadata = result.metadata;
    metadata_ = metadata;
    savedMetadata = metadata;
    metadataPresence = presence;
    savedMetadataPresence = presence;
    imageDefaultCrop = result.defaultCrop;
    lensModel = result.lensModel;
    adjustments = params;
    savedAdjustments = params;
    snapshots_ = snapshots;
    savedSnapshots = std::move(snapshots);
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

void DevelopSession::swapDecodedBuffers(const LoadResult& result) {
    previewBuffer = result.preview;
    fullResBuffer = result.fullRes;
    // The sensor-clip mask comes from pre-demosaic mosaic values, so it is the
    // same across algorithms — refresh it anyway to keep the buffers consistent.
    sensorClipPreviewBuffer = result.sensorClipPreview;
    sensorClipFullResBuffer = result.sensorClipFullRes;
    rebuildDerivedBuffers(); // re-derive lens/spot buffers over the new pixels
}

const ImageBuffer& DevelopSession::sensorClipPreviewForDisplay() const {
    if (correctedSensorClipPreviewBuffer.valid())
        return correctedSensorClipPreviewBuffer;
    return sensorClipPreviewBuffer;
}

const ImageBuffer& DevelopSession::sensorClipFullResForDisplay() const {
    ensureFullResDerived();
    if (correctedSensorClipFullResBuffer.valid())
        return correctedSensorClipFullResBuffer;
    return sensorClipFullResBuffer;
}

void DevelopSession::setParams(const GlobalAdjustment& params) {
    if (params == adjustments)
        return; // no-op: avoids re-warping buffers when the undo command replays the same state
    const bool rebuild = derivedBufferInputsDiffer(adjustments, params);
    adjustments = params;
    if (rebuild)
        rebuildDerivedBuffers(); // skip the costly warp when only shader-side params changed
    recomputeDevelopDirty();
}

void DevelopSession::setLocalAdjustments(std::vector<LocalAdjustment> localAdjustments) {
    adjustments.localAdjustments = std::move(localAdjustments);
    recomputeDevelopDirty();
}

void DevelopSession::setSpots(std::vector<Spot> spots) {
    adjustments.spots = std::move(spots);
    rebuildDerivedBuffers();
    recomputeDevelopDirty();
}

void DevelopSession::addSnapshot(QString name, GlobalAdjustment state) {
    snapshots_.push_back(Snapshot{std::move(name), std::move(state)});
    recomputeDevelopDirty();
}

void DevelopSession::renameSnapshot(int index, QString name) {
    if (index < 0 || index >= static_cast<int>(snapshots_.size()))
        return;
    snapshots_[static_cast<size_t>(index)].name = std::move(name);
    recomputeDevelopDirty();
}

void DevelopSession::removeSnapshot(int index) {
    if (index < 0 || index >= static_cast<int>(snapshots_.size()))
        return;
    snapshots_.erase(snapshots_.begin() + index);
    recomputeDevelopDirty();
}

void DevelopSession::recomputeDevelopDirty() {
    isDevelopDirty = adjustments != savedAdjustments || snapshots_ != savedSnapshots;
}

void DevelopSession::setUserMetadata(
    const UserMetadata& metadata, const UserMetadataPresence& changedFields) {
    metadata_ = metadata;
    mergePresence(metadataPresence, changedFields);
    isMetadataDirty = metadata_ != savedMetadata || metadataPresence != savedMetadataPresence;
}

void DevelopSession::setBaseLook(bool on) {
    useBaseLook = on;
}

void DevelopSession::markDevelopSaved() {
    savedAdjustments = adjustments;
    savedSnapshots = snapshots_;
    isDevelopDirty = false;
    sidecar = SidecarState::Loaded;
}

void DevelopSession::markDevelopSaveFailed() {
    recomputeDevelopDirty();
    sidecar = SidecarState::WriteError;
}

void DevelopSession::markMetadataSaved() {
    savedMetadata = metadata_;
    savedMetadataPresence = metadataPresence;
    isMetadataDirty = false;
    sidecar = SidecarState::Loaded;
}

void DevelopSession::markMetadataSaveFailed() {
    isMetadataDirty = metadata_ != savedMetadata || metadataPresence != savedMetadataPresence;
    sidecar = SidecarState::WriteError;
}

namespace {
LensCorrectionToggles togglesOf(const GlobalAdjustment& a) {
    return {
        .distortion = a.lensCorrectDistortion,
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
    correctedSensorClipPreviewBuffer
        = (correctionActive(lensModel, toggles) && sensorClipPreviewBuffer.valid())
              ? applyLensCorrection(sensorClipPreviewBuffer, lensModel, toggles)
              : ImageBuffer{};

    // Spots clone on the lens-corrected base when present, else on the clean buffer.
    const ImageBuffer& base = correctedPreviewBuffer.valid() ? correctedPreviewBuffer
                                                             : previewBuffer;
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
    correctedSensorClipFullResBuffer
        = (correctionActive(lensModel, toggles) && sensorClipFullResBuffer.valid())
              ? applyLensCorrection(sensorClipFullResBuffer, lensModel, toggles)
              : ImageBuffer{};
    const ImageBuffer& base = correctedFullResBuffer.valid() ? correctedFullResBuffer
                                                             : fullResBuffer;
    spottedFullResBuffer = (!adjustments.spots.empty() && base.valid())
                               ? applySpots(base, adjustments.spots)
                               : ImageBuffer{};
}
