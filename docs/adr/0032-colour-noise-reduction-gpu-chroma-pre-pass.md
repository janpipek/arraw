# Colour Noise Reduction is a cached GPU chroma pre-pass

High-ISO captures carry two kinds of noise: **luminance** (grainy brightness
speckle) and **chroma** (coloured blotches). This milestone ships **Colour Noise
Reduction only** — the chroma half. It is the cheaper, lower-frequency artifact,
and crucially it stays faithful at the quarter-res preview, so it does not need
the full-res-to-judge handling that luminance NR does. Luminance NR is deferred
to its own milestone (it wants full-res to judge and destroys fine detail — a
different problem).

Colour NR runs **on the GPU, as a cached multi-pass pre-pass inside
`RendererCore`**, immediately before the main `image.frag` pass. It samples the
already-uploaded image texture (the lens-corrected, spotted negative) and writes
a denoised texture that the main pass samples instead of the raw slot.

## Why GPU, not CPU

The first design was a CPU separable Gaussian on the decoded `ImageBuffer`,
recomputed once per Amount change (the same exception as Spot removal `0017` and
the lens-corrected negative `0027`). It was **abandoned after benchmarking**: a
large-radius full-res Gaussian took ~minutes per recompute — sigma reaches 25
full-res pixels, so the kernel is ~150 taps in each direction, and the cost is
paid synchronously on the buffer chain. "Slow as hell" in practice; unusable as
an interactive control.

The GPU pre-pass is ~16× cheaper still by working at **quarter resolution** with
a bilinear upsample, and runs as cached texture work rather than blocking the
buffer chain. The trade is accepting the chroma filter as the **last** pipeline
stage rather than the first (see Placement), and a small shader surface — but one
that deliberately never touches `image.frag`/`image.vert`/`Ubuf`, so the
"adding a new adjustment" uniform-block checklist (`0006`) does not apply.

## The filter

Working space is linear Rec.2020. For each pixel, luminance is
`Y = kLumaR·r + kLumaG·g + kLumaB·b` (the shared `kLuma*` weights, which sum to
1). The pixel is decomposed into `Y` and a **unit-luma chroma ratio** `r = c / Y`
(near-black pixels get a neutral ratio). A two-pass **separable Gaussian** blurs
the ratio (horizontal then vertical), and the pixel is recombined as
`c' = Y · blurredRatio`.

Per-pixel luma is preserved **exactly, by construction**: the blurred ratio is a
convex combination of unit-luma vectors, so it still has luma 1, and multiplying
by the original `Y` restores it. Only colour is smoothed; detail (which lives in
luma) is mathematically untouched.

This maps to **four shader passes** over three textures (one `NrUbuf` shared by
all of them, since each blur shader hardcodes its direction so the uniform is
constant for the frame):

1. `nr_extract.frag` → quarter-res `chromaA`: emit the chroma ratio `r`.
2. `nr_blur_h.frag` → quarter-res `chromaB`: horizontal Gaussian over `r`.
3. `nr_blur_v.frag` → quarter-res `chromaA`: vertical Gaussian.
4. `nr_recombine.frag` → full-res `denoised`: read the raw pixel's `Y` and the
   bilinearly-upsampled blurred ratio, output `Y · r`.

`Amount` (0..100) maps linearly to a Gaussian **sigma in full-res pixels**
(0..25, `colorNoiseReductionSigmaPx`). Pixels are the correct unit because noise
grain is a fixed sensor-pixel size, independent of image dimensions — a
fraction-of-long-edge radius would denoise a high-MP capture more strongly than a
low-MP one of the same grain, and cropping would change the strength. The blur
runs at quarter res, so the kernel uses `sigma / 4`; the tap radius is clamped to
`[1, 64]` (the shader's compile-time cap). `Amount 0` is an exact no-op — the
pre-pass is skipped and the main pass samples the raw slot directly.

`colorNoiseReductionSigmaPx` is the **single source of truth** for the mapping —
one tested C++ function the shader takes a scalar from, not a formula duplicated
in GLSL.

## Placement: last, and why that is safe

Colour NR is the **last** pipeline stage before display, not the first. The GPU
pre-pass samples the texture that was already uploaded — i.e. after lens
correction (`0027`) and spot removal (`0017`) baked into the decoded buffer. That
is the opposite end of the pipeline from the abandoned CPU design, and it is
order-independent for chroma:

- Lens **vignette** correction is an achromatic per-pixel scalar gain — it scales
  `Y` and leaves the chroma ratio `r` untouched, so denoising before or after it
  gives the same `r`.
- **Spot** removal is geometry-only cloning; it introduces no chroma noise for a
  later stage to need to catch.
- White balance (`0025`) is a per-channel gain applied in the main pass *after*
  the denoised texture is sampled, so a smooth chroma field stays smooth through
  it.

So running last costs nothing in correctness for the chroma half, and buys the
contained, `image.frag`-free shader stage.

## Caching and debounce

The denoised texture is **cached per slot, keyed by `(amount, generation)`** —
`generation` bumps whenever the source texture is recreated. Pan, zoom, exposure,
and every other per-pixel edit reuse the cached texture; only an Amount change or
a new upload triggers the four passes. There are three slots: `[0]` Preview,
`[1]` FullRes, `[2]` export's temporary full-res texture (forced to recompute
each export call since its source changes every time).

A stable 1×1 throwaway render target defines one `nrRpDesc` shared by every NR
target and pipeline, so pipelines never dangle when a slot's textures are resized.

Because the pre-pass is the one expensive GPU stage, the **viewport debounces**
it: `ImageViewport` renders for `nrAmountEffective` (kept in the cache) and only
promotes the live slider value after the slider has been still ~200 ms
(`nrTimer`). Dragging the slider repaints cheaply against the cached texture; the
recompute fires once, when the user settles. Export and full-res-readback paths
bypass the debounce and always use the committed Amount.

## Consequences

- **No `Ubuf` change.** NR has its own `NrUbuf` (`static_assert`ed) and its own
  shaders; the main pass's uniform block is untouched, so the three-place uniform
  gotcha that blacks out the viewport does not apply. `recordPass` already accepts
  an arbitrary source `QRhiTexture*`, so feeding it the denoised texture is
  transparent.
- **Orientation.** The NR targets are textures rendered then immediately
  re-sampled, so on a Y-up framebuffer (OpenGL, the default backend on Linux)
  texel row 0 lands at the bottom — the opposite of the top-row-first uploaded
  image texture. Left alone, `denoisedTex` would be V-flipped (and luma/chroma
  would split, since the luma path skips the pre-pass). `NrUbuf::flipV`
  (`= isYUpInFramebuffer()`) mirrors the sampled V in `nr.vert`, making every pass
  an identity sample so the chain stays aligned with the uploaded orientation on
  all backends.
- The result is **GPU-only** — golden-image tests for it skip on machines without
  a GPU, so the math that *is* unit-tested is the sigma mapping and the XMP
  round-trip; the spatial result is verified visually.
- Persisted as Lightroom-compatible `crs:ColorNoiseReduction` (Amount 0..100),
  **default 0** — arraw stays neutral on import rather than mirroring Adobe's
  default 25 (the `0031` precedent: same field name, our interpretation). Carried
  in Develop Presets under the Detail group; one undo command per change.
- Plain separable Gaussian can bleed colour across hard saturated edges; accepted
  for these broad, low-frequency blobs. Luma-guided edge protection and a Detail
  sub-control are the natural follow-up, alongside the deferred luminance NR.
