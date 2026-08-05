# arraw — Design Document

How arraw is built **today**: the layers, the components, the threading model, and
the exact order every pixel travels in. Companion documents:

- [CONTEXT.md](CONTEXT.md) — the domain glossary. Terms are defined there, not here.
- [docs/adr/](docs/adr/README.md) — *why* each hard-to-reverse choice was made,
  including the options rejected and what a reversal would cost.

---

## Overview

A lightweight, cross-platform RAW photo editor with a Lightroom-style development
workflow. Focus: fast, non-destructive editing with a real-time GPU preview and
clean export. Not a DAM, not a cataloguing tool — just open a folder, edit, export.

## Goals

- Real-time GPU-accelerated preview: slider moves never block the UI
- Non-destructive: edits stored as XMP sidecar files alongside originals
- Cross-platform: Linux, macOS, Windows
- Minimal dependencies: Qt6 (RHI for the GPU), libraw, lcms2, lensfun, exiv2
- Clean codebase: one class per responsibility, no premature abstraction

## Target Stack

| Layer | Current | Notes |
|---|---|---|
| UI | Qt6 Widgets (≥ 6.8) + QRhiWidget | [ADR 0006](docs/adr/0006-rhi-migration-single-renderer-core.md) |
| GPU | Qt RHI — platform default backend (Metal on macOS, D3D11 on Windows, OpenGL on Linux; Vulkan opt-in) | one Vulkan-GLSL shader source, `qsb`-compiled |
| RAW decode | libraw | demosaic selectable per image ([ADR 0036](docs/adr/0036-demosaic-selection-redecode-via-load-path.md)) |
| Colour mgmt | lcms2, linear Rec.2020 working space, soft-proofing, monitor ICC | — |
| Lens profiles | lensfun + embedded (DNG opcodes / maker notes) | optional dependency |
| Export metadata | exiv2 | optional dependency ([ADR 0043](docs/adr/0043-exported-metadata-exiv2-corrected-passthrough.md)) |
| Build | CMake + Ninja, pkg-config | — |

---

## Source Layout

`src/` is organised into seven dependency-ordered layers, with the application shell
(`main.cpp`, `MainWindow`, the current-image aggregate `DevelopSession`, the
decoded-buffer caches `DecodeCache`/`ThumbnailCache`, and the workflow/orchestration
helpers `ExportWorkflow`, `ImageLoadWorkflow`, `BatchPaste`, `MainWindowStatus`,
`MainWindowZoom`, `ChromeHider`) left at the `src/` root. The un-foldered root **is**
the application; each subdirectory is a layer it is built from
([ADR 0041](docs/adr/0041-layered-source-directories.md)):

| layer | holds | may depend on |
|---|---|---|
| `core/` | shared value types + pure geometry + dependency-free leaves (`ImageBuffer`, `WorkingSpace`, `ImageMetadata`, `CropGeometry`, `Orientation`, `Trace`, `ThemeColors`, `DisplayLut`, `NoiseReduction`, `AppIdentity`) | — |
| `develop/` | the adjustments model + model→GPU math (`GlobalAdjustment`, `DevelopParameter/Group/Preset`, `LocalAdjustment`, `BrushMask`, `Spot`, `Snapshot`, `DemosaicAlgorithm`, `FieldSpec`, `BasicTone`, `WhiteBalance`, `CurveLut`) | `core` |
| `pipeline/` | CPU pixel compute (`RawProcessor`, `ColorManagement`, `OkLab`, `LensCorrection`, `LoadResult`, the `ImagePipeline` free functions) | `core`, `develop` |
| `render/` | the GPU engine (`RendererCore`, `ViewportGeometry`, `OffscreenRender`, `HeadlessRenderContext`) — shaders live in repo-root `shaders/`. Sibling of `pipeline`, not above it | `core`, `develop` |
| `io/` | persistence (`XmpSidecar`, `PresetStore`) | `core`, `develop` |
| `cli/` | the `arraw` command front-end (dispatch, `export`, `preset`, `info`) | `core`, `develop`, `pipeline`, `render`, `io` |
| `ui/` | reusable widgets/panels/dialogs (`ImageViewport`, every `*Panel`, `FilmStrip*`, `Theme`, …) | all of the above |

