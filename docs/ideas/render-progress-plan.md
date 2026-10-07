# Render progress and cancellation — plan

Status: accepted, 2026-10-06.

## Goal

A photographer sees that the program is working when a render takes long
enough to notice, and how far it has got; and a render that an edit has made
pointless stops instead of being finished. Today a CPU preview with noise
reduction or Dehaze takes 1–2 s at 24 MP and shows nothing, and it cannot be
interrupted.

## Decisions (user, 2026-10-06)

| Question | Answer |
|---|---|
| Scope | Engine progress and cancellation (the out-of-band channel ADR 011 decided and left unbuilt), used by the preview |
| Look | A thin determinate bar (2–3 px) along the top edge of the photo view; the step named in the status bar ("Decoding…", "Reducing noise…", "Developing…") |
| Flicker | Shown only when a render outlasts about 250 ms, and kept briefly once shown, so fast GPU previews show nothing |
| Cancellation | A preview render superseded by a newer request stops part-way on the CPU; on the GPU between passes |
| Out of scope | A status-bar activity area for thumbnails and exports (exports already show progress) |

## Shape

- **Engine**: a caller-supplied progress and cancellation channel on the
  develop entry points (and decode), as ADR 011 describes for warnings,
  progress and cancellation. Progress is a fraction with the current step's
  name; the CPU passes report per row band, the GPU per render. Cancellation
  is cooperative, checked between bands and between renders, and ends the
  render with a distinct outcome, never a partial image. With no channel the
  cost is nothing and the output is bit-identical.
- **Preview**: `PreviewRenderer` passes a channel per request, cancels the one
  in flight when a newer request arrives, and forwards progress to the GUI
  thread at a bounded rate.
- **Photo view**: the bar and the status text, with the delay and the hold.

## Steps

One workflow: Opus for the engine channel and the preview's use of it
(architecture), Sonnet for the bar, the status text and the tests, then an Opus
review. Agents never commit; the reviewed result is committed on its own, with
an ADR.
