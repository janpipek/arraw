# Orientation is a coarse, lossless develop edit on a native-orientation buffer

[[Orientation]] — the eight EXIF states (four 90° turns × an optional mirror) —
is modelled as a discrete develop edit kept **separate** from [[Rotation]] (the
continuous ±45° level). The decoded [[ImageBuffer]] stays in the file's *native*
sensor orientation; Orientation is applied downstream in the geometry layer as an
**exact** UV/aspect remap *before* the existing ±45° rotation, seeded from the
camera's EXIF tag on first load, and persisted as `tiff:Orientation` (1–8).

## Context

Two unrelated facts forced the decision. First, the glossary's **Rotation** was
*"the signed angle (±45°)"* — there was no coarse 90°/flip concept at all, and
folding 90° turns or mirrors into that continuous angle is impossible (a mirror
is not any rotation angle; a 70° tilt breaks the ±45° crop-inscribe assumption in
`maxInscribedCrop`). Second, decode was *inconsistent*: `RawProcessor` left
`user_flip` at libraw's default (-1), so RAW came out **already oriented**, while
`StandardImageLoader` never set `QImageReader::setAutoTransform`, so JPEGs came
out **sideways**. Any orientation feature had to unify those paths.

The pipeline promise is that Spot and Local-Adjustment geometry is anchored to
image content before [[Rotation]] or [[Crop]] are projected for display: Spots
use corrected-buffer pixels, and Local-Adjustment masks use normalised corrected-
image UVs (ADR 0010). Orientation had to fit that invariant rather than break it.

## Considered options

- **Develop edit on a native buffer, applied downstream (chosen).** Turn libraw
  auto-rotate **off** (`user_flip = 0`), leave the buffer native, read EXIF
  ourselves to *seed* an Orientation enum that lives in `GlobalAdjustment`, on the
  undo stack, in the sidecar — exactly like Rotation/Crop. RAW and JPEG take one
  path. Applied as an exact corner-permutation/negation of the quad UVs plus an
  aspect swap, *in front of* the unchanged ±45° shader rotation, so 90°/flip are
  bit-exact (no resampling). The buffer-space invariant holds: Spot/masks still
  see native pixels.

  **Correction (during implementation).** The "shader stays *untouched*" framing
  did not survive the pipeline. Crop and the ±45° rotation both run in the
  oriented display frame (`0007`), so Orientation must map the *final* oriented-
  frame UV → native buffer — it applies to `vUV` *after* the rotation, which can
  only happen in the shader. Permuting the quad's `aUV` instead fights the
  crop-in-oriented-frame rule, and a mirror cannot be folded into `cropRect`/
  `rotation`/`aspect`. So `image.vert` gains ~4 lines applying `orientedToBuffer`
  to `vUV`, plus two ints (`orientQuarterTurns`, `orientMirrored`) reusing the old
  `pad_[2]` slot in the std140 block (size unchanged, all offsets preserved). It
  is still exact and still the *only* arbiter of rotation/orientation; the CPU
  `viewport::Geometry` mirror applies the identical formula at the identical
  point, pinned by a buffer↔viewport round-trip test.
- **Bake orientation into the buffer at decode (rejected).** Keep libraw
  orienting RAW, add `setAutoTransform(true)` for JPEG, treat only the user's
  *extra* 90° turns as an edit. Simpler rendering, but splits "Orientation" across
  baked-EXIF + develop-delta, makes buffer space depend on EXIF (two files of one
  scene get differently-oriented buffers), and muddies the round-trip.
- **One unified continuous angle, ±180° (rejected).** A single float. Cannot
  represent a mirror without a bolted-on flag, breaks the ±45° crop-inscribe
  maths, and buys no more continuity than the chosen model (the ±45° fine angle
  already feels continuous within each quadrant).

## Consequences

- **Migration cost, accepted.** Switching `user_flip` to 0 makes existing RAW
  buffers native instead of pre-oriented. The *display* stays correct (a sidecar
  lacking `tiff:Orientation` falls back to the EXIF seed), but Spot/mask
  coordinates authored against the old oriented buffer now address the wrong
  pixels. Pre-1.0, this is judged acceptable: such edits may need redoing.
- **Seeding precedence:** absent `tiff:Orientation` ⇒ seed from EXIF; present ⇒
  use the stored value (the user edited it). This is also the migration path for
  old sidecars.
- **Compose order is Orientation → Rotation → Crop.** Crop stays stored in the
  oriented+rotated display frame (consistent with `0007`). Local-Adjustment masks
  store subject-space coordinates in the full oriented image frame, before Crop
  and fine Rotation (`0010`). A 90° turn or mirror rotates/mirrors both the
  committed crop and existing masks *with* the content (keeps the subject framed
  and masked), while fine Rotation rides underneath unchanged. The [[Aspect Ratio
  Lock]] swaps landscape↔portrait. A straighten that exposes the rotation's empty
  corners auto-refits the crop (shrink-only, centre + ratio preserved) on the
  *same* undo command.
- **Persistence:** Orientation as `tiff:Orientation` 1–8. Separately, Rotation's
  field is renamed `crs:StraightenAngle` → `crs:CropAngle` (the former does not
  exist in Adobe's schema; the latter is the real one). Full Adobe round-trip of
  `CropAngle` also needs `crs:HasCrop=true` — tracked as a follow-up, not done
  here.
- **Naming:** "Orientation" is reclaimed for the image; the crop tool's old
  "Flip Orientation" toggle is renamed [[Flip Aspect]] to end the collision.
- **Develop Group:** Orientation joins the **Geometry** group (`0023`); that group
  now ships *unchecked by default* in the copy/paste checklist (opt-in), since
  geometry rarely transfers between photos.
- A headless parity test pins `rotateTextureUv` to the shader output so the CPU
  mirror cannot silently diverge as Orientation is layered in.