**Includes are layer-qualified from the `src/` root** (`#include
"develop/GlobalAdjustment.h"`), so a file's include block is a dependency manifest
and a downward violation is greppable (e.g. `grep -rn '"ui/' src/pipeline` must
return nothing). One file per *concept* (a primary type carries its small
satellites); existing functional namespaces (`crop::`, `orient::`, `tone::`, …) mark
pure, headless modules. `MainWindow` and `ImageViewport` are known
internal-decomposition targets, deferred (ADR 0041).

### Build targets

- **`arraw_engine`** — decode, develop model, `RendererCore`, IO. No QtWidgets.
- **`arraw_ui`** — widgets, panels, `MainWindow`. Links `arraw_engine`.
- **`arraw_cli`** — command dispatch and the commands (CLI11, vendored in
  `vendor/CLI11/`). Links `arraw_engine` only.
- **`arraw`** — the front-end binary. On Windows it is console-subsystem and is
  joined by `arraw-gui` (GUI subsystem); on Linux/macOS it is the whole app.
  See [ADR 0049](docs/adr/0049-arraw-command-front-end.md).

---

## Data Model

### `GlobalAdjustment`

Plain struct; default-constructed = no adjustments. Serialises to XMP. The
authoritative field list with ranges and `crs:` mappings is
[`src/develop/GlobalAdjustment.h`](src/develop/GlobalAdjustment.h) — it carries a
comment per group and is the file to read. In outline:

```
Tone:        exposure (EV), contrast, highlights, shadows, whites, blacks (-100..100),
             filmicHighlights (0..100, default 25)
Tone curve:  curveLuma, curveR, curveG, curveB (control points in [0,1]²)
Colour:      temperature (Kelvin 2000..12000), tint, saturation, vibrance (-100..100)
HSL:         hslHue / hslSat / hslLum, 8 hue bands each (-100..100)
Black&White: convertToGrayscale (toggle) + bwMix, 8 hue bands (-100..100)
Colour grade: colorGradeHue[3] (degrees), colorGradeSat[3] (0..100),
             colorGradeBalance (-100..100), colorGradeBlending (0..100)
Detail:      demosaicAlgorithm (decode-time token), texture, clarity, dehaze
             (-100..100), sharpening (0..100), colorNoiseReduction +
             colorNoiseReductionSmoothness, luminanceNoiseReduction +
             luminanceNoiseReductionDetail (0..100)
Lens:        lensCorrectDistortion / lensCorrectVignetting / lensCorrectCA
             (toggles only — coefficients come from the profile)
Geometry:    orientation (coarse 90°/flip), rotation (-45..45), cropRect
             (normalised), cropConstrained
Effects:     postCropVignetteAmount/Midpoint/Feather, grainAmount/Size/Roughness,
             grainSeed (hidden per-image identity; 0 = uninitialised)
Per-image:   localAdjustments (≤16), spots
```

Tone, tint, saturation, and vibrance live in `SharedAdjustment`, shared with
`LocalAdjustment` ([ADR 0010](docs/adr/0010-parametric-local-adjustments.md)); a
local adjustment's temperature is a relative -100..100 shift rather than Kelvin.

### `ImageBuffer`

```
data:   std::vector<float>  — interleaved RGB, linear-light Rec.2020, [0..1] nominal
width, height: int
```

The working colour space is linear Rec.2020 everywhere
([ADR 0001](docs/adr/0001-linear-rec2020-working-space.md)): libraw decodes into it
directly (`output_color=8`), and `toWorkingSpaceBuffer()` (ColorManagement) converts
standard images / thumbnails into it, honouring embedded ICC profiles and assuming
sRGB when untagged.

### `LoadResult`

Returned by the background decode task:

```
fullRes:  ImageBuffer   — stored in memory, used for export and high-zoom viewing
preview:  ImageBuffer   — 1/4 res (half W × half H), box-filtered
error:    QString       — non-empty on failure
```

---

## Architecture

