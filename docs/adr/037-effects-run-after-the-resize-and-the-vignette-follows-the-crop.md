# Effects run after the resize, and the vignette follows the crop

The post-crop vignette darkens or lightens the edges of the frame the
photographer cropped to, and grain must be anchored to that frame too: fixed
across pan, zoom, preview and export. The pointwise pass runs on source pixels
before geometry, so it cannot know where a pixel will land in the crop. This
ADR records where effects run, how a pixel finds its place in the crop frame,
and the vignette, the first effect. It implements D3 of the
[global adjustments plan](../ideas/global-adjustments-plan.md), with the
user's decision there (darken as an exposure gain, lighten as a screen).
Grain followed in its own step, into the seam described below (ADR 038).

## Decision

**A fourth pass boundary, `Stage::Effects`, after `Resize`.** `stageCount` is
4. The plan gains an `EffectsPlan effects` block, the fourth group of
`stagesOf`, so `prefixMatches` and the full-depth guard cover it as they
cover the others (ADR 011, ADR 015). The block holds only what the settings
resolve to; nothing about the request.

**Where a pixel is in the crop frame comes from the resize block.** Output
pixel `(x, y)`, at its centre, is at

```text
(region origin + (pixel + 0.5) * region size / output size) / cropped size
```

per axis, in fractions of the cropped frame. `frameMappingOf(plan)` works it
out in double from `ResizePlan` (region and output size) and the geometry's
output size, as an origin and a step (`FrameMapping`); the step is the
footprint of one output pixel in the frame, which grain band-limits to (ADR 038),
and `aspect` is there for effects that must stay round. Both are earlier
groups of the plan, so the effects block need not copy them to be compared
correctly: a resumed render that changed the region already fails at the
resize. A region (ADR 025) therefore gets exactly the falloff the whole frame
has at the same place, at any zoom, and a pyramid level (ADR 023) the same up
to the level's sub-pixel rounding of the crop.

**With every effect off the boundary collapses onto the resize** (ADR 011):
the CPU hands the resized buffer on untouched, and the GPU shares the resized
texture and renders nothing (`renderCount` is unchanged). A checkpoint at
`Effects` then holds the resize's pixels; `Resize` keeps its meaning. A
setting at its default resolves to the default block, so the plan, the pixels
and the GPU's passes are identical to before this change.

