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
- **Local adjustments** — masked edits with Linear (graduated), Radial, and freehand Brush masks (up to 16 per image)
- **Lens corrections** — profile-driven distortion, vignetting, and chromatic-aberration correction (lensfun / embedded profiles)
- **Spot removal** — clone-based spot/blemish healing
- **Tone curve** — Luma curve plus independent R/G/B channel curves, drawn over a gamma-encoded input histogram
- **Black & White** — hue-aware monochrome treatment with an 8-band mixer, not a flat desaturation
- **Colour grading** — three-zone (shadows/midtones/highlights) toning in Oklab, over colour or B&W
- **Noise reduction** — orthogonal luminance and chroma halves, edge-aware, as a cached GPU pre-pass
- **Culling** — per-file rating (0–5 stars, reject) and colour labels, stored in the sidecar (no database)
- **Develop presets** — named, partial bundles of develop settings (arraw-native)
- **Snapshots & history** — named A/B develop states saved per image; a session history of every edit step
- **Copy / paste & batch** — copy settings between images, batch-paste, batch export
- **Command line** — `arraw export`, `preset`, and `info` run headless; no window needed
- **GPU export** — full-resolution offscreen readback through the same shader pipeline; JPEG, PNG, TIFF (8- or 16-bit) output
- **Exported metadata** — corrected capture-EXIF passthrough plus your descriptive XMP, per-group opt-in (GPS off by default)
- **Film strip** — thumbnail strip with EXIF tooltips, arrow-key folder navigation, and filtering by rating or colour label
- **Info panel** — editable Title, Caption, Keywords, Creator, Copyright alongside the read-only camera EXIF
- **Caching** — developed-thumbnail and decode caches for fast browsing
- **Focus modes** — full-screen, lights-out (hide panels), and collapsible adjustment dock
- **Dual-res textures** — quarter-res preview for interaction, full-res texture swapped in lazily at high zoom
- **Undo/redo** — per-slider-drag coalescing via `QUndoStack`

### Adjustments

Listed in panel order. A Colour ↔ Black & White treatment switch sits at the top:
turning on Black & White hides Color and HSL and reveals the B&W Mixer.

| Group | Controls |
|---|---|
| White Balance | Temperature (2000–12000 K), Tint; WB presets; neutral-pick tool |
| Tone | Exposure (±5 EV), Contrast, Highlights, Shadows, Whites, Blacks, Filmic Highlights |
| Tone Curve | Luma curve + per-channel R/G/B curves |
| Color | Saturation, Vibrance; HSL (hue/sat/luminance across 8 colour ranges) |
| Black & White | 8-band hue mixer — each original hue weighted into its own grey |
| Colour Grading | Shadows / Midtones / Highlights hue + saturation, plus Balance and Blending |
| Detail | Demosaic algorithm; Texture, Clarity, Dehaze, Sharpen; Luminance Noise (amount/detail); Colour Noise (strength/smoothness) |
| Geometry | Orientation (90° rotate / flip), Straighten (±45°), Crop (aspect presets) |
| Lens Corrections | Distortion, Vignetting, Chromatic Aberration correction |
| Effects | Post-Crop Vignette (amount/midpoint/feather), Grain (amount/size/roughness) |

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
| `C` | Toggle the Crop tool |
| `M` / `Q` | Select the Masks / Spots adjustment tab |
| `S` | Toggle soft-proofing |
| `J` | Toggle clipping overlay |
| `\` (hold) | Before/after toggle |
| `1`–`5`, `0`, `X` | Rating (stars / unrated / reject) |
| `F11` / `F12` | Full screen / hide panels |

Zoom: scroll wheel (0.05×–32×). Pan: Alt+drag or middle-button drag.

See [docs/keybindings.md](docs/keybindings.md) for the complete list.

## Command line

`arraw` is a command as well as an editor — the same shader pipeline, no window:

```bash
arraw                      # open the editor
arraw ui photo.arw         # open the editor on a file or folder
arraw export *.arw -o out/ # render through each file's develop sidecar
arraw preset list          # list, show, or apply saved Develop Presets
arraw info photo.arw       # report EXIF and edit state, read-only
```

Add `--json` to `info` for machine-readable output. Full flags are in
[docs/installation.md](docs/installation.md#command-line).

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
    qt6-qttools-devel LibRaw-devel lcms2-devel lensfun-devel exiv2-devel cmake ninja-build
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
brew install qt libraw little-cms2 lensfun exiv2 cmake ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=$(brew --prefix qt)
ninja -C build
./build/arraw
```

### Windows (vcpkg)

Install vcpkg using the [vcpkg installation guide](https://learn.microsoft.com/en-us/vcpkg/get_started/get-started)
(Note: scoop-based install did not work for me)

```bat
vcpkg install qtbase qttools qtshadertools libraw[openmp] lcms lensfun exiv2
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

Developer maintenance commands, including regenerating app icons after editing
`resources/icon.svg`, are documented in [docs/development.md](docs/development.md).

## Dependencies

- **Qt 6** ≥ 6.8 (Widgets, Concurrent; RHI via GuiPrivate; ShaderTools at build time)
- **libraw** (≥ 0.21) — RAW decoding into the Rec.2020 working space
- **lcms2** — output color transforms and ICC profile handling
- **lensfun** — lens correction profiles (distortion, vignetting, CA)
- **exiv2** — exported EXIF/XMP metadata embedding
- GPU rendering via **Qt RHI** — Metal on macOS, D3D11 on Windows, OpenGL on
  Linux (Vulkan opt-in); no direct OpenGL dependency
- CMake 3.21+, Ninja

## Architecture

All adjustments live in `GlobalAdjustment`, a plain struct. The GLSL fragment
shader is the single source of truth for *adjustments* — the same shader runs
during preview, export, and the CLI, always in linear Rec.2020. Only the final
colour encode differs per destination: in-shader for the display, lcms2 on the CPU
for export ([ADR 0002](docs/adr/0002-shader-adjustments-cpu-encode.md)).

- [`DESIGN.md`](DESIGN.md) — components, threading model, and the full pixel
  pipeline in execution order
- [`CONTEXT.md`](CONTEXT.md) — the domain glossary
- [`docs/adr/`](docs/adr/README.md) — why each hard-to-reverse choice was made
- [`AGENTS.md`](AGENTS.md) — working agreements for contributors and AI assistants
