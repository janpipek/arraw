# Histograms are computed from GPU readback, not a CPU mirror

Both histograms (the panel histogram and the curve input histogram) are
computed by rendering the preview through the real fragment shader into a small
offscreen FBO and histogramming the readback — not by re-implementing the
adjustment math on the CPU. The previous CPU mirror in `Histogram.cpp` had
already drifted from the shader (it ignored curves, tint, HSL, and vibrance,
and hand-duplicated the tone-region ramp constants); every shader change had to
be ported twice or the histogram lied. The shader is the single source of truth
for all adjustments — this extends that principle from export to histograms.

## Consequences

- The shader gains a pipeline-stage uniform so the curve input histogram can be
  rendered as "stop after tone regions, then gamma-encode".
- Histogram updates cost a throttled small render + `glReadPixels` on the main
  thread per parameter change, instead of a CPU pass over the preview buffer.
- `Histogram.cpp` keeps only binning and painting; its adjustment math is deleted.
