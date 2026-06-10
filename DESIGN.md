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
- Minimal dependencies: Qt6, libraw, OpenGL
- Clean codebase: one class per responsibility, no premature abstraction

---

## Target Stack

| Layer | MVP | Future |
|---|---|---|
| UI | Qt6 Widgets + QOpenGLWidget | Migrate to QRhiWidget (Metal/Vulkan/D3D native) |
| GPU | OpenGL 3.3 core + GLSL | Qt RHI shaders via `qsb` |
| RAW decode | libraw | — |
| Color mgmt | `QColorSpace::SRgb` tag | lcms2 (wide-gamut, print profiles) |
| Build | CMake + Ninja, pkg-config | — |

---

## Data Model

### `AdjustmentParams`
Plain struct, zero-initialized = no adjustments. Serialises cleanly to XMP.

```
Tone:       exposure (EV, float), contrast, highlights, shadows, whites, blacks (-100..100)
Color:      temperature (Kelvin, 2000–12000), tint (-100..100),
            saturation, vibrance (-100..100)
Detail:     sharpening (0..100)
Geometry:   rotation (degrees, -45..45), cropRect (normalised 0..1 QRectF)
```

### `ImageBuffer`
```
data:   std::vector<float>  — interleaved RGB, linear light, [0..1] nominal
width, height: int
```

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
├── ImageViewport (QOpenGLWidget, centre)    ← GPU preview, zoom/pan, crop overlay
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
    └── Reset All button
```

---

## Component Responsibilities

### `RawProcessor`
- Wraps libraw. Called only from background threads.
- Decodes RAW → linear float32 RGB buffer with camera white balance applied.
- Returns `LoadResult` containing `fullRes` + `preview` (via `downsample2x`).

### `ImagePipeline`
- `AdjustmentParams` and `ImageBuffer` struct definitions.
- `downsample2x()`: box-filter 2× downsample, thread-safe, no Qt dependency.

### `ImageViewport` (QOpenGLWidget + QOpenGLFunctions_3_3_Core)
- Holds two textures: `m_previewTex` (always uploaded) and `m_fullResTex` (uploaded
  lazily when zoom crosses the preview pixel threshold).
- `paintGL` binds the appropriate texture and uploads adjustment uniforms each frame.
- Zoom: scroll wheel, 0.05×–32×. Pan: Alt+drag or middle-button drag.
- Crop mode: activated by `C` key. Renders darkened overlay outside crop rect.
  Corner/edge handles are draggable. Drag outside rect = rotate. `Enter` confirms,
  `Escape` cancels.
- Before/after: `\` key held → renders with zeroed `AdjustmentParams`.
- Export: `renderToImage(const AdjustmentParams&, const ImageBuffer& fullRes)`
  — renders full-res buffer through the shader pipeline into an offscreen FBO,
  calls `glReadPixels`, returns a `QImage` tagged `QColorSpace::SRgb`.

### `AdjustmentPanel`
- Emits `paramsChanged(AdjustmentParams)` on any slider change.
- `setParams()` restores all sliders atomically with signals blocked (used by
  XMP load and undo/redo).
- WB preset combobox sets `temperature` + `tint` in Kelvin scale.

### `Histogram`
- Bins the shader-rendered sample from `ImageViewport::histogramsReady`.
  Log-scale, RGB channels overlaid. No adjustment math of its own.

### `XmpSidecar`
- `pathFor(rawPath)` → same dir, same base name, `.xmp` extension.
- `load()` → `AdjustmentParams`. Returns defaults if file absent or unparseable.
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
Main thread:   UI, GL context, QUndoStack, XmpSidecar I/O
Worker thread: RawProcessor::load() via QtConcurrent::run
               (fullRes + preview produced in one task)
```

The `QFutureWatcher<LoadResult>` fires `finished()` on the main thread.
The GL context is never accessed from the worker thread.

---

## Processing Pipeline (full order)

Every step that touches pixels, from file to screen/disk, in execution order.
The fragment-shader order is enforced by `main()` in `shaders/image.frag` —
that file is the source of truth; keep this list in sync with it.

**Load time — CPU, background thread, once per image**

