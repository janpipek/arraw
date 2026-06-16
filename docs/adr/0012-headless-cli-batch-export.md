# Headless CLI batch export: windowless QRhi, the shader stays the one truth

A second executable (`arraw-cli`) renders RAW → export from the command line with
no window: decode, restore the sidecar, run the develop pipeline, write the file.
It reuses the *exact* code the GUI already ships — `RawProcessor::load`,
`XmpSidecar::load`, and `RendererCore::renderOffscreen` — so the [[CLI]] and the
app cannot drift. This is the door ADR 0006 deliberately left open ("windowless
renderer core … enables headless goldens and CLI batch export; the renderer core
keeps that door open"); we now walk through it.

The governing constraint is unchanged from ADR 0006: **one shader is the single
source of truth for the algorithm.** The CLI must therefore run the same
`image.frag`/`image.vert` on a GPU, not a CPU re-implementation of the pipeline.
That single fact decides everything below.

- **Qt stays; QtWidgets goes.** The processing subset needs QtGui (QRhi, QImage,
  QColorSpace), QtCore, and QtConcurrent — not QtWidgets. So we split `arraw_core`
  into **`arraw_engine`** (decode, `AdjustmentParams`, `RendererCore`,
  `ColorManagement`, `XmpSidecar`, `ImagePipeline` — no Widgets) and the UI layer
  (`MainWindow`, panels, `ImageViewport`) that links it. The CLI and the tests
  link only `arraw_engine`.
- **One device bootstrap, two callers.** RHI device creation is extracted out of
  `ImageViewport` (which today does `core.initialize(rhi())`, borrowing the
  widget's device) into a reusable **[[Headless Render Context]]** — a `QRhi`
  over a `QOffscreenSurface` (GLES2) or the platform headless Vulkan/Metal path.
  The widget keeps using the device `QRhiWidget` hands it; the CLI creates a
  headless one. `RendererCore` is unchanged: it already takes a `QRhi*` and never
  knew about the widget.

## Considered Options

- **No Qt at all (fully dependency-free CLI).** Rejected. Removing Qt removes
  QRhi, which *is* how the algorithm runs. The only two exits both lose: a CPU
  re-implementation of the pipeline forks the algorithm into a second copy that
  must stay byte-identical to the shader forever (violates the SPOT rule that
  governs this codebase, see [[spot-for-algorithms]] / ADR 0006); or a
  from-scratch Vulkan/GL substrate re-creating device, pipeline, upload, readback,
  and the `qsb` shader bake by hand — a rewrite of the rendering substrate to buy
  a smaller binary. No requirement justifies either.
- **CLI links the whole `arraw_core` (no library split).** Smallest diff, but the
  CLI then drags in QtWidgets and the entire UI for a batch tool. The split is
  cheap (move files between two `add_library` targets, no code change) and pays
  off as a clean engine/UI boundary the tests already want.
- **Reuse the test harness's "spin a hidden widget for a live device" trick.**
  The golden tests get their `QRhi` by realising a `QApplication` + shown
  `ImageViewport`. Convenient, but it pulls QtWidgets and an event loop into a
  headless tool and keeps device creation tangled in the widget. The
  [[Headless Render Context]] supersedes it — and the goldens can adopt it next,
  dropping their hidden-widget dance.

## Consequences

- **Runtime needs a GPU or a software rasteriser.** On a headless server that
  means llvmpipe/Mesa (or `-platform offscreen`). This is the real operational
  cost, not a code cost — and it is the same path the offscreen golden tests
  already exercise, so it is known-good.
- **`arraw-cli` is a `QGuiApplication`, not `QApplication`** — no widgets, no
  Qt event loop beyond what offscreen frames need. It links `arraw_engine` only.
- **The engine/UI library split is the bulk of the work** and is mechanical:
  reassign sources between targets, fix include directions so the engine never
  includes UI. `RendererCore::renderOffscreen` and `RawProcessor::load` need no
  change; the only new processing code is the [[Headless Render Context]] and the
  CLI's argument handling.
- **CLI surface is intentionally minimal in v1**: `arraw-cli <raw> [-o out.jpg]`,
  reading the adjacent `.xmp` sidecar if present (identical resolution to the
  GUI). Batch/glob, format/quality flags, and override flags are follow-ons, not
  v1 — and batch export also surfaces at the GUI level in Milestone 10.
- **Readback row order and colour tagging are already handled** by
  `renderOffscreen` (Y-flip per `QRhi::isYUpInFramebuffer()`, output tagged
  `QColorSpace::SRgb`); the CLI inherits both for free.
