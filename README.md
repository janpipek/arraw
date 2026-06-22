# arraw

A lightweight, cross-platform RAW photo editor with a Lightroom-style development workflow. Real-time GPU preview, non-destructive editing, XMP sidecar output.

⚠️✨ **WARNING** Though a lot of thinking and some basic knowledge of C++ and Qt has been put to the development, it is very heavily vibe-coded. Use at your own risk. ✨⚠️

## Motivation

- Adobe Lightroom is great, but it does not run on Linux and has a quite expensive subscription model.
- [Darktable](https://www.darktable.org/) is great but it is difficult to grasp mentally.
- I wanted to experiment with Claude's capabilities.

## Features

- **Real-time adjustments** — global edits run as GLSL uniforms; no CPU processing during preview
- **Non-destructive** — edits saved as `.xmp` sidecar files, Lightroom-compatible (`crs:` namespace)
- **Color-managed** — linear Rec.2020 working space; export to sRGB, Display P3, or Adobe RGB (lcms2, ICC embedded); embedded profiles honoured on load
- **Soft-proofing** — preview against any printer/paper ICC profile (`S`), Perceptual/Relative intent, black point compensation, gamut warning; monitor ICC profile support
- **Local adjustments** — masked edits with Linear (graduated) and Radial masks (up to 16 per image)
- **Lens corrections** — profile-driven distortion, vignetting, and chromatic-aberration correction (lensfun / embedded profiles)
- **Spot removal** — clone-based spot/blemish healing
- **Tone curve** — Luma curve plus independent R/G/B channel curves, drawn over a gamma-encoded input histogram
- **Culling** — per-file rating (0–5 stars, reject) and colour labels, stored in the sidecar (no database)
- **Develop presets** — named, partial bundles of develop settings (arraw-native)
- **Copy / paste & batch** — copy settings between images, batch-paste, batch export, headless CLI export
- **GPU export** — full-resolution offscreen readback through the same shader pipeline; JPEG, PNG, TIFF (8- or 16-bit) output
- **Film strip** — thumbnail strip with EXIF tooltips and arrow-key navigation through a folder; EXIF panel for the current image
- **Caching** — developed-thumbnail and decode caches for fast browsing
- **Focus modes** — full-screen, lights-out (hide panels), and collapsible adjustment dock
- **Dual-res textures** — quarter-res preview for interaction, full-res texture swapped in lazily at high zoom
- **Undo/redo** — per-slider-drag coalescing via `QUndoStack`

### Adjustments

| Group | Controls |
|---|---|
| White Balance | Temperature (2000–12000 K), Tint; WB presets; neutral-pick tool |
| Tone | Exposure (±5 EV), Contrast, Highlights, Shadows, Whites, Blacks |
| Tone Curve | Luma curve + per-channel R/G/B curves |
| Color | Saturation, Vibrance; HSL (hue/sat/luminance across 8 colour ranges) |
| Detail | Sharpening |
| Effects | Post-Crop Vignette (amount/midpoint/feather), Grain (amount/size/roughness) |
| Lens | Distortion, Vignetting, Chromatic Aberration correction |
| Geometry | Orientation (90° rotate / flip), Straighten (±45°), Crop (aspect presets) |

### Supported RAW formats

CR2, CR3, NEF, ARW, DNG, RAF, ORF, RW2, PEF, SRW (via libraw)

## Keyboard shortcuts

| Key | Action |
|---|---|
| `Ctrl+O` / `Ctrl+Shift+O` | Open file / folder |
| `Ctrl+S` | Save adjustments (XMP sidecar) |
| `Ctrl+E` | Export |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / Redo |
| `Ctrl+0` | Reset zoom to fit |
| `←` / `→` | Previous / next file in folder |
| `C` / `M` / `Q` | Crop / Masks / Spots tool |
| `S` | Toggle soft-proofing |
| `J` | Toggle clipping overlay |
| `\` (hold) | Before/after toggle |
| `1`–`5`, `0`, `X` | Rating (stars / unrated / reject) |
| `F11` / `F12` | Full screen / hide panels |

Zoom: scroll wheel (0.05×–32×). Pan: Alt+drag or middle-button drag.

See [docs/keybindings.md](docs/keybindings.md) for the complete list.

## FAQ

Common runtime questions — including making arraw render on a laptop's discrete
GPU instead of the integrated one — are in [docs/faq.md](docs/faq.md).

## Installing

Pre-built downloads (Linux AppImage / Fedora RPM, Windows installer or portable
ZIP) are published on the [Releases page](https://github.com/janpipek/arraw/releases).
See [docs/installation.md](docs/installation.md) for per-platform install steps.
To build from source instead, follow the section below.

## Building

### Linux (Fedora)

```bash
sudo dnf install qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtshadertools-devel \
    qt6-qttools-devel LibRaw-devel lcms2-devel lensfun-devel cmake ninja-build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
./build/arraw
```

Build the native Fedora RPM/SRPM from a clean, committed checkout with `just rpm`.
Run `just rpm-smoke` to install and verify it in a clean Fedora 44 container. The
commands, required packages, and release workflow are documented in
[the Linux build guide](docs/linux-build.md).

### macOS (Homebrew)

```bash
brew install qt libraw little-cms2 lensfun cmake ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=$(brew --prefix qt)
ninja -C build
./build/arraw
```

### Windows (vcpkg)

Install vcpkg using the [vcpkg installation guide](https://learn.microsoft.com/en-us/vcpkg/get_started/get-started)
(Note: scoop-based install did not work for me)

```bat
vcpkg install qtbase qttools qtshadertools libraw[openmp] lcms
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
ninja -C build
```

`libraw[openmp]` multithreads the RAW demosaic (much faster loads). See
[docs/windows-build.md](docs/windows-build.md) for the full Windows setup.

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
- **lensfun** — lens correction profiles (distortion, vignetting, CA)
- GPU rendering via **Qt RHI** — Metal on macOS, D3D11 on Windows, OpenGL on
  Linux (Vulkan opt-in); no direct OpenGL dependency
- CMake 3.21+, Ninja

## Architecture

See [`DESIGN.md`](DESIGN.md) for the full design document.

Brief overview:

- `RawProcessor` — libraw wrapper, runs on a background thread via `QtConcurrent::run`
- `RendererCore` — owns all RHI resources; the single place the shader pass is recorded (preview, export, histograms)
- `ImageViewport` — `QRhiWidget` with zoom/pan/crop logic, delegating all drawing to `RendererCore`
- `AdjustmentPanel` — slider UI, emits `paramsChanged(GlobalAdjustment)` on every change
- `XmpSidecar` — reads/writes `.xmp` files using Adobe Camera Raw field names
- `FilmStrip` — folder-scoped thumbnail strip dock (navigation, culling marks, EXIF tooltips)
- `MainWindow` — wires everything together; owns the `QUndoStack`

All adjustments live in `GlobalAdjustment` (a plain struct). The GLSL fragment shader is the single source of truth for all *adjustments* — the same shader runs during preview and export, always in linear Rec.2020. Only the final color encode differs per destination: in-shader for the display, lcms2 on the CPU for export (see `docs/adr/0002`).
