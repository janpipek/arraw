# Export and developed-thumbnail CPU tail runs off the GUI thread

Single-file export, batch export, and the developed thumbnail all freeze the UI:
each does a synchronous offscreen render+readback **and then** a long CPU tail —
output transform (lcms), unsharp, encode (libjpeg/png/tiff), metadata embed
(exiv2, `0043`), and disk write — entirely on the GUI thread. The thumbnail tail
runs after *every* load and *every* save, so rapid culling stutters; a full-res
export blocks for the whole encode. We move the **CPU tail** to a worker thread
while keeping the GPU render+readback synchronous on the GUI thread. We
deliberately do *not* make the render itself async here.

## Considered options

- **Second QRhi on a render thread.** Render entirely off the GUI thread. Rejected
  as overkill: it forces resource re-upload and a second context to manage, for no
  gain over offloading the tail — the render is the *cheap* part (a 512 px
  thumbnail render is sub-millisecond; a full-res render is short next to the
  encode).
- **Async readback for export/thumbnail** (extend `0035`'s in-flight-frame
  pattern). Deferred, not chosen now. The full-res GPU→CPU readback is the only
  remaining GUI-thread cost after this change; if it proves a felt stall we can
  layer `0035`'s `recordOffscreenReadback` underneath without disturbing the tail
  worker. Kept as a clean future seam rather than paid for up front.
- **Offload the CPU tail only (chosen).** The tail is the dominant cost and is
  already pure CPU/IO over a self-contained `QImage`, freely movable. Smallest,
  safest change; the render stays where the QRhi already lives.

This refines `0035` rather than contradicting it. `0035` made the *histogram*
readback async but explicitly kept the **blocking** offscreen path "for export …
where blocking is acceptable" — judging the blocking *render* tolerable for a
one-off user action. That judgement still holds; what `0035` did not address is
the **CPU tail after** the readback, which is what actually hangs here.

## Decision

- **The tail is a worker job over a snapshot.** The GUI thread renders to the
  linear `QImage` as today (the blocking `renderToImage`), then hands that image
  plus value copies of `ExportOptions`, the output/source paths, and `UserMetadata`
  to a worker (`QtConcurrent::run` + `QFutureWatcher`, consistent with the existing
  load/redecode/batch-decode paths). The worker composes the *already pure*
  `prepareExportImage` → `saveExportImage` (`ExportWorkflow`) → `embedExportMetadata`
  (`ExportMetadata`). No worker touches `DevelopSession`, so the user may edit or
  switch images while it runs.

- **Single export is non-blocking.** After the option/file dialogs and the render,
  control returns to the user immediately. Progress shows in the status bar
  (`Exporting X…` → `Exported X`); a failure raises a non-blocking modal from the
  `finished` callback on the GUI thread (the export is already done, so the dialog
  blocks nothing). Concurrent single exports are allowed — concurrency is naturally
  low (user-driven), and each holds at most one large buffer transiently.

- **Batch export is a bounded pipeline, depth 1.** Render file N on the GUI thread,
  dispatch its tail to a single worker, then decode+render N+1 while the tail runs;
  at most ~1–2 full-res buffers are alive at once. This matters because the export
  readback is `RGBA32F` (16 B/px) — a 24 MP frame is ~384 MB, so wide fan-out would
  OOM. The progress dialog stays modal (an explicit bulk action) but responsive,
  since the GUI thread no longer encodes.

- **Cancellation stops new work; in-flight finishes.** Batch cancel stops
  dispatching further files and aborts any in-flight decode (the existing `cancel`
  flag), but lets the current tail finish writing — encode/IO do not interrupt
  cleanly, and a half-written file is worse than one extra file. No hard cancel of
  in-flight encodes.

- **Thumbnails reuse `ThumbnailCache`'s existing per-path generation guard.**
  `ThumbnailCache` already serialises disk writes by a per-path generation counter
  (`storeIfGenerationMatches`, used by the embedded-thumbnail `request()` path), so
  the developed-thumbnail path joins it rather than adding a parallel guard
  (SPOT — the logic lives once). The GUI thread renders the linear thumbnail and
  calls a new `ThumbnailCache::storeDevelopedAsync`, which bumps the generation,
  then on a worker runs the output transform + encode + write and, if its
  generation is still current, emits `thumbnailReady` — the signal `FilmStrip`
  already routes to the strip. So an edit-save-edit-save burst settles neither the
  disk cache nor the strip on a stale develop.

## Consequences

- **Failures surface late and non-modally.** Because the call site no longer
  blocks, a save/encode/metadata failure is reported from the `finished` callback,
  not inline. Batch keeps its end-of-run summary.
- **The render+readback still briefly holds the GUI thread.** Acceptable per the
  measurements implied above; the async-readback seam (`0035`) is the documented
  next step if a full-res readback ever becomes a felt stall.
- **Headless-testable seam.** The tail is `runExportTail` — a pure function over a
  `QImage` and value inputs (no GUI, no RHI) composing the already-tested
  `prepareExportImage`/`saveExportImage`/`embedExportMetadata` — unit-tested for the
  success and failed-write paths, so the thread-marshalling stays thin in
  `MainWindow`. The thumbnail generation guard is the one already covered by
  `ThumbnailCache`'s generation tests. The GUI threading and the depth-1 pipeline
  are GPU/event-loop bound, so they are covered by the mandatory RHI smoke-run
  (AGENTS.md), per the `0035` precedent (`0005`, `0043`).
- **No `CONTEXT.md` change.** Asynchrony is implementation, not domain vocabulary;
  no new ubiquitous-language term emerged. Export, thumbnail, and `UserMetadata`
  keep their current meanings.
