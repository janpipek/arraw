#pragma once
#include <vector>

// 3D display LUT sampled as the shader's final stage. Indexed by sRGB-encoded
// working-space RGB (shaper — concentrates resolution in the shadows);
// RGB = display-encoded output, A = 1 in-gamut / 0 out-of-gamut.
//
// A plain value type: produced by ColorManagement (lcms, pipeline layer) and
// consumed by RendererCore (render layer), so it lives in core/ to keep both on
// the same foundation without a render→pipeline edge.
struct DisplayLut {
    std::vector<float> data; // size³ × RGBA, red axis fastest
    int size = 0;

    bool valid() const { return size > 0 && !data.empty(); }
};
