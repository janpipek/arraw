# The curve input is sampled through the render and binned on the host

[ADR 011](011-a-plan-spatial-passes-and-one-pointwise-chain.md) decided that a
caller reads an intermediate value by asking for it: `sample(tap)` hands back
pixels to measure, `stopAfter(boundary)` a checkpoint to carry on from. It named
the first tap, the curve input, and said the perceptual encoding it hands back
needs a name. The tone curve widget (phase 4 of the global adjustments plan)
draws over a histogram of that input. This ADR builds the tap and the histogram.

## Decision

**A tap is a public enumeration beside the render functions.** `Tap` lives in
`include/Develop.h`, with one value, `CurveInput`, and the free function
`sample(source, state, tap, request)` sits beside `develop`, `developUntil` and
`resumeFrom`. It returns an `ImageBuffer`, never a checkpoint: looking and
continuing stay two verbs. Nothing can be resumed into a sample, because the only
boundary before a tap is the source itself. The tap's position is not in the
public header but in `src/core/ProcessingPlan.h`, the one place ADR 011 wants
it: `developToCurveInput` is the first half of the chain (matrix, exposure,
`shapeTone`), and `developPixel` calls it rather than repeating it. The name and
the position therefore cannot drift. `developToTap` switches on the tap.

**On the GPU a tap is a probe.** The pointwise shader already branches on the
`probe` uniform, for the per-stage parity tests. `probeFor(Tap::CurveInput)` is
the existing `PointwiseProbe::AfterTone` stop, so the shader and the uniform
block did not change. `sampleOnGpu` (in `src/gpu/GpuDevelop.h`, from a host or an
uploaded source) runs the same pass sequence as `developOnGpu` with that probe,
reads the result back and encodes it. `runPasses` now takes a probe and returns
pixels; `developOnGpu` wraps those in a checkpoint and `sampleOnGpu` does not.
When the probe stops before the curves, the curve table is not uploaded.

**The tap runs through geometry and the resize, in linear light, then is
encoded.** The chain writes the colour at the tap in the working encoding.
Geometry, the region and the resize then run exactly as for a render of the same
request, and only then is each colour channel encoded. Encoding is one host
function, `encodeTap`, used by both backends. So a sample covers the same frame
at the same size as `develop` with the same arguments: the histogram describes
the cropped picture the photographer sees, a preview's reduced source
([ADR 023](023-a-preview-develops-a-reduced-copy-of-the-source.md)) plus a
small request gives preview-resolution data, and the passes are the ones whose
parity is already tested.

- (a) **Tap, then geometry and the resize in linear, then encode (chosen).**
  Resampling stays in linear light, as for every render
  ([ADR 020](020-a-render-is-resized-after-geometry-in-one-separable-pass.md)).
  With nothing after the tap that changes a colour, the sample is bit for bit
  the encoded render, which the tests check.
- (b) Encode in the shader and resample perceptual values. This saves the
  host's per-channel power on the GPU path. But it resamples in a coordinate no
  render uses, and it needs a new probe value and shader branch.
- (c) No geometry: count the whole source. This is cheapest, but a crop would
  not change the histogram, and the preview and an export would count
  different pixels.

A resampled value is a linear mean of tapped colours, not a colour the curve
literally sees. At preview scale the curve already sees reduced pixels, so the
histogram is a statistic of the frame, not of sensor pixels.

**The histogram resizes bilinearly, at a bounded size by default.**
`curveHistogram(source, state, request)` replaces the request's filter with
`ResizeFilter::Bilinear`. Lanczos rings at hard edges, and its overshoot past
black or white would land in the end bins, which a photographer reads as
clipping. The tent kernel never rings. It still averages, so the distribution is
somewhat narrower than the pixels'. `sample` itself honours the request as
given; a caller who wants another filter samples and counts in two steps. The
default request is `curveHistogramRequest`: the frame fitted inside
`curveHistogramLongEdge` (1024) pixels, never enlarged. 256 bins do not need a
full-resolution render, and the default should not cost seconds on a 24 MP
source. Python's `curve_histogram` has the same defaults: `size=1024`, with
`None` for the full cropped resolution, and no `filter` keyword.