```
MainWindow (QMainWindow)
├── QMenuBar — File / Edit / Presets / View (Zoom, Monitor Profile) / Image
│                (Rating, Label) / Help
├── Tools toolbar (top) — Straighten, White-balance picker, crop + aspect controls
├── Collapsed-dock strips (left: History, right: Adjustments) — ADR 0012
├── Status bar — zoom dropdown, proofing state (MainWindowStatus)
│
├── History (QDockWidget, left)            ← session-only develop step list (ADR 0038)
│
├── ImageViewport (QRhiWidget, centre)     ← GPU preview, zoom/pan, crop + tool overlays
│
├── FilmStrip (QDockWidget, bottom)        ← folder thumbnails, multi-select, culling,
│                                            rating/colour filter (ADR 0018, 0042)
│
└── Adjustments (QDockWidget, right) — QTabWidget
    ├── "Adjustments" tab (scrolled column)
    │   ├── Histogram
    │   ├── Treatment switch (Colour | Black & White)
    │   ├── White Balance   — preset combo, Temperature (K), Tint
    │   ├── Tone            — Exposure, Contrast, Highlights, Shadows, Whites,
    │   │                     Blacks, Filmic Highlights
    │   ├── Tone Curve      — Luma + R/G/B channel curves over the curve-input histogram
    │   ├── Color           — Saturation, Vibrance          (hidden in B&W)
    │   ├── HSL / Color Mix — Hue / Saturation / Luminance, 8 bands (hidden in B&W)
    │   ├── Black & White   — 8-band mixer                  (shown only in B&W)
    │   ├── Colour Grading  — Shadows / Midtones / Highlights + Balance, Blending
    │   ├── Detail          — Demosaic; Texture, Clarity, Dehaze, Sharpening;
    │   │                     Luminance Noise; Color Noise
    │   ├── Geometry        — Orientation, Rotation, Crop
    │   ├── Lens Corrections— Distortion, Vignetting, Chromatic Aberration
    │   ├── Effects         — Post-Crop Vignette; Grain
    │   └── ProofingPanel   — profile, intent, BPC, gamut warning
    ├── "Masks" tab  — LocalAdjustmentPanel (Linear / Radial / Brush)
    ├── "Spots" tab  — SpotRemovalPanel
    └── "Info" tab   — editable User Metadata + read-only EXIF (ADR 0037)
```

`DevelopSession` — not a widget — owns the canonical state of the open image (path,
decoded buffers, develop parameters, user metadata, EXIF, sidecar status, dirty
baselines). Widgets mirror or edit that state through `MainWindow`; none of them is
the source of truth for "the current image"
([ADR 0020](docs/adr/0020-develop-session-active-image-state.md)).

### Data flow: open → display

1. `MainWindow` starts a `QtConcurrent::run` decode task keyed by path + demosaic
   algorithm, checking `DecodeCache` first (`decodeCacheKey`, ADR 0036).
2. On the worker thread, `decodeImage()` extracts the camera's **embedded JPEG
   preview** and fires `onEmbeddedPreview` so something appears almost immediately,
   then runs the full libraw decode → `normalizeExposure` → lens corrections →
   spots → `downsample2x`, producing a `LoadResult` (`fullRes` + `preview`).
3. `QFutureWatcher<LoadResult>::finished` fires on the main thread.
4. `resolveLoadedImage()` reads the `.xmp` sidecar and merges User Metadata by
   precedence (EXIF prefill ← embedded XMP packet ← sidecar), yielding the
   develop parameters, metadata, sidecar state, and snapshots.
5. `DevelopSession` takes ownership of that state; `MainWindow` pushes it out to
   `AdjustmentPanel::setParams()`, the Info panel, the film strip, and
   `ImageViewport`, which holds `preview` in its Preview slot and `fullRes` in the
   FullRes slot (uploaded lazily when zoom crosses the preview pixel threshold).
6. The viewport renders, then debounces a histogram sample; `histogramsReady`
   feeds the panel histogram and the tone-curve background.

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
- `downsample2x()`: box-filter 2× downsample, thread-safe, no Qt dependency.
- `normalizeExposure()`: scales the buffer so the 99.5th-percentile luma lands at
  0.78, with the gain clamped to 0.5–4.0×. RAW only.

### `LensCorrection`
- Resolves a lens profile from the embedded data (DNG opcodes / maker notes) or the
  lensfun database, matched on the EXIF lens identity, then applies distortion,
  vignetting, and chromatic-aberration correction CPU-side to produce the
  **corrected negative** every later stage develops on
  ([ADR 0032](docs/adr/0032-lens-corrections-cpu-corrected-negative.md)).
- Coefficients are re-derived on load; the sidecar records only which profile and
  source were used plus the three toggles.

