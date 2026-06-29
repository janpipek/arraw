# arraw — Design Document

## Overview

A lightweight, cross-platform RAW photo editor with a Lightroom-style development
workflow. Focus: fast, non-destructive editing with a real-time GPU preview and
clean export. Not a DAM, not a cataloguing tool — just open a folder, edit, export.

---

## Goals

- Real-time GPU-accelerated preview: slider moves never block the UI
- Non-destructive: edits stored as XMP sidecar files alongside originals
- Cross-platform: Linux, macOS, Windows
- Minimal dependencies: Qt6 (RHI for the GPU), libraw, lcms2
- Clean codebase: one class per responsibility, no premature abstraction

---

## Target Stack

| Layer | Current | Notes |
|---|---|---|
| UI | Qt6 Widgets (≥ 6.8) + QRhiWidget | docs/adr/0006 |
| GPU | Qt RHI — platform default backend (Metal on macOS, D3D11 on Windows, OpenGL on Linux; Vulkan opt-in) | one Vulkan-GLSL shader source, `qsb`-compiled |
| RAW decode | libraw | — |
| Color mgmt | lcms2, linear Rec.2020 working space, soft-proofing, monitor ICC | — |
| Build | CMake + Ninja, pkg-config | — |

---

## Data Model

### `GlobalAdjustment`
Plain struct, zero-initialized = no adjustments. Serialises cleanly to XMP.

```
Tone:       exposure (EV, float), contrast, highlights, shadows, whites, blacks (-100..100)
Color:      temperature (Kelvin, 2000–12000), tint (-100..100),
            saturation, vibrance (-100..100)
Detail:     sharpening (0..100)
Geometry:   rotation (degrees, -45..45), cropRect (normalised 0..1 QRectF)
Effects:    vignetteAmount (-100..100), vignetteMidpoint, vignetteFeather,
            grainAmount, grainSize, grainRoughness (all 0..100),
            grainSeed (hidden per-image identity; 0 means uninitialised)
```

### `ImageBuffer`
```
data:   std::vector<float>  — interleaved RGB, linear-light Rec.2020, [0..1] nominal
width, height: int
```

The working color space is linear Rec.2020 everywhere (see `docs/adr/0001`):
libraw decodes into it directly (`output_color=8`), and `toWorkingSpaceBuffer()`
(ColorManagement) converts standard images / thumbnails into it, honouring
embedded ICC profiles and assuming sRGB when untagged.

### `LoadResult`
Returned by the background decode task:
```
fullRes:  ImageBuffer   — stored in memory, used only for export
preview:  ImageBuffer   — 1/4 res (half W × half H), box-filtered
error:    QString       — non-empty on failure
```

---

## Architecture

```
MainWindow (QMainWindow)
├── QMenuBar
│   ├── File: Open (Ctrl+O), Save Adjustments (Ctrl+S), Export (Ctrl+E), Quit
│   └── View: Reset Zoom (Ctrl+0)
│
├── FileBrowser (QDockWidget, left)          ← filename list, arrow-key navigation
│
├── ImageViewport (QRhiWidget, centre)       ← GPU preview, zoom/pan, crop overlay
│
└── AdjustmentDock (QDockWidget, right)
    ├── Histogram (custom QWidget)
    ├── WhiteBalance group
    │   ├── Preset QComboBox (As Shot / Daylight / Cloudy / Shade / Tungsten / Fluorescent / Flash)
    │   └── Temperature (K), Tint sliders
    ├── Tone group
    │   └── Exposure, Contrast, Highlights, Shadows, Whites, Blacks sliders
    ├── Color group
    │   └── Saturation, Vibrance sliders
    ├── Detail group
    │   └── Sharpening slider
    ├── Geometry group
    │   └── Rotation slider (±45°)
    ├── Effects group
    │   ├── Vignette: Amount, Midpoint, Feather sliders
    │   └── Grain: Amount, Size, Roughness sliders
    └── Reset All button
```

---

## Component Responsibilities

### `RawProcessor`
- Wraps libraw. Called only from background threads.
- Decodes RAW → linear float32 Rec.2020 buffer with camera white balance applied.
- Returns `LoadResult` containing `fullRes` + `preview` (via `downsample2x`).
- **Embedded preview first:** before the slow `unpack`/`dcraw_process`, the camera's
  embedded JPEG preview is extracted on the same file handle and dispatched to the
  UI via the `onEmbeddedPreview` callback, so an image appears almost immediately.
  Cameras embed full-resolution (20+ MP) previews, so it is downscaled to a
  ≤2048 px edge **before** the lcms working-space conversion — converting all of it
  would cost several seconds for an image the full decode replaces moments later.
