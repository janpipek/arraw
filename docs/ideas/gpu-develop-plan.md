# Developing on the GPU — execution plan

Status: proposed, for review before any code is written. This is the concrete
follow-up to [gpu-implementation-plan.md](gpu-implementation-plan.md), within
the boundary [ADR 015](../adr/015-a-checkpoints-pixels-may-live-on-a-device.md)
decided.

## Goal

Everything `arraw-cli export` does to pixels today can run on the GPU, and the
CLI prefers the GPU but lets the user choose.

"Everything export does" is exactly what `arraw::develop()` does
(`src/core/Develop.cpp`):

| Step | CPU reference | GPU today |
|---|---|---|
| Sample conversion (u8/u16/f32, RGB/RGBA → RGBA float) | `toUnit` in `developSamples` | upload takes `RgbaF32` only |
| Source → working colour matrix, including white balance | `developPixel`: `plan.toWorking` | — |
| Exposure gain | `developPixel` | — |
| Tone: contrast, shadows, highlights, blacks, whites | `shapeTone` / `shapeLuminance` | — |
| Highlight shoulder and chroma fade | `rollHighlights` | — |
| Alpha passes through unchanged | `developSamples` | — |
| Camera orientation, quarter-turns, straighten, flips, crop, bilinear premultiplied resample | `applyGeometry` / `sample` | — |

Decoding (LibRaw, Qt codecs), planning (`planFor`) and encoding/writing the
output file (`exportImage`) stay on the CPU. Planning is per photograph, not
per pixel. Decoding and encoding are I/O-bound libraries.

Not in scope: new photographic features, `RenderRequest::targetSize`, and the
GUI preview. The app follows once this works, as the implementation plan's
step 6.

## Precondition

A session that can build and run Vulkan: a compiler, CMake, Qt 6.10 with
ShaderTools, LibRaw, Catch2, the Vulkan loader and headers, and lavapipe at
least. The current agent container has none of these (see
[dev-container-plan.md](dev-container-plan.md)). Nothing below should be
attempted without it, because every step ends in "build and compare".

## Design decisions

Each has a recommendation. The alternatives are what a reviewer should push on.

### D1. Where input conversion happens

GPU upload accepts `RgbaF32` only. RAW import produces `RgbaU16`; Qt decoding
produces 8-bit, 16-bit or float, with and without alpha.

- **(a) Convert on the CPU to `RgbaF32`, then upload (recommended first).**
  This reuses `toUnit` exactly, so there is one conversion and nothing new to
  get wrong. It costs a CPU pass and 4× the upload size for 16-bit input
  (about 380 MB for 24 MP).
- (b) Upload the native layout and convert in the shader. This is faster and
  smaller, but QRhi appears to have no RGBA16 unorm texture format (check this on the
  pinned Qt), and
  3-channel formats have no texture format at all. It becomes a later
  optimisation, measured against (a).

### D2. Fragment or compute shaders

- **(a) A fragment shader on a fullscreen triangle into an RGBA32F render target
  (recommended).** This is what the implementation plan and ADR 011 describe.
  It works on every QRhi backend, including OpenGL, and the pass is naturally
  one output pixel per invocation.
- (b) Compute with load/store images. It needs `GpuDeviceInfo::compute` and
  image load/store of `rgba32f` (already requested at upload). It will matter
  later for histograms and spatial filters, but has no advantage for these two
  passes.

### D3. Passes

Two passes, matching the two `Stage` boundaries that exist:

1. **Pointwise**: input texture → developed texture, same size.
   `Stage::Pointwise`.
2. **Geometry**: developed texture → output texture, `plan.geometry->outputSize`.
   `Stage::Geometry`. It is skipped under the same identity condition as
   `applyGeometry`, so identity output is the developed texture itself.

They are kept separate rather than fused, as the implementation plan argues: a
cache boundary, a place to compare numbers, and what ADR 015's checkpoints
already name.

### D4. Shader data contract

A new `GpuPlan` in `src/gpu/GpuPlan.h`, packed from `ProcessingPlan` by one
function, with an explicit std140 layout. It is never `memcpy`'d from
`ProcessingPlan`.

