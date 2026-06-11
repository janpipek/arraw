# Golden-image tests run the real export path on any GPU, with a dual tolerance

Shader regression tests call the actual `ImageViewport::renderToImage()` (not a
standalone shader harness, which would duplicate `setAdjustmentUniforms` and
drift — the same failure mode that motivated ADR 0004). The float linear
readback is compared against committed PFM goldens with two thresholds:
per-pixel channel diff ≤ ~4/255 to absorb GPU/driver variance, and per-channel
mean diff over the whole image ≤ ~0.3/255 so a systematic shift (e.g. a 1.5%
exposure drift, invisible to the loose per-pixel band) still fails. Driver
noise is random and centered on zero; real tone/color regressions are
systematic — the mean exploits that.

## Considered Options

Pinning goldens to Mesa llvmpipe (`LIBGL_ALWAYS_SOFTWARE=1`) would allow a
single tight ~1/255 threshold but confine the tests to machines with Mesa;
we chose run-anywhere so the suite executes on every dev machine (Linux,
macOS, Windows) — there is no CI to fall back on.

## Consequences

- Goldens are stored as PFM (portable float map): matches the float32 linear
  readback of `renderToImage`, trivial to read/write, no new dependency.
- Golden inputs are synthetic (gradient ramps, saturated patches, hue wheel)
  plus one render of the fixture DNG; synthetic inputs make clipping and tone
  drift visible where photos hide them.
- Tests need a realized `QOpenGLWidget`: a `QApplication` and a created window
  (offscreen platform or a real display).
- A regression smaller than the per-pixel band *and* the mean band passes
  silently; tighten the mean threshold first if that ever bites.
