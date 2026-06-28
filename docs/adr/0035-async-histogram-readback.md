# Histogram readback is async, recorded into the widget's own frame

The panel and curve-input histograms (`0004`) were refreshed by two synchronous
offscreen render+readbacks on the GUI thread, each opening its own
`begin/endOffscreenFrame`. `endOffscreenFrame()` blocks until the GPU drains
*everything* queued ahead of it — including the preview render the user is
actively driving — so a slider nudge during the ~150 ms-debounced refresh felt
unresponsive (issue #51). We replace this with a **non-blocking** path: the two
histogram passes are recorded into the viewport's *in-flight* swapchain frame and
read back via `QRhiReadbackResult::completed` callbacks, so nothing waits on GPU
idle on the main thread.

## Considered options

- **Quick mitigation** — collapse the two offscreen frames into one and bump the
  debounce 150 → 250 ms. Rejected: the dominant cost is the flush-to-GPU-idle, not
  the *number* of readbacks. One flush still stalls the GUI thread on a full
  round-trip, so this halves the symptom without removing its class. We'd likely
  redo it.
- **Async readback (chosen).** Removes the blocking call entirely. Costs texture
  lifetime across frames and coordinating two independent readback completions —
  contained to `RendererCore` + `ImageViewport`.

## Decision

- **Additive API, blocking path untouched.** `RendererCore` gains a generic
  non-blocking method alongside the existing blocking `renderOffscreen`:

  ```cpp
  void recordOffscreenReadback(
      QRhiCommandBuffer* cb, Slot slot, const FrameParams& fp,
      QSize size, QRhiTexture::Format fmt,
      std::function<void(QImage)> onReady);
  ```

  It records one shader pass into a pooled offscreen target and enqueues a
  readback into the **caller-supplied** command buffer — no `beginOffscreenFrame`,
  no flush. `RendererCore` stays histogram-agnostic (mirrors the format/slot-
  parameterised blocking `renderOffscreen`); the "why two samples" semantics live
  in the caller. The blocking path is kept verbatim for export, the WB picker, and
  `renderClipSample` — one-off explicit user actions where blocking is acceptable.

- **Driven from `render(cb)`, not its own frame.** `histoTimer` (still 150 ms)
  no longer calls a blocking `renderHistograms()`; it sets `histogramsDirty` and
  calls `update()`. `ImageViewport::render(cb)`, after the main `core.record(...)`,
  enqueues the two histogram passes (curve-input RGBA8, final RGBA32F) into the
  same `cb` when the flag is set, then clears it. This mirrors the existing
  `nrTimer` → effective-values → `update()` precedent (`0034`) and lets the
  histogram passes reuse the same frame's **cached NR denoised texture** (slot-
  keyed) for free, so the sample still matches the preview.

- **Pooled targets with an `inFlight` skip-guard.** Histogram targets are few and
  fixed-size (256×~h, RGBA8 + RGBA32F), so they are pooled in `RendererCore` keyed
  by `(size, fmt)` rather than allocated per refresh. Each pooled target carries an
  `inFlight` flag: if its previous readback hasn't completed, a new request is
  **skipped** (the 150 ms debounce retries). This gives natural back-pressure —
  under GPU load we drop histogram refreshes (invisible; the next debounce catches
  up) instead of piling up pending readbacks.

- **Generation-keyed pair matching.** The two readbacks complete independently,
  possibly in different frames. `ImageViewport` holds an inline
  `PendingHistogram { quint64 gen; std::optional<QImage> finalSample, curveInput; }`.
  Each enqueue bumps `histoGen`; each `onReady` captures its gen and on completion
  **drops** the result if `gen != histoGen` (superseded), else stores it and emits
  `histogramsReady(final, curveInput)` once both optionals are filled. Matching the
  *current* pair explicitly — rather than trusting both `inFlight` guards to line
  up — guards against the worst failure mode here: nothing crashes, the panel just
  shows a mismatched histogram.

## Consequences

- This supersedes the specific consequence of `0004` that histogram updates "cost
  a throttled small render + `glReadPixels` **on the main thread** per parameter
  change." The readback is now off the main-thread critical path; everything else
  in `0004` (shader as source of truth, the pipeline-stage uniform, `Histogram.cpp`
  doing only binning + painting) is unchanged.
- **`completed` fires on the GUI thread** during RHI frame processing, a frame or
  two after the readback is recorded — so emitting the Qt signal from the callback
  is safe and needs no cross-thread marshalling.
- **No automated coverage for the new logic.** The coordinator stays inline in
  `ImageViewport` (not extracted into a testable unit) — the change is GPU plumbing
  that the headless CI cannot run (`0005`), so it is verified by the mandatory RHI
  smoke-run (AGENTS.md) with the existing `Histogram.cpp` binning tests as the
  regression guard. The generation guard earns its keep precisely because nothing
  else will catch a mismatched pair.
- **No `CONTEXT.md` or `Ubuf` change.** Async readback is implementation, not
  domain vocabulary, and the histogram passes reuse the existing `FrameParams` /
  uniform block — the three-place uniform gotcha (`0006`) does not apply.
