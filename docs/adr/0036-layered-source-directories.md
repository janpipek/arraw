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
├── <shell>     main.cpp, MainWindow, the workflow helpers
├── core/       shared value types + pure geometry math   (no deps)
├── develop/    the adjustments model                      depends: core
├── pipeline/   CPU pixel compute                          depends: core, develop
├── render/     the GPU engine                             depends: core, develop
├── io/         persistence                                depends: core, develop
└── ui/         reusable widgets/panels/dialogs            depends: all of the above
```

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
- **`develop/DevelopSession` → `pipeline/LoadResult` + `LensCorrection`** — the
  session bundles the loaded image (a pipeline output) with develop state
  (ADR 0020). A real `develop → pipeline` dependency; revisit when decomposing
  the session.

The one other edge the qualified includes exposed has since been closed:
`render/RendererCore` read `ThemeColors` (the GPU clear colour, single-sourced
with the widget palette so the viewport surround and chrome never drift). The
header is a dependency-free `<QColor>` leaf, so it moved `ui/ → core/` — both
`Theme` (ui) and `RendererCore` (render) now depend *down* on it, no inversion.
