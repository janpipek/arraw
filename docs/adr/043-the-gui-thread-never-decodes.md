# The GUI thread never decodes

The window decoded a photograph on the GUI thread: opening a 24 MP RAW froze
it for one to three seconds, and the first entry into the crop mode froze it
again for up to a second while the camera's embedded preview was read
([ADR 040](040-the-crop-mode-straightens-on-screen-over-an-uncropped-render.md)).
[ADR 042](042-a-decode-or-a-render-reports-progress-and-stops-on-request.md)
gave the decode progress and cancellation but left "Decoding…" waiting for a
threaded decode. This ADR records an audit of what the GUI thread does, what
moved off it, how the window behaves while a photograph's pixels are on their
way, and what stays.

## The audit

Measured on the release tree with eight threads and a CPU preview (offscreen
platform, window 1600×1000). The source is a generated 6000×4000 16-bit Bayer
DNG with a full-size JPEG preview, the shape of a modern camera's RAW (none is
committed): a folder of three, and one of 300 hard links to them. Each
operation is timed with a `detail::TimingSpan` (`QT_LOGGING_RULES=
"arraw.timing.debug=true"`), and the event loop with a 1 ms timer that records
its longest gap. The driver is the hidden test case `[.bench]` in
`tests/test_MainWindowOpening.cpp`, run with `ARRAW_BENCH_FOLDER` naming the
folder. Ranges span runs, quiet ones and ones where the film strip was making
thumbnails of the 300 shots on all cores.

| Work | Before, on the GUI thread | After, on the GUI thread | Where it went |
|---|---|---|---|
| Decode of a 24 MP RAW (`loadImage`) | 1060–1420 ms; 2800–3000 ms while the strip makes thumbnails | none | `PhotoLoader`'s decode thread |
| Reading the photograph's description, EXIF orientation and sidecar (`openPhoto`) | 1–3 ms | 1–3 ms | stays |
| The rest of opening (session, view, panel) | 2–10 ms | 1–7 ms | stays |
| Pyramid levels for the first render | none (370–420 ms on the render thread) | none (88–117 ms on the render thread) | banded, no float copy |
| Listing a folder of 300 shots (`listShots`) | 4–8 ms | 4–8 ms | stays; marks and EXIF were already on the shot model's thread |
| First crop entry, embedded preview not cached | 400–865 ms | under 1 ms | `PhotoLoader`'s preview thread |
| First crop entry, embedded preview cached | 1–5 ms | under 1 ms | the same |
| Saving the sidecar | 4–16 ms | 2–11 ms | stays |
| Queuing an export (a snapshot of the state and a shared pointer) | under 1 ms | under 1 ms | stays; the dialogs are the user's time |
| Reducing a whole-frame render to the strip's live thumbnail | 2–26 ms per render | none | the render thread (1–3 ms) |
| The curve histogram, the strip's thumbnails | none | none | already on their threads |
| Painting the photo view | 3–15 ms | 3–15 ms | stays: painting |
| **Opening or switching a photograph, the call** | 1065–3008 ms | 2–9 ms | |
| **Longest stall of the event loop while opening** | 1065–3050 ms | 14–20 ms (once 37 ms) | |

