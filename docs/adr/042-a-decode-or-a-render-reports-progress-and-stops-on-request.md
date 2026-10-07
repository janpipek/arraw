# A decode or a render reports progress and stops on request

[ADR 011](011-a-plan-spatial-passes-and-one-pointwise-chain.md) decided that
warnings, progress and cancellation travel on a caller-supplied channel, and
built the warnings half as `DiagnosticLog`. Progress and cancellation waited
for a caller. The GUI preview is that caller: a CPU preview with noise
reduction or Dehaze takes one to two seconds at 24 MP, shows nothing while it
runs, and cannot be stopped when an edit makes it pointless. This ADR records
the engine's progress and cancellation channel, how the passes report into it,
how the preview uses it, and the indicator that shows it. It implements the
[render progress plan](../ideas/render-progress-plan.md), whose decisions table
is binding.

## Decision

**The public types are in `include/Progress.h`.**

| Type | What it is |
|---|---|
| `ProgressStep` | The named parts of a decode or a render, in the order a render runs them: `Decode`, `Denoise`, `Context`, `Pointwise`, `Geometry`, `Resize`, `Effects`. Engine names, which the caller words, as `describe` words a `Notice`. |
| `Progress` | A report: `fraction` from 0 to 1, and the `step` being worked on. |
| `ProgressChannel` | The channel of one operation: an optional callback for progress out, and `cancel()` in. Neither copyable nor movable. |
| `Cancelled` | What an operation throws when its channel was cancelled. A `std::runtime_error`, so that code which only reports errors still does not take it for a result, but its own type, so that a caller can tell "stopped because I asked" from "could not". |

`progressStepCount` is computed from the last enumerator, `Effects`, which
must stay last. `ProgressChannel::report` is private, with the engine's
`detail::ProgressRoot` as its only friend: a caller cannot inject reports.

**The entry points take a trailing `ProgressChannel* progress = nullptr`.**
`develop`, `developUntil`, `resumeFrom`, `sample`, `curveHistogram` and
`loadImage` in the public API. Inside the engine the GPU's `developOnGpu` and
`sampleOnGpu` overloads take the same parameter. A null channel means that
nothing observes the operation. Then the cost is one read of a thread-local
pointer per loop over rows, and the output is bit for bit what it was before
(tested).

**There are two sinks, not one.** The user decided, on the review's F5, to
keep `DiagnosticLog&` and the nullable `ProgressChannel*` side by side. This
refines ADR 011's "a caller-supplied channel" into two types, because the two
halves differ in every property that shapes an interface:

| | `DiagnosticLog` | `ProgressChannel` |
|---|---|---|
| Direction | Out only. | Out (progress) and in (cancellation). |
| Threads | One thread's work goes to one log (`Diagnostics.h`). | `cancel()` comes from another thread by design. The callback runs on the operation's thread, never concurrently with itself. |
| Lifetime | May span a batch: a CLI export run collects into one log. | One operation, or one chain of resumes. Once cancelled it stays cancelled, so it cannot be reused for the next request. |
| Absent | `discardedDiagnostics()`, a log that drops everything. A call site records unconditionally, and a discarding object costs nothing. | `nullptr`. Whether anyone listens changes what the engine does: observed loops run in chunks and count them, unobserved ones do not. Null makes "no observer" a fact the engine can test once, which is what keeps the "costs nothing, same bits" promise checkable. A `noProgress()` inert object would hide that fact behind a virtual call or a flag. |

Two alternatives were weighed and rejected:
- An `OperationContext { DiagnosticLog& log; ProgressChannel* progress; }`
  would mean one trailing parameter instead of two once decode takes both. It
  costs churn at every `DiagnosticLog&` caller, and it puts a one-thread object
  and a cross-thread one in one value whose thread rules would then need
  stating per member.
- Virtual `progress()` and `cancelled()` on `DiagnosticLog` would match ADR
  011's wording most closely. But they would put a thread-safe flag into a type
  documented as one thread's, and every log (the CLI's, the collected one,
  the discarding one) would grow members it has no use for.

Today only `loadImage` takes both, so the parameter-count argument for a
context is weak. If a third out-of-band concern arrives, the context is the
place to revisit.

