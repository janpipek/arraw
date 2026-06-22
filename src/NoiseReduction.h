#pragma once

// Colour (chroma) Noise Reduction — parameter mapping. The spatial filter itself
// runs on the GPU as a cached pre-pass inside RendererCore (docs/adr/0032); this
// header owns only the tested Amount→sigma calibration that the renderer uploads
// as a uniform, keeping that logic single-sourced and headlessly testable.

// Amount slider (0..100) → Gaussian sigma in full-res pixels. Amount 0 maps to
// sigma 0 (the renderer treats that as "NR off"). Larger amounts smooth larger
// colour blobs.
float colorNoiseReductionSigmaPx(float amount);
