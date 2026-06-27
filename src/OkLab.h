#pragma once

#include <array>

// Perceptual colour for arraw's adjustments (docs/adr/0034) and the highlight
// roll-off (docs/adr/0035). Everything here is the CPU reference that the shader
// (shaders/image.frag) mirrors line-for-line — the [[spot-for-algorithms]]
// contract the golden-image tests enforce. All RGB triples are linear-light
// Rec.2020, the working space (docs/adr/0001).
namespace colour {

using Rgb = std::array<float, 3>;

// Oklab: a perceptual space with a near-orthogonal lightness axis and even hue
// spacing (Björn Ottosson, 2020). Scaling only a/b changes colourfulness while
// holding L (lightness) and hue.
struct Lab {
    float L;
    float a;
    float b;
};

// Linear Rec.2020 <-> Oklab. The Rec.2020->XYZ step is folded into Oklab's first
// matrix (see OkLab.cpp), so this is a single matrix + cube root + matrix.
Lab toOklab(const Rgb& rgb);
Rgb fromOklab(const Lab& lab);

// Saturation scales Oklab chroma uniformly; amount in [-1, 1] (-1 = greyscale,
// 0 = identity, +1 = double chroma). Lightness and hue are preserved.
Rgb applySaturation(const Rgb& rgb, float amount);

// Vibrance scales Oklab chroma weighted by (1 - currentChroma), so muted colours
// move more than already-vivid ones; amount in [-1, 1].
Rgb applyVibrance(const Rgb& rgb, float amount);

// Highlight roll-off (docs/adr/0035).
//
// shoulderMap is the scalar luminance shoulder: amount 0 is the exact identity
// (including values above 1); amount in (0, 1] bends the approach to white,
// anchoring 0, staying monotonic, leaving midtones nearly untouched, and
// compressing all headroom smoothly below 1.
float shoulderMap(float luminance, float amount);

// applyHighlightRolloff runs the shoulder on luminance (scaling RGB by the
// luminance ratio, so hue is preserved by the compression) and then the
// Path to White: it fades chroma toward white in proportion to how hard the
// shoulder pulled the tone down. amount 0 is the exact identity.
Rgb applyHighlightRolloff(const Rgb& rgb, float amount);

} // namespace colour