```text
Pointwise block (std140)
  vec4  toWorking[3]      // rows of the 3x3; .w unused
  float exposureGain
  float contrastSlope, contrastScale
  float shadowShift, highlightShift, blackShift, whiteShift
  float shoulderKnee
  uint  shapesTone        // 0/1; bool has no portable std140 layout
  uint  rollsHighlights   // 0/1; see below

Geometry block (std140)
  vec4  matrix            // row-major 2x2
  vec2  sourceSize, uprightSize
  vec4  crop              // left, top, width, height
  uvec2 outputSize
```

- `shoulderKnee` is `+inf` when there is no roll-off. Some drivers flush
  infinities or compile comparisons with them oddly, so the shader gets an
  explicit `rollsHighlights` flag. The packing function derives it as
  `std::isfinite(knee)`, and the shader's comparison matches the CPU's
  `!(luminance > knee)`, so NaN input behaves the same way.
- Geometry is `double` on the CPU and `float` in the shader. At 8000 px, float
  coordinates are good to about 1e-3 px. That is a documented tolerance, not a
  bug. The two exact paths are special-cased so they stay bit-exact: the
  identity skip, and exact quarter-turns with pixel-aligned crops, where
  `sample` snaps to pixel centres and copies.
- Bilinear sampling is done by hand with `texelFetch`: clamp-to-edge,
  premultiplied weights, division by summed alpha, zero when alpha is zero.
  Hardware filtering is not used, because it would not match CPU clamping or
  premultiplication, and float filtering is optional on some devices.

### D5. Shaders and their build

- GLSL 440 sources in `src/gpu/shaders/`: `fullscreen.vert`, `develop.frag`
  and `geometry.frag`. They are compiled at build time with `qt_add_shaders`
  (ShaderTools is already a required component) into `.qsb` resources linked
  into `arraw-gpu`. A shader that does not compile fails the build, which is
  the build-time check the implementation plan asks for.
- `develop.frag` mirrors `developPixel` in the same order, with the same
  helper names (`shapeLuminance`, `shadowWeight`, …). The C++ is annotated to
  say the shader must change with it.

### D6. Engine-side API

This stays out of `include/` for now, because `GpuContext` itself is internal.

```cpp
// src/gpu/GpuDevelop.h
namespace arraw {
/// Develops a decoded photograph on a device, stopping after @p stopAfter.
[[nodiscard]] RenderCheckpoint developOnGpu(GpuContext& context, const ImageBuffer& source,
                                            const DevelopSettings& settings,
                                            Stage stopAfter = Stage::Geometry);
}
```

- It returns a resident `RenderCheckpoint` (payload `DeviceImage`) built with
  `makeCheckpoint`, so the existing prefix rule applies. Export calls
  `readBack()`, then `exportImage`.
- It throws on anything it cannot do: a texture too large, no float render
  targets, or a lost device. It never falls back itself; fallback is the
  caller's policy (ADR 015: "fallback must not be able to hide a missing GPU").
- `GpuContext` gains what the passes need: pipelines and shader resources
  created once per device and cached in `detail::GpuDevice`, render-target
  textures (`QRhiTexture::RenderTarget | UsedAsTransferSource`), and a private
  way to mint a `DeviceImage` from a pass's output texture. `DeviceImage`'s
  constructor stays private, and `GpuContext` stays its only minter.
- CPU `develop()` is unchanged. A matching CPU `developToCheckpoint` is not
  needed yet; add one only when there is a caller.

Alternative: a public `RenderDevice`/`Renderer` choice in `Develop.h`, for the
future scripting API. Rejected for now. It would put device lifetime (one
owner thread, destroy on that thread) into the public contract before anyone
has needed it. See Q1.

### D7. CLI: choosing the device

New `export` options:

| Option | Values | Default |
|---|---|---|
| `--device` | `auto`, `gpu`, `cpu` | `auto` |
| `--gpu-backend` | `vulkan`, `opengl`, `d3d11`, `d3d12`, `metal` | platform default (`defaultGpuBackend()`) |
| `--allow-software` | flag: accept llvmpipe, lavapipe or WARP | off |