- (a) **Force Bilinear (chosen).** Cheap, uses a kernel both backends already
  have, and removes the overshoot. Some averaging remains.
- (b) A point-sampling filter for statistics. It keeps the marginal
  distribution, which is what a histogram estimates, but needs a new
  `ResizeFilter` on both backends. Worth adding if phase 4 shows visibly
  smoothed histograms.
- (c) Honour the request's filter and document it. This leaves every caller
  one argument away from false clipping.

**The perceptual encoding is named.** `NamedEncoding::Rec2020Gamma22`, with
the constant `perceptualEncoding`. The value is named for its maths, as the
other `NamedEncoding` values are; the constant names its role. Rec.2020 primaries, each channel stored as
`sign(v) * |v|^(1/2.2)` (`toPerceptualSigned`, `fromPerceptualSigned`). That is
ADR 010's coordinate, made odd so a channel outside the gamut survives the round
trip, as ADR 033 keeps it alive. It is internal:
- development refuses it as a source, as it refuses every encoding other than
  the working one or a camera's;
- export refuses it as a target, like the working encoding;
- as a source for export it maps to Qt's Bt2020 primaries with a 2.2 gamma, so a
  sample can be saved to look at. Below zero Qt does not know the odd extension.

**The histogram is a public value computed on the host.** `CurveHistogram`
(`include/CurveHistogram.h`) holds four arrays of `curveHistogramBins` (256)
`uint64_t` counts, `luma`, `red`, `green` and `blue`, plus `pixels`, the number
counted. `curveHistogram(curveInput)` counts a sample of any pixel layout in the
perceptual encoding and refuses any other encoding.
`curveHistogram(source, state, request)` samples at `CurveInput` and counts the
result. The rules:
- Bin `i` holds `floor(v * 256) == i`. Anything at or below 0, and NaN, is the
  first bin, as the curves treat NaN as black. Exactly 1, and anything above
  white, is the last bin.
- **Luma is the luma curve's input:** each channel decoded to linear, weighted
  by the working luminance coefficients, and the sum encoded again. It is not a
  weighted sum of perceptual channels: pure red is about bin 139, where that sum
  would put it at 67.
- **Red, green and blue are the channels at the tap.** They are the channel
  curves' input only when the luma curve is the identity, because those curves
  run after it (ADR 033). A histogram after the luma curve would be a second
  tap and a second render, which nothing asks for yet.
- **A fully transparent pixel is not counted**, and neither is one with NaN
  alpha. Every other pixel counts once. Counts are not weighted by alpha, which
  would make them fractional for a case that does not arise: development keeps
  alpha as the source had it, and geometry keeps crops inside the image.

**Python binds all of it** ([ADR 018](018-python-binds-the-public-api-and-nothing-else.md)):
`Tap.CURVE_INPUT`, `sample(source, tap, state=None, *, size, filter,
allow_upscale)` for a buffer or a `Photo`, `CurveHistogram` (read-only `uint64` numpy arrays
and `pixels`), `CURVE_HISTOGRAM_BINS`, `NamedEncoding.REC2020_GAMMA22`, and
`curve_histogram` in three forms:
- a sample;
- a buffer and a required state, with `size=1024` and `allow_upscale`;
- a `Photo` and an optional state, likewise.

The command line does not print a histogram yet.

## Consequences

- **Each histogram costs a render**, as ADR 011 foresaw. That is a pointwise
  pass, geometry and a resize, with no checkpoint to reuse, because the
  preview's pointwise checkpoint holds developed colours, not the tap.
- **One request size: 1024 on the long edge** (`curveHistogramRequest`), about
  0.7 MP for a 3:2 frame. A GPU sample reads that back once at 16 bytes a pixel
  (RGBA-F32): 1024 x 683 is 11.2 MB. A viewport-sized request would read back
  far more (2560 x 1600 is about 65 MB) for no better a 256-bin histogram, so
  a caller does not ask for one.