```
1. Decode            RAW: libraw demosaic (camera WB, linear gamma, 16-bit)
                     Standard images: QImage decode → srgbToLinearBuffer()
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

**Color — fragment shader (`image.frag`), per frame, linear light throughout**

```
 6. Base look        fixed S-curve + slight sat boost (uBaseLook; on for the
                     final image and export, off for interim embedded-preview
                     display and the before/after view)
 7. Exposure         pow(2, uExposure) multiply
 8. Contrast         linear scale around 0.5
 9. Tone regions     highlights, shadows, whites, blacks — luma-masked
                     (smoothstep ramps), applied as one combined luma delta
10. Tone curves      256×1 LUT texture: luma curve first (scales RGB
                     proportionally, preserves hue), then per-channel R/G/B.
                     Applied on gamma-encoded values (docs/adr/0001), then
                     decoded back to linear
11. Temperature      Kelvin → red/blue shift relative to 5500K neutral
12. Tint             green axis shift
13. HSL color mix    8 hue ranges, smoothstep-weighted hue/sat/lum shifts
14. Saturation       luma-preserving saturation scale
15. Vibrance         saturation boost weighted toward desaturated pixels
16. sRGB gamma       pow(x, 1/2.2) — applied on screen AND on export readback
```

**Export only — CPU, after FBO readback (`MainWindow::exportFile`)**

```
17. Resize           scale to the dimensions chosen in the export dialog
18. Sharpening       unsharp mask (the Sharpen slider has no preview effect —
                     it is applied only here)
19. Save             JPEG/PNG/TIFF, tagged QColorSpace::SRgb
```

Histograms are exact: `ImageViewport::renderHistograms()` renders the preview
through the real shader into a small offscreen FBO (debounced on parameter
changes) and reads back two samples — the full pipeline for the panel
histogram, and a "stop after tone regions, gamma-encode" pass (`uCurveInput`)
for the histogram behind the tone curve (docs/adr/0002).

---

## Export Pipeline

1. User triggers export (Ctrl+E).
2. `ImageViewport::renderToImage()` called on the main thread:
   - Upload `m_fullRes` as a float32 RGB texture.
   - Bind offscreen FBO at the cropped pixel size.
   - Run the full shader pipeline (steps 4–16 above) with current `AdjustmentParams`.
   - Read back → `QImage(Format_RGB888)`, scaled to the requested output size.
   - Tag: `image.setColorSpace(QColorSpace::SRgb)`.
3. Unsharp mask (sharpening slider, CPU).
4. `QImage::save(path)` — JPEG, PNG, or TIFF.

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
| `R` | Reset all adjustments |

---

## Persistence

- **XMP sidecar**: loaded automatically on file open, saved on `Ctrl+S`.
  No auto-save on every slider move.
- **Window state**: geometry, dock layout, last opened folder — via `QSettings`
  on close, restored on launch.

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
- [x] WYSIWYG export via FBO readback, sRGB-tagged output (JPEG/PNG/TIFF)
- [x] Filename list dock with arrow-key navigation
- [x] Window state persistence via `QSettings`

---

## Post-MVP Milestones

### Milestone 2 — Tone Curve ✅
- Curve editor widget: draggable control points on a histogram background.
- Per-channel (R/G/B/Luma) curves, ghost overlays, modified-channel indicators.
- 256-entry LUT uploaded as a 1D texture uniform; applied in gamma space.
- Stored in XMP as `crs:ToneCurvePV2012*` point lists.

### Milestone 3 — Thumbnail Strip
- Replace filename list with a horizontal thumbnail dock.
- `QAbstractItemModel` + custom delegate.
- Lazy thumbnail generation: thread pool, one `QtConcurrent::run` per file,
  capped concurrency, LRU cache.

### Milestone 4 — Color Management (lcms2)
- Proper linear → sRGB transform via lcms2 on export.
- Wide-gamut output (AdobeRGB, Display P3).
- Soft-proofing for print profiles.
- Camera input profile support (DCP or ICC).

### Milestone 5 — Qt RHI Migration
- Swap `QOpenGLWidget` → `QRhiWidget`.
- Compile GLSL shaders via `qsb` → SPIR-V → MSL/HLSL/GLSL.
- Native Metal on macOS, Vulkan on Linux, D3D11 on Windows.
- Remove OpenGL dependency.

### Milestone 6 — Tile-based LOD
- At high zoom, stream tiles from full-res buffer rather than uploading entire texture.
- Tile cache with LRU eviction.
- Async tile upload via PBO (Pixel Buffer Objects).
