#pragma once
#include "ImageMetadata.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include <QImage>
#include <QPointF>
#include <QString>
#include <QRectF>

// Slider ±100 → shader uniform ±0.2 (gentler than dividing by 100 alone).
inline constexpr float kToneSliderToUniform = 500.0f;

// Rec. 2020 luma coefficients (the working space, see docs/adr/0001) —
// must match kLuma in shaders/image.frag.
inline constexpr float kLumaR = 0.2627f;
inline constexpr float kLumaG = 0.6780f;
inline constexpr float kLumaB = 0.0593f;

// Tone curve control points in [0,1]×[0,1] space.
inline bool isIdentityCurve(const std::vector<QPointF>& pts) {
    return pts.size() == 2
        && std::abs(pts[0].x()) < 1e-4 && std::abs(pts[0].y()) < 1e-4
        && std::abs(pts[1].x() - 1.0) < 1e-4 && std::abs(pts[1].y() - 1.0) < 1e-4;
}

struct CurvePoints {
    std::vector<QPointF> points = {{0.0, 0.0}, {1.0, 1.0}};
    bool isIdentity() const { return isIdentityCurve(points); }
    bool operator==(const CurvePoints&) const = default;
};

struct AdjustmentParams {
    // Tone
    float exposure    = 0.0f;     // -5.0 .. +5.0 EV
    float contrast    = 0.0f;     // -100 .. +100
    float highlights  = 0.0f;     // -100 .. +100
    float shadows     = 0.0f;     // -100 .. +100
    float whites      = 0.0f;     // -100 .. +100
    float blacks      = 0.0f;     // -100 .. +100

    // Tone curve (Luma + per-channel R/G/B), control points in [0,1]×[0,1]
    CurvePoints curveLuma;
    CurvePoints curveR;
    CurvePoints curveG;
    CurvePoints curveB;

    // Color
    float temperature = 5500.0f;  // Kelvin, 2000 .. 12000
    float tint        = 0.0f;     // -100 .. +100
    float saturation  = 0.0f;     // -100 .. +100
    float vibrance    = 0.0f;     // -100 .. +100

    // HSL: 8 ranges [Red, Orange, Yellow, Green, Aqua, Blue, Purple, Magenta], -100..+100
    std::array<float, 8> hslHue = {};
    std::array<float, 8> hslSat = {};
    std::array<float, 8> hslLum = {};

    // Detail
    float sharpening  = 0.0f;     // 0 .. 100

    // Geometry
    float  rotation = 0.0f;                    // degrees, -45 .. +45
    QRectF cropRect = {0.0, 0.0, 1.0, 1.0};   // normalised UV, full image by default

    bool operator==(const AdjustmentParams&) const = default;
};

// Linear float32 RGB image buffer, interleaved, [0..1] nominal.
struct ImageBuffer {
    std::vector<float> data;
    int width  = 0;
    int height = 0;

    bool valid() const { return !data.empty() && width > 0 && height > 0; }
};

// Returned by the background load task.
struct LoadResult {
    ImageBuffer    fullRes;   // stored for export only
    ImageBuffer    preview;   // 1/4-res (half W, half H) — used for viewport + histogram
    ImageMetadata  metadata;
    QString        error;     // non-empty on failure
    QRectF         defaultCrop = {0.0, 0.0, 1.0, 1.0};
};

// Box-filter 2× downsample (half W, half H). Safe to call off the main thread.
ImageBuffer downsample2x(const ImageBuffer& src);

// Compute a 256-entry output LUT from tone-curve control points (Fritsch-Carlson monotone spline).
std::array<float, 256> computeCurveLUT(const std::vector<QPointF>& pts);
