# A render runs one stage table and keeps its checkpoints on a ladder

[ADR 011](011-a-plan-spatial-passes-and-one-pointwise-chain.md) decided that a
render stops and resumes at pass boundaries, and [ADR 024](024-renders-stop-and-resume-at-pass-boundaries.md)
that the preview keeps the checkpoints. Both were built twice: the CPU
(`Develop.cpp`) and the GPU (`GpuDevelop.cpp`) each had a driver with the
skip rule of every pass written out, three more per-stage switches lived in
`RenderProgress.cpp` and `RenderCheckpoint.cpp`, and the rung policy (deepest
first, a bad request told apart from a stale rung) lived in the app, in a
template with three lambdas that chained four or five public calls, each
planning again and opening its own progress root. This ADR records the
restructuring that ends that, which changes no pixel. It is step 1 of the
[local adjustment plan](../ideas/local-adjustment-plan.md); the design is in
[the pipeline restructuring plan](../ideas/pipeline-restructuring-plan.md).

## Decision

### 1. One stage table

`src/core/StageTable.h` holds one `StageRow` per `Stage`, in pipeline order
(a `static_assert` pins that row *i* is stage *i*): the first and last
progress step the pass runs, `runs(plan)` (whether the pass runs or its
boundary collapses onto the one before), and `sizeAt(plan, source)` (the size
of the pixels at the boundary). The plan block a stage reads is `stagesOf`,
which is already one entry per stage. `stepAfter`, `stepThrough` and the size
check of `staleReason` are reads of the table. What each backend calls to run
a pass is one `switch (stage)` in the backend, because core does not link the
GPU; a stage without its case is a compiler warning on both.

### 2. One driver

`src/core/StageDriver.h` has `runStages<B>`, a template over a backend with
three functions (`run`, `checkpoint`, `borrow`) and its own `Pixels`: on the
CPU `HostPixels` (a borrowed buffer, the source or a rung's, or an owned one),
on the GPU `DeviceImage`. The two cannot share a base: a buffer is move-only
and may be borrowed, an image is a shared handle. The loop runs the passes
after a boundary up to another, skips those whose row says they collapse, and,
with a ladder, stores a rung at each boundary passed before the last. Every
entry point goes through it: `develop`, `developUntil`, `resumeFrom`,
`sample`, `resumeOrDevelop`, the `developOnGpu` overloads, `sampleOnGpu` and
`resumeOrDevelopOnGpu`. The CPU's copy counts are what they were: a pass that
only reads (Denoise, the pointwise chain) reads a borrowed buffer in place; a
pass that consumes one (Geometry, Resize, Effects) clones it once.

### 3. The checkpoint ladder

`CheckpointLadder` (`include/CheckpointLadder.h`) holds one rung per boundary
of one source, and is public because the verbs that take it,
`resumeOrDevelop` and `resumeOrDevelopOnGpu`, sit beside `develop` and
`developOnGpu`. The engine decides everything about it; the caller holds it
and clears it when something the plan cannot see changes.

- **Plan first.** The render is planned once, with the whole request, before
  the ladder is touched: a bad request throws and drops nothing.
- **Bound to a source buffer**, which the ladder keeps alive so that its
  address identifies it (until the plan has the decode block of
  [ADR 012](012-a-render-resolves-from-a-photo-and-a-request.md)). Another
  buffer clears the rungs.
- **Deepest first.** From Resize down to Denoise, a rung that is on the wrong
  backend (a host rung to the GPU entry, a resident one to the CPU entry, or
  one of another device) or that `staleReason` refuses is dropped; the first
  that passes is resumed from. A rung's stored plan is the whole plan of the
  render that made it; only its prefix is compared.
- **Which rungs.** Every boundary a render passes before the last, except a
  Denoise that collapsed (it would be a copy of the source the caller keeps).
  A collapsed Geometry is kept: a viewport change resumes from it whether or
  not a geometry is set. The Effects result is returned, never kept.
- **Failure and cancellation.** A rung is stored only after its pass
  returned, so a render that fails or is cancelled leaves the rungs it
  finished and no partial one ([ADR 042](042-a-decode-or-a-render-reports-progress-and-stops-on-request.md)).
- **Progress.** One root from the resumed rung's share to the end, instead of
  one per call of a chain: the same 0 to 1, monotone, with fewer end-of-call
  reports.
- **Threads.** Not thread-safe. A ladder with resident rungs belongs to the
  thread that owns its device: it is cleared and destroyed there ([ADR
  015](015-a-checkpoints-pixels-may-live-on-a-device.md)).

`PreviewRenderer` is the only user, with four ladders as it had four caches
(CPU shown, CPU background, GPU shown, GPU background); `CheckpointCache` is
deleted. Exports, the CLI, thumbnails and Python render once, through the same
driver with no ladder.

## Consequences

- Skip rules, step ranges and expected sizes are written once. A new pass
  (ADR 012's decode, lens, spots) adds a stage, a table row, a plan block and
  one `case` per backend.
- Mask edits (ADR 044) need nothing here: they change the pointwise block, so
  they resume from the Denoise rung by the prefix rule, or from the source when
  noise reduction is off (the same cost: a collapsed Denoise reads the source
  without a copy).
- A request with a bad region, rendered with no rungs held, now stores none
  before failing; before, it stored the Pointwise and Geometry rungs first.
  The error is the same.
- An identity Geometry or Resize is skipped by the driver, so a render with no
  rotation or resizing has no `cpu.geometry` or `cpu.resize` span; pixels and
  progress are unchanged.
- The `cpu.pointwise` timing span now covers the pointwise pass alone; it
  included the Denoise pass of the same render before.
- Verified by the render digest (`tests/test_RenderDigest.cpp`,
  `tests/gpu/test_GpuRenderDigest.cpp`, hidden behind `[.digest]`): the
  SHA-256 of every public render path on both backends is identical before and
  after. `tests/test_CheckpointLadder.cpp` and `tests/gpu/test_GpuLadder.cpp`
  pin the ladder's rules, and `test_PreviewRenderer` is unchanged.

## What was rejected, and why

- **A virtual backend interface over `CheckpointPixels`.** Every stage would
  unwrap a variant, and the CPU's borrowed source (any layout, not a
  checkpoint) has no place in it. One template with a three-function concept
  is smaller.
- **One table holding function pointers for both backends.** Core would have
  to name GPU functions; that needs the device seam of the architecture
  roadmap. Shared rows plus one `switch` per backend give the same single
  place for the skip rule.
- **A private ladder.** The app may include `src/core`, but a public type
  keeps Python and the services layer open.
- **Keeping the policy in the app.** It chained four or five public calls per
  render, each planning again, and every second client would have copied it.
