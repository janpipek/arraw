# Two anchors for geometry: the sensor frame and the composition frame

This ADR is written ahead of the code it describes. It exists because the
previous implementation is a worked example of what happens without it.

`main` names three coordinate frames, and only in a vertex shader: `vUV` (the
decoded buffer after the coarse-orientation remap), `vImageUV` (the upright
image, cropped and straightened, before that remap), and `vFrameUV` (normalised
to the final crop). It *uses* at least four more that are named only in prose —
sensor space for lens falloff, the corrected image buffer for Spots, the
rotated display frame for Crop, and the viewport for pan and zoom.

Every one of them is a `QPointF`:

```cpp
QPointF destination;                              // Spot.h:11 — buffer pixels
QPointF center{0.5, 0.5};                         // LocalAdjustment.h:30 — normalised
float spotWeight(const Spot&, QPointF px);
float maskWeight(const LinearMask&, QPointF uv, float aspect);
```

Four meanings, one type, no diagnostic. That is how `CONTEXT.md` came to assert
that Linear and Radial masks are "frame-anchored (fixed to the upright cropped
frame)" while `image.frag` evaluates them at `vImageUV` and anchors them to the
content. The documentation and the shader disagree, and nothing catches it.

## Decision

**Two frames anchor stored geometry. Everything else is derived.**

**The sensor frame** is the decoded buffer's own geometry: before lens
corrections, before orientation, before any user transform. Coordinates are
normalised to the decoded buffer's dimensions rather than expressed in pixels,
so that a preview computed at a quarter size addresses the same point as the
full-resolution export.

Everything the photographer places on the photograph is stored here — Spot
positions and radii, Brush rasters, Linear and Radial Mask geometry, and the
guide lines of a perspective correction. The test is whether the mark belongs
to the *subject*: a dust speck is on the sensor, not in the composition, and it
must not move when the frame is recropped, straightened, rotated, or when a
lens profile is changed or switched off.

Storing before lens correction rather than after is a deliberate choice, and
the more expensive one. The two frames differ by a smooth invertible warp, but
distortion moves pixels by percent-level amounts and a spot is a few pixels
wide, so the difference is visible. Paying it means a stroke painted on the
corrected image is inverse-warped on capture, and the warp must be tested in
both directions. What it buys is that editing a lens profile never disturbs an
edit made underneath it.

**The composition frame** is the result of the whole geometric chain — lens
corrections, perspective, straighten, orientation, crop — normalised 0 to 1
over the cropped rectangle. Nothing is *stored* here, because settings that
read it carry sizes rather than positions: post-crop vignette (its name says
where it lives), film grain, aspect. `main` reached the same conclusion, in
that `vFrameUV` feeds exactly the vignette and the grain.

**The crop rectangle is the one exception, and lives in the upright frame** —
the image after lens correction, perspective and straighten, but before the
crop itself. A crop is axis-aligned, and axis alignment only means anything
after rotation. `main` stores it the same way for the same reason.

**Frames are distinct types, not all `QPointF`.** `SensorPoint`,
`UprightPoint`, `CompositionPoint` and `ViewportPoint` convert only through
named functions. A function that wants normalised subject coordinates cannot be
handed viewport pixels, which is the single defect this ADR exists to prevent.

**A setting carrying a spatial size names its frame** in the descriptor table
of ADR 008. There are two legal answers, sensor and composition, and no
default: whoever adds a radius decides where it belongs and writes it down.

**Anything that determines a transform is stored upstream of it.** Perspective
guide lines produce a homography, so they are stored in the sensor frame and
the homography is derived. Stored downstream, adjusting one line would move the
others, and switching the correction off would strand them.

**No backend computes a transform from develop settings.** The chain —
composed matrices, warp coefficients, the crop rectangle and each frame's
dimensions — is computed once, on the CPU, as a plain value. The CPU reference
consumes it directly and the GPU uploads the same value as uniforms, which is
the layout ADR 007 wants anyway. What remains duplicated is the few lines that
*evaluate* a non-linear warp, in C++ and in GLSL, and those are held by
per-stage comparison tests rather than end-to-end ones alone.

`main` shows what the alternative costs. `image.vert` carries a comment above
its orientation remap describing itself as a "bit-exact mirror" of
`orient::orientedToBuffer`, asking the next reader to keep two implementations
in lock-step by hand. Generating one from the other was considered and deferred:
it removes the last duplication at the price of a build step, a generator, and
debugging through generated shaders, and it is only worth that once the
duplicated evaluation grows well beyond geometry.

**The viewport and the output are not storage frames.** Pan, zoom and the
requested export size belong to a render request, never to a document.

## Consequences

- **Rotation does not mutate the document.** `main` rewrites mask geometry when
  orientation changes — `rotateMaskQuarterTurns`, `flipMask` — because the
  geometry is stored in the upright frame. Stored in the sensor frame, a
  quarter turn is a change to the transform and nothing else, so it is exactly
  reversible and cannot accumulate error.
- **Every tool converts on input and output.** A mask handle dragged in the
  viewport travels viewport -> composition -> upright -> sensor before being
  stored. That chain has to exist and be tested in both directions, including
  for mirrored orientations, non-square pixels and odd dimensions.
- **Lens corrections must be invertible.** Storing before the warp means the
  forward warp renders and the inverse warp captures. A model that can only be
  evaluated in one direction needs a numerical inverse.
- **The chain runs whole-frame until the crop.** A crop keeping 10% of the
  frame still costs a full-frame lens correction and spot pass. Region
  evaluation can fix it later, for the stages that commute; the frames named
  here are what makes "which stages commute" answerable.
- **Sidecar compatibility is affected.** `crs:` Spot coordinates and Lightroom's
  crop are expressed in Adobe's own frames; the mapping to these two is a
  conversion, not a copy, and belongs with the sidecar work rather than here.

Still open: the tolerance at which the two backends are held to agree. "How
close is close enough" for a resampled pixel is not answerable without the code
and the fixtures in front of us, and a number invented now would only be a
number.