### `ColorManagement` (lcms2)
- `toWorkingSpaceBuffer()`: any QImage → linear Rec.2020 `ImageBuffer`, honouring
  the embedded ICC profile (sRGB fallback). Thread-safe.
- `toOutputImage()`: linear working-space float QImage → sRGB / Display P3 /
  Adobe RGB, 8-bit `RGB888` or 16-bit `RGBA64`, colour-space tagged.
- All built-in profiles are synthesized from primaries — no `.icc` files shipped.
- `buildDisplayLut()`: bakes working→[proof→]display into a 33³ RGBA LUT for the
  shader. Indexed in sRGB-encoded coordinates (shadow precision); alpha = in-gamut
  flag, computed twice — lcms alarm-code check (cLUT printer profiles) plus an
  unclamped transform into the proof space (matrix-shaper profiles, where the alarm
  check never fires).
- `scanSystemProfiles()`: display-/output-class `.icc`/`.icm` files from the OS
  profile directories.

### `ProofingPanel`
- Soft-proofing controls under the adjustments column: profile (scan + browse),
  intent (Perceptual / Relative Colorimetric), black point compensation, gamut
  warning. View state only — persisted in QSettings, never in the XMP.
- `S` toggles proofing; the status bar shows "Proofing: <profile>" while on.
- The monitor profile (View → Monitor Profile) reuses the same LUT path with no
  proof profile in the chain; "sRGB (assume)" keeps the fast shader path.

### `RendererCore` (RHI)
- Owns every RHI resource: vertex/uniform buffers, samplers, the curve, Basic Tone,
  brush-mask and display LUT textures, the preview/full-res image textures, and
  pipelines per render-pass format.
- `record()` is the single place the main shader pass is recorded
  ([ADR 0006](docs/adr/0006-rhi-migration-single-renderer-core.md)); the widget's
  on-screen paint, export, and the histogram samples are three callers of it.
  `renderOffscreen()` wraps it in an offscreen frame with a readback.
- Also owns the **pre-passes** that run before the main shader: the unified,
  cached, debounced Noise Reduction pass ([ADR 0034](docs/adr/0034-colour-noise-reduction-gpu-chroma-pre-pass.md),
  [ADR 0046](docs/adr/0046-luminance-noise-reduction-unified-edge-aware-pre-pass.md))
  and the reduced-resolution blur context for Clarity/Dehaze
  ([ADR 0011](docs/adr/0011-preview-blur-pass-for-clarity.md)).
- Accepts images/LUTs before the QRhi exists; uploads ride the next pass.

### `ImageViewport` (QRhiWidget)
- View/crop/input logic; all drawing is delegated to `RendererCore` with the
  widget's render target. Holds preview + full-res image slots; full-res is used
  when zoom crosses the preview pixel threshold.
- The crop overlay, align grid, mask handles, spot circles, and brush cursor are
  QPainter drawings on a transparent child widget (QRhiWidget content cannot be
  over-painted directly).
- Zoom: scroll wheel, 0.05×–32×. Pan: Alt+drag or middle-button drag.
- Crop mode: activated by `C`. Renders a darkened overlay outside the crop rect.
  Corner/edge handles are draggable; dragging outside the rect rotates. `Enter`
  confirms, `Esc` cancels.
