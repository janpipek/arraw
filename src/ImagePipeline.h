#pragma once
#include "ImageMetadata.h"
#include <cstdint>
#include <vector>
#include <QImage>
#include <QString>
#include <QRectF>

// Slider ±100 → shader uniform ±0.2 (gentler than dividing by 100 alone).
inline constexpr float kToneSliderToUniform = 500.0f;

struct AdjustmentParams {
    // Tone
    float exposure    = 0.0f;     // -5.0 .. +5.0 EV
    float contrast    = 0.0f;     // -100 .. +100
    float highlights  = 0.0f;     // -100 .. +100
    float shadows     = 0.0f;     // -100 .. +100
    float whites      = 0.0f;     // -100 .. +100
    float blacks      = 0.0f;     // -100 .. +100

    // Color
    float temperature = 5500.0f;  // Kelvin, 2000 .. 12000
    float tint        = 0.0f;     // -100 .. +100
    float saturation  = 0.0f;     // -100 .. +100
    float vibrance    = 0.0f;     // -100 .. +100

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
};

// Box-filter 2× downsample (half W, half H). Safe to call off the main thread.
ImageBuffer downsample2x(const ImageBuffer& src);

// Convert an sRGB QImage (any format) to a linear-light float32 ImageBuffer.
// Safe to call off the main thread.
ImageBuffer srgbToLinearBuffer(const QImage& img);
