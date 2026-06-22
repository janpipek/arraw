#pragma once

#include "ImagePipeline.h"
#include "UserMetadata.h"

#include <QObject>
#include <QString>

/**
 * Canonical state for the image currently open in the develop view.
 *
 * DevelopSession owns the active file path, decoded buffers, user metadata,
 * read-only EXIF metadata, develop parameters, sidecar status, and dirty
 * baselines. GUI widgets should mirror or edit this state through MainWindow;
 * they should not be treated as the source of truth for "the current image".
 */
class DevelopSession : public QObject {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(DevelopSession)

public:
    enum class LoadState { Empty, Loading, Loaded, Failed };
    Q_ENUM(LoadState)

    enum class SidecarState { Unknown, Missing, Loaded, ParseError, WriteError };
    Q_ENUM(SidecarState)

    explicit DevelopSession(QObject* parent = nullptr);

    LoadState loadState() const { return state; }

    SidecarState sidecarState() const { return sidecar; }

    const QString& path() const { return currentPath; }

    bool hasImage() const { return state == LoadState::Loaded && previewBuffer.valid(); }

    const ImageBuffer& preview() const { return previewBuffer; }

    const ImageBuffer& fullRes() const { return fullResBuffer; }

    const ImageBuffer& previewForDisplay() const;
    const ImageBuffer& fullResForExport() const;

    const ImageMetadata& metadata() const { return imageMetadata; }

    const UserMetadata& userMetadata() const { return metadata_; }

    const QRectF& defaultCrop() const { return imageDefaultCrop; }
    // Resolved lens-profile name (empty when none matched), for the UI label.
    const QString& lensProfileName() const { return lensModel.lensName; }
    const GlobalAdjustment& params() const { return adjustments; }

    bool baseLook() const { return useBaseLook; }

    bool developDirty() const { return isDevelopDirty; }

    bool metadataDirty() const { return isMetadataDirty; }

    void beginLoading(QString path);
    void setLoadedImage(
        QString path,
        const LoadResult& result,
        const GlobalAdjustment& params,
        SidecarState sidecarState,
        const UserMetadata& metadata = {});
    void setParams(const GlobalAdjustment& params);
    void setLocalAdjustments(std::vector<LocalAdjustment> localAdjustments);
    void setSpots(std::vector<Spot> spots);
    void setUserMetadata(const UserMetadata& metadata);
    void setBaseLook(bool on);
    void markDevelopSaved();
    void markDevelopSaveFailed();
    void markMetadataSaved();
    void markMetadataSaveFailed();

private:
    LoadState state = LoadState::Empty;
    SidecarState sidecar = SidecarState::Unknown;
    QString currentPath;
    ImageBuffer previewBuffer;
    ImageBuffer fullResBuffer;
    // Lens-corrected derivatives of the clean buffers (docs/adr/0027); empty when no
    // profile or all toggles off, in which case the clean buffer is the base. The
    // preview derivatives are eager (cheap, drives the live view); the full-res ones
    // are computed lazily on first access (export / deep zoom) because warping a
    // 25 MP buffer on every toggle would freeze the UI.
    LensCorrectionModel lensModel;
    ImageBuffer correctedPreviewBuffer;
    ImageBuffer spottedPreviewBuffer;
    mutable ImageBuffer correctedFullResBuffer;
    mutable ImageBuffer spottedFullResBuffer;
    mutable bool fullResDerivedDirty = true;
    ImageMetadata imageMetadata;
    UserMetadata metadata_;
    UserMetadata savedMetadata;
    QRectF imageDefaultCrop{0.0, 0.0, 1.0, 1.0};
    GlobalAdjustment adjustments;
    GlobalAdjustment savedAdjustments;
    bool isDevelopDirty = false;
    bool isMetadataDirty = false;
    bool useBaseLook = false;

    // Rebuild the eager preview derivatives (lens-corrected then spotted) and mark the
    // lazy full-res derivatives dirty. Spots build on the lens-corrected base.
    void rebuildDerivedBuffers();
    void rebuildPreviewDerived();
    void ensureFullResDerived() const;
};
