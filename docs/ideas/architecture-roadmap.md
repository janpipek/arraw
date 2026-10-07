# Architecture roadmap — plan

Status: accepted direction, 2026-10-07. Each step below gets its own detailed
plan before code, as the earlier `*-plan.md` files did.

## Why

The architecture reassessment
(`docs/reviews/claude-opus-5-5_2026-10-07_architecture.md`) found the engine
sound (one plan, descriptors, value-type documents) and the layer above it
missing. §4 of the reimplementation plan put an *application interface*
between the front ends and the engine, and it was never built. So the GUI, the
CLI and Python each carry their own edit rules, export orchestration and
GPU/CPU policy, and the copies already disagree. For example, a turn with a
crop carries the crop in the GUI, resets it in the CLI, and does neither in
Python. The pipeline takes new settings easily, but not new passes or masks.

## Decisions (user, 2026-10-07)

| Question | Answer |
|---|---|
| Where the shared application layer lives | **Inside core `arraw`**, behind a public device interface in `include/` that `arraw-gpu` implements. Core does not link the GPU; whoever creates a device hands it in. ADR 018 stands. |
| Python and the GPU | **Python gets the GPU through that layer, but only if PyQt/PySide can be avoided.** The module creates its own `QGuiApplication` (offscreen platform) when none exists. Open points: the wheel's size (Qt Gui, the offscreen plugin, RHI); creating the application on the main thread; falling back to the CPU when a host already has a plain `QCoreApplication`; whether Vulkan needs the Gui application at all. |
| Windows and the GPU | **CPU by default on Windows for now** (app and CLI `auto`), until D3D11 has run the parity suite. |
| Features next | **All three, in this order:** (a) presets, copy/paste and a session history view; (b) batch export; (c) local adjustments. Each comes after the restructuring it needs. |
| Snapshots | **Deferred, but the design must not make them hard.** |

## Steps

### 0. Small fixes from the review (in progress)

- the CLI overwrite and collision bug;
- the `canResumeFrom` query;
- private headers kept out of the front ends;
- the shader-layout reflection test and shared GLSL includes;
- dropping Qt Concurrent;
- Windows UTF-8 paths and manifest;
- test labels and a parallel test run;
- pytest in CI;
- `MainWindow` tidy-ups;
- ADR 015, 018 and 019 wording.

Then the Windows CPU default (decision above).

### (a) Presets, copy/paste, session history

Restructuring first:
- **Library-owned edit operations,** a public `Edits.h` of pure state-to-state
  functions:
  - setting a value with its compound rules: temperature or tint makes the
    white balance Custom, and turning grain on reseeds it;
  - turning, flipping and straightening, with the crop carried. The geometry
    rules of `CropEditing` move from app-core into core; its gestures stay in
    the app;
  - applying a look by setting groups, respecting `SettingScope::Photo`.
- **The GUI, the CLI, Python and `EditSession` all call them.** The CLI carries
  the crop as the GUI does instead of resetting it.

Then the features:
- **Presets and copy/paste:** settings by group, the presets stored as files.
  The detailed plan decides where and in what format.
- **Session history view:** present it as `origin/main` does (its ADR 0038 and
  `HistoryPanel`):
  - a dock lists the edit steps, newest on top and the opening state at the
    bottom;
  - the current step is highlighted, and a click returns to a step;
  - the list is not persisted and resets when a photograph opens.

  Unlike `main`, the list is drawn from the library's `EditSession` history,
  not from a `QUndoStack` in the window. Each step gets a name the library can
  give ("Exposure +0.5", "Crop"), so the CLI and Python can list history too.
- **Room for Snapshots:**
  - history steps stay whole develop states, so a snapshot is a named whole
    state;
  - returning to a step, or later restoring a snapshot, is one undoable change
    of the whole state;
  - the dock leaves room for a Snapshots list beneath History;
  - persisting snapshots waits for the structured sidecar format of step (c).

### (b) Batch export

Restructuring first, with an ADR:
- **The shared layer inside core:**
  - a concrete render or device facade: lazy context, a software-renderer
    policy, CPU fallback on loss, one environment switch;
  - an export batch: inputs and partial edits, request, options, metadata,
    naming, a `CollisionPolicy` and output preflight. It reports progress and
    returns per-file results.
- **The CLI, `ExportQueue` and `PreviewRenderer` use it.**
- **Python gets GPU export through it,** under the decision above.

Then the feature: batch export from the GUI's selection, and Python batch.

### (c) Local adjustments

Restructuring first:
- one stage table and one generic driver for the CPU and the GPU, and an
  engine-owned `CheckpointLadder`;
- `PointwisePlan` and `TonePlan` extracted, with the per-pixel chain taking
  sub-blocks;
- `PreviewPipeline` and `CropModeController` out of `MainWindow`, on the way to a
  widget-free document controller;
- a masks ADR choosing how masks reach the chain. The review leans to a
  mask-weight context pass, like Presence;
- the same ADR covers the structured sidecar content (lists with stable ids)
  that masks and Snapshots both need.

Then the feature: up to 16 masked adjustments (linear, radial, brush), as
`docs/desired-features.md` describes.

## Not decided here

- The presets' file format and location, and the copy/paste dialog: the
  detailed plan of (a).
- The services API in detail: the ADR of (b).
- The masks pipeline and the sidecar list format: the ADR of (c).
- GPU CI on Windows (WARP) and macOS: after (a), or when Windows GPU support is
  wanted.
