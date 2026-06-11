# arraw

A lightweight, cross-platform RAW photo editor with a Lightroom-style development workflow. Real-time GPU preview, non-destructive editing, XMP sidecar output.

## Features

- **Real-time adjustments** — all edits run as GLSL uniforms; no CPU processing during preview
- **Non-destructive** — edits saved as `.xmp` sidecar files, Lightroom-compatible (`crs:` namespace)
- **Color-managed** — linear Rec.2020 working space; export to sRGB, Display P3, or Adobe RGB (lcms2, ICC embedded); embedded profiles honoured on load
- **Soft-proofing** — preview against any printer/paper ICC profile (`S`), Perceptual/Relative intent, black point compensation, gamut warning; monitor ICC profile support
- **GPU export** — full-resolution FBO readback through the same shader pipeline; JPEG, PNG, TIFF (8- or 16-bit) output
- **Dual-res textures** — quarter-res preview for interaction, full-res texture swapped in lazily at high zoom
- **Undo/redo** — per-slider-drag coalescing via `QUndoStack`
- **File browser** — filename list dock with arrow-key navigation through a folder

### Adjustments

| Group | Controls |
|---|---|
| White Balance | Temperature (2000–12000 K), Tint; WB presets |
| Tone | Exposure (±5 EV), Contrast, Highlights, Shadows, Whites, Blacks |
| Color | Saturation, Vibrance |
| Detail | Sharpening |
| Geometry | Rotation (±45°), Crop |

### Supported RAW formats

CR2, CR3, NEF, ARW, DNG, RAF, ORF, RW2, PEF, SRW (via libraw)

## Keyboard shortcuts

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
| `R` | Reset all adjustments |

Zoom: scroll wheel (0.05×–32×). Pan: Alt+drag or middle-button drag.

## Building

### Linux (Fedora)

```bash
sudo dnf install qt6-qtbase-devel qt6-qttools-devel LibRaw-devel lcms2-devel cmake ninja-build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
./build/arraw
```

### macOS (Homebrew)

```bash
brew install qt libraw little-cms2 cmake ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=$(brew --prefix qt)
ninja -C build
./build/arraw
```

### Windows (vcpkg)

```bat
vcpkg install qt6-base qt6-tools libraw lcms
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
ninja -C build
```

### Release build

```bash
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-release
```

Shaders are copied from `shaders/` to `build/shaders/` automatically. If the app fails to display images, verify that `build/shaders/image.vert` and `image.frag` exist.

## Dependencies

- **Qt 6** (Widgets, OpenGL, Concurrent, Xml)
- **libraw** (≥ 0.21) — RAW decoding into the Rec.2020 working space
- **lcms2** — output color transforms and ICC profile handling
- **OpenGL 3.3 core** — GPU preview and export pipeline
- CMake 3.21+, Ninja

## Architecture

See [`DESIGN.md`](DESIGN.md) for the full design document.

Brief overview:

- `RawProcessor` — libraw wrapper, runs on a background thread via `QtConcurrent::run`
- `ImageViewport` — `QOpenGLWidget` managing two textures (preview + full-res) and the GLSL pipeline
- `AdjustmentPanel` — slider UI, emits `paramsChanged(AdjustmentParams)` on every change
- `XmpSidecar` — reads/writes `.xmp` files using Adobe Camera Raw field names
- `FileBrowser` — folder-scoped filename list dock
- `MainWindow` — wires everything together; owns the `QUndoStack`

All adjustments live in `AdjustmentParams` (a plain struct). The GLSL fragment shader is the single source of truth for all *adjustments* — the same shader runs during preview and export, always in linear Rec.2020. Only the final color encode differs per destination: in-shader for the display, lcms2 on the CPU for export (see `docs/adr/0002`).
