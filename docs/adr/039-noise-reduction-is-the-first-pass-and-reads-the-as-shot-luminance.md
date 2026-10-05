# Noise reduction is the first pass, and reads the as-shot luminance

High-ISO photographs carry luminance noise (grain in brightness) and colour
noise (coloured blotches). Both are spatial: a pixel's result depends on its
neighbours, so neither fits the pointwise chain. This ADR records where noise
reduction runs, how it splits a pixel into luminance and colour, the two
filters, how a reduced preview and a region render treat them, and the GPU
passes. It implements the Denoise part of D4 of the
[global adjustments plan](../ideas/global-adjustments-plan.md), with the
decision taken there (several luminance filters behind a setting, the
separable bilateral first). The maths ports `main`'s ADR 0034 (colour) and
ADR 0046 (luminance), whose controls, ranges, defaults and calibration it
keeps.

## Decision

**A first pass boundary, `Stage::Denoise`, before `Pointwise`.** `stageCount`
is 5 and the stage indices shift by one (Python's `Stage` too: `DENOISE` is
0). The plan gains a `DenoisePlan denoise` block, the first group of
`stagesOf`, so `prefixMatches` and the full-depth guard cover it (ADR 011,
ADR 015). A checkpoint at `Denoise` holds the source after noise reduction,
RGBA float, in the *source's* encoding and pending orientation; every later
boundary is unchanged. `developUntil`, `resumeFrom`, `developOnGpu` accept
the new stop, and `requireResumable` expects the source's size there.
Resuming from `Denoise` on the CPU makes no copy: the pointwise chain only
reads its input.

**With all noise reduction off the pass does not exist.** Off means
luminance amount 0, and colour amount 0 or smoothness 0 (`main`'s rule;
`reducesNoise(settings)` says it for front ends). The block is then the
default whatever Detail, Smoothness and the filter say, so the plan, the
pixels and the GPU's render count are those of before this change. The
boundary collapses onto the source: the CPU chain reads the source directly
(`develop` pays nothing), the GPU shares the upload, and a CPU stop at
`Denoise` is a float copy of the source. The preview keeps no `Denoise`
checkpoint then, since it would only be that copy.

**The luma/chroma split uses the source's as-shot luminance row, never
`toWorking`.** For a camera-native source the row is
`workingLuminance * camera.toWorking`, the camera matrix as shot (ADR 007);
for a working-space source, Rec.2020's luminance. The plan's `toWorking`
carries the white balance the photographer chose and exposure is a later
gain, so neither reaches the denoise group: a temperature, tint or exposure
edit resumes from the `Denoise` checkpoint. The neutral of unit luminance,
`n = (1, 1, 1) / sum(row)`, is the block's other colour constant.

**Decomposition and recombination are shared by both halves.** A colour `c`
with luminance `Y = row . c` is split into `Y` and a unit-luma ratio

```text
D = max(Y, 0) + 2^-14
r = (c + (D - Y) n) / D            c = D r - (D - Y) n
```

