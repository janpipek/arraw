#pragma once
#include "ImageMetadata.h"
#include "LensCorrection.h"
#include "LocalAdjustment.h"
#include "Orientation.h"
#include "Spot.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>

// Slider ±100 → shader uniform ±0.2 (gentler than dividing by 100 alone).
inline constexpr float kToneSliderToUniform = 500.0f;

// Rec. 2020 luma coefficients (the working space, see docs/adr/0001) —
// must match kLuma in shaders/image.frag.
inline constexpr float kLumaR = 0.2627f;
inline constexpr float kLumaG = 0.6780f;
inline constexpr float kLumaB = 0.0593f;

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
    float sharpening = 0.0f; // 0 .. 100
    // Colour (chroma) Noise Reduction — Amount 0..100, applied as a cached GPU
    // chroma pre-pass in RendererCore (see NoiseReduction.h, docs/adr/0032).
    float colorNoiseReduction = 0.0f; // 0 .. 100

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

// Linear float32 RGB image buffer, interleaved, [0..1] nominal.
struct ImageBuffer {
    std::vector<float> data;
    int width = 0;
    int height = 0;

    bool valid() const { return !data.empty() && width > 0 && height > 0; }
};

// Returned by the background load task.
struct LoadResult {
    ImageBuffer fullRes; // stored for export only
    ImageBuffer preview; // 1/4-res (half W, half H) — used for viewport + histogram
    ImageMetadata metadata;
    QString error; // non-empty on failure
    QRectF defaultCrop = {0.0, 0.0, 1.0, 1.0};
    // Lens profile resolved at decode (docs/adr/0027). Empty has* flags = no
    // profile matched; correction is applied (toggle-gated) downstream.
    LensCorrectionModel lensModel{};
    // Coarse Orientation read from the file's EXIF/camera flag, used to seed the
    // develop edit when the sidecar has none (docs/adr/0028). The decoded buffer
    // stays in native orientation — this only records what the camera intended.
    orient::Orientation seededOrientation = {};
};

// Box-filter 2× downsample (half W, half H). Safe to call off the main thread.
ImageBuffer downsample2x(const ImageBuffer& src);

// Scale a linear buffer so its near-max luma (99.5th percentile) lands at a fixed
// target, giving every photo a comparable starting exposure. Gain is clamped to
// [0.5, 4.0] so a bad estimate can't blow out or crush the image. Applied to both
// the demosaiced full-res and the embedded preview, so the two match on load.
void normalizeExposure(ImageBuffer& buf);

// Compute a 256-entry output LUT from tone-curve control points (Fritsch-Carlson monotone spline).
std::array<float, 256> computeCurveLUT(const std::vector<QPointF>& pts);

// White balance as a per-channel multiplicative gain in linear Rec.2020
// (docs/adr/0025): the gain is blackbody-derived so the Kelvin numbers mean
// something, normalised so neutral (5500 K, tint 0) returns {1,1,1}. Applied as
// c *= gain, so a black pixel stays black by construction. `kelvin` is absolute
// (2000..12000), `tint` is in slider units (-100..100, + = green, - = magenta).
std::array<float, 3> whiteBalanceGain(float kelvin, float tint);

// Inverse of whiteBalanceGain for the WB picker: given a pre-WB pixel that the
// user declares neutral, recover the kelvin/tint that would neutralise it.
void whiteBalanceFromNeutral(float r, float g, float b, float& kelvin, float& tint);

// Pixel size of a developed thumbnail: the source cropped to crop, scaled down so
// its longer edge is at most maxEdge (never upscaled), aspect preserved.
// Output pixel size of a developed thumbnail. `srcW`/`srcH` are the *native*
// buffer dims; an odd quarter-turn Orientation swaps them so the thumbnail keeps
// the oriented aspect (docs/adr/0028) — otherwise turned shots come out squished.
QSize developedThumbSize(
    int srcW, int srcH, const QRectF& crop, int maxEdge, orient::Orientation orientation = {});