- Before/after: `\` held → renders with a default-constructed `GlobalAdjustment`.
- Histogram sampling: `renderHistograms()` renders the preview through the real
  shader into a small offscreen target, debounced, reading back asynchronously
  ([ADR 0035](docs/adr/0035-async-histogram-readback.md)).

### `offscreen::renderToImage` (`render/OffscreenRender`)
- The export render: uploads the full-res buffer, runs the full shader pipeline at
  the cropped pixel size with `displayEncode` off, and reads back a linear
  working-space `QImage` (`Format_RGBX32FPx4`) for the CPU output transform. Shared
  by the GUI, the CLI, and the golden tests — no widget or event loop required
  ([ADR 0049](docs/adr/0049-arraw-command-front-end.md)).

### `AdjustmentPanel`
- Emits `paramsChanged(GlobalAdjustment)` on any control change.
- `setParams()` restores all controls atomically with signals blocked (used by XMP
  load, undo/redo, snapshot restore, and preset apply).
- WB preset combobox sets `temperature` + `tint` in Kelvin scale.
- Hides the Color and HSL groups and reveals the B&W mixer under the Black & White
  treatment ([ADR 0048](docs/adr/0048-black-and-white-treatment-hue-mixer.md)).

### `Histogram`
- Bins the shader-rendered sample from `ImageViewport::histogramsReady`. Log-scale,
  RGB channels overlaid. No adjustment math of its own.

### `XmpSidecar`
- `pathFor(rawPath)` → same dir, same base name, `.xmp` extension.
- `load()` → `GlobalAdjustment` + user metadata. Returns defaults if the file is
  absent or unparseable.
- `save()` → merges into any existing sidecar **by property ownership**: arraw
  replaces the whole `arraw:` namespace, its modeled `crs:` properties,
  `xmp:Rating`/`xmp:Label`, and the five descriptive `dc:` properties; everything
  else is read first and written back untouched
  ([ADR 0027](docs/adr/0027-shared-xmp-property-ownership.md),
  [ADR 0037](docs/adr/0037-editable-dc-user-metadata-ownership.md)).
- `crs:Temperature` is stored in absolute Kelvin (compatible with Lightroom); other
  modeled fields use the internal -100..100 scale.

### `PresetStore`
- One partial arraw-native JSON file per Develop Preset under
  `QStandardPaths::AppDataLocation/presets/`; presence of a group in the file *is*
  the active-group flag ([ADR 0023](docs/adr/0023-develop-presets-partial-json.md)).
- Name is the sole identity, compared case-insensitively and post-sanitisation, so
  save/rename can prompt before an overwrite.

### `FilmStrip` (QDockWidget, bottom)
- `QAbstractListModel` + `QListView` (left-to-right, no wrap) + a custom
  `QStyledItemDelegate`. The model exposes thumbnails as `QImage` so it is testable
  headless under the offscreen platform.
- Natural (case-insensitive, numeric-aware) filename sort; aspect-correct cells at
  fixed strip height; format label chip; culling marks; EXIF tooltips backed by a
  JSON cache ([ADR 0019](docs/adr/0019-filmstrip-exif-tooltip-cache.md)).
- LR-style selection: the last-clicked item is **active** (drives the viewport and
  is always in the selection); Ctrl/Shift-click extends the batch target
  ([ADR 0018](docs/adr/0018-multi-select-batch-paste-export.md)).
- A rating + colour-label filter narrows which shots the strip shows
  ([ADR 0042](docs/adr/0042-film-strip-rating-and-colour-filter.md)).

### `DecodeCache` / `ThumbnailCache`
- `ThumbnailCache`: disk-backed, keyed by path+size+mtime — the embedded RAW preview
  developed to a small JPEG for the strip.
- `DecodeCache`: in-session cache of decoded buffers so revisiting a neighbour in
  the folder skips the libraw round-trip
  ([ADR 0024](docs/adr/0024-developed-thumbnail-and-decode-caches.md)).

### `QUndoStack` (in MainWindow)
- One `AdjustmentCommand` per slider gesture (not per `valueChanged` signal).
- Commands coalesce via `mergeWith()` while the same slider is being dragged
  (matched by `id()` = slider index). A new undo step begins when a different
  slider is touched or the mouse is released.
- Every `QUndoCommand` subclass sets meaningful text, so the Edit menu and the
  History dock read as "Paste Settings (6 files)".
- The History dock is this stack's visible face — session-only, reset on load
  ([ADR 0038](docs/adr/0038-snapshots-arraw-native-history-session-only.md)).

---

## Threading Model

```
Main thread:   UI, QRhi (all GPU work), QUndoStack, XmpSidecar I/O
Worker thread: RawProcessor::load() via QtConcurrent::run
               (fullRes + preview produced in one task)
               plus the export and developed-thumbnail CPU tail (ADR 0045)
```

The `QFutureWatcher<LoadResult>` fires `finished()` on the main thread. The QRhi is
never accessed from a worker thread: the GPU render stays on the main thread and
only the CPU work either side of it (decode; output transform, sharpening, encode)
is offloaded ([ADR 0045](docs/adr/0045-async-export-and-thumbnail-cpu-tail.md)).

---

## Processing Pipeline (full order)

Every step that touches pixels, from file to screen/disk, in execution order. The
fragment-shader order is enforced by `main()` in `shaders/image.frag` — **that file
is the source of truth; keep this list in sync with it.**

**Load time — CPU, background thread, once per image**

```
1. Decode            RAW: libraw demosaic (camera WB, linear gamma, 16-bit,
                     output_color=8 → linear Rec.2020). The demosaic algorithm is
                     a per-image develop field, so changing it re-runs this step
                     (docs/adr/0036)
                     Standard images: QImage decode → toWorkingSpaceBuffer()
                     (embedded ICC honoured, sRGB assumed when untagged)
