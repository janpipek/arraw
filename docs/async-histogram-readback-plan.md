# Implementation plan — Async histogram readback (issue #51)

Plan for making the panel + curve-input histogram refresh non-blocking.
Architecture and rationale are in
[ADR 0033](adr/0033-async-histogram-readback.md); it supersedes the
"main-thread `glReadPixels`" consequence of
[ADR 0004](adr/0004-histograms-via-gpu-readback.md). Grilled design is settled;
this is the build order for the `/pocock:tdd` session.

## The problem (recap)

`ImageViewport::renderHistograms()` (`src/ImageViewport.cpp:1030`) opens **two**
`begin/endOffscreenFrame` round-trips on the GUI thread via
`RendererCore::renderOffscreenTex` (`src/RendererCore.cpp:726`). Each
`endOffscreenFrame()` (`:763`) blocks until the GPU drains everything queued
ahead of it. We move the two passes into the viewport's own in-flight frame and
read them back via `QRhiReadbackResult::completed`.

## Guiding constraints

- **Additive.** The blocking `renderOffscreen` / `renderOffscreenTex` stay
  verbatim — export, WB picker, and `renderClipSample` keep using them. Only the
  histogram hot path switches.
- **GPU plumbing, not unit-testable headless.** Per ADR 0005 the golden/GPU path
  skips on CI. The coordinator stays **inline** in `ImageViewport` (no extraction).
  Verification is the mandatory RHI smoke-run (AGENTS.md) plus the existing
  `Histogram.cpp` binning tests as the regression guard. Keep the inline logic
  small and obvious — the generation guard is the only safety net.
- **Reuse the frame's cached NR texture.** Enqueue the histogram passes *after*
  the main `core.record(...)` in the same `render(cb)`, so the slot-keyed denoised
  texture (ADR 0032) is already current and the sample matches the preview.

## Decisions (from the grill)

1. **Async readback** — no quick-mitigation half-step.
2. **Trigger:** `histoTimer` (150 ms, unchanged) → `histogramsDirty = true; update()`
   → enqueue in `render(cb)`. Mirrors the `nrTimer` precedent.
3. **API:** one generic `RendererCore::recordOffscreenReadback(...)`, called twice.
4. **Targets:** pooled in `RendererCore`, keyed by `(size, fmt)`, with a per-target
   `inFlight` skip-guard (back-pressure: drop, don't queue).
5. **Pair matching:** generation-keyed inline `PendingHistogram` holder in
   `ImageViewport`; emit on the matched current pair, drop stale.
6. **No coordinator extraction.**
7. **Blocking path untouched.**

## `RendererCore` — the non-blocking API

```cpp
// Records ONE shader pass into a pooled offscreen target and enqueues a readback
// into `cb` (the caller's in-flight frame). No beginOffscreenFrame, no flush.
// `onReady(QImage)` fires on the GUI thread when `completed` signals, a frame or
// two later. If the matching pooled target is still inFlight, the call is a no-op
// (returns false) and the caller retries on the next debounce.
bool recordOffscreenReadback(
    QRhiCommandBuffer* cb, Slot slot, const FrameParams& fp,
    QSize size, QRhiTexture::Format fmt,
    std::function<void(QImage)> onReady);
```

Pooled target entry (keyed by `(size, fmt)`):

```cpp
struct ReadbackTarget {
    std::unique_ptr<QRhiTexture> tex;          // RenderTarget | UsedAsTransferSource
    std::unique_ptr<QRhiTextureRenderTarget> rt;
    std::unique_ptr<QRhiRenderPassDescriptor> rp;
    QRhiReadbackResult rr;                      // must outlive the in-flight window
    bool inFlight = false;
};
```

Recording mirrors the body of `renderOffscreenTex` **minus** the frame
open/close: `prepareToneLut`, a resource-update batch, NR `ensureDenoised` reuse
(Preview slot → cached, no recompute), `recordPass(cb, rt, tex, fp, batch)`, then
a readback batch with `rr.completed = [...]` and `cb->resourceUpdate(readBatch)`.
The `completed` lambda: build the `QImage` via the existing `readbackToImage(rr)`,
clear `inFlight`, invoke `onReady(img)`.

Open questions to resolve **in code** during the session (verify on the GPU box):

- **Lambda capture lifetime.** `rr.completed` must not dangle if the
  `RendererCore`/target is destroyed mid-flight. Capture by stable pointer to the
  pooled entry (entries are long-lived, owned by `RendererCore`), guard teardown.
- **Does `completed` fire reliably for a readback recorded in a swapchain frame
  without `finish()`?** Expected yes — QRhi invokes it when the frame's GPU work is
  processed (a later `beginFrame`). Confirm in the smoke-run; if it only fires on
  the *next* frame, the histogram lands one repaint late, which is fine.

## `ImageViewport` — trigger + coordination

`src/ImageViewport.cpp`:

1. **Timer** (`:65-67`): drop the direct `renderHistograms` connection; instead
   ```cpp
   connect(&histoTimer, &QTimer::timeout, this, [this] {
       histogramsDirty = true;
       update();
   });
   ```
2. **`render(cb)`** (`:182`, after `core.record(...)` at `:211`): if
   `histogramsDirty && hasImage && core.hasImage(Preview)`, build the two
   `FrameParams` (curve-input RGBA8, final RGBA32F — same values as the current
   `renderHistograms`, `:1041-1062`), bump `++histoGen`, capture it, call
   `core.recordOffscreenReadback(cb, Preview, fpCurve, sz, RGBA8, onCurve)` and the
   final variant. Clear `histogramsDirty` **only if both enqueued** (a skipped
   `inFlight` target leaves it set so the next frame retries).
3. **Holder** (member):
   ```cpp
   struct PendingHistogram {
       quint64 gen = 0;
       std::optional<QImage> finalSample, curveInput;
   } pendingHisto;
   quint64 histoGen = 0;
   ```
   Each `onReady` lambda: if `g != histoGen` return (stale); if
   `pendingHisto.gen != g` reset the holder to `g`; store its image; when both
   optionals filled → `emit histogramsReady(*finalSample, *curveInput)` and reset.
4. **Delete** the body of the old blocking `renderHistograms()` (or repurpose the
   `FrameParams` construction into a small helper shared by the two enqueues).

## Build order (red where we can, smoke where we can't)

1. **Keep `Histogram.cpp` binning tests green** throughout — they are the
   regression guard that the readback→bin contract is unchanged.
2. `RendererCore::recordOffscreenReadback` + pooled-target plumbing. No headless
   test (GPU). Compiles clean, no new warnings.
3. Rewire `ImageViewport` trigger + inline holder.
4. Remove the old offscreen-frame histogram path.
5. **Smoke-run** (AGENTS.md): launch the app, confirm the viewport renders (watch
   stderr for the uniform-link error), drag/nudge a slider and confirm (a) no
   perceptible input lag attributable to histogram refresh, and (b) histograms
   still update after adjustments settle — the issue's acceptance criteria.

## Acceptance (from issue #51)

- Dragging/nudging sliders stays responsive while histograms update.
- Histograms still update after adjustments settle.
