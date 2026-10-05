# A render may be a region of the frame

A viewer that zooms and pans shows a part of the developed photograph at a
chosen scale. Until now a render was always the whole frame
([ADR 020](020-a-render-is-resized-after-geometry-in-one-separable-pass.md)),
so a zoomed view could only ask for a larger whole frame and crop it itself.

## Decision

**A `RenderRequest` has an optional `region`**: a rectangle in fractions
(`left`, `top`, `right`, `bottom`, each in `[0, 1]`) of the developed, cropped
frame. Edges must be finite, with `left < right` and `top < bottom`, else the
render is refused with `std::invalid_argument`. The region is snapped outward
to whole pixels, at least 1x1. The request's size then resolves against the
region's pixel size, with the same FitInside, Scale and upscale rules, so the
result is that part of the frame at that scale. A region covering the frame is
the same render as none.

**The region is cut after geometry, as part of the resize stage.** It is
defined on the geometry output because that is the frame the photographer sees
and crops: orientation, rotation and crop are already applied, so a region
means the same thing whatever produced the frame. It lives in `ResizePlan`
(`region` as a `PixelRegion`, and in its equality), so a geometry checkpoint
is valid for every region ([ADR 011](011-a-plan-spatial-passes-and-one-pointwise-chain.md),
[ADR 024](024-renders-stop-and-resume-at-pass-boundaries.md)). `stagesOf` and
`prefixMatches` need no change: the resize group already compares the whole
resize block. Panning redoes only the last stage.

**Both backends execute the plan's region.** The CPU cuts the region into a
new buffer and resamples it. The GPU does not copy: the horizontal resize
passes read the image at an offset (`GpuResizeBlock::offset`) and clamp tap
indices to the region's own length before adding it, with weights built for
the region's size. That is the same taps and the same edge rule as the CPU's
cut-then-resample, so the two agree to the existing resize tolerance, and one
path serves resize, crop and both together. A region that is only cut runs the
same passes with identity weights; on opaque pixels that is exact.

**The pyramid level accounts for the region.** `pyramidLevelFor` takes the
largest level whose region, in pixels at that level, still covers the size the
request resolves to. A closer view needs a finer level for the same output: a
quarter of the frame at the same box is one level finer.

**What is rendered is said by the engine.** `renderedRegion(request, frame)`
gives the region after snapping, as fractions of the frame it was cut from.
Snapping is outward and depends on the frame's size, so on a pyramid level it
lands on that level's coarser pixels. A caller placing the result on screen
uses this, not its own request. An edge within a millionth of a pixel of a
pixel boundary counts as on it, so a region given as fractions of whole pixels
renders exactly those pixels.

**The opaque hint is unchanged.** It is scanned from the source whenever the
resize does anything, a cut included.

## Not decided

- Python and the command line do not expose the region.
- A tone tick at close zoom still develops the whole level-0 frame, because
  the earlier stages know nothing of the region. Restricting them to the
  region's footprint (plus the geometry's and the kernel's margin) is future
  work, and is where the real saving at high zoom lies.
- A region at the edge snaps outward, so the result can be up to a pixel
  larger than the fraction asked for, and a pan by less than a pixel does not
  move it.

## Note, 2026-10-04

Effects run after the region is cut and resized
([ADR 037](037-effects-run-after-the-resize-and-the-vignette-follows-the-crop.md)).
Each output pixel's place in the cropped frame is worked out from `ResizePlan`
and the geometry's output size, so a region shows the falloff the whole frame
has at the same place.

## Note, 2026-10-05

Noise reduction runs on the whole source before geometry
([ADR 039](039-noise-reduction-is-the-first-pass-and-reads-the-as-shot-luminance.md)),
so a region is still the crop of the whole render, bit for bit, and needs no
margin. When the earlier stages are restricted to a region's footprint,
`denoiseReach(plan.denoise)` gives the margin in source pixels the Denoise
pass needs around it.