2. Exposure normalisation (RAW only)
                     gain so the 99.5th-percentile luma lands at 0.78 (clamped
                     to 0.5–4.0×)
3. Lens corrections  distortion / vignetting / CA from the lens profile, applied
                     CPU-side to produce the corrected negative (docs/adr/0032)
4. Spots             clone-based pixel replacement on the corrected buffer
                     (docs/adr/0017)
5. downsample2x      box filter → half-res preview for viewport + histogram
```

**Pre-passes — GPU, recorded by `RendererCore` before the main shader**

```
6. Noise Reduction   one cached, debounced pass decomposing each pixel into luma Y
                     and unit-luma chroma ratio r = c/Y: an edge-aware bilateral
                     smooths Y (Luminance NR) and a separable Gaussian smooths r
                     (Colour NR), each preserving the other axis exactly. Samples
                     the already-uploaded corrected/spotted texture, so it sits
                     last before the main shader (docs/adr/0034, 0046)
7. Blur context      reduced-resolution blurred luminance for Clarity and Dehaze
                     (docs/adr/0011)
```

**Geometry — vertex shader (`image.vert`), per frame**

Listed in the order a photographer sees them; the shader computes the same
composition in the reverse direction, mapping a display-frame UV back to a buffer
UV to sample:

```
8. Orientation       coarse 90° quarter-turns + mirroring define the upright
                     display frame Crop and Rotation work in. Lossless — the
                     shader maps the oriented UV back to native buffer
                     coordinates, bit-exactly mirroring orient::orientedToBuffer
                     so the CPU overlays stay in lock-step (docs/adr/0029)
9. Crop              quad UVs remapped into u.cropRect
10. Rotation         aspect-corrected rotation around the image centre, applied
                     inside the crop frame — crop is axis-aligned in the rotated
                     display frame (docs/adr/0007). Zoom/pan are applied to vertex
                     positions, not pixels
```

**Colour — fragment shader (`image.frag`), per frame, linear Rec.2020 throughout**

```
11. Base look        fixed S-curve + slight sat boost (u.baseLook; on for the
                     final image and export, off for interim embedded-preview
                     display and the before/after view)
12. Basic Tone       256×17 CPU-generated LUT atlas (docs/adr/0033): gamma-domain
                     logistic Exposure, log-odds Contrast, endpoint-anchored
                     Shadows/Highlights, and clipping-point Blacks/Whites.
                     Row 0 is global; rows 1–16 serve Local Adjustments
13. Tone curves      256×1 LUT texture: luma curve first (scales RGB
                     proportionally, preserves hue), then per-channel R/G/B.
                     Applied on gamma-encoded values (docs/adr/0003), decoded
                     back to linear, and extrapolated above 1 by final slope
14. White balance    per-channel multiplicative gain in linear Rec.2020,
                     blackbody-derived from temperature (Kelvin) + tint and
                     computed CPU-side, so black stays black (docs/adr/0025)
15. Treatment branch Colour: HSL colour mix (8 hue ranges, smoothstep-weighted
                       hue/sat/lum shifts) → Saturation (luma-preserving scale in
                       Oklab) → Vibrance (weighted toward desaturated pixels)
                       (docs/adr/0039)
                     Black & White: the 8-band hue mixer weights each original hue
                       into its own grey, by saturation, so neutrals never shift
                       (docs/adr/0048). Runs right after White Balance, so the mix
                       responds to Temperature/Tint
16. Colour grading   three-zone (Shadows/Midtones/Highlights) hue+saturation
                     tint in Oklab (docs/adr/0052), after the Colour/Black &
                     White branch merges so it tints a colour image or the B&W
                     neutral grey. Perceptually-encoded luminance picks a
                     normalised zone blend (Balance shifts the crossover,
                     Blending widens the transitions); each zone adds an Oklab
                     a/b offset, holding lightness fixed. Zero saturation is the
                     exact identity
