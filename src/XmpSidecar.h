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
    AdjustmentParams adjustments;
    UserMetadata     metadata;
};

class XmpSidecar {
public:
    static QString pathFor(const QString& rawPath);

    // Reads the whole sidecar. Returns defaults if it doesn't exist or can't be parsed.
    static SidecarData load(const QString& rawPath);

    static AdjustmentParams loadAdjustments(const QString& rawPath) {
        return load(rawPath).adjustments;
    }
    static UserMetadata loadMetadata(const QString& rawPath) {
        return load(rawPath).metadata;
    }

    // Namespace-scoped, read-first saves: each replaces only its own half of the
    // sidecar and preserves the other half already on disk (docs/adr/0007).
    static bool saveAdjustments(const QString& rawPath, const AdjustmentParams& params);
    static bool saveMetadata(const QString& rawPath, const UserMetadata& metadata);
};