**Callers that ended at the resize end at the effects.** `develop` runs the
effects last. `developUntil`, `resumeFrom` and `developOnGpu` accept
`Stage::Effects` (the GPU's default stop is now `Effects`), read the request
for any stop at or after `Resize`, and `requireResumable` expects the resize's
output size for both late boundaries. The command line's GPU path and the
export queue stop at `Effects`; Python's `develop` and the CPU paths call
`develop`. Python's `Stage` gains `EFFECTS`.

**The preview keeps a resize checkpoint too.** `CheckpointCache` holds the
pointwise, geometry and resize checkpoints and tries them newest first. A
vignette edit resumes from the resize and costs one cheap pass; a pan or zoom
from the geometry; a crop from the pointwise result. With effects off, the
resize checkpoint costs nothing on the GPU (the effects checkpoint shares its
texture) and one viewport-sized copy on the CPU, where every resume copies
(ADR 024). `PreviewResult::resumedFrom` can now say `Resize`.

**Samples stop before the effects.** `sample(Tap::CurveInput)` and its GPU
twin still run to the resize: the tap is inside the pointwise chain, before
the tone curve, so no effect can be in it, and `sameAtTap` needs no change.

**Export sharpening stays last.** `exportImage` sharpens what `develop`
returns (ADR 026), which now includes the effects.

**The GPU runs one `Effects` pass** (`src/gpu/shaders/effects.frag`), one
input, with a std140 `GpuEffectsBlock`: the mapping's origin and step
narrowed to float (good to about 1e-7 of the frame), then the vignette's flags
and three floats, then grain's members (ADR 038). The shader's
functions mirror `Effects.cpp` one for one.

**The vignette.** `VignetteSettings` (in `EffectsSettings`, `DevelopSettings::effects`):

| Key | Range | Default | Meaning |
|---|---|---|---|
| `vignetteAmount` | -100 to 100 | 0 | negative darkens, positive lightens; 100 is two stops at the full falloff |
| `vignetteMidpoint` | 0 to 100 | 50 | where the falloff begins, from the centre outward |
| `vignetteFeather` | 0 to 100 | 50 | softness; 0 is a hard edge |

Group `Effects`, applicable to every photograph, affecting `Stage::Effects`.
These are `main`'s three controls, ranges and defaults (its ADR 0026); `main`
has no roundness and neither does this.

- *Falloff.* Radius `r = sqrt(((2x - 1)^2 + (2y - 1)^2) / 2)` over the frame
  position: 0 at the centre, 1 in the corners, along ellipses fitted to the
  frame. `inner = 0.85 * midpoint / 100`; `outer = inner + (1 - inner) *
  feather / 100`, kept at least 1e-4 above `inner`; the weight is
  `smoothstep(inner, outer, r)`, or a step at `inner` when the feather is
  exactly 0. That is `main`'s shape.
- *Darken* (amount below 0): every channel is multiplied by
  `2^(-stops * weight)`, `stops = 2 * |amount| / 100`. In the perceptual
  coordinate `v = y^(1/2.2)` (ADR 010) that is exactly `v * g` with
  `g = 2^(-stops * weight / 2.2)`, an exposure change, hue-preserving, and
  negatives scale with it. It is done as the linear gain, which is the same
  number and needs no power.
- *Lighten* (amount above 0): each channel goes into the perceptual coordinate
  (signed, so a negative channel keeps its magnitude), `v -> 1 - (1 - v) * g`,
  and back. A screen: it lifts toward white and never past it for inputs up to
  1, is monotone in the value and in the amount, and needs no second shoulder.
  It is the darkening mirrored about the middle of the perceptual range, so
  both halves meet smoothly at amount 0. Values already above white are
  brought toward it, never past their own value.
- *Exactness.* A weight of 0 returns the colour exactly, without a gain of one
  or a round trip through the perceptual coordinate, so the centre of a
  falloff is the input bit for bit. An amount of 0 is off whatever the shape,
  and the plan is the default; non-finite values are refused, values out of
  range clamped, as for every setting the maths reads (ADR 008).

**The grain seam.** `effectsPixel(plan, placement, column, row, colour)` is
the fixed order of the effects, mirrored by the shader's `effectsPixel`: the
vignette, then grain through `grainAt`, which picks the model. `EffectsPlan`
has a `GrainPlan` member and `active()` its flag; `EffectsPlacement` carries
the mapping and the grain's lattices placed on the render. Grain itself, its
seed and its band-limit are ADR 038; `GpuEffectsBlock` is 176 bytes with it.

## Consequences

- A vignette edit never invalidates the pointwise, geometry or resize
  checkpoints, and a crop, pan or zoom never invalidates the pointwise one.
- Parity: the effects pass alone agrees with the CPU to 1.0e-6 relative on
  lavapipe (darken, lighten, hard edge, full lift, with values below 0 and
  above 1, after regions, shrinks and a 2.5x enlargement). It is held to the
  pointwise tolerance (3e-5), whose powers are the same; after a resize, that
  plus the resample tolerance.
- Tests hold: the centre untouched and the corners darkest; darkening exactly
  an exposure gain in the perceptual coordinate (two stops is a quarter,
  exactly); lightening never above 1 and monotone; the same crop-frame pixel
  getting the same factor in a region at 1:1 and at 2x (to 1e-6); a preview
  level matching full resolution (1e-5 where the pyramid divides exactly);
  defaults bit-identical with no extra GPU pass; checkpoints at the new
  boundary refusing another vignette or region and resuming from the resize.
- The command line takes `--vignette-amount`, `--vignette-midpoint` and
  `--vignette-feather`; Python `VignetteSettings`, `EffectsSettings`,
  `DevelopSettings.effects` and the flat keys; the develop panel an Effects
  group with the three rows. Sidecar and JSON follow from the descriptor table
  (`arraw:` only).
- The vignette follows the shoulder, so nothing rolls off a darkened or
  lightened value afterwards; darkening cannot create overshoot and lightening
  is bounded by construction, so nothing new is clipped on export.
- A preview at an odd pyramid level shows the falloff shifted by up to a
  pixel of that level, since the reduced crop is floored (ADR 023); the
  difference is below the falloff's slope over a pixel.

## What was rejected, and why

- **Effects inside the pointwise chain**, mapping each source pixel through the
  inverse geometry. One pass fewer, but every crop or pan would redevelop the
  whole frame, and the preview's reduced source would sample grain coarsely.
- **Effects fused into the resize shaders.** Fastest, but four resize variants
  would grow an effects tail and an identity resize has no pass to carry it.
- **Lightening as an exposure gain followed by the shoulder again** compresses
  a highlight twice and does nothing with Filmic Highlights at 0; **accepting
  clipping**, as `main` did, loses the edges' highlights on export.
- **The frame mapping inside the effects block.** It would duplicate the
  resize block; keeping it derived means the block compares settings only and
  the mapping cannot drift from what the resize did.
