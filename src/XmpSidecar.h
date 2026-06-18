#pragma once
#include "ImagePipeline.h"
#include "UserMetadata.h"
#include <QString>

// Reads and writes the XMP sidecar file: develop settings (crs: namespace) and
// user-authored culling marks (xmp:Rating / xmp:Label). See docs/adr/0007.
// Sidecar path: same directory + base name as the RAW file, .xmp extension.
// Note: Temperature is stored in absolute Kelvin (2000-12000, matching crs:);
// Tint and all other develop fields use the internal -100..100 scale.

// Whole-file in-memory view of one sidecar.
struct SidecarData {
    GlobalAdjustment adjustments;
    UserMetadata metadata;
};

enum class SidecarLoadStatus {
    Missing,
    Loaded,
    ParseError,
};

struct SidecarLoadResult {
    SidecarData data;
    SidecarLoadStatus status = SidecarLoadStatus::Missing;
};

struct SidecarAdjustmentResult {
    GlobalAdjustment adjustments;
    SidecarLoadStatus status = SidecarLoadStatus::Missing;
};

class XmpSidecar {
public:
    static QString pathFor(const QString& rawPath);

    // Reads the whole sidecar. Returns defaults if it doesn't exist or can't be parsed.
    static SidecarData load(const QString& rawPath);
    static SidecarLoadResult loadWithStatus(const QString& rawPath);

    static GlobalAdjustment loadAdjustments(const QString& rawPath) {
        return load(rawPath).adjustments;
    }

    static UserMetadata loadMetadata(const QString& rawPath) { return load(rawPath).metadata; }

    // The adjustments to apply when opening rawPath: the sidecar's if one exists,
    // otherwise defaults with the crop set to defaultCrop (e.g. a DNG DefaultCrop).
    static GlobalAdjustment resolveAdjustments(const QString& rawPath, const QRectF& defaultCrop);
    static SidecarAdjustmentResult resolveAdjustmentsWithStatus(
        const QString& rawPath, const QRectF& defaultCrop);

    // Namespace-scoped, read-first saves: each replaces only its own half of the
    // sidecar and preserves the other half already on disk (docs/adr/0007).
    static bool saveAdjustments(const QString& rawPath, const GlobalAdjustment& params);
    static bool saveMetadata(const QString& rawPath, const UserMetadata& metadata);
};