The flags are separate because "which device" and "which API" are different
questions. `gpu-test --backend` keeps its name; renaming it to `--gpu-backend`
for consistency is Q3.

What each mode does:

- **`auto`**: create one `GpuContext` for the whole batch. If creation fails,
  the device is a software rasteriser (without `--allow-software`), or
  `ARRAW_DISABLE_GPU` is set, log one warning (`Notice::GpuFallback`, reason
  attached) and export the whole batch on the CPU. If a GPU develop fails for
  one input (for example, larger than the device's texture limit), that input
  is retried on the CPU with a warning. If the device is lost, the rest of the
  batch continues on the CPU.
- **`gpu`**: no fallback. Context failure is `Failed` (exit 1) before any
  input. A per-input GPU failure fails that input, as any other export failure
  does. `ARRAW_DISABLE_GPU` together with `--device gpu` is a usage error
  (exit 2).
- **`cpu`**: today's behaviour. It asks only for `ApplicationKind::Core`, so no
  platform plugin is loaded.

Also:

- The application kind follows the mode. `cpu`, or `auto` with the GPU
  disabled, gets `Core`; otherwise `Gui`. This keeps the existing guarantee
  that help and usage errors load no platform (the CTest cases in
  `tests/CMakeLists.txt`).
- One info notice per batch says which device was used (backend and device
  name), so a user can tell whether they got the GPU. It is quiet under
  `--quiet`, and structured under `--log-format json`.
- An `ARRAW_DEVICE` environment variable as a persistent default is Q2.

### D8. Threading

The CLI is single-threaded. The context is created, used and destroyed on the
main thread, and every `DeviceImage` is released before the context goes, as
`GpuContext`'s contract requires. Nothing here adds threads. Parallel decode
alongside GPU develop is a later performance step.

## Verification

### Where GPU tests run

`arraw-tests` runs inside a `QCoreApplication`, so it cannot create a device.
The options are:

- **(a) A new `arraw-gpu-tests` executable (recommended)**, on the headless
  platform like `arraw-headless-tests`. It skips (exit 4) when no Vulkan device
  exists, and runs with lavapipe otherwise, so CI can run it after installing
  `mesa-vulkan-drivers`. It is Linux-only at first, like the platform.
- (b) Add the cases to `arraw-headless-tests`. That is less CMake, but mixes
  "does the platform work" with "is the GPU develop right".

### What is compared

These run on both backends, from the same `ImageBuffer` and `DevelopSettings`,
and compare every sample:

1. Identity settings, RGB and RGBA, for u8, u16 and f32 input.
2. Exposure ±, each tone control at its extremes and combined, values above
   white, negatives from the camera matrix, zero and near-zero luminance (the
   "lifted black" branch).
3. Shoulder on and off (`filmicHighlights` 0 and 100), including NaN and inf
   input where the CPU defines the behaviour.
4. RAW fixtures (`tests/fixtures/*.dng`) with as-shot and custom white balance.
5. Geometry: every `ImageOrientation`, quarter-turns (bit-exact), straighten
   ±45°, flips, auto crop, explicit crop, locked aspect, and 1×N and N×1
   images.
6. Checkpoints: `stopAfter = Pointwise` gives a resident checkpoint of the
   source size. `prefixMatches` agrees between CPU and GPU plans.

Stage values are compared as well as final output where practical. A debug
uniform (`stopAt`: after matrix, after exposure, after tone) makes the
pointwise shader write an intermediate. That is ADR 011's "branch on a
uniform", and it tells us which of `pow`, the matrix layout or the tone order
disagrees.

### Tolerances

These are to be measured, then fixed in `tests/support/ImageCompare.h` with a
comment giving the reason:

- Pointwise: max relative error of about 1e-5, absolute 1e-6 near zero. GLSL
  `pow` is only loosely specified (Vulkan allows several ULP), and the chain
  uses it twice.
- Geometry resample: about 1e-4 relative. Float coordinates and weights vs
  double.
- Exact paths (identity, quarter-turn copy, alpha): bit-exact.
- Exported 8-bit and 16-bit files, CPU vs GPU: at most one code value apart.

### CLI tests

These go in `arraw-tests` and need no GPU:

- Parsing of the three new options, the usage errors, and `ARRAW_DISABLE_GPU`
  with `gpu` (exit 2) and with `auto` (CPU, with a warning).
- The `start` callback receives `Core` for `--device cpu`, and for `auto` when
  the GPU is disabled.

And one CTest case on the real binary, in the GPU suite: `export --device gpu`
and `--device cpu` on a fixture produce files within one code value.

### Manual

`arraw-cli gpu-test` on real hardware, then `export --device gpu` on a real
shoot. Record the timings (CPU vs GPU per image) in the review that closes this
work. Performance is not claimed from lavapipe.

## Steps

Each step ends with `just build`, `just test` and `just format-check` passing.

1. **Plan packing.** Add `GpuPlan`, the std140 packing function and unit tests
   of the packed bytes (no device needed). Add the `rollsHighlights` rule.
2. **Pass infrastructure.** Add `qt_add_shaders` wiring, a pipeline cache in
   `GpuDevice`, render-target textures, and minting a `DeviceImage` from a pass
   output. Prove it with a copy shader that must round-trip bit-exact, which
   extends the `gpu-test` invariant from upload/readback to a render.
3. **Pointwise pass.** Add `develop.frag`, `developOnGpu(…, Stage::Pointwise)`,
   the CPU-side conversion (D1a), and comparison tests 1–4 plus the stage
   probe.
4. **Geometry pass.** Add `geometry.frag`, `developOnGpu(…, Stage::Geometry)`,
   and comparison tests 5–6.
5. **CLI.** Add `--device`, `--gpu-backend` and `--allow-software`, the batch
   context, the fallback policy, the new notices and the CLI tests.
6. **Docs.** Write ADR 016 ("the command line prefers the GPU and says when it
   does not") covering D7 and the fallback policy. Update ADR 006 (command
   line), the README usage, `export --help`, and the implementation plan's
   notes. Add `mesa-vulkan-drivers` and the GPU test executable to CI.
7. **Review.** Write a review to `docs/reviews/`, focusing on shader and C++
   divergence, thread and lifetime rules, and fallback honesty.

## Running it as a workflow

Steps 1–2 are foundations, so they run in order with one agent each. After
that the work fans out:

```text
[1 plan packing] → [2 pass infrastructure] ─┬─ [3 pointwise pass + tests] ─┐
                                            ├─ [4 geometry pass + tests]  ─┼─ [integrate: developOnGpu end to end]
                                            └─ [5 CLI options + policy]   ─┘            │
                                                                              [verify: 2 adversarial reviewers]
                                                                              (shader/CPU parity; lifetime + fallback)
                                                                                         │
                                                                                     [6 docs]
```

- Steps 3, 4 and 5 touch disjoint files (`develop.frag`, `geometry.frag`,
  `src/cli/`), so they run in parallel. Each gets its own worktree, and they
  are merged at integrate.
- Every agent must build and test before reporting. A result is accepted only
  with its `ctest` output attached.
- Verifiers do not fix anything. Their findings return to the owning step's
  agent.
- The total is about 8 agents, within the configured medium size.

## Open questions

- **Q1.** Should GPU development become public API now (a device choice on
  `develop`, for the future Python/Lua bindings), or stay internal until the
  scripting work needs it?
- **Q2.** Should there be an `ARRAW_DEVICE` environment variable (or a config
  file) as a persistent default, beside `--device`?
- **Q3.** Should `gpu-test --backend` be renamed to `--gpu-backend` for
  consistency, keeping the old name as an alias?
- **Q4.** In `auto`, should a software rasteriser mean falling back to the CPU
  (recommended: it is usually slower than the CPU path), or using it?
- **Q5.** Is the pointwise tolerance in Tolerances acceptable, or must CPU and
  GPU agree more tightly? That would rule out native `pow`, and the shader
  would have to reproduce the C library's `powf`.
- **Q6.** Should geometry keep one bilinear resample, as today, or is this the
  moment to decide on a better downsampling filter for large straightens?
  (Recommended: keep it, and change both backends together later.)
