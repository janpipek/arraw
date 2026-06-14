# Crop is axis-aligned in the rotated display frame (crop after rotation)

The crop rectangle is defined in the *rotated display frame* — the image the
user sees — and the shader applies rotation underneath it, so cropping happens
after rotation. The crop overlay maps to the viewport with fit+pan but **not**
rotation (`cropUVToViewport`), so an axis-aligned crop stays axis-aligned on
screen, and `cropInsideImage()` — which mirrors the rotation in `image.vert` —
keeps the crop within the real image so a rotation's empty corners can't be
cropped in.

## Context

`image.vert` maps the output quad into `cropRect` and then rotates to sample the
source (crop selects a region of the rotated frame). The original overlay code
instead treated `cropRect` corners as *source* coordinates and rotated them when
mapping to the viewport, so the drawn rectangle and the shader's crop diverged
as soon as rotation ≠ 0.

## Considered options

- **Dual coordinate space (rejected, was the prior implementation).** Edit the
  crop in viewport pixels while rotated (`activeCropViewport`) and convert back
  to unrotated UV on commit via an axis-aligned bounding box. The AABB round-trip
  is lossy and collapsed to a zero-pixel rectangle in some rotation/zoom combos.
- **Display-frame crop (chosen).** One coordinate space; the shader is the single
  arbiter of rotation. Removed `useViewportCrop`, `activeCropViewport`,
  `viewportCropToTextureCrop`, `textureCropToViewportBounds`,
  `rotatedImageViewportBounds`, and `applyCropDragViewport`.

## Consequences

- The overlay deliberately does **not** rotate while the rest of the pipeline
  does. A future reader must not "fix" `cropUVToViewport` by re-adding rotation —
  that reintroduces the original divergence.
- `cropInsideImage()` duplicates the rotation math from `image.vert`; the two
  must stay in sync (`rotateTexUV` is the shared CPU mirror).
- Straighten changes rotation without refitting an already-committed crop, so
  black edges can appear until crop is re-entered (which refits via
  `maxInscribedCrop`). Auto-refit on straighten is a possible future change.