**A render's fraction is measured against the whole render, from the source
to the effects.** Each step has a weight. The weight is zero for a step the
plan does not run, and otherwise is the step's expected wall time on the CPU:
nanoseconds per pixel, measured on the release tree at 24 MP with eight
threads, times the pixels the step covers. The constants live beside the code
they describe:
- `pointwiseCost`, `presenceSampleCost`, `geometryCost`, the two resize costs,
  `vignetteCost` and `grainCost` in `src/core/RenderProgress.cpp`;
- `denoiseLoopWeights` in `Denoise.cpp`;
- `presenceLoopWeights` in `Presence.cpp`.

The resize and the effects are sized from the request rather than the plan.
A call that stops before the resize plans none, so its plan would say
otherwise. As a result every call of a chain of resumes uses the same weights.
A call that resumes from a boundary starts at that boundary's share, and one
that stops early ends at its own boundary's share. **A chain of resumes on one
channel therefore reads as one render**, from 0 to 1 (tested). `sample` stops
before the effects, so its effects weight is zero and it ends at 1. A decode
is one step, `Decode`.

The GPU uses the same shares. Its pass ratios are close enough, and a GPU
preview is rarely long enough to be shown at all.

**The passes report through an ambient span, not through parameters.**
`src/core/ProgressScope.{h,cpp}` holds the machinery:
- `detail::ProgressRoot` is installed by each entry point for its whole call.
  With a channel it opens the top span. Without one it hides any outer
  operation's span, so a public call made inside another (`sample` inside a
  caller's observed work, for instance) cannot report into it.
- `detail::ProgressSpan` is a step's share. Spans nest. A span divided into
  *units* (equal, or weighted) counts each loop over rows, each GPU render,
  each `completeUnit()` and each span opened inside it as one unit, in order.
- A pass that runs several loops declares them as weighted units:
  - Denoise: its conversion, luma, perceptual encodings and bilateral passes,
    and the colour grid's reduce, two blurs and combine;
  - Presence: each base's reduce, octagon extrema, reconstruction steps and
    two blurs;
  - Resize: the horizontal and vertical passes.

  The span must run exactly the units it declares. A debug build asserts this
  when the span closes, and the tests run observed renders over the
  combinations of passes on the CPU and the GPU.
- The pass signatures do not change, and an unobserved span costs one
  thread-local read.

