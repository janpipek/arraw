#pragma once

#include "ImagePipeline.h"
#include <array>

namespace tone {

inline constexpr int kLutSize = 256;
inline constexpr int kLutRows = 17; // global + the 16 Local Adjustment cap

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
