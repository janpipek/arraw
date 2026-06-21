# AGENTS.md

This file provides guidance to AI coding assistants when working with code in this repository.

## Commands Reference

Use these commands for building, running, and testing the project.

When a `Justfile` recipe needs more than a couple of commands, put the logic in a `tools/*.py` script (stdlib, argparse, styled like `tools/package_windows.py`) and make the recipe a thin `uv run tools/<name>.py {{args}}` wrapper — not an inline bash recipe, which isn't portable since the Justfile sets `windows-powershell := true`. See `bump`, `rpm`, and `windows-installer`.

### Build and Run

#### Linux (Fedora)
```bash
# Install dependencies
sudo dnf install qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtshadertools-devel \
    qt6-qttools-devel LibRaw-devel lcms2-devel cmake ninja-build

# Configure
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

# Build
ninja -C build

# Run
./build/arraw
```
For the release **AppImage** (built in CI on Ubuntu 24.04 with Qt 6.8 via aqtinstall)
and the Linux packaging gotchas, see the **[Linux build & deployment guide](docs/linux-build.md)**.
For the native Fedora package, run `just rpm` from a clean committed checkout, then
`just rpm-smoke` to verify installation in a clean Fedora 44 container. Artifacts
are written to `dist/fedora/`.

#### macOS (Homebrew)
```bash
# Install dependencies
brew install qt libraw little-cms2 cmake ninja

# Configure
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=$(brew --prefix qt)

# Build
ninja -C build

# Run
./build/arraw
```

#### Windows (vcpkg)
Windows needs extra setup (the MSVC developer environment plus Qt-plugin/libraw
deployment quirks). See the dedicated **[Windows build guide](docs/windows-build.md)**
for the complete walkthrough and troubleshooting. In short:
```powershell
# Install dependencies (libraw[openmp] multithreads the demosaic — much faster loads)
vcpkg install qtbase qttools qtshadertools libraw[openmp] lcms --triplet x64-windows

# From "Developer PowerShell for VS 2022" (so rc.exe/mt.exe are on PATH), or
# import vcvars64.bat into the current PowerShell as shown in docs/windows-build.md.
#
# CMakePresets.json is local and untracked because it contains machine-specific
# vcpkg paths. Create it per docs/windows-build.md, then:
cmake --preset default
cmake --build build
ctest --test-dir build --output-on-failure
```
Without a local preset, configure by passing the vcpkg toolchain directly:
```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows
```
A release ZIP of the runnable app is produced by `python tools/package_windows.py`.

#### Release Build
```bash
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-release
```