`detail::forEachRowBand` is where the CPU reports. Observed, each band runs its
rows in chunks of about `minimumPixelsPerBand` (65 536) pixels. Between chunks
it counts the rows done with one relaxed `fetch_add` and checks for a
cancellation, and the calling thread reports. A band that fails or is
cancelled raises a flag, and the other bands stop at their next chunk. The
body sees the same rows in more calls. By `forEachRowBand`'s contract (a body
writes only its rows) that gives the same bits. Inside the body the span is
hidden (`detail::HiddenProgress`), on the calling thread as on the workers, so
that a body which itself loops or opens a span cannot spend the loop's units
from band 0 only (the review's F7). `detail::forEachRowInTurn` does the same
for the loops that stay on one thread: geometry, the resize passes and the
effects. `toRgbaF32` became a banded loop so that it reports, which also makes
it parallel.

**Reports are thinned and monotone.**
- A report is sent when the step changes, and otherwise only once the fraction
  has grown by 1/512.
- The fraction never decreases. Rounding cannot leave a finished operation
  short of 1, and a finished operation always reports its end.
- At most about 512 reports are sent, plus two per step.

**The callback runs on the operation's thread.** A callback that throws stops
the operation with what it threw (tested). A span's end report during
unwinding is skipped, and one that throws in a destructor is swallowed,
because the work is already done.

**Cancellation is cooperative and leaves nothing partial.**
- *On the CPU* it is noticed between chunks, so every band stops within one
  chunk.
- *On the GPU* `GpuContext::render` checks before each render and counts the
  render as a unit once it is done. A cancellation is therefore noticed between
  renders, never inside one, and no render follows it (tested by
  `renderCount`).
- *A RAW decode* registers LibRaw's progress callback while it is observed.
  The callback answers LibRaw's own checks with the channel's state, from
  whichever of LibRaw's threads asks, and a `LIBRAW_CANCELLED_BY_CALLBACK`
  becomes `Cancelled`. The decode reports three equal units: the unpack, the
  demosaic and the copy out. LibRaw gives no measure within its stages. Any
  other file reports only its start and its end.

A cancelled call throws `Cancelled` and returns nothing. Checkpoints that the
caller already holds stay as valid as they were, because the engine never
writes into them. A resume cancelled part-way leaves its source checkpoint
usable (tested). A channel cancelled before a call stops it before any pass
or report; planning (with its opacity scan) and, on the GPU, the upload still
run first. A channel cancelled after a call has no effect on what that call
returned.

**The preview uses one channel per job and cancels it on supersession.**
`PreviewRenderer` creates a channel for each piece of work: the shown render,
the refreshed fallback beneath a region, and the histogram recount. It holds
the current one as `inFlight_`, under the same mutex as `pending_`.
- `request()` cancels the in-flight channel when it queues a newer request.
- `startChannel` cancels a new channel at once if a request arrived while the
  work was being taken.
- The destructor cancels too, so closing the window does not wait for a slow
  render.
- `Cancelled` is caught separately from failures. A cancelled render delivers
  nothing (no image, no error text) and the worker goes straight to the
  pending request.
- A GPU render that is cancelled does not fall back to the CPU.
- `CheckpointCache` assigns a checkpoint only from a pass that returned, and
  drops a stale one before the render it would have served. A cancellation
  therefore leaves only whole checkpoints that are valid for the newer state,
  which the newer request resumes from.
- The newest request is never cancelled by another, so it always ends with an
  image or an error.
- A cancel that arrives after the last check yields a complete image. That is
  correct, since nothing partial comes out, and the window drops it as stale by
  request id.

Progress is forwarded only for the shown render. It is thinned again on the
worker, to one report per `previewProgressInterval` (33 ms), plus every step
change and the end. `MainWindow` hands each report to the GUI thread through a
queued `QMetaObject::invokeMethod`. There `showRenderProgress` drops any report
whose request is not the newest.

**The indicator follows the plan's decisions table.**
`RenderActivity` is a pure state machine over time points the caller supplies,
and `RenderIndicator` drives it from a timer:
- Nothing is shown before 250 ms (`renderActivityDelay`).
- Once shown, the bar stays at least 300 ms (`renderActivityHold`).
- A completed render's full bar stays at least 150 ms after it finished
  (`renderActivityFilled`). This takes the review's F9, option 3: a slow render
  visibly ends instead of vanishing the instant its image lands, while a fast
  one still flickers nothing.
- A newer render within a busy period keeps the last fraction and step until
  it reports itself (F10, option 1). With cancellation that is within
  milliseconds, so a slider drag over a slow render does not alternate between
  a bar and a busy one.

The indicator is a permanent status-bar widget (`RenderProgressBar`): the
step's wording ("Reducing noise…", "Analysing local contrast…",
"Developing…") beside a `QProgressBar` with the percentage, busy while there
is no fraction yet. It was first a thin bar painted along the top of the photo
view, with the wording alone in the status bar; once seen in use, the step and
its progress belonged together, and a native widget that covers nothing of
the photograph or the crop handles was worth the short glance away from the
picture. The newest result ends the busy period even when it is no longer
wanted (a photograph was opened, or the crop mode was left), so the bar cannot
stay busy forever (F8).

*Amended 2026-10-07 (user):* the label and bar become one small pie chart
(`RenderProgressPie`), a permanent status-bar widget on the right beside the
device label, so a status message neither hides it nor is hidden by it. It is
always present: a full green pie while nothing is to be shown (no render, or
within the show delay), and while shown a red pie filled with the fraction (an
empty red outline until there is one). The fill is the cue that does not depend on
colour; the colours are a second cue. The step's wording moves to the tooltip
("Reducing noise… 40%"; "Up to date" when done) and the accessible name and
description. The show delay and holds are unchanged.

**Python gets no progress callable yet.** The bindings
([ADR 018](018-python-binds-the-public-api-and-nothing-else.md)) keep their
signatures. The C++ default (`nullptr`) means they call the same functions as
before, and the stubs do not change. The callable is deferred until a script
needs it, because it has two costs worth paying only then:
- The callback runs on the render thread. If the binding releases the GIL
  around `develop`, the trampoline must take it back for every report.