- **Demosaic cost:** `dcraw_process` dominates the full-res load. libraw built with
  OpenMP multithreads it (the vcpkg `libraw[openmp]` feature on Windows; the system
  libraw on Linux/macOS). Set `ARRAW_TRACE` to print per-stage load timings (see
  `src/core/Trace.h`).

### `ImagePipeline`
- `GlobalAdjustment` and `ImageBuffer` struct definitions.
- `downsample2x()`: box-filter 2× downsample, thread-safe, no Qt dependency.

### `ColorManagement` (lcms2)
- `toWorkingSpaceBuffer()`: any QImage → linear Rec.2020 `ImageBuffer`,
  honouring the embedded ICC profile (sRGB fallback). Thread-safe.
- `toOutputImage()`: linear working-space float QImage → sRGB / Display P3 /
  Adobe RGB, 8-bit `RGB888` or 16-bit `RGBA64`, color-space tagged.
- All built-in profiles are synthesized from primaries — no `.icc` files shipped.
- `buildDisplayLut()`: bakes working→[proof→]display into a 33³ RGBA LUT for
  the shader. Indexed in sRGB-encoded coordinates (shadow precision); alpha =
  in-gamut flag, computed twice — lcms alarm-code check (cLUT printer
  profiles) plus an unclamped transform into the proof space (matrix-shaper
  profiles, where the alarm check never fires).
- `scanSystemProfiles()`: display-/output-class `.icc`/`.icm` files from the
  OS profile directories.

### `ProofingPanel`
- Soft-proofing controls under the adjustments column: profile (scan + browse),
  intent (Perceptual / Relative Colorimetric), black point compensation,
  gamut warning. View state only — persisted in QSettings, never in the XMP.
- `S` toggles proofing; the status bar shows "Proofing: <profile>" while on.
- The monitor profile (View → Monitor Profile) reuses the same LUT path with
  no proof profile in the chain; "sRGB (assume)" keeps the fast shader path.

### `RendererCore` (RHI)
- Owns every RHI resource: vertex/uniform buffers, samplers, the curve and
  display LUT textures, the preview/full-res image textures, pipelines per
  render-pass format.
- `record()` is the single place the shader pass is recorded (docs/adr/0006);
  the widget's on-screen paint, export, and the histogram samples are three
  callers of it. `renderOffscreen()` wraps it in an offscreen frame with a
  synchronous readback.
- Accepts images/LUTs before the QRhi exists; uploads ride the next pass.

### `ImageViewport` (QRhiWidget)
- View/crop/input logic; all drawing is delegated to `RendererCore` with the
  widget's render target. Holds preview + full-res image slots; full-res is
  used when zoom crosses the preview pixel threshold.
- The crop overlay and align grid are QPainter drawings on a transparent
  child widget (QRhiWidget content cannot be over-painted directly).
- Zoom: scroll wheel, 0.05×–32×. Pan: Alt+drag or middle-button drag.
- Crop mode: activated by `C` key. Renders darkened overlay outside crop rect.
  Corner/edge handles are draggable. Drag outside rect = rotate. `Enter` confirms,
  `Escape` cancels.
