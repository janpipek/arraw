# Tone curve operates on gamma-encoded values

The shader pipeline runs in linear light end to end, but the tone curve is the
exception: `applyCurve` encodes to sRGB gamma, applies the LUT, and decodes back
to linear, at its existing position in the pipeline. We store curve points as
`crs:ToneCurvePV2012`, which Lightroom interprets in gamma space — applying them
in linear light made the same sidecar render visibly differently (worst in the
shadows) and made the curve widget's x-axis unintuitive (linear 0.25 is
upper-midtone gray, not "the dark quarter"). The two extra `pow()` calls per
pixel are negligible on the GPU.

## Consequences

- Sidecars with curves saved before this change render differently afterwards.
  Deliberately no version flag or migration: the linear interpretation was a bug
  (it disagreed with the `crs:` format we write), and reinterpreting old points
  in gamma space is closer to what was intended.
- Both histograms (the curve widget background and the panel histogram) must be
  computed in the same gamma space the curve's input axis lives in.