- A Python exception in the callback would stop the render, as documented
  here.

When it is added, `ProgressChannel` is bound with a callable and `cancel()`,
and `Cancelled` becomes a Python exception.

## Measurements

These were taken on the release tree with eight threads, on a 6000×4000
RGBA-float source rendered to fit 2560 px. "Heavy" means luminance and colour
noise reduction, Texture, Clarity and Dehaze, a 2° straighten with a crop,
vignette and grain.

| Render | Without a channel | With a channel | Reports |
|---|---|---|---|
| Exposure only | 1261 ms (median of 7) | 1294 ms (+2.6 %) | 290 |
| Noise reduction | 3029–3134 ms | −2 % to +6 % across runs (noise) | 380 |
| Heavy | 5969 ms | 5831 ms (−2.3 %, noise) | 389 |

The observed path costs about 2.5–3 % on a cheap render, consistent with the
review's micro-benchmark. It disappears into run-to-run noise on a heavy one.

**Linearity.** With the measured weights, the largest gap between the reported
fraction and the share of wall time is 2.8 % on the heavy render, and each
step takes 0.90–1.09 of its weight. The worst gaps on cheaper renders (10–16 %)
are allocations that report nothing. Zeroing a new 24 MP float buffer takes
about 190 ms on one thread, and so does the pointwise result. The pointwise
span is opened before that allocation, so that at least the right step is
named.

**Cancel latency.**
- A heavy 24 MP CPU `develop`, cancelled at 50 ms to 1.7 s, threw 22–61 ms
  after `cancel()`. Most of that is unwinding: freeing one 24 MP float buffer
  costs about 24 ms. A cancellation that lands during a buffer's zeroing waits
  for it.
- In the preview, a 24 MP level-0 CPU render with noise reduction, superseded
  100 or 600 ms in by a small render, delivered nothing. The newer image
  arrived 122–154 ms after its request, against 25 ms for the same render
  alone, so superseding costs about 100–130 ms instead of the remaining
  seconds of the old render.

## Consequences

- **The weights need remeasuring when a pass changes speed a lot**, such as a
  faster bilateral or a GPU-like Presence on the CPU. A wrong weight only
  makes the bar uneven; it never makes it wrong-way or short.
- **A pass that gains or loses a loop must update its declared units.** The
  debug assertion fails the observed tests if it does not. A new
  `ProgressStep` needs a weight, a `renderStepText` wording, and `Effects` must
  stay the last enumerator.
- **Allocation is the uncancellable part.** At 24 MP each new float buffer is
  about 190 ms of zeroing and about 24 ms of freeing. Not zeroing buffers that
  are about to be overwritten would shorten both renders and cancel latency.
  That would change `ImageBuffer`'s "starts zeroed" contract and was left for
  its own decision; [ADR 043](043-the-gui-thread-never-decodes.md) kept the
  contract and made the zeroing about three times faster by doing it on every
  thread.
- **Decode reports, but the GUI does not show it yet.** `MainWindow::showPhoto`
  decodes on the GUI thread, so "Decoding…" needs the threaded decode first.
  [ADR 043](043-the-gui-thread-never-decodes.md) threads it and shows it.
  Thumbnails and exports keep their own progress (out of scope by the plan).
- **The GPU's progress moves a render at a time**, and a GPU render is only
  ever stopped between renders. A single long render, such as a large
  geometry pass, cannot be interrupted.

## What was rejected, and why

- **Progress and cancellation as parameters of every pass.** These would
  change every pass signature and every test of them, for information that
  only the entry point and the loops need. The thread-local span reaches the
  loops without that churn and costs one pointer read when unobserved.
- **Per-call fractions (each call from 0 to 1).** The preview renders through
  a chain of resumes, so the bar would restart at every boundary. Measuring
  against the whole render makes the chain read as one.
- **A partial image on cancellation.** A cancelled render's pixels are
  incomplete in ways no caller can use, and handing them out would invite
  showing them. A distinct exception and nothing returned is the one outcome
  no caller can misread.
- **Polling a cancel flag inside GPU shaders.** A GPU render is short, and
  splitting renders into tiles to stop inside one would cost more than the
  latency it saves.