#### Shaders
Shaders are Vulkan-dialect GLSL in `shaders/`, compiled at build time by `qsb` and baked into the binary as Qt resources (`:/shaders/*.qsb`). Modifying them requires a rebuild (see architectural records in [docs/adr/](file:///home/jan/code/my/arraw/docs/adr/)).

**The uniform block `buf` must be declared identically in three places: `image.vert`, `image.frag`, and the `Ubuf` struct in `RendererCore.h` — same fields, same order, same std140 layout, byte-for-byte.** This holds even for a field a stage never reads: the OpenGL RHI backend links the vertex and fragment stages into one program, and *any* divergence in the shared block makes linking fail at runtime with:

```
Failed to link shader program: error: uniform 'u' declared as type 'buf' and type 'buf'
```

When that happens the viewport shader never loads, so the **image area renders black** (often with stale, vertically-flipped framebuffer contents showing through) while the rest of the Qt widget UI looks fine. The headless golden-render tests are skipped without a GPU, so the test suite will **not** catch this — after any change to the uniform block, do a manual GPU smoke-run (launch the app, confirm the viewport renders) and watch stderr for the link error. When adding a field, add it to all three declarations in the same change, even if only one stage uses it.

#### Diagnostics
Set the `ARRAW_TRACE` environment variable to print per-operation timings (`[trace] <label> N ms` on stderr) for expensive work — RAW load stages, the standard image loader, and the lcms colour transforms. The facility is in `src/Trace.h` (`trace::Scope` for a whole scope, `trace::Laps` for multi-stage ops); it is free when the variable is unset. On Windows the app is a GUI-subsystem binary with no attached console, so redirect to capture it: `arraw.exe 2> trace.txt`.

### Tests
Use `just` or direct `ctest` execution:
* **Run tests (recommended)**: `just test` or `ctest --test-dir build --output-on-failure`
* **Test location**: Catch2 v3 tests live in `tests/`, linking the `arraw_core` static library (all source except `main.cpp`).
* **Test fixtures**: Located in `tests/fixtures/`. The DNG is generated by `make_test_dng.py`.
* **Testing Strategy & Tolerances**: See [DESIGN.md#testing-strategy](file:///home/jan/code/my/arraw/DESIGN.md#testing-strategy) and the guidelines in [docs/adr/](file:///home/jan/code/my/arraw/docs/adr/).

### Formatting and Linting
Use `clang-format` for mechanical C++ formatting. The normal CMake build enables baseline compiler warnings (`/W4` on MSVC, `-Wall -Wextra -Wpedantic` elsewhere).
```bash
# Format all C++ source/header files
just format

# Check formatting without modifying files
just format-check

# Optional Qt-aware static analysis, requires clazy
just clazy

# Optional general static analysis, requires clang-tidy
just tidy
```

* **Formatter**: `clang-format` 18+ preferred. Keep formatting mechanical; do not mix broad formatting churn with behavioral changes unless the task is explicitly cleanup-focused.
* **Warnings**: Treat compiler warnings as the primary lint baseline. Do not introduce new warnings in touched code.
* **Qt analysis**: `clazy` is optional and advisory. `just clazy` rebuilds the application target only with tests disabled, because Catch/test diagnostics are noisy and not useful for Qt analysis. Fix clear Qt correctness/performance issues, but avoid large style rewrites just to satisfy analyzer output.
* **General analysis**: `clang-tidy` is optional and advisory (config in `.clang-tidy`). `just tidy` runs it over `src/` using the compile database from the normal `build` dir (`CMAKE_EXPORT_COMPILE_COMMANDS`). The check set covers `bugprone/performance/modernize/readability`; it overlaps a little with clazy but catches non-Qt issues clazy ignores. Address clear bug/perf findings; do not chase every readability nit.
* **Tool overrides**: set `CLANG_FORMAT`, `CLAZY`, `CLANG_TIDY`, or `RUN_CLANG_TIDY` if the local binaries are versioned, for example `CLANG_FORMAT=clang-format-18 just format-check`.

---

## Code Style

For complete styling paradigms and developer guidelines, see [docs/code_guidelines.md](file:///home/jan/code/my/arraw/docs/code_guidelines.md).

* **Language**: Modern C++20.
* **Hungarian Notation**: Strictly avoid (no `m_` prefixes on members, no type prefixes).
* **`m_` prefix refactoring**: The legacy code uses `m_`. When editing or touching a file, **strip existing `m_` prefixes from names** within that file and do not introduce new ones.
* **Variable naming**: Use plain names (`zoom`, `params`, `curveLutTex`) for class members, local variables, and parameters.
* **Type inference & correctness**: Use `auto` where the type is obvious from context. Prefer `const` by default.

---

## Architecture & Data Flow

Detailed architectural decisions are documented in the [docs/adr/](file:///home/jan/code/my/arraw/docs/adr/) directory.

### 1. Data Flow: Open → Display
1. `MainWindow::loadImage()` fires a `QtConcurrent::run` background task.
2. `RawProcessor::load()` (background thread) calls libraw, produces a linear float32 RGB `ImageBuffer` (`fullRes`), and calls `downsample2x()` to produce the `preview` (half width, half height).
3. `QFutureWatcher<LoadResult>::finished` fires on the main thread, executing `MainWindow::onLoadFinished()`.
4. `m_fullRes` (to be renamed/stripped of `m_`) is stored silently (used for exports only). `m_preview` is handed to `ImageViewport::setImage()`.
5. Histograms are generated by viewport rendering of small shader samples (debounced) emitting `histogramsReady`, which `AdjustmentPanel::setHistogramSamples()` forwards to the histogram and the tone curve background.
6. `XmpSidecar::load()` reads the `.xmp` sidecar if present and calls `AdjustmentPanel::setParams()` to restore prior edits.

### 2. Real-Time GPU Preview
* `AdjustmentPanel` emits `paramsChanged(GlobalAdjustment)` on every slider move.
* `ImageViewport` receives this, stores the parameters, and calls `update()`.
* In `render()`, all adjustments are sent in one uniform block (no CPU-based image processing occurs during preview).
* The shader pipeline order is defined in [shaders/image.frag](file:///home/jan/code/my/arraw/shaders/image.frag) and detailed in [DESIGN.md](file:///home/jan/code/my/arraw/DESIGN.md).

### 3. RendererCore and Image Slots
* All GPU operations go through `RendererCore`, which owns all RHI resources.
* `record()` is the single place the shader pass is recorded (called by widget paint, export, and histogram sampling).
* It holds two image slots: `Preview` (always present) and `FullRes` (uploaded lazily when zoom crosses the point where preview pixels become visible). `ImageViewport::activeSlot()` picks the active slot.

### 4. Export
* Offscreen RHI rendering is used for exports (no `QImage` pixel manipulation).
* `ImageViewport::renderToImage()` uploads the full-res buffer into a temporary texture, executes the full shader pipeline at full resolution, and reads back the result.
* The output is tagged with `QColorSpace::SRgb` before saving.

### 5. XMP Sidecar
* `XmpSidecar` reads/writes the `crs:` (Adobe Camera Raw) XML namespace for Lightroom compatibility.
* `crs:Temperature` is stored in absolute Kelvin (2000–12000K).
* Tint and all other fields use the internal `-100..100` scale.
* Sidecar files are stored in the same directory as the RAW file, with the same base name and a `.xmp` extension.

### 6. GlobalAdjustment ↔ Slider Scale
* `AdjustmentPanel` sliders use integers for Qt's `QSlider`:
  * `exposure`: slider × 0.01 = EV (slider range -500..500 → -5.0..5.0 EV).
  * `temperature`: slider value = Kelvin directly (range 2000..12000).
  * All other fields: slider value = float value directly (-100..100).
* **Adding a new adjustment requires updating**: `GlobalAdjustment` (struct), the slider in `AdjustmentPanel`, the uniform block in **both** `image.vert` **and** `image.frag`, the `Ubuf` mirror in `RendererCore.h` (must match std140 layout exactly), `RendererCore::fillUbuf()`, and `XmpSidecar` load/save. Update all three uniform-block declarations in the same change even if a stage never reads the field — a mismatch fails shader linking and blanks the viewport (see [Shaders](#shaders) above).

### 7. UI Theme & Colors
* The neutral dark theme is applied once in `main()` via `Theme::apply()` (Fusion style + a dark `QPalette`), **before** any widget is constructed. Dark-only for now; the palette is built in one function so a light variant / user-settable colors slot in behind the same seam (ADR 0030).
* **All UI colors are single-sourced in `src/ThemeColors.h`** (a dependency-free leaf header), read by both `Theme` (the palette) and `RendererCore` (the viewport surround, `kCanvas`). Add or change a chrome color *there*, not inline.
* `kCanvas` must stay bit-identical to `0.15,0.15,0.15` — it backs the golden-image references (ADR 0005).
* **Semantic / data-viz colors stay hardcoded with their feature** (histogram channels, filmstrip flags/ratings, curve channel buttons, viewport overlays & clipping warnings) — they encode meaning, not chrome, so don't route them through the theme.
* Prefer the palette over QSS. Any QSS stays minimal and centralized in `Theme::apply()`; the per-widget sheet on the `AdjustmentPanel` curve buttons is the one sanctioned exception (semantic colors).

---

## Domain Vocabulary Constraints

For a more comprehensive domain definition guide, refer to [CONTEXT.md](file:///home/jan/code/my/arraw/CONTEXT.md). Architectural design choices behind these concepts are documented in [docs/adr/](file:///home/jan/code/my/arraw/docs/adr/).

To maintain alignment and conceptual integrity, adhere to the following terminology constraints:

| Preferred Term | Term(s) to AVOID | Description / Domain Meaning |
|---|---|---|
| **Culling** | cataloguing, library, DAM | Marking frames in the open folder as keepers/rejects (per-file triage, no database). |
| **Rating** | stars, pick/reject flag | Culling value from 0-5 stars (0 = unrated, -1 = reject). No separate pick flag. |
| **Colour Label** | tag, keyword, free-text | Category from fixed 5-colour set (Red, Yellow, Green, Blue, Purple) or none. |
| **User Metadata** | image metadata, tags | User-authored metadata (Rating & Colour Label). EXIF is read-only. |
| **Working color space** | internal, pipeline space | Linear-light Rec.2020 primaries. Used by all adjustments from decode to display/output. |
| **Display transform** | gamma step, sRGB step | Final conversion from working color space to monitor-expected format. |
| **Output transform** | export color conversion | Conversion from working color space to chosen export profile (sRGB, P3, etc.). |
| **Soft-proofing** | print preview, proof mode | Simulating a printer/paper profile on the screen. |
| **Clipping** | extremes, blown, crushed | Pixel hitting display range limit (≥ 1.0 highlight, ≤ 0.0 shadow) at Display transform. |
| **Clipping Overlay** | show extremes, zebra, mask | Highlights painted red, shadows painted blue. Toggled via QSettings, not sidecar. |
| **Gamut Warning** | clipping, out-of-range | Soft-proofing-only overlay (red) for pixels outside the proofed output gamut. |
| **Tone Curve** | curves | Luma Curve + three independent Channel Curves. |
| **Luma Curve** | master curve, RGB curve | Tone curve applied to luminance (scales RGB proportionally to preserve hue). |
| **Channel Curve** | (none) | Tone curve applied to R, G, or B channel independently (shifts hue by design). |
| **Curve Input Histogram**| (none) | Gamma-encoded histogram drawn behind the Tone Curve. |
| **Local Adjustment** | filter, layer, brush | A develop edit that applies to a masked region (Mask + tonal/colour deltas). |
| **Mask** | selection, channel | Stencil for Local Adjustment (0 to 1). Parametric in v1. |
| **Mask Type** | (none) | Linear or Radial in v1; Luminance/Colour range planned next. |
| **Develop Preset** | profile, template, style | Named bundle of develop settings stored as arraw-native files. |
| **Straighten** | deskew, level, auto-rotate | The gesture of drawing a reference line to set the Rotation. |
| **Rotation** | (none) | The signed angle (±45°) applied to the image. |