- **Host cost is measured, and the source size dominates it, not the request.**
  The pointwise pass runs over the whole source before the resize, so a
  reduced source (a preview pyramid level) is what keeps it cheap. Release
  build, one core, Intel i7-7700HQ, mean of five runs, RGBA-F32 source with
  Basic Tone moved, bilinear:

  | source      | request long edge | pixels counted | `sample` | `curveHistogram` (count) |
  |-------------|-------------------|----------------|----------|--------------------------|
  | 1536 x 1024 | 1024              | 699,392        | 315 ms   | 48 ms                    |
  | 3072 x 2048 | 1024              | 699,392        | 1040 ms  | 38 ms                    |
  | 3072 x 2048 | 3072              | 6,291,456      | 1052 ms  | 327 ms                   |
  | 6000 x 4000 | 1024              | 699,392        | 3570 ms  | 35 ms                    |

  Sampling costs about 150 to 200 ns per source pixel; counting about 50 ns
  per counted pixel. At the preview's pyramid level for a 1024 request
  (between 1024 and 2048 on the long edge) the host sample is therefore
  roughly 0.3 to 0.6 s, plus 40 to 50 ms to count. That is too slow to repeat
  on every edit: it is the case for `sameAtTap`, which lets a curve drag skip
  the render, and for the GPU path where the sample is passes and one
  readback. If the host cost matters, the powers in the encode and decode are
  the first thing to replace (a table, or binning in linear against
  precomputed bin edges). The numbers are from a temporary timing run, not a
  committed benchmark.
- **Parity.** The GPU sample is compared to the CPU's after decoding back to
  linear, where the tolerances were measured: the pointwise plus resample
  tolerance (1.3e-4). On lavapipe the worst measured error was 1.0e-5, and no
  histogram count moved bin over the fixtures, geometries and requests tested.
  The test allows 1% of pixels to move one bin, for drivers whose `pow` is less
  exact.
- **What the tests pin.**
  - The CPU tap equals matrix, exposure and `shapeTone` spelled out by hand,
    encoded, bit for bit.
  - With nothing after the tap, a sample with geometry, a region and both
    filters equals the encoded render, bit for bit.
  - Changing the curves, the shoulder, saturation, vibrance or grading changes
    neither the sample nor the histogram. Exposure, Basic Tone and white
    balance do change them.
  - Synthetic buffers pin the binning rules: end bins, NaN, infinities,
    transparency and integer layouts.
  - GPU parity, and a host sample equalling an uploaded one bit for bit.
  - A hard edge that Lanczos rings past leaves the histogram's end bins empty,
    and the default request is bounded by `curveHistogramLongEdge`.
  - `sameAtTap` holds across edits after the tap and fails for exposure, Basic
    Tone, white balance, geometry and a different size.
  - Development and sampling refuse a perceptual sample as a source.
- **Staleness is decided from the plan.** While the user drags a curve, the
  histogram does not change, but every edit before the tap, and every change of
  frame, does. `sameAtTap(planA, planB, tap)`, private in `ProcessingPlan.h`
  beside `developToCurveInput`, compares the fields the prefix reads
  (`curveInputFieldsOf`: the matrix, exposure and the Basic Tone fields), then
  geometry and the resize, which the sample runs through. It is built and
  tested now because it is a few lines beside the prefix. Phase 4 owns using
  it: the GUI keeps the plan its histogram was sampled with and samples again
  only when `sameAtTap` says the new plan differs, as it uses the private
  `sampleOnGpu`. It must not keep its own list of "settings before the curves",
  which would drift from the chain. The source is not in the plan, so a new
  preview level also invalidates the histogram.
- **The GUI is the next caller.** Phase 4 draws the histogram behind the curve
  widget, from `sampleOnGpu` on the preview's uploaded level, or from
  `curveHistogram` on the CPU fallback. A GPU sample meant for a histogram
  passes `curveHistogramRequest` (or another request with Bilinear and a
  bounded size): `sampleOnGpu` honours its request as given, so its default
  would be a full-resolution Lanczos sample, with the ringing and the cost the
  host overload avoids.
- A second tap is one more enumerator, one more prefix function in
  `ProcessingPlan.h` that `developPixel` calls, one more case in `probeFor`
  (and a probe value if none exists), and its encoding in `tapEncoding`.
