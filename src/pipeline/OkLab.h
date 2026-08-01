#pragma once

#include <array>

// Perceptual colour for arraw's adjustments (docs/adr/0039) and the highlight
// roll-off (docs/adr/0040). Everything here is the CPU reference that the shader
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

// Black & White (docs/adr/0048): convert to an achromatic (R==G==B) grey through
// an 8-band hue mixer. `mix` holds the per-hue-band weights (-100..100) for Red,
// Orange, Yellow, Green, Aqua, Blue, Purple, Magenta — the same bands as HSL. The
// base grey is the Rec.2020 luminance; each band darkens (<0) or lightens (>0) the
// greys made from pixels of its hue, scaled by the pixel's saturation, so neutrals
// never shift and an all-zero mix is the plain luminance. The shader mirrors this
// immediately after white balance.
Rgb applyBlackAndWhite(const Rgb& rgb, const std::array<float, 8>& mix);

// Colour Grading (docs/adr/0052): a three-zone hue+saturation tint in Oklab. `hue`
// and `sat` are indexed [Shadows, Midtones, Highlights]; hue is in degrees (0..360)
// and sat in 0..100. A pixel's perceptually-encoded luminance selects a normalised
// blend of the three zones (Balance, -100..100, shifts the shadow<->highlight
// crossover; Blending, 0..100, widens the zone transitions), and each zone's
// (hue, sat) contributes an Oklab a/b offset — so lightness and the tone/toning
// separation are preserved, and a neutral grey can be tinted (sepia, split-toning).
// An all-zero-saturation grade is the exact identity. The shader mirrors this after
// the Colour/Black & White branch merges.
Rgb applyColourGrading(
    const Rgb& rgb,
    const std::array<float, 3>& hue,
    const std::array<float, 3>& sat,
    float balance,
    float blending);

// Filmic Highlights (docs/adr/0040).
//
// shoulderMap is the scalar luminance shoulder: amount 0 is the exact identity
// (including values above 1); amount in (0, 1] bends the approach to white,
// anchoring 0, staying monotonic, leaving midtones nearly untouched, and
// compressing all headroom smoothly below 1.
float shoulderMap(float luminance, float amount);

// applyFilmicHighlights runs the shoulder on luminance (scaling RGB by the
// luminance ratio, so hue is preserved by the compression) and then the
// Path to White: it fades chroma toward white in proportion to how hard the
// shoulder pulled the tone down. amount 0 is the exact identity.
Rgb applyFilmicHighlights(const Rgb& rgb, float amount);

} // namespace colour
