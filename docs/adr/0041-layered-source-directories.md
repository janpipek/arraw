# Layered source directories with the application shell at the root

The codebase grew to ~60 files sitting flat in `src/`. A newcomer (or the author
six months on) could not tell at a glance that `BasicTone`, `OkLab`, and
`RawProcessor` are processing internals while `AdjustmentPanel` and
`ProofingPanel` are UI — everything was one alphabetical list. We restructure
`src/` so the directory layout tells the truth about *what each file is* and
*which layers may depend on which*.

## The structure

Six dependency-ordered layers, with the **application shell left at the `src/`
root** (`main.cpp`, `MainWindow`, and the orchestration helpers `ExportWorkflow`,
`ImageLoadWorkflow`, `BatchPaste`, `MainWindowStatus`, `ChromeHider`):

```
src/
├── <shell>     main.cpp, MainWindow, DevelopSession, the caches, workflow helpers
├── core/       shared value types + pure geometry + leaf constants   (no deps)
├── develop/    the adjustments model + model→GPU math      depends: core
├── pipeline/   CPU pixel compute                           depends: core, develop
├── render/     the GPU engine                              depends: core, develop
├── io/         persistence                                 depends: core, develop
└── ui/         reusable widgets/panels/dialogs             depends: all of the above
```

`pipeline` and `render` are **siblings** — neither depends on the other. The
GPU-upload math the renderer needs (`BasicTone`'s tone-LUT atlas, `WhiteBalance`'s
gain, the `DisplayLut` value type, the `NoiseReduction` param mapping) is pure
*model→GPU numbers*, not CPU pixel work, so it lives in `develop`/`core`, **not**
`pipeline`. That keeps `render → {core, develop}` only. The decoded-buffer caches
(`DecodeCache`, `ThumbnailCache`) are session/orchestration state, not
persistence, so they sit at the shell root, not in `io`.

The un-foldered root **is** the application; each subdirectory is a layer the app
is built *from*. We deliberately did **not** create an `app/` directory: it would
have become "the box of things not yet refactored." Keeping the shell at the root
says the same thing more honestly, and the root naturally shrinks as `MainWindow`
is decomposed and logic sinks down into the layers.

## Conventions

- **Includes are layer-qualified from the `src/` root** (`#include
  "develop/GlobalAdjustment.h"`). `src/` is the *only* include root, so the
  qualified form is enforced, not optional. A file's include block becomes a
  dependency manifest, and a downward violation is greppable — `grep -rn '"ui/'
  src/pipeline` returning anything is a red flag.
- **One file per *concept*, not per class.** A primary type carries its small
  satellites (`CurvePoints` rides in `GlobalAdjustment.h`; `LoadResult` rides
  with nothing else but lives beside `ImageBuffer` in spirit). We never make a
  grab-bag header — the failure mode being dismantled here.
- **Existing functional namespaces are kept** (`crop::`, `orient::`, `tone::`,
  `filmstrip::`, …). They mark *pure, headless modules* (free functions + small
  value types, no `QObject`) — an axis orthogonal to the directory, and they
  guard collision-prone generic names (`orient::Orientation` vs `Qt::Orientation`).
- **Per-layer namespaces were considered and declined.** Directories + qualified
  includes already make the layering legible; namespacing every widget would add
  sweeping, MOC-hostile churn (`connect`, forward declarations, signal/slot
  signatures) for readability we already have.

## How it landed

Two mechanical, build-verified phases (solo repo, single PR):

1. **Relocation** — `git mv` every file to its layer, rewrite all local includes
   to the qualified form, update `CMakeLists.txt` and `tests/`. Pure rename, no
   logic change, so `git blame --follow` survives. Green before commit.
