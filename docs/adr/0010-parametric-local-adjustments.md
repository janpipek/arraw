# Local adjustments are parametric masks stored in an arraw-native namespace

A [[Local Adjustment]] is a [[Mask]] plus a subset of develop deltas, applied to
one region of one image. Two questions decide the shape of the feature forever:
how the mask is represented, and how it is persisted. We chose **parametric
("described") masks** — Linear and Radial in v1, evaluated analytically inside
`image.frag` — stored under **arraw's own XMP namespace** in the existing develop
sidecar, capped at **16 per image**.

Because masks are evaluated in the fragment shader, the preview stays single-pass
(no per-edit compositing pass). The mask count is a fixed array bound mirrored
across `image.frag`, the `Ubuf` mirror in `RendererCore.h` (std140, byte-
identical), and `RendererCore::fillUbuf()` — the same discipline CLAUDE.md
already requires for uniforms. Masks store normalised coordinates in the
cropped/rotated display frame (`0007-crop-after-rotation-display-frame`) so
recropping does not move them. The deltas are the dodge/burn + colour-grade
subset only (exposure, contrast, highlights, shadows, whites, blacks,
temperature, tint, saturation, vibrance) — no geometry, no tone curve.

## Considered Options

- **Raster (painted) masks, multi-pass compositing.** Brush strokes are native
  and the edit count is unbounded, but it forces a render pass per local edit,
  CPU/GPU mask rasterisation, and resolution-tied mask storage. Rejected for v1
  in favour of the simpler parametric model; a brush layer is the documented
  Option-C extension on top of this, not a replacement for it.
- **Full Lightroom `crs:MaskGroupBasedCorrections`.** Local edits would round-trip
  to Lightroom, but that schema is deeply nested, GUID-bearing, and version-
  specific — the most fragile corner of the CRS spec to match. Rejected; the
  win does not justify the fragility for a RAW-first editor whose global edits,
  rating, and label already cover the Lightroom-compat promise.
- **Range masks (luminance/colour) in v1.** Cut to keep v1 to one interaction
  (drag handles on the image). They are the planned next mask types, and the
  parametric model already accommodates them.

## Consequences

- Local edits are **arraw-only**: Lightroom will not show them and we do not read
  Lightroom's. Global develop settings, rating, and label remain Lightroom-
  compatible as before. The boundary is explicit, consistent with
  `0007-culling-marks-in-develop-sidecar` (we do not round-trip foreign tags).
- The 16-mask cap is **raisable** (bump the synced constant, rebuild the shader)
  but must **never be lowered below counts a saved file already uses** — surplus
  masks would silently drop on load. The arraw-native sidecar stores a plain
  list, so reading old files after a raise is safe.
- Local adjustments preview live and sit on the develop `QUndoStack` (add / move /
  delete / slider tweak each a step), unlike culling marks which stay off it.
- Pure logic — per-type mask weight, handle ↔ normalised-coordinate maths,
  namespace load/save — extracts to `arraw_core` for headless TDD.
