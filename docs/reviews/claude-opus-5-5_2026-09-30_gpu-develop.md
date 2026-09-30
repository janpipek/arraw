**GPU develop — implementation review, 2026-09-30**

Closes step 7 of [the GPU develop plan](../ideas/gpu-develop-plan.md). Covers
branch `gpu-develop`, `326f4fd..236da42` (14 commits, 45 files, about +3.9k lines).
The plan's lenses were shader/C++ divergence, thread and lifetime rules, and
fallback honesty. Steps 1–2 were written by Opus. Steps 3–5, the fixes and the
docs were written by Sonnet agents in parallel worktrees, each checked by an
independent adversarial Opus reviewer that probed with throwaway tests and
mutations. Every finding below was reproduced before it was fixed.

State at `236da42`: `just test` 248/248 on lavapipe, `just format-check`
clean.

## What holds

- **Pointwise parity.** `develop.frag` mirrors `developPixel` stage by stage.
  Mutation testing caught a changed luminance coefficient, swapped
  Shadows/Highlights, a 1e-5 exponent error, a 0.1 % chroma change, a 1e-4
  matrix change and a forced shoulder. Swapping Blacks and Whites was not
  caught, but that swap is genuinely equivalent: their windows do not overlap
  within the reach of the controls. Alpha is bit-exact. Identity settings are
  bit-exact, negatives included.
- **Exact geometry.** All eight orientations, the quarter-turns, flips,
  pixel-aligned crops and 1×N/N×1 images are bit-exact.
- **Packing.** The std140 offsets are asserted in C++ and match the generated
  HLSL `packoffset`s.
- **Lifetimes.** Destruction order is correct: pipeline, then layout, then
  render pass, then sampler, then QRhi, then abandoned readbacks and the
  instance. Per-render targets and SRBs die before their textures. Export
  releases every `DeviceImage` before its context, on the main thread (D8).
- **Fallback policy (D7).** Each mode was checked by running the real binary:
  `auto` warns once and falls back, `gpu` never falls back, `cpu` loads no
  platform, `ARRAW_DISABLE_GPU` with `--device gpu` exits 2, a software
  rasteriser is refused in `auto` without `--allow-software`, and `--quiet` and
  JSON logging behave.
- **Size limits.** A per-input failure (an image one texel wider than the
  largest texture) is retried on the CPU in `auto` and fails the input in
  `gpu`. Both are tested.

## Found and fixed during the work

1. **Geometry precision at photo sizes (major).** Positions were evaluated in
   float, so the error grew with the source size: about 5e-4 px at 6000 px,
   which is 1e-2 relative and tens of 16-bit codes at edges. Every test image
   was 1000 px or smaller, so none of them showed it. The geometry block is now
   an exact split. The origin is carried as an integer part plus a fraction,
   and each step as a 9-bit high part plus a low remainder, so
   `(x+0.5)·high` is exact for x < 2^14. Measured at 6000×64: at most 2.2e-6
   relative. `packGeometry` refuses outputs larger than 16384 px per side.
2. **GPU-only texel snap (minor).** It copied a transparent texel's colour
   where the CPU blends, and removing it failed no test. It is gone, and a
   stripe test covers that case.
3. **Lifted black under denormal flushing (minor).** GPUs flush denormals, so
   the CPU and GPU took different branches for tiny luminance. Both now use
   `liftedBlackThreshold = 1e-20`. **This changes the CPU reference** for
   luminance at or below 1e-20; nothing a camera produces is affected.
4. **Decode failures blamed on the GPU (major).** A file that failed to decode
   was reported as `gpu_fallback` and decoded twice. It is now decoded and
   planned on the host first, and only `developOnGpu` and the readback sit
   inside the fallback.
5. **`auto` export aborted on an unusable `QT_QPA_PLATFORM` (major).** For
   example, xcb with no display exited 134, where the old CPU export had
   worked. Export now always uses arraw's headless platform, except with
   `--gpu-backend opengl`. A CTest case on the real binary pins this.
6. **Device loss missed (minor).** A loss reported when a frame began, when it
   ended during unwinding, or through `QRhi::isDeviceLost()` did not mark the
   device lost. All three now do.

## Open

1. **Only Vulkan is verified (medium).** Every tolerance, and the NaN/inf
   parity, was measured on lavapipe alone. The HLSL and MSL compilers may use
   fast-math that folds `!(x > k)` or changes how `pow` rounds. OpenGL
   (llvmpipe through xcb), D3D and Metal have never run the passes.
   Solutions: (a) run `arraw-gpu-tests` against OpenGL in CI under Xvfb, and
   on Windows (WARP) and macOS runners; (b) make the input finite before
   development on both backends, so that no guarantee depends on how a
   compiler treats NaN.
2. **Pointwise tolerance is looser than planned (low).** The plan asked for
   1e-5; it is 3e-5 (floor 1e-5). The metric is relative to each pixel's
   largest channel rather than to each sample, because a channel near zero
   after cancellation otherwise reads as a large error. It still catches every
   meaningful mutation that was tried. Accept it, or reproduce `powf` in the
   shader (Q5, rejected).
3. **Unproven regression tests (low).** The new 6000 px and stripe tests were
   not run against the pre-fix code to watch them fail. The reviewer's
   measurements came from separate probes, not these tests. Solution: run each
   test once against `27a0629`'s shader.
4. **Untested paths (low).** The device-lost branch of export, and item 6's
   comparison of checkpoint prefixes between CPU and GPU, have no tests. The
   first would need a hook in production code, and the second an engine-private
   way to read a checkpoint's plan.
5. **The 16384 px cap is a new limit (low).** Outputs wider or taller than
   16384 px now go to the CPU in `auto` and fail in `gpu`, even on devices that
   accept larger textures. Solution: a third split level, or a tiled render.
6. **No performance numbers yet.** Step "Manual" still needs a run of
   `gpu-test` and `export --device gpu` on real hardware, with CPU and GPU
   timings recorded here.

## Verdict

Ready to merge into `rewrite` once open item 1 is either accepted as a known
limit (ADR 017 and the plan already state it) or covered by an OpenGL CI job.
Items 2–6 can follow.
