#pragma once
#include "ImagePipeline.h"
#include <QString>

// Reads and writes AdjustmentParams as an XMP sidecar file.
// Sidecar path: same directory + base name as the RAW file, .xmp extension.
// Uses crs: (Camera Raw Settings) namespace for Lightroom-compatible fields.
// Note: Temperature is stored in absolute Kelvin (2000-12000, matching crs:);
// Tint and all other fields use the internal -100..100 scale.
class XmpSidecar {
public:
    static QString pathFor(const QString& rawPath);

    // Returns default AdjustmentParams if the sidecar doesn't exist or can't be parsed.
    static AdjustmentParams load(const QString& rawPath);

    // Returns true on success.
    static bool save(const QString& rawPath, const AdjustmentParams& params);
};