which is `c / Y` (`main`'s ratio) for any luminance well above the floor, a
bounded vector near black and below it, continuous between, and exactly
invertible: the recomposed colour's luminance is `Y` whatever the ratio. So
colour smoothing keeps luminance exactly and luminance smoothing keeps the
ratio exactly (`main`'s duality), negatives and zero included. The pixel is
`recompose(mix(Y, Y', luminanceMix), mix(r, r', colorMix))`.

**Colour NR blurs the ratio on a box-reduced grid.** The grid cell is a
`R x R` block of source pixels, `R = max(1, 4 >> level)`: four sensor pixels a
side at full resolution (`main`'s quarter resolution), and the same four
sensor pixels at levels 1 and 2. Each cell is the plain mean of the colours it
covers (edge cells average the pixels they have), and its ratio is that of the
mean colour, so dark pixels weigh as their luminance does. The ratio is
blurred by a normalised separable Gaussian, across then down, edges clamped,
sigma `smoothness * 0.25 * 2^-level / R` cells (Smoothness 100 is 25 sensor
pixels, `main`'s calibration), radius `ceil(3 sigma)` in 1 to 64. Each pixel
reads the grid bilinearly at `u = (x + 0.5) / R - 0.5`, clamped to the grid,
rows first. Strength 0 to 100 is `colorMix` 0 to 1.

**Luminance NR is a filter behind a setting.** `luminanceNoiseFilter` is an
enumeration encoded by name like `grainModel`; its one value is `bilateral`.
The seam is "linear luminance in, filtered luminance out": every filter
measures edges as it likes, and the decomposition and recombination above
stay outside it (CPU `filterLuminance`, GPU `denoiseOnGpu`'s switch). Amount
is `luminanceMix` (0 to 1) and Detail the edge-stop for every filter. The
bilateral is `main`'s ADR 0046: spatial sigma 2 sensor pixels (`2 * 2^-level`
on a level), radius `ceil(3 sigma)`; a horizontal pass, then a vertical pass
on its result; each tap weighs `g(tap) * exp(-(P(y_t) - P(y_c))^2 / (2 s^2))`
with the centre weighing 1, where `P(y) = max(y, 0)^(1/2.2)` is the
perceptual coordinate (ADR 010) and `s` runs linearly from 0.20 (Detail 0)
to 0.02 (Detail 100). A guided filter later is a new enumeration value with
its own steps, and documents keep rendering with the filter they name.

**Radii are in sensor pixels; a reduced source divides them by its pixel
scale.** `ImageBuffer::pixelScale()` says how many sensor pixels one pixel of
the buffer spans along each side, and `planFor(buffer, …)` reads it from the
source, so nothing a caller passes can get it wrong. A full decode is 1. A
half-size RAW decode is 2, whoever halved it (LibRaw, or the linear-DNG path).
`halved` doubles it, so a pyramid level `n` of a full decode is `2^n` and the
thumbnail's levels of a half-size decode are `2^(n+1)`. A resize multiplies it
by the reduction of the width (`resampledPixelScale`, one rule for both
backends). Copies, conversions, the Denoise and pointwise passes, geometry
(crop, turn, straighten) and a region's cut keep it. A device image carries it
from the upload through every pass, and reads it back. With scale `s` the
bilateral's sigma is `2 / s` source pixels, the grid cell is
`clamp(floor(4 / s), 1, 4)` source pixels, and the colour sigma is
`smoothness * 0.25 / s / cell` cells. For the powers of two the pyramid and the
half-size decode give this is exactly what the level gave before. A scale that
is not finite and above zero is refused, both by `setPixelScale` and by the
plan. The preview approximates and 1:1 is where noise reduction is judged.

**A region render needs no margin today.** The region is cut after geometry
(ADR 025) and the Denoise pass sees the whole source, so a region is the crop
of the whole render bit for bit (tested). When the earlier stages are
restricted to a region's footprint, `denoiseReach(plan)` is the margin in
source pixels: the bilateral's radius, or `(colorRadius + 2) * R` for the
colour grid, whichever is larger; the grid must then also stay aligned to the
source's origin.

**The GPU runs up to six renders**, sharing one std140 `GpuDenoiseBlock` (352
bytes): `DenoiseFilter` for the grid reduction, the two blur steps and the
bilateral's two steps (`DenoiseStep` picks one), and `DenoiseCombine`, with the
source, the blurred grid and the filtered luminance on bindings 0, 2 and 3 (the
source stands in for a half that is off). The spatial weights are computed on
the host by the CPU's own `denoiseWeights` and uploaded, so only the
edge-stop's `exp`, the perceptual `pow` and float sums are the device's. Every
shader function mirrors the C++ of the same name in `Denoise.cpp`. Colour alone
is four renders, luminance alone three, both six.

**The luminance intermediates are as small as QRhi allows.** A render can ask
for a one-channel target (`GpuTarget{.format = GpuTargetFormat::R32F}`).
`GpuDeviceInfo::scalarFloatTextures` says whether the device has R32F; without
it the target is RGBA32F, which a pass reads the same in its first channel. The
bilateral's down step writes the filtered luminance into R32F. Its across step
writes `(luminance, perceptual)`, so that down reads both instead of evaluating
`pow` for every tap, as the CPU reads its precomputed perceptual planes. QRhi
(6.10) has no RG32F, so across stays RGBA32F. Each intermediate is dropped as
soon as the next step has read it. The pass then holds at most the source, one
RGBA intermediate and the scalar luminance, 36 bytes a pixel, where it held 48
(about 0.86 GB instead of 1.15 GB at 24 MP, the upload included). The combine
step needs the same 36 (source, luminance, result), so an RG32F across would
not lower the peak. Pipelines are cached per pass and target format, since a
render pass is only compatible with targets of its own format.

**The CPU pass splits rows across threads and keeps the bits.**
`detail::forEachRowBand` (`src/core/RowBands.h`) runs the luminance plane, the
perceptual planes, both bilateral passes, the grid reduction, both colour
blurs and the recombination over contiguous bands of rows, one `std::jthread`
each, as many as there are hardware threads and no band under 65 536 pixels.
Every pixel is computed from planes no band writes, by the same code whatever
the split, so the result is the single-threaded one bit for bit (tested with 1,
3 and 4 bands). The filters have an interior loop that indexes neighbours
directly, and the clamped one runs only within a radius of an edge. Both call
one per-pixel function, so the arithmetic, and its order, is the same. The
edge-stop's `exp` was left alone: a table or a polynomial would change the
bits that make the CPU the reference.

**Settings** (`NoiseReductionSettings`, `DevelopSettings::noiseReduction`,
group `Detail`, `Stage::Denoise`, applicable to every photograph):

| Key | Range | Default | `main` / Lightroom |
|---|---|---|---|
| `luminanceNoiseReduction` | 0 to 100 | 0 | Amount (`crs:LuminanceSmoothing`) |
| `luminanceNoiseDetail` | 0 to 100 | 50 | Detail |
| `luminanceNoiseFilter` | `bilateral` | `bilateral` | none: `main` hard-wires it |
| `colorNoiseReduction` | 0 to 100 | 0 | Strength (`crs:ColorNoiseReduction`, "Color") |
| `colorNoiseSmoothness` | 0 to 100 | 50 | Smoothness |

The settings' and the descriptor table's default for Colour NR stays the
neutral 0, so a default-constructed state, every document that omits the key
and every picture that is not a RAW are untouched. Non-finite values are
refused and out-of-range ones clamped (ADR 008).

**A RAW starts with colour noise reduction 25, as in Lightroom**
(`rawDefaultColorNoiseReduction`). Demosaiced sensor data always carries
colour noise, and an encoded picture has been through someone's noise
reduction already. One function says what a photograph of a kind starts from,
`defaultStateFor(encoding)`: 25 for a camera-native encoding, neutral for
anything else. Everything that needs a photograph's defaults goes through it:

- **A new photograph.** `openPhoto` with no sidecar, or with one that records
  no state, and the two-argument `Photo(path, metadata)`, which is what the
  command line's `--no-sidecar` and Python's `open(…, sidecar=False)` build.
  An unreadable sidecar opens as if there were none.
- **What a sidecar means.** A sidecar records a state when it holds arraw's
  develop settings: an `arraw:version` or any settings key.
  `SidecarContents::state` is then that state, applied onto the *neutral*
  defaults, and is empty otherwise (a sidecar of marks alone, or one only
  Lightroom wrote in). The writer has always written every key, so a key is
  missing only from a sidecar written before the setting existed. Every
  sidecar written so far omits the noise reduction keys, so it keeps rendering
  as it did, with colour NR 0. A RAW beside a Lightroom XMP starts at 25.
  `writeSidecarMarks` on a photograph with no sidecar now writes the marks
  alone, with no version and no settings, so rating a RAW before opening it
  does not pin it to neutral. `writeSidecar` writes the 25 explicitly.
- **Reset to default.** The develop panel's `PanelContext::defaults` holds
  `defaultStateFor(photo encoding).settings`, and a double-click on a row's
  label restores that value: 25 for a RAW's Colour, 0 for a JPEG's.
- **The command line and Python.** They open photographs through the same
  `openPhoto` and `Photo` constructor, so a RAW without a sidecar exports with
  25. `info` lists it, since it differs from the neutral defaults the JSON
  document assumes. Python's `develop(buffer)` and `sample(buffer, tap)` with
  no state use `default_state(buffer)`, and `default_state(metadata)` is bound
  too. The thumbnail worker reads the file's header to choose the defaults when
  there is no recorded state, and keys its cache on the state it develops with.

`main` (ADR 0034) chose a neutral 0 on import. That kept a RAW looking as it
was decoded, at the price of colour blotches in every high-ISO file until
someone moved the slider. The command line takes
the four amounts through the descriptor table and `--luminance-noise-filter`;
Python has `NoiseReductionSettings`, `LuminanceNoiseFilter` and the flat keys.

## Consequences

- A tone, colour, white balance, exposure, crop, pan or effect edit never
  reruns noise reduction; a noise reduction edit, or another preview level,
  reruns everything.
- Parity: the Denoise pass alone agrees with the CPU to 8.0e-7 relative on
  lavapipe (both halves, pixel scales 1, 2 and 4, an odd size with partial
  grid cells, a camera-native fixture, negatives and near-black values),
  unchanged by the scalar target and the precomputed perceptual value. It is
  held to the pointwise tolerance, 3e-5; a wrong tap, grid cell or bilinear
  weight disagrees by 1e-3 or more. Only lavapipe was measured.
- Tests hold: defaults and off settings give the default block, the same plan
  and the same pixels, with no GPU render; colour NR keeps every pixel's
  luminance (to float rounding, also below zero) and removes most chroma
  variance on a flat patch; the bilateral removes most variance on a flat
  noisy patch, keeps a step edge's levels and keeps grey grey; white balance
  and exposure leave the denoise group and the checkpoint valid; radii and the
  grid scale with the pixel scale (powers of two, 3, 0.5, and refusals), which
  a halving, a half-size decode, a resize and a region carry on both backends;
  a level's colour smoothing is within 0.01 of the full render's on a
  flat-luminance source, and the rejected quarter-of-the-level grid is more
  than twice as far; a region is the crop of the whole render, also on a
  reduced source; stop and resume at the new boundary on both backends, with a
  tone edit on the GPU skipping the six renders, and another pixel scale
  refused; the threaded pass equals the single-threaded one byte for byte; a
  scalar target keeps the first channel. For the defaults: a RAW opens, saves,
  exports (`--no-sidecar` too), resets and develops in Python with 25, and a
  JPEG with 0; an old-style sidecar keeps 0; a marks-only or foreign sidecar
  records no state; a no-sidecar RAW's thumbnail is cached under the 25.
- CPU cost at 24 MP (6000 x 4000 RgbaF32, release, `applyDenoise` alone,
  default radii at scale 1, best of three, on an i7-7700HQ with 4 cores and 8
  threads). Before is one thread and clamped reads everywhere. After is 8 bands
  and the interior loop, with bit-identical output:

  | case | before | after |
  |---|---|---|
  | luminance 50 (r = 6) | 4.59 s | 1.44 s |
  | colour 50 (blur r = 10 cells) | 0.76 s | 0.40 s |
  | both | 5.08 s | 1.57 s |

  The interior loop alone is within noise: the edge-stop's two `exp`s a tap
  dominate. Threads give 3.2x on 4 cores. The colour half is bound by
  per-pixel divisions and memory, so it gains less. A preview level costs a
  quarter of that per level.
- Every RAW now runs the colour half by default: about 0.4 s at 24 MP on the
  CPU, a few hundred ms on a preview level, and four renders on the GPU. The
  preview keeps a Denoise checkpoint for it, a level-sized float buffer.
- The develop panel shows the noise reduction rows
  (`buildNoiseReductionGroup`); the filter is hidden as a choice of one.
- The preview holds a render back 200 ms after an edit that changes only the
  noise reduction settings (`renderDelayFor`), as `main` did, so a drag of
  Luminance or Detail asks for one render at rest rather than one per tick: a
  noise edit reruns the whole chain, the most expensive render there is. The
  edit itself is recorded at once, so undo and the panel see every step. Any
  other control, undo, redo and a change of view render at once, and a newer
  request cancels the held one. Renders still cannot be cancelled once under
  way, so the threaded CPU pass (1.4 s at 24 MP) bounds the wait after a
  drag, down from about 5 s.
- Only lavapipe was measured for the scalar target (it has R32F). The RGBA
  fallback is the same shader and target as before this change, but no device
  in the suite exercises it.

## What was rejected, and why

- **Splitting luminance with `toWorking`** (the white-balanced matrix). One
  less matrix, but every temperature tick would rerun the most expensive pass.
- **`main`'s placement, colour NR last.** `main` ran the chroma blur after
  everything, which was safe for its pipeline; here a first pass lets every
  later slider reuse it, and luminance NR wants the undeveloped signal.
- **A grid that is always a quarter of the level.** On a preview level it
  would blur blotches four times larger in sensor pixels than at 1:1, so the
  preview would overstate colour smoothing; a grid fixed in sensor pixels down
  to one pixel keeps them comparable.
- **A plain ratio `c / Y` with a neutral below a threshold**, as `main` did.
  It is discontinuous at the threshold and has no answer for negative
  luminance; the floored form is continuous and exactly invertible.
- **Evaluating the spatial Gaussian in the shaders.** Uploading the CPU's
  weights removes one source of disagreement for free.
- **The source's pyramid level on `RenderRequest`** (this ADR's first form).
  A fact about the pixels on the request meant every caller that developed a
  reduced buffer had to remember it, and forgetting it gave no error, only
  stronger smoothing. It also could not express a half-size decode, which is
  a reduction of 2 but not a pyramid level. Two others were rejected as well.
  A float pixel pitch on the request covers the half-size case, but a caller
  can still forget it. A note that the burden is the caller's leaves the
  thumbnail wrong.
- **RG32UI for the across step**, with the floats' bits packed into
  unsigned integers. It would be exact, but it needs integer samplers in the
  next shader and integer render targets, which QRhi supports less widely. It
  would also not lower the peak, which the combine step sets.
- **A table or polynomial for the edge-stop's `exp`.** It would be several
  times faster again, but the CPU would no longer give the bits the GPU parity
  tests and the checkpoint tests are held to.
- **Colour NR 25 on every photograph, or a different default in the
  descriptor table.** The table describes documents; a JPEG and a sidecar
  that omits the key must stay neutral. The kind is the photograph's, so the
  default lives with it.
- **A missing sidecar key meaning the kind's default.** It is simpler to
  state, but every sidecar written before noise reduction existed would gain
  colour NR on a RAW and change how an edited photograph looks.
