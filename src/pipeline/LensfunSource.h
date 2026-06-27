#pragma once

#include "pipeline/LensCorrection.h"

#include <QString>

#include <optional>

// A shot's identity for lens-profile matching, taken from EXIF (libraw).
struct LensQuery {
    QString cameraMaker;      // EXIF Make, e.g. "SONY"
    QString cameraModel;      // EXIF Model, e.g. "ILCE-6700"
    QString lensModel;        // EXIF LensModel, e.g. "56mm F1.4 DC DN | Contemporary 018"
    float focal = 0.0f;       // mm
    float aperture = 0.0f;    // f-number (<= 0 → a neutral default is used)
    float distance = 1000.0f; // focus distance in metres (1000 ≈ infinity)
    int width = 0;
    int height = 0;
};

// True when arraw was built with lensfun support (ARRAW_HAS_LENSFUN).
bool lensfunAvailable();

// Resolve a per-shot LensCorrectionModel from a lensfun database. An empty dbPath
// uses lensfun's automatic system/user database discovery (production); a non-empty
// dbPath loads exactly that directory (deterministic tests). lensfun resolves its
// parametric model for this shot and we sample
// the resolution into the model's RadialCurve LUTs, so the apply step stays
// source-agnostic (docs/adr/0027). Returns nullopt when lensfun is unavailable, the
// database fails to load, no lens matches, or the matched lens has no corrections.
std::optional<LensCorrectionModel> resolveLensfunModel(const QString& dbPath, const LensQuery& query);