17. Spatial global   Texture (small-neighbourhood luminance taps) plus Clarity and
                     Dehaze, which read the step-7 blur context (docs/adr/0011)
18. Local adjustments per-mask tone/colour edits (docs/adr/0010, 0033): each
                     tone row maps the incoming luminance and blends by Mask
                     weight — analytic for Linear/Radial, a sampled raster layer
                     for the Brush (docs/adr/0047) — and remaining colour deltas
                     use the same weight. Single-pass, in array order. Colour
                     deltas are suppressed under the Black & White treatment.
                     Skipped by the curve-input and WB-picker readbacks (which
                     return earlier); included on screen, in export, and the panel
                     histogram
19. Post-crop vignette
                     centred elliptical falloff in crop-frame coordinates, after
                     local colour work. Amount maps -100..100 to -2..+2 EV at
                     maximum falloff; Midpoint and Feather shape the transition.
                     Applied as a hue-preserving linear multiplier
20. Grain            docs/adr/0026. Working values are temporarily passed through
                     the sRGB transfer curve, monochromatic zero-mean grain is
                     added, then values are decoded back to linear working space.
                     The continuous noise field is keyed by crop-frame position +
                     a per-image seed, so preview and export sample the same
                     pattern
21. Filmic highlights
                     the last develop step (docs/adr/0040), in the shared chain
                     before the display/export fork so preview and export agree:
                     the upper range — including recoverable headroom from every
                     upstream control, Local Adjustments included — is compressed
                     smoothly into range with a shoulder, and bright saturated
                     colour fades toward white. 0 restores the hard digital clip
22. Encode           u.displayEncode on (screen):
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
                     Sensor-clip overlay (u.sensorClipWarn): magenta where the
                       sensor-clip texture marks a channel saturated at capture
                     Mask overlay (u.maskOverlay, on-screen only): tints the mask
                       being edited red so it is visible while placing or
                       painting it (docs/adr/0047)
```

**Export only — CPU, after the offscreen readback**

```
23. Resize           linear-light float scale to the chosen dimensions
24. Output transform lcms2: working space → sRGB / Display P3 / Adobe RGB,
                     8-bit (RGB888) or 16-bit (RGBA64, TIFF only)
25. Sharpening       unsharp mask in encoded space (the Sharpen slider has no
                     preview effect — it is applied only here)
26. Save             JPEG/PNG/TIFF with the output ICC profile embedded
27. Metadata embed   exiv2 writes the selected metadata groups into the encoded
                     file: corrected capture-EXIF passthrough, GPS, and the
                     descriptive User Metadata XMP. Best-effort — a metadata
                     failure never costs the rendered image (docs/adr/0043)
