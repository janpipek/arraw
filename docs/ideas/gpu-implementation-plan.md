# GPU implementation plan

Status: proposed implementation sequence for the first GPU backend.

The current code already provides the correct division between preparation and
execution. `ProcessingPlan` resolves settings into concrete values, and
`developPixel()` is the readable CPU implementation of the pointwise chain. The
GPU work should preserve that contract rather than introduce a second settings
orchestration path.

## First milestone

Implement a GPU renderer for floating-point RGBA input that performs the
pointwise development chain, reads the result back, and compares it with the
CPU renderer. The first milestone should cover:

- source-to-working colour conversion;
- exposure;
- tone shaping;
- highlight roll-off;
- alpha preservation; and
- a CPU fallback when the GPU backend is unavailable.

Geometry follows as a separate pass. Demosaic, lens correction, spots, noise
reduction, and other spatial stages should be added only after this path is
correct and measurable.

## Backend boundary

Keep graphics resources out of the public API and out of `ImageBuffer`. Add an
internal renderer boundary so the CPU implementation remains usable without a
graphics context:

```cpp
class Renderer {
public:
    virtual ImageBuffer render(const ImageBuffer&, const ProcessingPlan&) = 0;
};
```

> **Note (2026-09-22).** ADR 015 supersedes this interface. It is right for the
> comparison milestone and wrong for everything after it: requiring CPU pixels
> on every call means a slider change reads a full-resolution texture back and
> uploads it again, and a viewport cannot present the result at all. A retained
> GPU result is a `RenderCheckpoint` whose payload is a device image, and the
> device that holds it travels with it. The rest of this document stands.

The initial GPU implementation should use QRhi and Qt Shader Tools, as the
project already targets Qt and the intended texture formats include RGBA16F.
QRhi types belong in the GPU/application layer; the core library should see
only ordinary C++ values and `ImageBuffer`.

If QRhi's availability or private API status is unsuitable for the selected Qt
version, Vulkan can implement the same boundary. That choice must not leak
into the processing plan or the public library types.

## Shader data contract

Do not upload `ProcessingPlan` directly. Its C++ layout contains padding,
`bool`, and `std::optional`, none of which should define a shader ABI. Create a
shader-facing POD with explicit scalar and matrix fields, for example:

```cpp
struct GpuProcessingPlan {
    Matrix3x4 toWorking;
    float exposureGain;
    float contrastSlope;
    float contrastScale;
    float shadowShift;
    float highlightShift;
    float blackShift;
    float whiteShift;
    float shoulderKnee;
    float shapesTone;
};
```

Serialize this structure explicitly, document its alignment, and test the
serialized values. The shader consumes resolved values only; it must not
reimplement `DevelopSettings` resolution.

## Processing passes

The first pipeline should be:

```text
input texture
    -> pointwise development pass
    -> geometry/crop/resample pass
    -> output texture
```

The pointwise pass uses a fullscreen triangle and a fragment shader that
mirrors `developPixel()` in exactly the same order. The geometry pass performs
the inverse mapping and bilinear premultiplied-alpha sampling currently in
`applyGeometry()`.

Keep geometry separate initially. It is a spatial operation and a useful cache
boundary; combining it prematurely would make numerical comparison and later
stage caching harder.

Internally, use four-channel floating-point textures. Convert integer input
formats at the upload boundary. The first implementation may accept only
`RgbF32` and `RgbaF32`, with other formats using the CPU path until upload
conversion is tested.

## Verification

> **Note (2026-09-23).** `arraw-cli gpu-test` is the probe ADR 015 refers to.
> It creates one backend's device with no fallback, refuses a software
> rasteriser unless `--allow-software` is given, and checks that an RGBA32F
> upload and readback is exact bit for bit. The suite cannot run it -- no runner
> is assumed to have a GPU -- so it is run by hand on each platform we claim.
> On Linux it needs no display session: unless `QT_QPA_PLATFORM` names another,
> the CLI runs on its own Qt platform, `arraw-headless` (`src/platform/headless/`,
> a static plugin whose one real job is a `QVulkanInstance` without surface
> extensions), because Qt's offscreen platform cannot create Vulkan instances at
> all. That platform has no OpenGL; `QT_QPA_PLATFORM=xcb` or `wayland` is how to
> probe OpenGL. `ARRAW_DISABLE_GPU` (any value but empty or `0`) keeps the
> CLI on a `QCoreApplication` and fails the probe. The separate
> `arraw-headless-tests` suite runs a Vulkan round trip on that platform, and
> skips it where the runner has no Vulkan driver.
> ADR 015's "`struct Impl` defined in the translation unit" is now
> `arraw::detail::DeviceImageState` in `src/gpu/DeviceImageState.h`, a Qt-free
> base whose QRhi-texture subclass lives in `src/gpu/GpuContext.cpp`.

The CPU renderer remains the reference implementation. Add small deterministic
tests that render the same input and plan through both backends and compare
every pixel with documented tolerances.

The test set should include:

- identity settings and colour matrices;
- exposure and each tone control;
- values above white and shoulder roll-off;
- RGB and RGBA input, including alpha;
- quarter-turns, crops, and bilinear resampling; and
- shader compilation as a build-time check.

Where practical, compare intermediate stage values as well as final output.
This isolates shader differences in `pow`, `sqrt`, matrix layout, and tone
ordering instead of leaving them as whole-image failures.

## Application integration

Integrate the GPU renderer into the application only after the standalone
renderer and comparison tests pass. The preview should retain GPU textures and
reuse prefixes of a plan when only later pointwise settings change. Readback to
`ImageBuffer` should be reserved for export, diagnostics, tests, or callers
that explicitly request CPU pixels.

The implementation sequence is therefore:

1. Define the renderer boundary and GPU resource lifetime.
2. Add explicit `ProcessingPlan` serialization for shaders.
3. Upload RGBA float input and implement the pointwise shader.
4. Add readback and CPU/GPU comparison tests.
5. Implement the geometry pass.
6. Add texture reuse and application preview integration.
7. Add further spatial stages one at a time, with each stage represented in the
   plan and tested against a CPU reference.

This keeps the CPU path complete throughout the work and gives every later GPU
stage the same plan, pass, and comparison discipline.
