#pragma once

// Colour (chroma) Noise Reduction — parameter mapping. The spatial filter itself
// runs on the GPU as a cached pre-pass inside RendererCore (docs/adr/0034); this
// header owns only the tested Amount→sigma calibration that the renderer uploads
// as a uniform, keeping that logic single-sourced and headlessly testable.

// Smoothness slider (0..100) → Gaussian sigma in full-res pixels. Smoothness 0
// maps to sigma 0 (the renderer treats that as "no blur"). Larger values smooth
// larger colour blobs. (Was the single Amount slider before issue #59.)
float colorNoiseReductionSigmaPx(float smoothness);

// Strength slider (0..100) → recombine blend factor in [0,1]: the opacity of the
// denoised chroma over the original. The renderer uploads this as a uniform and
// the recombine shader mixes raw vs blurred chroma by it. Clamped to [0,1].
float colorNoiseReductionStrengthMix(float strength);

// --- Luminance Noise Reduction (the dual of the above; docs/adr/0046) ----------
// Luma NR smooths Y with an edge-aware bilateral and preserves the chroma ratio.
// Like chroma, the renderer takes these tested scalars as uniforms rather than
// duplicating the calibration in GLSL.

// Amount slider (0..100) → recombine blend factor in [0,1]: the opacity of the
// denoised luma over the original (Amount 0 = luma NR off). The dual of
// colorNoiseReductionStrengthMix. Clamped to [0,1].
float luminanceNoiseReductionAmountMix(float amount);

// Detail slider (0..100) → bilateral range sigma, in the perceptual
// (tone::kGamma-encoded) luma domain the edge-stop measures differences in.
// Higher Detail preserves more detail, i.e. a *tighter* edge-stop (smaller sigma);
// the mapping is monotonically decreasing and never reaches zero (a zero range
// sigma would smooth nothing). Clamped to the [0,100] Detail range.
float luminanceNoiseReductionRangeSigma(float detail);

// The bilateral's spatial reach, a small fixed constant in full-res pixels: Amount
// drives the blend and Detail the range term, so the user sees exactly two knobs
// (docs/adr/0046). The renderer scales this per slot (half on the Preview buffer).
inline constexpr float kLuminanceNoiseReductionSpatialSigmaPx = 2.0f;
