# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

**Linux (Fedora)**
```bash
sudo dnf install qt6-qtbase-devel qt6-qttools-devel LibRaw-devel lcms2-devel cmake ninja-build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
./build/arraw
```

**macOS (Homebrew)**
```bash
brew install qt libraw little-cms2 cmake ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=$(brew --prefix qt)
ninja -C build
./build/arraw
```

**Windows (vcpkg)**
```bat
vcpkg install qt6-base qt6-tools libraw lcms
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
ninja -C build
```

**Release build**
```bash
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-release
```

Shaders are copied from `shaders/` to `build/shaders/` automatically post-build. If the app fails to display images, check that `build/shaders/image.vert` and `image.frag` exist.

There are no tests yet.

## Code Style

Modern C++20. No Hungarian notation — no `m_` prefix on members, no type prefixes.
Use `auto` where the type is obvious from context. Prefer `const` by default.
Class members, locals, and parameters are plain names (`zoom`, `shader`, `previewTex`).
The existing code uses `m_` — strip it when touching a file, don't introduce new uses.

Shader hot-reload: `ImageViewport::reloadShaders()` exists but is not wired to a key
or file watcher yet. When iterating on shaders, call it manually or wire `Ctrl+Shift+R`.

## Architecture

The full design rationale lives in `DESIGN.md`. Key things that require reading multiple files to understand:

### Data flow: open → display

1. `MainWindow::loadImage()` fires a `QtConcurrent::run` task.
2. `RawProcessor::load()` (background thread) calls libraw, produces a linear float32 RGB `ImageBuffer` (`fullRes`), then calls `downsample2x()` to produce `preview` (half W × half H).
3. `QFutureWatcher<LoadResult>::finished` fires on the main thread → `MainWindow::onLoadFinished()`.
4. `m_fullRes` is stored silently (export only). `m_preview` is handed to `ImageViewport::setImage()`. Histograms are not fed directly: the viewport renders small shader samples (debounced) and emits `histogramsReady`, which `AdjustmentPanel::setHistogramSamples()` forwards to the panel `Histogram` and the `ToneCurveWidget` background.
5. `XmpSidecar::load()` reads the `.xmp` sidecar if present and calls `AdjustmentPanel::setParams()` to restore prior edits.

### Real-time preview

`AdjustmentPanel` emits `paramsChanged(AdjustmentParams)` on every slider move. `ImageViewport` receives this, stores the params, and calls `update()`. In `paintGL()`, all adjustments are passed as GLSL uniforms — no CPU image processing happens during preview. The shader pipeline order is defined in `shaders/image.frag` and documented in `DESIGN.md`.

### Two textures in ImageViewport

`ImageViewport` maintains `m_previewTex` (always present) and `m_fullResTex` (uploaded lazily when zoom crosses the point where preview pixels become visible). `paintGL` selects which to bind based on current zoom level. The full-res buffer is never uploaded until needed.

### Export

Export does not use `QImage` pixel manipulation. `ImageViewport::renderToImage()` uploads `m_fullRes` into an offscreen FBO, runs the full GLSL pipeline at full resolution, and calls `glReadPixels`. The result is tagged `QColorSpace::SRgb` before saving. This keeps the shader as the single source of truth for all adjustments.

### XMP sidecar

`XmpSidecar` reads/writes the `crs:` (Adobe Camera Raw) XML namespace so files are Lightroom-compatible. `crs:Temperature` is stored in absolute Kelvin (2000–12000K). Tint and all other fields use our internal -100..100 scale. Sidecar path = same directory as RAW file, same base name, `.xmp` extension.

### AdjustmentParams ↔ slider scale

`AdjustmentPanel` sliders work in integers for Qt's `QSlider`. The mapping:
- `exposure`: slider × 0.01 = EV (slider range -500..500 → -5.0..5.0 EV)
- `temperature`: slider value = Kelvin directly (range 2000..12000)
- All other fields: slider value = float value directly (-100..100)

When adding a new adjustment, update `AdjustmentParams` (struct), the slider in `AdjustmentPanel`, the GLSL uniform in `image.frag`, the `setUniformValue` block in `ImageViewport::paintGL()`, and `XmpSidecar` load/save.
