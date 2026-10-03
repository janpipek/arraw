# Remaining global adjustments — plan

Status: accepted, 2026-10-03, revised the same day with the user's answers
(see Decisions taken). Recreated from the session transcript after the working
tree was lost; phases are ticked off below as they land.

## Goal

Finish the global (whole-image) controls of `desired-features.md`, on both
backends, through settings, sidecar, JSON, CLI, Python and the develop panel:

| Group | Controls | Kind (ADR 011) |
|---|---|---|
| Tone Curve | Luma curve; R, G, B curves | pointwise |
| Colour Grading | Shadows, Midtones, Highlights hue + saturation; Balance; Blending | pointwise |
| Effects | Post-crop vignette (amount, midpoint, feather); grain (amount, size, roughness) | pointwise, but reads the crop frame |
| Detail | Texture, Clarity, Dehaze; Luminance NR (amount, detail); Colour NR (strength, smoothness) | spatial |

Already done: White Balance, Tone + Filmic Highlights, Color, HSL, Black & White,
Geometry, export sharpening (ADR 026).

Also in scope: the curve-input histogram the curve widget draws over, which
needs ADR 011's `sample(tap)`.

Not in scope: Lens Corrections, Local Adjustments, Spots, the panel histogram,
crs: interoperability (ADR 019 still holds: `arraw:` only), a develop-time
Sharpen (ADR 026 stands), and the demosaic choice: it is a decode option
(LibRaw, a re-decode, ADR 011's future `Decode` block), so it gets its own plan
with RAW reading rather than riding along with Detail.

## How each control is added

The pattern ADR 027 followed, unchanged:

1. Settings struct in `include/`, a field on `DevelopSettings`, rows in
   `developSettingDescriptors` (key, range, group, applicability, stage).
2. Resolution into the plan (`ProcessingPlan.cpp`); a control at its default
   resolves to a flag that is off, so defaults stay bit-identical.
3. CPU reference maths (`src/core/*.cpp`), unit-tested.
4. GPU mirror: `GpuPlan` packing with std140 `static_assert`s, shader functions
   mirrored one for one, parity tests in `tests/gpu/` with a stated tolerance.
5. CLI flags, Python binding and `.pyi`, descriptor-driven tests.
6. Develop panel rows via `SettingPresentation`.
7. An ADR per group.

## Design decisions

Each has a recommendation; the alternatives are what review should push on.

### D1. Tone curve representation and evaluation

**Storage.** `ToneCurveSettings` holds four curves, each an ordered list of
control points `(x, y)` in [0, 1], at least the two endpoints, at most 16. The
identity is `{(0,0), (1,1)}`. This needs a new leaf type in the accessor
variant and `SettingCodec`: JSON `[[x, y], ...]`, XMP `"x,y;x,y;..."`.

- (a) **Points in [0, 1] (recommended).** Matches every other float setting
  and the widget.
- (b) Points in 0–255 as Lightroom's `ToneCurvePV2012`. Only worth it with crs:
  interoperability, which is out of scope; convertible later.

**Interpolation.**

- (a) **Monotone cubic (Fritsch–Carlson) (recommended).** No overshoot, so a
  curve the photographer drew rising never dips; a dragged point never makes
  a neighbour's segment ring.
- (b) Natural cubic spline, as Lightroom and `main`. Smoother on wild curves,
  but it overshoots and can go non-monotone, which then needs clamping.

**Evaluation.** The plan holds a resolved LUT per curve (1024 entries over
[0, 1], linear between entries); the curve's slope at x = 1 extends linearly
above 1, as `main` did, so headroom stays recoverable for the shoulder. A curve
equal to identity resolves to "off". GPU: the four LUTs as one 1024×1 RGBA32F
texture bound beside the source (a 16 KB UBO also fits the guaranteed minimum,
but a texture leaves the block small). Plan equality compares the LUT, which is
exact and cheap (ADR 011).

**Coordinate.** The existing perceptual coordinate `y^(1/2.2)` (ADR 010), the
same one ADR 011 names as the curve input tap. `main` used sRGB gamma (its
ADR 0003); one perceptual coordinate in the engine beats two.

**Luma vs RGB.** The luma curve shapes luminance and scales the colour by the
ratio (hue-preserving, like `shapeTone`). R, G, B curves act per channel
afterwards, and are allowed to shift hue: that is their purpose.

**Placement.** After `shapeTone`, before `rollHighlights`: ADR 011's tap
definition ("immediately before the tone curve", after Basic Tone) and
Lightroom's order. The shoulder then still catches whatever the curve lifts
past white.
Alternative: after the shoulder, so the widget's x-axis is displayed values;
rejected because a curve could then clip.

### D2. Colour Grading

Port `main`'s ADR 0052 maths unchanged in substance: three zones weighted on
perceptual luminance, Balance (Lightroom's sign) and Blending, tint in Oklab,
hue + saturation per zone, no per-zone luminance. Keys `gradeShadowHue`,
`gradeShadowSaturation`, … `gradeBalance`, `gradeBlending`; own group
`ColorGrading`.

**Placement.** At the end of `adjustColor`, after the Colour/B&W branch
merges, so it tints B&W too (the feature's reason to exist).

- UI: eight sliders (main's precedent; no colour wheels — a wheel is a new
  custom widget). Wheels can come later over the same settings.

### D3. Where Effects run

Vignette is relative to the *cropped* frame, and grain must be anchored to it
too (fixed across pan, zoom, preview and export). The pointwise pass runs on
source pixels before geometry, so it does not know the crop frame.

- (a) **A new `Stage::Effects` pass after `Resize` (recommended).** A cheap
  per-pixel pass whose position in the crop frame is
  `(region origin + (pixel + 0.5) * region / outputSize) / croppedSize`, all in
  the plan's `ResizePlan`. A crop or zoom change never invalidates the
  pointwise checkpoint; a vignette change recomputes only this pass. With both
  effects off the boundary collapses onto `Resize` (ADR 011) and costs nothing.
- (b) Inside the pointwise chain, mapping each source pixel through the
  inverse geometry. One pass fewer, but every crop edit then redevelops the
  whole frame, and the preview's reduced source pixels sample grain coarsely.
- (c) Fused into the resize shaders. Fastest, but four resize variants grow an
  effects tail and the identity resize has no pass to hang it on.

Consequences of (a):
- **Vignette follows the shoulder.** A lightening vignette applied as an
  exposure gain could push values past white with nothing after it to roll
  them off; the output conversion would clip them. Options:
  - (i) **Darken as an exposure gain, lighten as a screen (recommended).** In
    the perceptual coordinate, a negative amount multiplies (exactly an
    exposure change, as `main`); a positive one maps `v → 1 − (1 − v)·g`, which
    lifts the edges toward white and never past it. No second shoulder, no
    clipping, and both halves are smooth at amount 0.
  - (ii) Re-run `rollHighlights` after a positive vignette. Reuses the
    shoulder, but a highlight is then compressed twice, and with Filmic
    Highlights at 0 it does nothing.
  - (iii) Accept clipping, as `main` did.

  Export adds nothing here: export sharpening (ADR 026) is the only operation
  that waits for the output encoding, because it is tuned for the output. The
  effects are part of the look, so the preview shows them and export renders
  them the same way, through the `Effects` boundary.
- `Stage` gains a fourth value; callers that stop after `Resize` (preview,
  export) move to `Effects`. `RenderCheckpoint`, the GPU resume path and the
  preview's checkpoint reuse all see it.

**Grain is a replaceable model.** The algorithm will change, so it sits behind
one seam:
- The settings are what a photographer sees and any model can honour:
  `grainAmount`, `grainSize`, `grainRoughness`, the seed, and
  `grainModel` (an enumeration, one value to start: `valueNoise`). A new
  model is a new enumeration value, not new settings.
- The plan holds a `GrainPlan`: the model, its resolved parameters and the
  seed. One function per model on the CPU (`GrainModels.cpp`), one shader
  function per model, chosen by a uniform; the Effects pass does not know
  which model it calls. Each model owns its band-limiting.
- The seed policy is separate from the model (below), so swapping one does not
  touch the other.

The first model ports `main`'s value-noise grain in an encoded (sRGB-like)
coordinate, monochrome, zero mean; size relative to the crop's long edge, so
it is resolution-independent. One change: **band-limit by pixel footprint.**
Each render knows how many crop-frame units an output pixel covers; octaves
finer than that fade out instead of aliasing. That is what makes the
"a preview may soften grain but never rearrange it" promise actually hold.

**Grain seed.**
- (a) **A hidden `grainSeed` setting (uint32), default 0 (recommended).** When
  the grain amount first leaves 0 with seed 0, the *front end* (GUI edit, CLI,
  Python helper) picks a random seed through one function,
  `arraw::chooseGrainSeed`, so the policy lives in one place and (b) can
  replace it there; the core never invents randomness, so a
  plan stays a pure function of the state. Seed 0 itself renders with a fixed
  constant, so a document without one is still deterministic. Excluded from
  the panel; future presets/copy-paste skip it (as `main`).
- (b) Derive the seed from the photo's identity (file name hash). Stateless,
  but renaming a file re-rolls the grain.
- (c) One fixed seed for everything. Repeats visibly across a batch.

### D4. Detail: two new spatial passes

The pipeline today is `Pointwise → Geometry → Resize`. Proposed:

```text
Denoise → Pointwise(+context) → Geometry → Resize → Effects
```

**Denoise (Luminance + Colour NR)** is a pass on the source, before
pointwise, as `main` effectively had it. It is a `Stage` and a checkpoint, so
every slider after it reuses the expensive result.

- The luma/chroma split uses the as-shot source → working luminance row,
  *not* the plan's `toWorking`: a temperature or exposure move must not
  invalidate the denoise checkpoint (the same reasoning as ADR 007).
- Colour NR: `main`'s construction — blur the unit-luma chroma ratio, preserve
  luma exactly — at quarter resolution with a defined box downsample and
  bilinear upsample, identical on both backends.
- Luminance NR has **more than one filter**, chosen by a setting
  `luminanceNoiseFilter` (an enumeration, as the demosaic choice will be), with
  Amount and Detail meaning "how much" and "how much edge to keep" for every
  filter. Each filter is a function on both backends behind the same seam:
  perceptual luma in, filtered luma out; decomposition and recombination are
  shared.
  - **First: separable bilateral**, `main`'s ADR 0046 — two passes, a direct
    port, a known look. Cross artefacts at strong settings are accepted for now.
  - Later: guided filter (box means only, no cross artefacts, and an
    edge-aware base Clarity could share), then wavelet or non-local means if
    wanted.
- Radii are in sensor pixels; the preview's reduced source (ADR 023) divides
  them by `2^level`, so the preview approximates and 1:1 is where NR is judged.
- With all NR at 0 the pass does not exist and its boundary collapses.

**Texture, Clarity, Dehaze** need a blurred neighbourhood but act on tone.

- (a) **A context side-product, read by the fused pointwise pass
  (recommended).** After Denoise, compute log-luminance at reduced resolution
  and two filtered bases: a small Gaussian (texture) and a large one (clarity,
  dehaze). The large base starts as a plain Gaussian, as `main`'s; halos at
  strong Clarity are the known cost, and an edge-aware base (the guided filter,
  once it exists) is the planned refinement. The pointwise chain reads them at the
  pixel's coordinate and works on `log Y − base`: *detail in log luminance is
  invariant to exposure and white balance gains*, so moving those sliders
  never recomputes the context, and the chain stays one pass. The amounts live
  in the pointwise block; only the radii (constants) and denoise feed the
  context.
- (b) Split the pointwise pass in two around a spatial local-contrast pass on
  toned values. Truer to "local contrast of what you see", but every tone
  slider then pays for a spatial pass.

  Placement in the chain: after `shapeTone`, before the curve, so the shoulder
  still catches boosted highlights. Dehaze starts as `main`'s practical
  approximation (veil + local contrast + restrained chroma), not a
  dark-channel estimator.

- The context is a second buffer that the Denoise checkpoint must carry (or be
  recomputed from it). First cut: recompute when any of the three is on
  (quarter resolution; cheap on the GPU, ~100 ms CPU at 24 MP — to be
  measured); cache it on the checkpoint once measured as worth it. Either way
  `RenderCheckpoint`'s payload grows an optional auxiliary image.
- Clarity radius relative to the image's long edge (a composition-scale
  control: the preview stays faithful); Texture in sensor pixels (detail scale:
  approximate in preview, like NR).

### D5. Tolerances

CPU stays the reference. Pointwise additions keep the current parity
tolerance. Spatial passes get their own, stated in each ADR, measured on the
existing fixtures; reduced-resolution intermediates are defined by exact
formulas (box down, bilinear up, clamp-to-edge) so the backends compute the
same thing rather than "similar".

## Phases

Each phase is one workflow: Opus agents for the steps that fix a design in
code (new types, stage boundaries, plan and codec shape, GPU block layout),
Sonnet agents for the follow-through (frontends, bindings, tests). Opus reviews
every phase before the next; agents never commit, and each reviewed phase is
committed on its own. Each ends with an ADR and green
`just test`, including the GPU suite where a device exists.

1. **Tone Curve, engine.** Curve leaf type in the codec, `ToneCurveSettings`,
   monotone-cubic LUT, chain placement, GPU texture, CLI `--tone-curve`,
   `--tone-curve-red|green|blue` (`"x,y;x,y"`), Python.
2. **Colour Grading, engine.** Settings, maths, GPU, CLI, Python.
3. **Curve-input tap and histogram.** `sample(Tap::CurveInput)` on both
   backends (ADR 011: the GPU branches on a uniform and writes the tap instead
   of the developed colour), a histogram of luma and R, G, B over the
   perceptual coordinate, computed on the host from the tap at preview
   resolution. Exposed through the public API and Python; the CLI may print it
   later.
4. **GUI for 1–3.** Curve editor widget (painted, Qt Widgets; add, drag and
   remove points, channel switch, histogram behind it, refreshed when the
   preview pauses), Colour Grading group.
5. **Effects.** `Stage::Effects`, vignette, the grain seam with the
   value-noise model, seed policy, both backends, CLI/Python, panel group;
   callers moved to the new boundary.
6. **Denoise.** `Stage::Denoise`, the luma filter seam with the separable
   bilateral, the chroma blur, Luminance and Colour NR, preview debounce,
   panel rows.
7. **Texture, Clarity, Dehaze.** Context side-product, chain stage, panel rows.

1–4 are independent of 5–7; 7 depends on 6.

## Decisions taken (2026-10-03)

1. Curve-input histogram: build it now (phase 3).
2. Grain: as modular as possible — the algorithm is replaceable (D3). Seed:
   hidden setting chosen by the front end, behind one function (D3 a), unless
   review says otherwise.
3. Vignette past white: darken as an exposure gain, lighten as a screen
   (D3 i), accepted. Export does nothing special.
4. Luminance NR: several filters behind a setting; the separable bilateral
   first.
5. Develop-time Sharpen: out. Demosaic: out of this plan, to its own plan with
   RAW reading.
6. Colour Grading Global zone: not needed.