2. **Splitting the straddlers** — `ImagePipeline.h` had held three layers at once
   (`GlobalAdjustment` the model, `ImageBuffer`/`LoadResult` the data, the CPU
   free functions). Cut into `core/ImageBuffer.h`, `core/WorkingSpace.h`,
   `develop/GlobalAdjustment.h`, `pipeline/LoadResult.h`, and a slimmed
   `pipeline/ImagePipeline.h` (free functions only). `DemosaicAlgorithm` moved
   `pipeline/ → develop/`: it is a persisted model enum that `GlobalAdjustment`
   embeds, so it must sit *beneath* pipeline. Removing the umbrella header
   surfaced the latent include-what-you-use gaps it had masked.

## Deferred, recorded here on purpose

The directory work intentionally did **not** rework the class hierarchy. The
qualified includes made the remaining seams visible; we leave them as named
follow-ups rather than bundle risky surgery into a layout change:

- **`MainWindow` (2100+ lines) and `ImageViewport` (1370+ lines) are the primary
  internal-decomposition targets.** `ImageViewport` already has a clean
  engine/widget seam at its lower edge (it delegates all drawing to
  `RendererCore`, ADR 0006); a tool-state-machine extraction is the obvious next
  cut. `MainWindow`'s already-extracted workflow helpers show the peel-off
  pattern to continue.
- **`DevelopSession`'s derived-buffer cache** — the session still runs CPU
  pipeline compute internally (`applyLensCorrection` → `applySpots`, with eager
  preview / lazy full-res caching). Extracting that into a `pipeline/`
  "corrected negative" component (ADR 0032) would make the subtle caching
  independently testable and remove pipeline compute from the session. Deferred:
  it is performance-sensitive live-view code and the golden tests are GPU-skipped,
  so it warrants its own focused pass.
- **`pipeline/RawProcessor` → `io/XmpSidecar`** — _closed in #70._ This was the
  one remaining cross-layer edge: `RawProcessor` parsed the XMP packet embedded
  in a RAW file via `XmpSidecar::metadataPacketFromPacket`. The fix was control
  inversion — decode now emits the raw XMP *bytes* in `LoadResult`
  (`embeddedXmpPacket`), and `ImageLoadWorkflow` (shell, where the sidecar is
  already parsed) interprets them, colocated with the sidecar merge. The merge
  itself (`resolveUserMetadata`) was promoted to a pure, header-declared seam
  taking the already-parsed packet, so it stays independently testable with no
  XMP bytes in the test. `grep -rn '#include "io/' src/pipeline` now returns
  nothing — the standing invariant for this layering.

## Audited edges, and what was reclassified to close them

The qualified includes exposed several cross-layer edges the flat layout had
hidden (the original "zero upward edges" check missed *sibling* edges between
`pipeline`, `render`, and `io` — corrected here). All are now closed — the
sibling edges below by **reclassifying a misfiled file**, not by relaxing a
rule, and the final `pipeline → io` edge by control inversion (#70, above):

- `render → ui` (`ThemeColors`, the GPU clear colour): a dependency-free `<QColor>`
  leaf → **`core/`**.
- `render → pipeline` (`BasicTone`, `whiteBalanceGain`, `DisplayLut`,
  `NoiseReduction`): all pure *model→GPU* math/types, not pixel work →
  **`develop/`** (`BasicTone`, new `WhiteBalance`) and **`core/`** (`DisplayLut`,
  `NoiseReduction`). `render` now depends only on `core`+`develop`.
- `develop → pipeline` (`DevelopSession`): the class is the application's
  current-image aggregate (`QObject` owning the load state machine, decoded
  buffers, edit state; consumed only by the shell), **not** a develop-*model*
  type (those are the plain serializable structs it holds). → **`src/` root**.
  `develop/` now has no upward edges at all.
- `io ↔ pipeline` **cycle** (`ThumbnailCache`/`DecodeCache` → `RawProcessor` →
  `XmpSidecar`): the decoded-buffer caches are session/orchestration state, not
  persistence → **`src/` root**. That severs the `io → pipeline` arm, breaking
  the cycle and leaving only the acyclic `pipeline → io` edge above — itself then
  closed by the inversion in #70.