```

Histograms are exact: `ImageViewport::renderHistograms()` renders the preview
through the real shader into a small offscreen target (debounced on parameter
changes) and reads back two samples — the full pipeline (display transform,
`u.useLut` off, so the histogram is output-sRGB regardless of soft-proofing) for the
panel histogram, and a "stop after Basic Tone, gamma-encode" pass (`u.curveInput`)
for the histogram behind the tone curve (docs/adr/0004). The readback is
asynchronous and recorded into the widget's own frame (docs/adr/0035).

---

## Export Pipeline

1. User triggers export (Ctrl+E). Dialog: format, size, quality, sharpening,
   colour profile (sRGB / Display P3 / Adobe RGB, remembered via QSettings), 16-bit
   toggle (TIFF only), and the three metadata groups (capture info on, location
   off, descriptive on).
2. `offscreen::renderToImage()` on the main thread:
   - Upload `fullRes` as a float32 RGBA texture (temporary).
   - Render into an offscreen RGBA32F target at the cropped pixel size, in an
     offscreen RHI frame of its own.
   - Run the full shader pipeline (steps 6–21 above) with the current
     `GlobalAdjustment`, `u.displayEncode` off.
   - Readback → `QImage(Format_RGBX32FPx4)`, linear working space, scaled to the
     requested output size while still linear.
3. `toOutputImage()` (ColorManagement, lcms2): working space → chosen output
   profile, 8- or 16-bit, tagged with the matching `QColorSpace` so the ICC profile
   is embedded on save.
4. Unsharp mask (sharpening slider, CPU, encoded space).
5. `QImage::save(path)` — JPEG, PNG, or TIFF.
6. `embedExportMetadata()` (exiv2) writes the selected metadata groups.

Steps 3–6 are the CPU tail and run off the GUI thread (docs/adr/0045). Batch export
takes a directory instead of a filename and runs the same path per file behind a
modal progress dialog with cancel. `arraw export` is the same pipeline with no
window ([ADR 0049](docs/adr/0049-arraw-command-front-end.md)).

---

## Persistence

- **XMP sidecar**: loaded automatically on file open, saved on `Ctrl+S`. No
  auto-save on every slider move — but culling marks, User Metadata edits, and
  batch paste write immediately (docs/adr/0008, 0037, 0018).
- **Develop Presets**: partial JSON under `AppDataLocation/presets/`
  (docs/adr/0023).
- **Snapshots**: per-image, in the `arraw:` namespace of the sidecar
  (docs/adr/0038). **History** is session-only and never persisted.
- **Window state**: geometry, dock layout, last opened folder — via `QSettings` on
  close, restored on launch.
- **Colour settings**: export profile + bit depth, soft-proofing configuration,
  monitor profile — via `QSettings` (view/export state, never in the sidecar).
- **Clipping overlay**: the two toggles (`view/clipHighlights`,
  `view/clipShadows`) — view state via `QSettings`, never in the sidecar
  (docs/adr/0009).

Keyboard shortcuts are documented in [docs/keybindings.md](docs/keybindings.md).

---

## Testing Strategy

Framework: **Catch2 v3** via CMake `FetchContent` (identical on all three
platforms; float matchers suit image math; tags partition fast unit tests from
`[gpu]` goldens). Tests link the `arraw_cli` and `arraw_ui` static libraries — every
source file except the `main.cpp` entry points. Run via `just test` or
`ctest --test-dir build --output-on-failure`.

There is **no test CI**: the two GitHub Actions workflows (`release.yml`,
`windows-package.yml`) are manually dispatched packaging jobs, so the suite is a
local gate.

**Numeric core** (pure logic, no GPU):
- `computeCurveLUT`: property tests — endpoints pinned, monotonicity
  (Fritsch-Carlson), identity curve → identity LUT.
- `downsample2x`: exact box-filter averages on synthetic buffers; odd sizes.
- `XmpSidecar`: save→load round-trip on randomized params, **plus** a committed
  known-good `crs:` sidecar fixture and exact-string assertions on emitted fields
  (`crs:Temperature` in Kelvin, the rest -100..100) — the Lightroom-compat contract
  is tested, not aspirational. Round-trip alone cannot catch a matched
  reader/writer bug. Foreign-property survival is asserted the same way
  (docs/adr/0027).
- `ColorManagement`: lcms2 output transforms asserted against exact known values
  (the CPU encode stage is deterministic — see docs/adr/0002).
- `RawProcessor`: one or two tiny committed DNG fixtures (hundreds of KB); assert
  buffer validity, preview = half W × half H, metadata fields.
- Param↔slider mappings and `kLuma*` constants matching `image.frag`.
- The CLI argument parsers and output contract (docs/adr/0050).

**Golden images** (GLSL pipeline), locked in before the RHI migration so the
rework could not silently change output:
- Tests call the real headless `offscreen::renderToImage()`; comparison policy,
  tolerances, and PFM golden format are in
  [ADR 0005](docs/adr/0005-golden-image-tests-tolerance-policy.md).
- Scenarios in `tests/test_GoldenImages.cpp` over a synthetic gradient + colour-bar
  scene; goldens live in `tests/fixtures/golden/`, regenerated with
  `ARRAW_UPDATE_GOLDENS=1 ./build/tests/arraw_tests "[golden]"`.
- They are **skipped without a GPU**, so shader-only regressions (notably a uniform
  block mismatch) need a manual smoke-run — see [AGENTS.md](AGENTS.md#shaders).

Test fixtures live in `tests/fixtures/`; the DNG is generated by `make_test_dng.py`.

---

## Future work

- **Tile-based LOD.** At high zoom, stream tiles from the full-res buffer rather
  than uploading the entire texture: a tile cache with LRU eviction and async
  upload. Today the full-res texture is uploaded whole when zoom crosses the
  preview pixel threshold.
- **Luminance/Colour range Mask Types**, the next parametric set after Linear,
  Radial, and Brush (docs/adr/0010, 0047).
- **macOS packaging** — see [docs/distribution.md](docs/distribution.md).