- Before/after: `\` key held → renders with zeroed `GlobalAdjustment`.
- Export: `renderToImage(const GlobalAdjustment&, const ImageBuffer& fullRes)`
  — renders full-res buffer through the shader pipeline into an offscreen
  float target with `displayEncode` off, reads back, returns a linear
  working-space `QImage` (`Format_RGBX32FPx4`) for the CPU output transform.

### `AdjustmentPanel`
- Emits `paramsChanged(GlobalAdjustment)` on any slider change.
- `setParams()` restores all sliders atomically with signals blocked (used by
  XMP load and undo/redo).
- WB preset combobox sets `temperature` + `tint` in Kelvin scale.

### `Histogram`
- Bins the shader-rendered sample from `ImageViewport::histogramsReady`.
  Log-scale, RGB channels overlaid. No adjustment math of its own.

### `XmpSidecar`
- `pathFor(rawPath)` → same dir, same base name, `.xmp` extension.
- `load()` → `GlobalAdjustment`. Returns defaults if file absent or unparseable.
- `save()` → writes XMP using Qt's `QXmlStreamWriter`. Uses `crs:` namespace
  (Adobe Camera Raw) for Lightroom-compatible field names.
  `crs:Temperature` is stored in absolute Kelvin (compatible with LR).

### `FileBrowser` (QDockWidget)
- Populated from the directory of the currently opened file.
- `QListWidget` of RAW filenames. Click or arrow keys trigger load.
- Remembers selection across folder changes via `QSettings`.

### `QUndoStack` (in MainWindow)
- One `AdjustmentCommand` per slider gesture (not per `valueChanged` signal).
- Commands coalesce via `mergeWith()` while the same slider is being dragged
  (matched by `id()` = slider index). A new undo step begins when a different
  slider is touched or the mouse is released.

---

## Threading Model

```
Main thread:   UI, QRhi (all GPU work), QUndoStack, XmpSidecar I/O
Worker thread: RawProcessor::load() via QtConcurrent::run
               (fullRes + preview produced in one task)
```

The `QFutureWatcher<LoadResult>` fires `finished()` on the main thread.
The QRhi is never accessed from the worker thread.

---

## Processing Pipeline (full order)

Every step that touches pixels, from file to screen/disk, in execution order.
The fragment-shader order is enforced by `main()` in `shaders/image.frag` —
that file is the source of truth; keep this list in sync with it.

**Load time — CPU, background thread, once per image**

```
1. Decode            RAW: libraw demosaic (camera WB, linear gamma, 16-bit,
                     output_color=8 → linear Rec.2020)
                     Standard images: QImage decode → toWorkingSpaceBuffer()
                     (embedded ICC honoured, sRGB assumed when untagged)
2. Exposure normalisation (RAW only)
                     gain so the 99.5th-percentile luma lands at 0.78
3. downsample2x      box filter → half-res preview for viewport + histogram
```

**Geometry — vertex shader (`image.vert`), per frame**

```
4. Crop              quad UVs remapped into params.cropRect
5. Rotation          aspect-corrected rotation around the image centre
                     (zoom/pan are applied to vertex positions, not pixels)
```

**Color — fragment shader (`image.frag`), per frame, linear Rec.2020 throughout**

```
 6. Base look        fixed S-curve + slight sat boost (u.baseLook; on for the
                     final image and export, off for interim embedded-preview
                     display and the before/after view)
 7. Basic Tone       256×17 CPU-generated LUT atlas (docs/adr/0033): gamma-domain
                     logistic Exposure, log-odds Contrast, endpoint-anchored
                     Shadows/Highlights, and clipping-point Blacks/Whites.
                     Row 0 is global; rows 1–16 serve Local Adjustments.
 8. Tone curves      256×1 LUT texture: luma curve first (scales RGB
                     proportionally, preserves hue), then per-channel R/G/B.
                     Applied on gamma-encoded values (docs/adr/0003), decoded
                     back to linear, and extrapolated above 1 by final slope.
 9. White balance    per-channel multiplicative gain in linear Rec.2020,
                     blackbody-derived from temperature (Kelvin) + tint and
                     computed CPU-side, so black stays black (docs/adr/0025)
10. HSL color mix    8 hue ranges, smoothstep-weighted hue/sat/lum shifts
11. Saturation       luma-preserving saturation scale
12. Vibrance         saturation boost weighted toward desaturated pixels
13. Local adjustments per-mask tone/colour edits (docs/adr/0010, 0033): each
                     tone row maps the incoming luminance and blends by analytic
                     Mask weight; remaining colour deltas use the same weight.
                     Single-pass, in array order. Skipped
                     by the curve-input and WB-picker readbacks (which return
                     earlier); included on screen, in export, and the panel
                     histogram
14. Vignette         centred elliptical falloff in crop-frame coordinates,
                     after local colour work. Amount maps -100..100 to -2..+2
                     EV at maximum falloff; Midpoint and Feather shape the
                     transition. Applied as a hue-preserving linear multiplier
15. Grain            final develop adjustment (docs/adr/0026). Working values
                     are temporarily passed through the sRGB transfer curve,
                     monochromatic zero-mean grain is added, then values are
                     decoded back to linear working space. The continuous noise
                     field is keyed by crop-frame position + a per-image seed,
                     so preview and export sample the same pattern
