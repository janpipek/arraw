#pragma once
#include "core/Orientation.h"
#include "develop/DemosaicAlgorithm.h"
#include "develop/LocalAdjustment.h"
#include "develop/Spot.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include <QPointF>
#include <QRectF>

// Tone curve control points in [0,1]×[0,1] space.
inline bool isIdentityCurve(const std::vector<QPointF>& pts) {
    return pts.size() == 2 && std::abs(pts[0].x()) < 1e-4 && std::abs(pts[0].y()) < 1e-4
           && std::abs(pts[1].x() - 1.0) < 1e-4 && std::abs(pts[1].y() - 1.0) < 1e-4;
}

struct CurvePoints {
    std::vector<QPointF> points = {{0.0, 0.0}, {1.0, 1.0}};

    bool isIdentity() const { return isIdentityCurve(points); }

    bool operator==(const CurvePoints&) const = default;
};

struct GlobalAdjustment : SharedAdjustment {
    // Tone (exposure, contrast, highlights, shadows, whites, blacks) and
    // tint/saturation/vibrance live in SharedAdjustment, shared with
    // LocalAdjustment (docs/adr/0010).

    // Filmic Highlights — a global-only Tone control (0..100, default 25; 0 = off):
    // the shoulder + path-to-white stage applied last in the develop chain
    // (docs/adr/0035). On by default with a gentle shoulder, like the baked
    // highlight roll-off in Lightroom/Capture One — most photos read better with
    // graceful highlights than a hard digital clip. Travels in the Tone Develop
    // Group; stored arraw-native (arraw:FilmicHighlights), no Lightroom equivalent.
    float filmicHighlights = 25.0f;

    // Tone curve (Luma + per-channel R/G/B), control points in [0,1]×[0,1]
    CurvePoints curveLuma;
    CurvePoints curveR;
    CurvePoints curveG;
    CurvePoints curveB;

    // Color — temperature is global-only, in absolute Kelvin (a local adjustment's
    // temperature is a relative -100..100 shift instead; see LocalAdjustment).
    float temperature = 5500.0f; // Kelvin, 2000 .. 12000

    // HSL: 8 ranges [Red, Orange, Yellow, Green, Aqua, Blue, Purple, Magenta], -100..+100
    std::array<float, 8> hslHue = {};
    std::array<float, 8> hslSat = {};
    std::array<float, 8> hslLum = {};

    // Detail
    // Demosaic algorithm — a decode-time choice, not a shader uniform: changing
    // it re-runs the libraw decode through the load path (docs/adr/0033, issue
    // #22). Persisted as a token in arraw:DemosaicAlgorithm; AHD is the default.
    DemosaicAlgorithm demosaicAlgorithm = kDefaultDemosaic;
    float sharpening = 0.0f; // 0 .. 100
    // Colour (chroma) Noise Reduction — a cached GPU chroma pre-pass in
    // RendererCore (see NoiseReduction.h, docs/adr/0032, issue #59). Strength is
    // the blend opacity (Lightroom's "Color" amount, crs:ColorNoiseReduction);
    // Smoothness drives the Gaussian sigma (crs:ColorNoiseReductionSmoothness).
    float colorNoiseReduction = 0.0f;            // Strength, 0 .. 100
    float colorNoiseReductionSmoothness = 50.0f; // 0 .. 100

    // Lens Corrections (docs/adr/0027). Profile-driven, apply-once CPU corrections.
    // Enable toggles only — the coefficients come from the lens profile
    // (lensfun / embedded), so nothing numeric is stored here.
    bool lensCorrectDistortion = false;
    bool lensCorrectVignetting = false;
    bool lensCorrectCA = false;

    // Geometry
    orient::Orientation orientation;        // coarse 90°/flip, seeded from EXIF (docs/adr/0028)
    float rotation = 0.0f;                  // degrees, -45 .. +45 (fine straighten)
    QRectF cropRect = {0.0, 0.0, 1.0, 1.0}; // normalised UV, full image by default
    bool cropConstrained = false;           // crop is locked to its aspect ratio

    // Effects (docs/adr/0026). The seed is per-image identity: copy/paste and
    // presets transfer the six visible controls but preserve the target seed.
    float postCropVignetteAmount = 0.0f;    // -100 .. +100, mapped to -2 .. +2 EV
    float postCropVignetteMidpoint = 50.0f; // 0 .. 100
    float postCropVignetteFeather = 50.0f;  // 0 .. 100
    float grainAmount = 0.0f;               // 0 .. 100
    float grainSize = 50.0f;                // 0 .. 100
    float grainRoughness = 50.0f;           // 0 .. 100
    std::uint32_t grainSeed = 0;            // 0 = uninitialised

    // Local adjustments — arraw-native, capped at 16 (docs/adr/0010).
    std::vector<LocalAdjustment> localAdjustments;

    // Spots — clone-based pixel replacements applied before the shader (docs/adr/0017).
    std::vector<Spot> spots;

    bool operator==(const GlobalAdjustment&) const = default;
};
