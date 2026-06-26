#pragma once

// Colour (chroma) Noise Reduction — parameter mapping. The spatial filter itself
// runs on the GPU as a cached pre-pass inside RendererCore (docs/adr/0032); this
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
