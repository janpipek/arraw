# arraw

**WARNING** Though a lot of thinking and some basic knowledge of C++ and Qt has been put to the development, it is very heavily vibe-coded.

A lightweight, cross-platform RAW photo editor with a Lightroom-style development workflow. Real-time GPU preview, non-destructive editing, XMP sidecar output.

## Features

- **Real-time adjustments** — all edits run as GLSL uniforms; no CPU processing during preview
- **Non-destructive** — edits saved as `.xmp` sidecar files, Lightroom-compatible (`crs:` namespace)
- **Color-managed** — linear Rec.2020 working space; export to sRGB, Display P3, or Adobe RGB (lcms2, ICC embedded); embedded profiles honoured on load
- **Soft-proofing** — preview against any printer/paper ICC profile (`S`), Perceptual/Relative intent, black point compensation, gamut warning; monitor ICC profile support
- **GPU export** — full-resolution offscreen readback through the same shader pipeline; JPEG, PNG, TIFF (8- or 16-bit) output
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
sudo dnf install qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtshadertools-devel \
    qt6-qttools-devel LibRaw-devel lcms2-devel cmake ninja-build
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
vcpkg install qt6-base qt6-tools qt6-shadertools libraw lcms
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
ninja -C build
```

### Release build

```bash
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-release
```

Shaders are compiled at build time by `qsb` (Qt ShaderTools) and baked into
the binary as resources — there is nothing to copy or deploy.

## Dependencies

- **Qt 6** ≥ 6.8 (Widgets, Concurrent; RHI via GuiPrivate; ShaderTools at build time)
- **libraw** (≥ 0.21) — RAW decoding into the Rec.2020 working space
- **lcms2** — output color transforms and ICC profile handling
- GPU rendering via **Qt RHI** — Metal on macOS, D3D11 on Windows, OpenGL on
  Linux (Vulkan opt-in); no direct OpenGL dependency
- CMake 3.21+, Ninja

## Architecture

See [`DESIGN.md`](DESIGN.md) for the full design document.

Brief overview:

- `RawProcessor` — libraw wrapper, runs on a background thread via `QtConcurrent::run`
- `RendererCore` — owns all RHI resources; the single place the shader pass is recorded (preview, export, histograms)
- `ImageViewport` — `QRhiWidget` with zoom/pan/crop logic, delegating all drawing to `RendererCore`
- `AdjustmentPanel` — slider UI, emits `paramsChanged(AdjustmentParams)` on every change
- `XmpSidecar` — reads/writes `.xmp` files using Adobe Camera Raw field names
- `FileBrowser` — folder-scoped filename list dock
- `MainWindow` — wires everything together; owns the `QUndoStack`

All adjustments live in `AdjustmentParams` (a plain struct). The GLSL fragment shader is the single source of truth for all *adjustments* — the same shader runs during preview and export, always in linear Rec.2020. Only the final color encode differs per destination: in-shader for the display, lcms2 on the CPU for export (see `docs/adr/0002`).
