# Brush masks are buffer-anchored rasters sampled at the image coordinate

The [[Brush Mask]] is the third [[Mask Type]] (the Option-C extension deferred in
`0010-parametric-local-adjustments`), and the first one that is **raster, not
analytic**. ADR 0010 evaluates Linear/Radial masks as a few floats inside
`image.frag`; a freehand brush cannot be a handful of floats, so it reopens the
"raster masks" option 0010 rejected for v1 — but only for this one type, and on
terms that reuse the existing [[Spot]] machinery rather than the parametric one.

Two decisions fix the feature's shape. **Where the brush lives:** in
**corrected-buffer space** (the post-[[Lens Corrections]] decoded image, the same
space [[Spot]]s and the uploaded image texture live in), *not* the cropped
display frame the parametric masks use. **How it reaches the GPU:** as a single
`sampler2DArray` (one R8 layer per [[Local Adjustment]] slot), sampled in the
fragment shader at **`vUV`** — the coordinate the shader already uses to sample
the image texture.

Sampling at `vUV` means crop, [[Straighten]] ([[Rotation]]), and coarse
[[Orientation]] are *already baked into the coordinate*, so a brushed region
stays glued to the pixels it was painted on through every later geometry change —
the content-anchored behaviour a brush needs. This is the deliberate opposite of
Linear/Radial masks, which are anchored to the upright cropped *output* frame
(`vFrameUV`, rotation **not** applied) so a gradient "darkens the top of the
frame" regardless of content. The two anchorings are both correct for their
type: composition for the parametric shapes, content for the brush.

## Considered Options

- **Brush in the cropped display frame (`vFrameUV`), consistent with the
  parametric masks.** One coordinate convention for every mask type, trivial
  shader. Rejected: a baked raster in the output frame cannot re-derive aspect
  the way the analytic masks do, so an aspect-changing recrop *stretches* the
  painting, and a later [[Straighten]] slides it off the content — wrong for a
  brush, whose whole purpose is to stick to the subject.
- **Brush as a fourth parametric variant / GPU-painted into a render target.**
  Pushes the stroke math (dab falloff, spacing, flow, add/erase) into a shader
  where it cannot be headless-tested. Rejected against [[spot-for-algorithms]]:
  the stroke math is pure C++ that belongs in `arraw_core` under TDD; the GPU
  only ever *samples* the finished mask.
- **One bound 2D texture per mask.** A sampler per mask does not scale to the
  16-mask cap. Rejected for the `sampler2DArray` (one binding, layer = mask
  index).
- **Texture atlas (16 tiles in one 2D texture).** Avoids array textures but adds
  tile-offset UV math and edge-bleed seams. Rejected; `sampler2DArray` is the
  idiomatic RHI answer.
- **Raster stored as a separate sidecar file** (`<base>.arraw-masks` or PNG per
  mask). Keeps the `.xmp` small and deduplicates across [[Snapshot]]s by
  reference. Rejected for v1: it breaks the one-sidecar-travels-with-the-RAW
  invariant (ADR 0027) and needs reference/GC machinery for snapshot lifetime —
  cost without a v1 payoff.

## Consequences

- **Coordinate & resolution.** The brush bitmap is a single-channel (R8) raster
  in corrected-buffer UV space, a fixed **2048 px long edge** at the buffer
  aspect — decoupled from image resolution and from the crop. One buffer feeds
  both preview and full-res/export (bilinear upsample; mask edges are feathered,
  so it is invisible). A tight crop reduces effective mask resolution across the
  visible area; 2048 keeps that comfortable.
- **GPU delivery.** A `sampler2DArray` at a new binding, layer `i` ↔ local
  adjustment `i`, created lazily and sized to the brush-mask count. The mask-type
  tag (`laColor.z`, today 0=Linear 1=Radial) gains `2 = Brush`; `maskWeight()`'s
  new arm is `texture(uBrushMasks, vec3(vUV, layer)).r`. The single-pass shader
  invariant of ADR 0010 holds — a brush is one extra texture sample, not a
  compositing pass. The SRB is rebuilt on the existing generation counter when
  the array texture object changes.
- **Painting is CPU-side**, like [[Spot]] (ADR 0017): a stroke stamps circular
  dabs (size, feather, flow) along the cursor path into the R8 buffer; cursor→
  buffer uses the existing `ViewportGeometry::viewportToBufferPixel` inversion;
  the changed layer is uploaded (throttled during drag, final on release) for a
  live preview. Dab/spacing/flow/add-erase math lives in `arraw_core` for
  headless TDD.
- **Persistence is inline base64 PNG** (Grayscale8) in the `arraw:` namespace,
  one blob per brush mask, preserving the single-sidecar model. Soft/empty masks
  compress to a few KB. The blob is **duplicated into each [[Snapshot]]** that
  captures it (accepted: snapshots are few, blobs are small). [[Copy Settings]]/
  [[Develop Preset]]s exclude local masks as before, so no clipboard concern.
- **Undo.** One completed stroke (press→release) is one [[History]] step. The
  raster is held as an immutable `shared_ptr<const>` swapped per stroke, so
  `LocalAdjustment` keeps value semantics and cheap list copies; `operator==` is
  a pointer-identity proxy. The irreducible cost is one ~stroke-state raster per
  undoable stroke.
- **Brush controls (v1):** Size, Feather, Flow, and an Add/Erase toggle. A single
  Flow (no separate Density). Auto-mask (edge-aware), tablet pressure, and
  brush↔range-mask intersection are documented later extensions.
- **The brush shares the 16-mask cap** with Linear/Radial; no new cap to track.