16. Encode           u.displayEncode on (screen):
                       u.useLut off — display transform: Rec.2020→sRGB matrix
                       + true piecewise sRGB curve (sRGB monitor assumed)
                       u.useLut on — 33³ LUT texture baked by lcms2
                       (soft-proofing and/or monitor ICC profile; LUT alpha
                       carries the in-gamut flag for the gamut warning)
                     u.displayEncode off (export): unbounded float linear
                       working space; bounded output encoding clips only after
                       the CPU output transform (docs/adr/0033)
                     Clipping overlay (u.clipWarn, on-screen only — forced off
                       for export and histogram readbacks): judged sRGB-relative
                       from the pre-clamp value, painted last so it wins over the
                       gamut warning. Any channel ≥1 → red, ≤0 → blue, red wins
                       ties (docs/adr/0009)
```

**Export only — CPU, after the offscreen readback (`MainWindow::exportFile`)**

```
19. Resize           linear-light float scale to the chosen dimensions
20. Output transform lcms2: working space → sRGB / Display P3 / Adobe RGB,
                     8-bit (RGB888) or 16-bit (RGBA64, TIFF only)
21. Sharpening       unsharp mask in encoded space (the Sharpen slider has no
                     preview effect — it is applied only here)
22. Save             JPEG/PNG/TIFF with the output ICC profile embedded
```

Histograms are exact: `ImageViewport::renderHistograms()` renders the preview
through the real shader into a small offscreen target (debounced on parameter
changes) and reads back two samples — the full pipeline (display transform,
`u.useLut` off, so the histogram is output-sRGB regardless of soft-proofing)
for the panel histogram, and a "stop after Basic Tone, gamma-encode" pass
(`u.curveInput`) for the histogram behind the tone curve (docs/adr/0004).

---

## Export Pipeline

1. User triggers export (Ctrl+E). Dialog: format, size, quality, sharpening,
   color profile (sRGB / Display P3 / Adobe RGB, remembered via QSettings),
   16-bit toggle (TIFF only).
2. `ImageViewport::renderToImage()` called on the main thread:
   - Upload `fullRes` as a float32 RGBA texture (temporary).
   - Render into an offscreen RGBA32F target at the cropped pixel size, in an
     offscreen RHI frame of its own.
   - Run the full shader pipeline (steps 4–16 above) with current
     `GlobalAdjustment`, `u.displayEncode` off.
   - Synchronous readback → `QImage(Format_RGBX32FPx4)`, linear working space,
     scaled to the requested output size while still linear.
3. `toOutputImage()` (ColorManagement, lcms2): working space → chosen output
   profile, 8- or 16-bit, tagged with the matching `QColorSpace` so the ICC
   profile is embedded on save.
4. Unsharp mask (sharpening slider, CPU, encoded space).
5. `QImage::save(path)` — JPEG, PNG, or TIFF.

---

## Keyboard Shortcuts

| Key | Action |
|---|---|
| `Ctrl+O` | Open file |
| `Ctrl+S` | Save XMP sidecar |
| `Ctrl+E` | Export |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / Redo |
| `Ctrl+0` | Reset zoom to fit |
| `←` / `→` | Previous / next file in folder |
| `C` | Enter crop mode |
| `Enter` | Confirm crop |
| `Escape` | Cancel crop |
| `\` (hold) | Before/after toggle |
| `S` | Toggle soft-proofing |
| `J` | Toggle clipping overlay (highlights + shadows) |
| `R` | Reset all adjustments |

---

## Persistence

- **XMP sidecar**: loaded automatically on file open, saved on `Ctrl+S`.
  No auto-save on every slider move.
- **Window state**: geometry, dock layout, last opened folder — via `QSettings`
  on close, restored on launch.
- **Color settings**: export profile + bit depth, soft-proofing configuration,
  monitor profile — via `QSettings` (view/export state, never in the sidecar).
- **Clipping overlay**: the two toggles (`view/clipHighlights`, `view/clipShadows`)
  — view state via `QSettings`, never in the sidecar (docs/adr/0009).

---

## MVP Feature Set

- [x] Open RAW file (CR2, CR3, NEF, ARW, DNG, RAF, ORF, RW2, PEF, SRW)
- [x] Background decode via QtConcurrent, UI stays responsive
- [x] Dual-res: preview (1/4) for viewport + histogram, full-res held for export
- [x] Real-time GPU preview: all adjustments as GLSL uniforms
- [x] Full-res texture swap when zoom crosses preview pixel threshold
- [x] Adjustment panel: WB presets + Kelvin temperature, tone, color, detail, geometry
- [x] Before/after toggle (`\`)
- [x] Crop + straighten (`C`, handles, rotation slider + drag gesture)
- [x] Undo/redo (`QUndoStack`, coalesced per slider drag)
- [x] XMP sidecar load/save (`Ctrl+S`)
- [x] WYSIWYG export via offscreen GPU readback; sRGB / Display P3 / Adobe RGB output
      with embedded ICC (JPEG/PNG/TIFF, 16-bit TIFF)
- [x] Filename list dock with arrow-key navigation
- [x] Window state persistence via `QSettings`

---

## Post-MVP Milestones

### Milestone 2 — Tone Curve ✅
- Curve editor widget: draggable control points on a histogram background.
- Per-channel (R/G/B/Luma) curves, ghost overlays, modified-channel indicators.
- 256-entry LUT uploaded as a 1D texture uniform; applied in gamma space.
- Stored in XMP as `crs:ToneCurvePV2012*` point lists.

### Milestone 3 — Horizontal Filmstrip ✅

The current `FileBrowser` is a vertical icon **grid** in a left-side dock
(`QListWidget`, square 128px icons, inline path edit). This milestone turns it
into a true Lightroom-style **horizontal filmstrip** docked at the **bottom**,
full width under the viewport, replacing the left dock. Adjustments stays right.

Design resolved 2026-06-14:

- **Layout**: single-row horizontal strip at the bottom, ~110px tall, resizable
  via the dock and collapsible via the existing View-menu toggle. Single
  selection drives the loaded image (no multi-select). Current item is centered
  and highlighted.
- **Architecture**: `QAbstractListModel` + `QListView` (flow left-to-right,
  no wrap) + a custom `QStyledItemDelegate`. The model exposes thumbnails as
  **`QImage`** (the delegate converts to `QPixmap` at paint time) so the model
  is testable headless under the offscreen platform.
- **Cells**: aspect-correct thumbnails at fixed strip height (landscape wider
  than portrait), no filename caption — filename via tooltip; current item drawn
  with a highlight border.
- **Ordering**: case-insensitive natural sort by filename (`IMG_2` before
  `IMG_10`), replacing today's `QDir::Name` lexical order.
- **Directory controls**: an "Open Folder…" action in the File menu plus a
  compact current-folder label/button at the strip's left edge — no inline path
  field in the strip.
- **Pure logic extracted to `arraw_core`** (TDD targets, no widget needed):
  natural-sort comparison, aspect-fit cell width from strip height + image size,
  and the center-scroll offset for the current item.
- **Thumbnails**: reuse the existing disk-backed `ThumbnailCache` (embedded RAW
  preview → 512px JPEG, keyed by path+size+mtime); the model becomes its
  consumer, requesting lazily for the visible range and emitting `dataChanged`
  when a thumbnail arrives. In-memory LRU and explicit cancellation are deferred
  (see Milestone 6 / future perf work) — not part of this milestone.

### Milestone 4 — Color Management (lcms2)

Design resolved 2026-06 — see `docs/adr/0001-linear-rec2020-working-space.md` and
`docs/adr/0002-shader-adjustments-cpu-encode.md`. Camera input profiles (DCP/ICC)
are explicitly out of scope.

**Phase A — correct transforms + wide-gamut export — ✅ implemented**
(now described by the Processing/Export Pipeline sections above and the
`ColorManagement` component)

**Phase B — soft-proofing + monitor profiles — ✅ implemented**
(see the `ColorManagement` and `ProofingPanel` components and pipeline step 16)

### Milestone 5 — Qt RHI Migration ✅

Implemented 2026-06 — see `docs/adr/0006-rhi-migration-single-renderer-core.md`.

- `QOpenGLWidget` → `QRhiWidget` (Qt floor 6.8 LTS); big-bang, raw GL deleted,
  ADR 0005 goldens gated the change (not regenerated — their tolerance absorbs
  backend variance).
- One shader source: `image.frag`/`.vert` rewritten to Vulkan-dialect GLSL
  (440, uniform block), baked via `qsb`/`qt6_add_shaders` into resources.
  Runtime shader loading and hot-reload are deleted.
- `RendererCore` records the pass for all three consumers — widget paint,
  `renderToImage()` (signature unchanged), histogram samples.
- Backend is the Qt platform default: Metal on macOS, D3D11 on Windows,
  OpenGL (via RHI) on Linux; Vulkan available through `QRhiWidget::setApi()`.
  No direct OpenGL dependency remains.

### Milestone 6 — Tile-based LOD
- At high zoom, stream tiles from full-res buffer rather than uploading entire texture.
- Tile cache with LRU eviction.
- Async tile upload via PBO (Pixel Buffer Objects).

### Milestone 7 — Local Adjustments ✅

Design resolved 2026-06-15; implemented 2026-06 (Linear + Radial v1). See
`docs/adr/0010` (parametric masks, arraw-native storage). Per-region develop
edits with a real-time preview — both mask types support numeric and on-image
(drag-handle) editing, render live and in export, persist to the arraw XMP
namespace, and sit on the shared undo stack.

- **Mask model — parametric ("described"), not painted.** A [[Local Adjustment]]
  is a [[Mask]] plus the tonal/colour delta subset (exposure, contrast,
  highlights, shadows, whites, blacks, temperature, tint, saturation, vibrance —
  no geometry, no tone curve). Masks are evaluated analytically inside
  `image.frag`, so the preview stays **single-pass**; a freehand brush (raster
  mask) is the documented Option-C extension, deferred.
- **Mask Types v1: Linear (graduated) + Radial (oval)**, placed by dragging
  handles on the image. Luminance-range and Colour-range masks are the next set.
- **Cap: 16 Local Adjustments per image** — a fixed array bound mirrored in
  `image.frag`, the `Ubuf` mirror in `RendererCore.h` (std140, byte-identical),
  and `RendererCore::fillUbuf()`. Raisable later; never lower it below counts a
  saved file already uses (extra masks would drop on load).
- **Coordinate frame:** masks store normalised coordinates in the cropped/rotated
  display frame (`docs/adr/0007-crop-after-rotation-display-frame`), so recropping
  does not slide masks around.
- **Live preview + undo:** rendered every frame (unlike export-only sharpening);
  add/move/delete a mask and its slider tweaks are `QUndoStack` steps (unlike the
  culling marks of `docs/adr/0008-culling-marks-in-develop-sidecar`).
- **Persistence:** arraw-native XMP namespace in the same sidecar. Global edits,
  rating, and label remain Lightroom-compatible; local edits are arraw-only.
- **Pure logic to `arraw_core`** (TDD): mask weight evaluation per type, the
  parametric-handle ↔ normalised-coordinate maths, and namespace load/save.

### Milestone 8 — Settings Propagation (single-target) ✅

Design resolved 2026-06-15; refined 2026-06-16 (grilling session). Move develop
settings between photos without a catalogue. Batch/multi-photo application is
explicitly **out of scope here** — see Milestone 10.

- **Develop Groups partition every global field.** Milestone 8 introduced White
  Balance, Tone, Tone Curve, Colour, HSL, Detail, and **Geometry** (rotation +
  crop + aspect-lock flag bundled); Milestone 9 adds **Effects**. The taxonomy
  mirrors the AdjustmentPanel groups and is exhaustive, so no visible field can
  silently fail to copy. Per-image state such as Local Adjustment masks and the
  hidden Grain seed is not transferred by a group.
- **Apply = replace wholesale.** Applying a group overwrites every field in it on
  the target with the source's values, *including resetting to defaults* when the
  source group is unedited. Makes `applyGroups(target, source, selection)` a pure
  field-copy with no special cases — the single code path behind copy/paste and
  preset apply.
- **Copy → Paste, both checklisted.** Copy opens a checklist and snapshots the
  chosen groups + their values into an in-memory, session-only clipboard (one
  slot, not the OS clipboard). Paste opens its own checklist **bounded by what was
  copied** (you may narrow, never widen). Applying is one `QUndoStack` step via
  the existing `AdjustmentCommand(before, after)`.
- **Develop Presets.** Save a named bundle through the same checklist; apply in
  one click (no apply-time checklist — narrowing happened at save). Stored as one
  **partial JSON** file per preset under `QStandardPaths::AppDataLocation/presets/`
  (only the saved groups are written; presence in the file *is* the active-group
  flag), listed in a top-level **Presets** menu. Not Lightroom preset files — a
  clean break from the `crs:` sidecar contract (internal convenience, not an
  interop surface). See `docs/adr/0023`.
- **UI:** Edit menu gains *Copy Settings…* (Ctrl+Shift+C) and *Paste Settings…*
  (Ctrl+Shift+V); a new Presets menu holds *Save Current Settings as Preset…*,
  *Manage Presets…*, and the saved-preset list.
- **Dropped from the original sketch:** *Paste Previous* (propagation is always
  explicit — no implicit "copy from the previous photo") and auto-apply-on-load.

### Milestone 9 — Develop Depth (new adjustments)

Design resolved 2026-06-15; Vignette and Grain controls resolved 2026-06-20.
Splits by pipeline cost.

- **Cheap & honest (in this milestone): Post-crop Vignette + Grain.** Per-pixel
  uniforms, no neighbour reads, and live preview through the existing main pass.
  They form an **Effects** panel after Geometry and an eighth Develop Group; the
  group copies the six visible controls but preserves each target image's hidden
  Grain seed. See `docs/adr/0026`.
  - **Vignette:** Amount -100..100 (negative darkens, positive lightens) maps to
    -2..+2 EV at maximum falloff. Midpoint and Feather are 0..100. Defaults are
    Amount 0, Midpoint 50, Feather 50. The centred ellipse is evaluated in the
    final cropped frame, not source-image UVs, so crop and rotation cannot shift
    or truncate it.
  - **Grain:** monochromatic and zero-mean. Amount 0..100 maps linearly to
    0..0.08 standard deviation in perceptually encoded values. Size 0..100 maps
    logarithmically to about 0.5..4 pixels per 2048 pixels of crop-edge length;
    Roughness 0..100 blends multiple noise scales from uniform/fine to irregular/
    clustered. Defaults are Amount 0, Size 50, Roughness 50.
  - **Stable pattern:** a continuous procedural noise field uses cropped-frame
    coordinates and a hidden per-image seed. The seed is generated on the first
    transition from an unseeded state to non-zero Amount, survives Amount returning
    to zero, and is cleared only by Reset All. Preview, repaint, reopen, thumbnails,
    and export therefore sample the same field; undersampling may soften fine Grain
    but never rearranges it.
  - **Persistence:** visible fields use Lightroom-compatible `crs:` attributes
    (`PostCropVignetteAmount`, `PostCropVignetteMidpoint`,
    `PostCropVignetteFeather`, `GrainAmount`, `GrainSize`, and `GrainFrequency`;
    Grain Roughness maps to Adobe's `GrainFrequency` name). The seed uses
    `arraw:GrainSeed`. Develop Presets serialize the six controls, never the seed.
- **Texture / Clarity / Dehaze (spatial global Detail controls): live preview via
  a shared context pass.** These controls affect the whole image, but each pixel
  needs neighbourhood context rather than only its own value. Texture uses
  small-neighbourhood luminance taps in the main shader; Clarity and the practical
  Dehaze approximation share a reduced-resolution blurred luminance context
  recorded by `RendererCore` before the main shader — see `docs/adr/0011`.
  Reduced-resolution context is faithful enough for these broad tune-by-eye
  controls (unlike fine sharpening, which stays export-only).
- **Colour Noise Reduction (own milestone, shipped): cached GPU chroma pre-pass.**
  Removes coloured high-ISO blotches by smoothing the unit-luma chroma ratio with
  a separable Gaussian at quarter-res, run as a cached multi-pass GPU pre-pass in
  `RendererCore` immediately before the main shader (the CPU version was abandoned
  as too slow). It samples the already-uploaded lens-corrected/spotted texture, so
  it sits **last** in the pipeline — safe because the chroma ratio is unchanged by
  the achromatic vignette gain and geometry-only spots. Luma is preserved exactly
  by construction; chroma stays faithful at quarter-res, so it needs no
  full-res-to-judge handling. The recompute is debounced (~200 ms after the slider
  settles). See `docs/adr/0034`.
- **Deferred, listed with their cost:** Luminance Noise Reduction (wants full-res
  to judge, destroys fine detail — closer to its own milestone), and profile-based
  Lens Corrections (needs a lens-profile database).

### Milestone 10 — Multi-select & Batch Operations

Design resolved 2026-06-18 (grilling session). Revisits the Milestone 3
**single-select** filmstrip decision deliberately. Scope: **batch paste + batch
export**. Batch culling marks deferred.

- **Multi-select model (LR-style):** last-clicked item = **active** (drives the
  viewport and is always part of the selection); `QListView` `ExtendedSelection`
  (Ctrl+click, Shift+click) = batch target. Active item has a visually distinct
  highlight from the rest of the selection. Navigation arrows move the active item
  and collapse the selection to it.
- **Batch paste:** *Paste Settings* applies to all selected files including the
  active one. At paste time, the before-state `GlobalAdjustment` for each
  non-active file is read synchronously from its XMP sidecar (files are tiny;
  synchronous is fine for normal selection sizes). All N sidecars are written
  immediately — auto-save on paste, not on slider moves. One
  `BatchAdjustmentCommand` on the undo stack with `setText("Paste Settings
  (N files)")`.
- **Batch export:** same format/quality/profile/size controls as single-file
  export, but the output path becomes a **directory** picker; each file is saved
  as `<basename>.<ext>`. Single-file export keeps its current "save as" dialog.
  A **modal progress dialog with cancel** covers the run; the same widget will
  serve future long operations.
- **Undo descriptions:** all `QUndoCommand` subclasses must have meaningful
  `setText()` so the Edit menu shows "Undo Paste Settings (6 files)" etc.
  Audit existing single-file commands at the same time.

### Milestone 11 — Headless CLI

Design resolved 2026-06-15 in `docs/adr/0022-headless-cli-batch-export.md`. A
windowless `arraw-cli` executable renders RAW → export with no GUI, reusing the
shipped pipeline so the CLI and app cannot diverge.

- **`arraw-cli <raw> [-o out.jpg]`** — decode (`RawProcessor::load`), restore the
  adjacent `.xmp` (`XmpSidecar::load`), render (`RendererCore::renderOffscreen`),
  save. No CPU re-implementation of the pipeline: the shader stays the one truth.
- **Library split:** `arraw_core` divides into **`arraw_engine`** (processing,
  QtGui-only — no Widgets) and the UI layer; CLI and tests link the engine.
- **One device bootstrap:** RHI creation moves out of `ImageViewport` into a
  reusable headless `QRhi` context; the widget and the CLI are two callers.
- **Runtime:** needs a GPU or software rasteriser (llvmpipe / `-platform
  offscreen`) — the path the offscreen goldens already use.

---

## Testing Strategy

Design resolved 2026-06. Framework: **Catch2 v3** via CMake `FetchContent`
(identical on all three platforms; float matchers suit image math; tags
partition fast unit tests from `[gpu]` goldens). Tests link against an
**`arraw_core` static library** — every source file except `main.cpp` — so
both phases share one build restructure. Suite runs locally via `ctest` /
`just test`; **no CI for now**.

**Phase 1 — numeric core ✅** (pure logic, no GPU):
- `computeCurveLUT`: property tests — endpoints pinned, monotonicity
  (Fritsch-Carlson), identity curve → identity LUT.
- `downsample2x`: exact box-filter averages on synthetic buffers; odd sizes.
- `XmpSidecar`: save→load round-trip on randomized params, **plus** a
  committed known-good `crs:` sidecar fixture and exact-string assertions on
  emitted fields (`crs:Temperature` in Kelvin, the rest -100..100) — the
  Lightroom-compat contract is tested, not aspirational. Round-trip alone
  cannot catch a matched reader/writer bug.
- `ColorManagement`: lcms2 output transforms asserted against exact known
  values (the CPU encode stage is deterministic — see ADR 0002).
- `RawProcessor`: one or two tiny committed DNG fixtures (hundreds of KB);
  assert buffer validity, preview = half W × half H, metadata fields.
- Param↔slider mappings and `kLuma*` constants matching `image.frag`.

**Phase 2 — golden images ✅** (GLSL pipeline, locked in before the
Milestone 5 RHI migration so the rework cannot silently change output):
- Tests call the real `ImageViewport::renderToImage()`; comparison policy,
  tolerances, and PFM golden format are in
  `docs/adr/0005-golden-image-tests-tolerance-policy.md`.
- Seven scenarios (`tests/test_GoldenImages.cpp`) over a synthetic
  gradient + color-bar scene; goldens live in `tests/fixtures/golden/`,
  regenerated with `ARRAW_UPDATE_GOLDENS=1 ./build/tests/arraw_tests "[golden]"`.
