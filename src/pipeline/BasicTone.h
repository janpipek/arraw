#pragma once

#include "pipeline/ImagePipeline.h"
#include <array>
#include <cmath>

namespace tone {

inline constexpr int kLutSize = 256;
inline constexpr int kLutRows = 17; // global + the 16 Local Adjustment cap

// Perceptual encoding exponent. The LUT is indexed by the gamma-encoded
// coordinate x = luminance^(1/kGamma) so its 256 samples are spread evenly
// across perception — most of them where the tone controls actually act —
// instead of bunching in the bright linear values. The shader must encode the
// same way before sampling (see shaders/image.frag, applyBasicTone).
inline constexpr float kGamma = 2.2f;

// Linear luminance that maps to LUT column `index`. Inverse of the shader's
// encode step; shared so tests reason about the texture the GPU samples.
inline float lutIndexToLuminance(int index) {
    const float encoded = float(index) / float(kLutSize - 1);
    return std::pow(encoded, kGamma);
}

struct LutAtlas {
    // R = mapped luminance, G = slope above 1, B/A reserved.
    std::array<float, kLutSize * kLutRows * 4> rgba{};
};

// Maps linear-light luminance through the six Basic Tone controls. The function
// is the CPU reference used to build the GPU lookup texture.
float mapLuminance(float luminance, const SharedAdjustment& adjustment);

// Builds the texture sampled by the shader: row 0 is global and rows 1..16
// correspond to Local Adjustments in array order.
LutAtlas makeLutAtlas(const GlobalAdjustment& adjustment);

} // namespace tone