The remaining stalls are not window code: no span on the GUI thread exceeds
9 ms, and gaps of 12–15 ms appear with nothing at all on the GUI thread while
eight worker threads (the decode's, the render's bands, the thumbnails') keep
every core busy. That is most likely scheduling, which a desktop compositor
also sees; the one 37 ms gap was not traced further.

The budget was a frame, about 16 ms. Everything over it moved; what stays is
under it, and is either painting or a few kilobytes of file input and output.

Not in the table, so not measured here: leaving the crop mode and rotating
(the `[.bench]` case times them but they were not reported), the crop
overlay's conversion of a render to a pixmap, painting and scrolling the strip,
and listing a folder much larger than 300 shots or on a network share. Closing
the window is in the consequences below.

## Decision

**`PhotoLoader` decodes on a thread of its own** (`src/app/PhotoLoader.{h,cpp}`,
in `arraw-app-core`). It follows `PreviewRenderer`, `ExportQueue` and
`ThumbnailWorker`: long-lived `std::jthread`s, no Qt signals, callbacks on the
worker that the window marshals with a queued `QMetaObject::invokeMethod`, and
a member of the window declared last, so that its destructor (which cancels
and joins) runs before anything a callback touches is gone.
- Two *lanes*, each one thread serving only its newest job: the decode, and the
  camera preview read. Separate, so that a preview read, which cannot be
  stopped part-way, never holds up the decode of the next photograph.
- `decode(path)` cancels the decode in flight through its `ProgressChannel`
  (ADR 042) and replaces any queued one. A cancelled decode delivers nothing; a
  RAW stops at LibRaw's next check. The newest decode is never cancelled by
  another, so it always ends with pixels or an error, unless `cancelDecode()`
  drops it.
- The decoder is a constructor parameter, `loadImage` by default, so the tests
  can hold a decode until it is cancelled.

**Opening commits at once; the pixels follow.** `MainWindow::showPhoto` no
longer throws:
1. `openFile` and `activateShot` read the description and the sidecar
   (`openPhoto`, 1–3 ms) and ask about unsaved changes, as before. A file whose
   description cannot be read still leaves the window as it was.
2. `showPhoto` replaces the session with the new photograph's, gives the
   renderer no source (which cancels its work, see below), moves
   `firstRequest_` past every request made so far, clears the curve histogram,
   shows a stand-in, starts the decode and shows the panel. It returns in a few
   milliseconds.
3. `decodeLanded` drops a result whose request is not `decodeRequest_`, so a
   decode of a photograph left since can never reach the window. Otherwise it
   keeps the pixels, gives them to the renderer, enables editing and asks for
   the first render.

**While the pixels are on their way, editing is disabled, not queued.** The
panel shows the photograph's state from its sidecar immediately, greyed: the
develop dock, the crop mode, the geometry commands, the zoom, the picker and
Export are off until `decodeLanded` (`editable()` and
`updateEditingActions()`; every slot that reads `OpenPhoto::decoded` checks
it, so a shortcut or a signal cannot slip past). Export from the strip says
"<name> is still being decoded." in the status bar. Marks (rating, labels,
pick and reject) work throughout: they go through the session and need no
pixels. Saving has nothing to save. Queuing edits was rejected: a geometry
edit needs the decoded size and orientation, and an edit made blind is one the
user cannot judge.

**The view never shows one photograph for another.** The previous picture
goes the moment the new photograph is opened. In its place
`PhotoView::setStandIn` shows the film strip's thumbnail of the shot, fitted
inside the frame the file declares (`croppedSize` of the metadata's size and
orientation under the sidecar's state), so it sits where the render will land.
A thumbnail whose shape differs from that frame by more than 3 % is of another
frame (a camera preview of a turned or cropped photograph) and is not shown;
then the view is empty until the render. The stand-in is never what
`wholeFrameImage()` gives, so nothing takes it for a render (the crop mode's
placeholder, the tests' waits). The next `setImage` or `resetView` drops it.

**The bar shows "Decoding…".** `showPhoto` begins the indicator's busy period;
the loader's progress (three units for a RAW, ADR 042) is reported while its
request is `decodeRequest_`, and the first render continues the same busy
period with its own steps. While a decode is awaited, a late render result of
the previous photograph does not end the busy period. A failed decode ends it.

**A decode that fails closes the photograph.** The previous photograph was
left when the new one was opened, so there is nothing to go back to: the
window becomes empty (`closePhoto`), the strip clears its active shot, and the
message says why. A failure after the description read is rare, since that
read parses the same headers.

**`PreviewRenderer::setSource` cancels the work in flight.** Before, a render
of the previous photograph ran to its end and was dropped by request id; now
it stops within a chunk and leaves the cores to the decode, also when the new
source arrives between the worker taking a job and starting it. A render past
its last check is still delivered, and still dropped. The worker is woken to
let go of what it keeps of the previous photograph (its pyramid, the CPU and
GPU checkpoints, the whole-frame fallback), so that memory is free while the
next photograph decodes, not only at its first render.

**The crop mode reads the camera preview on the loader's preview thread.**
`seedCropOverlay` shows the developed frame alone (ADR 040's inner layer) and
asks for the preview once per photograph; `cameraPreviewLanded` keeps it in
`OpenPhoto::cameraPreview` and, while the mode still waits for its first
render, hands it to `CropOverlay::fillPlaceholder`, turned and flipped to the
geometry as it is then. A render that came first wins: `fillPlaceholder`
ignores a preview once a render is shown. On a cold cache the render usually
arrives before the preview; on a warm one the preview lands within a frame or
two.

**The live thumbnail is reduced on the render thread.** `PreviewView` gains
`thumbnailEdge`, and `PreviewResult` a `thumbnail`: the renderer reduces an
image of the whole frame to that edge, at a pixel ratio of 1, before
delivering it. The window asks for it outside the crop mode only, so the
mode's renders cost nothing extra.

**Halving a pyramid level is banded and reads the stored samples.** `halved`
made a full-size float copy of a 16-bit source before averaging, which at
24 MP is a 384 MB allocation, and averaged on one thread. It now reads each
sample through `toUnit`, as `toRgbaF32` would, and runs over bands of output
rows (ADR 039). The numbers are the same, so the bits are, on any number of
threads. The first render of a photograph waits 88–117 ms for its level
instead of 370–420 ms, and the strip's thumbnails gain too.

## Consequences

- **The first picture of a photograph is its thumbnail, or nothing.** A
  photograph whose strip cell has no thumbnail yet, or one of another shape,
  shows an empty view with the bar until its render. Reading the embedded
  preview for the view as well was not done: it costs as much as half a decode
  on a cold cache, and would compete with the decode for the cores.
- **What a photograph looks like while decoding is consistent but not
  editable.** Someone who starts dragging a slider within the first second
  finds it greyed until the pixels land.
- **A failed decode now closes the photograph** where it used to leave the
  previous one. The case needs a file whose headers parse and whose data does
  not.
- **Saving the sidecar stays on the GUI thread** (2–16 ms). Leaving a
  photograph needs to know whether its save succeeded before going on, and a
  save that ran on a thread would have to be awaited at exactly those moments.
  On a slow or network disk this is the next thing to move.
- **Painting stays where it is.** The photo view paints in 3–15 ms; the crop
  overlay converts a new render to a pixmap once (ADR 040). A smaller image to
  paint, or converting it on the render thread, would be the next step if a
  slow machine shows it.
- **A preview read cannot be stopped.** A read that is no longer wanted runs
  to its end on its own thread and is dropped; it holds up nothing but the next
  preview read, and closing the window: the loader's destructor cancels the
  decode and drops both queues before it waits for either thread, so closing
  waits for the slower of the two, up to a cold preview read (under a second),
  not their sum. A JPEG, PNG or TIFF decode through Qt cannot be stopped
  part-way either.
- **A decode that throws something other than an exception of the standard
  library** is delivered as an "unknown error", so the window never waits for
  a decode that ended.
- **The decode and the render compete for cores.** While a decode runs, the
  thumbnail worker and the renderer (now cancelled at once) are what else is
  busy; the decode of the photograph the user waits for is not prioritised
  over the strip's thumbnails beyond the thumbnail thread's lower priority.

## Threading the hot spots

A render that does not freeze the window can still take longer than it needs
to: on a cheap 24 MP preview the resize was about 70 % of the time, and it ran
on one thread. The loops below were measured on the Release tree on a 6000x4000
image, "one thread" being `detail::rowBandLimit` set to one, which is the loop
as it ran before, and "all" the eight hardware threads of the machine. It is a
shared machine with a load of three to ten, so each figure is the best of
several runs and is good to perhaps 20 %. Resizes include one `clone()` of the
source, which `resample` consumes.

| Work | One thread | All threads | Change |
| --- | ---: | ---: | --- |
| `ImageBuffer` of 24 MP, RGBA float | 203 ms | 62 ms | zero fill banded |
| `ImageBuffer` of 24 MP, RGB 8-bit | 38 ms | 12 ms | zero fill banded |
| `clone()` of a 24 MP float image | 229 ms | 92 ms | its zero fill banded; since, it copies without zeroing first (not measured again) |
| Resize to 1500x1000, opaque | 1020 ms | 323 ms | both passes banded |
| Resize to 1500x1000, translucent | 1851 ms | 571 ms | both passes banded |
| Resize to 3000x2000, opaque | 1259 ms | 399 ms | both passes banded |
| Resize to 7500x5000, opaque | 2603 ms | 780 ms | both passes banded |
| Geometry, straighten 3 degrees | 1515 ms | 347 ms | banded |
| Halving (pyramid level) | 121 ms | 36 ms | already banded |
| Export encode of 8-bit JPEG, no sharpening | 233 ms | 234 ms | library call |
| Export with sharpening 50 | 1846 ms | 1290 ms | its three loops banded |

- **The resize** runs each pass in bands of rows, each with its own scratch
  rows, as the other banded loops do. Every output row depends on the inputs
  alone, so the bits are those of one thread, and each pass is still exactly
  one unit of progress ([ADR 042](042-a-decode-or-a-render-reports-progress-and-stops-on-request.md)).
  The units are unchanged; their weights in `RenderProgress.cpp` are not, as
  the bar would otherwise crawl through the steps that got faster. Measured
  again on a 6000x4000 source turned by 3 degrees and resized to 1500 pixels:
  the pointwise chain 15 to 7 ns per pixel, the geometry 52 to 12, and the
  resize's passes 34 and 10 to 10 and 3. The intermediate picture between the passes is
  sized without being filled, and each band sets the range it keeps for its
  own rows, rather than one thread filling 100 MB first.
- **`ImageBuffer` starts zeroed as it did**, but its vectors no longer fill
  themselves: they use an allocator that default-initialises, so sizing one
  writes nothing, and the constructor then zeroes the samples in bands of rows
  on every thread. The zeroing is not a unit of progress, as it is not
  something an operation does. The allocator lives in `arraw::detail`; the
  public type of the storage names it only because the storage is a public
  `using`. The first touch of each page also happens on the thread that zeroes
  it, so the cost of the kernel's page faults is shared too. This is the
  decision ADR 042 left open: the contract stayed, and the cost went.
- **Geometry** is one resample loop per output row and is banded the same way.
- **The sharpening of an export** is three loops over rows (premultiply, two
  blurs, apply) and is banded; it is still the slowest part of an export that
  uses it, as the blurs read down columns and the picture goes through Qt as
  floats twice.
- **Not threaded: the encoders.** JPEG, PNG and TIFF writing is one call into
  Qt and its libraries and cannot be split. JPEG and TIFF take about 0.2 s at
  24 MP, which no one waits for; PNG took 6 s at Qt's default compression, and
  a lower zlib level is the lever there, not threads.
- **Not threaded: Qt's colour conversion on export.** An export of 24 MP from
  floats spent 0.9 to 1.2 s in it. Banding it was tried first and did not
  help, as Qt already runs it on several threads of its own: a profiled run
  took 12.6 s of CPU in 3.7 s of wall time. The conversion is left to Qt.

## What was rejected, and why

- **Keeping the previous picture until the decode lands.** It would show one
  photograph beside another's panel and title for seconds, which reads as the
  new photograph's edits applied to the wrong picture.
- **Queuing edits made during the decode.** See above: geometry needs the
  decoded size, and a blind edit cannot be judged.
- **Building the pyramid on the loader's thread before handing the pixels
  over.** It would delay the moment the photograph becomes editable by as much
  as it saved the render, and the renderer builds only the level it needs.
  Making the halving itself fast helps both.
- **Reading the embedded preview eagerly on every open.** Up to a second of a
  core per photograph, for a mode most photographs never enter, during the
  decode the user is waiting for.
- **`QThreadPool` or `QtConcurrent` for the reads.** A pool thread can outlive
  the window, and posting a result to a window being destroyed needs guards the
  owned-thread pattern does not: the window's destructor joins its own threads
  and no callback runs after it.
- **Decoding on the preview renderer's thread.** It owns the GPU context and
  serves renders; a decode there would block the render of the photograph
  still shown, and the decode's cancellation would be tangled with the
  renders'.
