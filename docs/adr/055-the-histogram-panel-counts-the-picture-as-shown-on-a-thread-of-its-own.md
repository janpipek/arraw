# The histogram panel counts the picture as shown, on a thread of its own

Status: accepted, 2026-10-20; implemented (plan steps 1 to 9) and reviewed. The plan is
[histogram-panel-plan.md](../ideas/histogram-panel-plan.md). Picks up the parked "Clipping overlay
(`J`) and highlight/shadow warnings" ([parked-ideas.md](../ideas/parked-ideas.md)) and the planned
`J` key ([keybindings.md](../keybindings.md)).

A photographer reads a histogram to answer two questions: where the tones of the finished picture
sit, and whether anything is clipped. The curve editor already draws a histogram
([ADR 035](035-the-curve-input-is-sampled-through-the-render-and-binned-on-the-host.md),
[ADR 036](036-the-curve-editor-is-painted-and-its-histogram-follows-the-plan.md)), but it counts
the *curve input*: Rec.2020 in the perceptual coordinate, before the curves, the shoulder, the
colour controls, the local adjustments after the tap and the effects. It cannot answer either
question about the final picture. This ADR adds a panel that can.

**What exists.** `Tap::CurveInput` is the only tap. There is no "final-output tap" and none is
needed: a tap names a point *inside* the pointwise chain that a render does not hand back
(ADR 011, ADR 035), whereas the final picture is what `develop()` hands back. Every preview
result is converted on the render worker by `toDisplayImage` (Qt's `convertedToColorSpace`, linear
Rec.2020 float to 8-bit sRGB `RGBA8888`), the same conversion an 8-bit export makes without
sharpening (`prepareExportImage`). The window then holds the shown render and a reduced
whole-frame fallback (`PhotoView::wholeFrameImage()`).

**User decisions (2026-10-20, final).**

| Question | Answer |
|---|---|
| What is counted | The final picture, as on screen after every edit, in the output colour space (sRGB today; Display P3, Adobe RGB, soft proofing and output encodings must not break it) |
| Region | The whole *cropped* frame as it will export, independent of zoom and pan; in the crop mode, the crop being drawn |
| Channels | R, G, B overlaid, and luma, with a luma/RGB switch |
| Clipped | A pixel is clipped if **any** of R, G, B is 0 (shadows) or 255 (highlights) in the displayed 8-bit picture; the overlay may colour by channel |
| Clipping indicators | Shadow and highlight triangles that toggle painting the clipped pixels on the picture; `J` toggles both, as in Lightroom |
| Vertical scale | Linear counts with a soft cap: the scale is a high percentile of the bins, so a spike at 0 or 255 does not flatten the rest; the end bins get a marker of their own; no log switch in v1 |
| Pixel readout | RGB in percent, under the pointer over the picture |
| Placement | Top of the develop dock, above the Adjustments/Masks tabs, collapsible, remembered in `QSettings` |
| Outside the app | Python only, small: a function returning the bins of a rendered image. The CLI's `info` is unchanged |
| Out of v1 | Drag-to-adjust on the histogram (recorded as a later idea) |

## Decision

### 1. What is counted: the 8-bit picture in the output encoding

The histogram counts **the 8-bit picture of the whole cropped frame, in the output encoding**:
the pixels a screen shows, and what an 8-bit export at that size would write, sharpening apart.

- **256 bins, one per 8-bit code.** Bin `i` holds the pixels whose channel code is `i`. That is
  exact for the shown picture, so the end bins *are* the clipping counts of decision 4, and the
  histogram, the triangles and the overlay can never disagree.
  - *Alternative:* 1024 or 4096 bins from the float render, for a 16-bit export. The plot is
    about 300 px wide, so the extra resolution cannot be seen, and clipping was decided at 8 bits.
  - *Alternative:* count the float render before quantising. That needs a second conversion on
    the worker and gives values the screen never shows.
- **Float and 16-bit inputs** (Python, step 2 of the plan) are quantised to the nearest code first,
  `round(clamp(v, 0, 1) * 255)`, as Qt's 8-bit conversion does. So a 16-bit value of 0.999 counts as
  255, though a 16-bit export would not clip it. This is accepted; it matches the screen.
- **Luma is the output space's luminance, encoded with its transfer function**: decode each
  channel to linear, weight the channels by the Y row of the encoding's RGB-to-XYZ matrix (sRGB and
  Rec.709: 0.2126, 0.7152, 0.0722; Display P3 and Adobe RGB: their own), encode the sum again, and
  round to a code. This is ADR 035's rule (luminance, then encoded) moved to the output space. A grey
  of code `g` therefore has luma `g` exactly, in every encoding, and pure sRGB red has luma code 127,
  not the 54 that a weighted sum of encoded values gives. It is computed with a 256-entry decode table and the 255 decoded code
  boundaries, so it is exact and needs no `pow` per pixel. The boundary is found by a lookup table
  of 65 536 equal cells of linear luminance, each holding where a binary search would land for the
  cell's start, then a short forward walk over the boundaries inside the cell (two lie within one
  cell at the dark end of Adobe RGB). That gives the same code as a binary search for every input
  (a test compares the two over every red and green with nine blues, in the three encodings), and
  it avoids the search's mispredicted branches, which were most of the cost of the count (section 9).
  - *Alternative:* Rec.601 Y′, a weighted sum of the *encoded* channels (Photoshop's
    "Luminosity"). It is cheaper, but it disagrees with the curve histogram's luma and depends on
    the encoding's gamma.
  - *Alternative:* Oklab L or CIE L\*. These are perceptually uniform, but they are a third
    coordinate that no other control in arraw uses.
- **Alpha.** A fully transparent pixel is not counted, as in ADR 035. The preview is opaque, so
  this matters only for Python.
- **The encoding is recorded with the counts** (`OutputHistogram::encoding`) and named in the
  panel's tooltip ("sRGB").

**The encoding that is counted is the output's, never the monitor's.** Today the display image is
sRGB, and sRGB is the default output, so the picture on screen *is* the picture in the output
encoding, and it is what gets counted. The rule for later:

- When the display gains the monitor's ICC profile (desired-features), the shown image stops being
  the output picture. The worker then also delivers the 8-bit picture in the output encoding
  (`PreviewResult::outputImage`, the same conversion with another target), and the histogram, the
  readout and the overlay read that image instead.
- With soft proofing (`S`, planned), the output encoding is the proof's target (Display P3 or
  Adobe RGB), and the same rule applies.
- In the app, one accessor (`MainWindow::measuredPicture()`) gives the image that is counted. Today
  it returns `PhotoView::wholeFrameImage()`, so either change touches one function.

### 2. A sibling type of `CurveHistogram`, not a reuse of it

`include/OutputHistogram.h` (namespace `arraw`):

```cpp
inline constexpr std::size_t outputHistogramBins = 256;

struct OutputHistogram {
    using Bins = std::array<std::uint64_t, outputHistogramBins>; // same type as CurveHistogram::Bins
    NamedEncoding encoding = NamedEncoding::Srgb;
    Bins luma{}, red{}, green{}, blue{};
    std::uint64_t pixels = 0;            // counted
    std::uint64_t shadowsClipped = 0;    // pixels with any channel at code 0
    std::uint64_t highlightsClipped = 0; // pixels with any channel at code 255
    friend bool operator==(const OutputHistogram&, const OutputHistogram&) = default;
};

// Counts a picture already in an output encoding (sRGB, Display P3, Adobe RGB), any pixel format.
[[nodiscard]] OutputHistogram outputHistogram(const ImageBuffer& picture);
// Converts a developed (working-encoding) render to `encoding`, as an 8-bit export does, then counts.
[[nodiscard]] OutputHistogram outputHistogram(const ImageBuffer& developed, NamedEncoding encoding);
```

- Each channel's clipping count is its own `bins[0]` and `bins[255]`. The two "any channel"
  counts cannot be derived from the per-channel bins, because the channels overlap, so they are
  fields of their own.
- **One counting loop**, a private `src/core/OutputHistogramCount.h`
  (`countOutputCodes(rows, size, stride, format, encoding, spans, abandon)`), serves both public
  overloads and the app. `arraw-app-core` sees `src/core`, as `CurveEditing` does (ADR 036). The app
  calls it on the `QImage`'s bits without a copy, and `spans` (optional, one `[begin, end)` per
  row) restricts it to the crop being drawn (decision 3).
  - **`RowSpan` is public.** The span type is declared in `include/OutputHistogram.h`, not in the
    private header: the app's `HistogramCounter.h`, and through it `MainWindow.h` in `arraw-app-ui`,
    name it, and `arraw-app-ui` does not see `src/core`. No public function takes one, so it is
    public API only so that the app can carry spans; it is a plain struct and stays small. The
    alternatives were to move it to an app-core header and have `countOutputCodes` take
    `std::span<const std::pair<std::uint32_t, std::uint32_t>>`, or to hide the job behind a pimpl
    in `HistogramCounter`; both cost more than a documented public struct.
- **The conversion is export's.** The Qt conversion in `prepareExportImage` is factored into a
  private `encodedForOutput(buffer, encoding, format)` in core, and `toDisplayImage` is checked
  against it (`tests/test_DisplayImage.cpp`, "The display image equals the sRGB output of an
  export, code for code"; a test, not shared code, because the app's conversion owns a view of the
  render). That is how the Python histogram of a develop and the app's histogram of the same request agree code
  for code.

*Alternatives:*
- (a) **A sibling type (chosen).** The two histograms count different quantities in different
  coordinates. The curve input is unbounded (values above white are meaningful before the
  shoulder), so "clipped" means nothing there. Sharing the `Bins` array type lets one painting
  helper draw both.
- (b) Rename `CurveHistogram` to a general `Histogram` with a "what was counted" tag. One type
  fewer, but it breaks the Python API, and the clipping fields would be meaningless for half the
  values.
- (c) A new `Tap::Output`. A tap is a point inside the pointwise chain; the final picture comes
  after geometry, the resize and the effects, and `develop` already hands it back.

### 3. Where and when it is counted: a counter thread fed from the GUI thread

`src/app/HistogramCounter.{h,cpp}` (`arraw-app-core`, no widgets) owns one `std::jthread` and a
pending slot that holds only the newest job, as `PreviewRenderer` does. A job is a `QImage` (a
shallow copy), optional row spans, the encoding and a serial. The result goes to a callback on the
counter thread, and `MainWindow` hands it to the GUI thread with `QMetaObject::invokeMethod`, as it
does with preview results.

- **The picture is never delayed.** The GUI thread submits an image after `PhotoView` has it, which
  costs one reference count and one notify. The render worker is not involved at all, so neither a
  render nor its next request waits for a count.
- **What is submitted.** After every change of the shown or fallback image, `measuredPicture()` is
  submitted if its `cacheKey()` differs from the last submitted:
  - a render that covers the whole frame (fit, or zoomed out past the frame) is counted as it
    lands, and that includes the **coarser stand-in** (ADR 044, section 8) and the **reduced
    pictures of a brush stroke**. The coarse render is therefore reused, never redone;
  - when zoomed in, the shown render is a region, so the whole-frame fallback beneath it is counted
    instead: the last whole-frame render, refreshed by the renderer once requests pause (150 ms
    after the last edit, then one render at 1024 px). During a drag at 1:1 the histogram holds the
    last whole-frame count and catches up at the pause. Nothing is marked stale; the lag is short
    and a flicker would be worse.
  - The thumbnail stand-in before the first render (ADR 043) is not counted. It is a cached
    thumbnail, not this render.
- **Zoom and pan never change the histogram** (user decision): it is always of the whole cropped
  frame. At 1:1 the shown region is not counted.
- **The crop mode counts the crop being drawn.** The mode's render is the uncropped,
  unstraightened frame (ADR 040). Each crop-mode render, and each `geometryEdited`, submits that
  image with the spans of the crop quad mapped into it (the frame rotated by the displayed angle),
  so a crop drag recounts without a render. Newest-wins coalesces a fast drag. Until the mode's
  first render lands the histogram keeps its last count, and the placeholders are not counted. The
  mode renders without effects (ADR 040), so the vignette and grain are missing from this count
  until the mode ends.
- **Masks.** The mask tint, handles and brush cursor are painted by `MaskOverlay` over the
  picture, not rendered into it, so they are never counted. The histogram shows the picture with
  the local adjustments applied, while a mask is being edited too.
- **Gating.** Nothing is submitted while the panel is collapsed, the dock is closed or hidden (F8,
  F12), or no photograph is open. When the panel becomes visible it submits `measuredPicture()` at
  once.
- **Cancellation and lifetime.** A newer job replaces the pending one and never cancels the
  running one: a count is bounded (decision 9), and cancelling it would starve the histogram during
  a drag whose frames come faster than a count. A new photograph (`clear()`) and the destructor
  abandon a running count at its next band of 64 rows. Results older than the newest shown, or
  belonging to an earlier photograph (by generation), are dropped on the GUI thread.
- **Thread safety.** The job owns a `QImage` copy. Its data is shared and reference-counted
  atomically, and any writer detaches first, so the counter reads pixels nobody writes. No other
  state is shared; the callback carries everything back by value.

*Alternatives:*
- (a) **A counter thread fed by the GUI (chosen).** It works for every image the window shows,
  crop-mode edits without a render included, and it leaves `PreviewRenderer` alone.
- (b) Count on the render worker after `toDisplayImage`, inside `PreviewRenderer`. This needs no
  new thread, but every count delays that worker's next render. The crop mode, whose edits make no
  render, would need a second path, and `PreviewRenderer` is already the most intricate class of
  the app.
- (c) Count on the GUI thread. This is the simplest, but it costs 5 to 25 ms of GUI time per frame
  on a large screen during a drag (estimate; section 9 gives the measured count), against the spirit of
  ADR 043.
- (d) A dedicated bilinear 1024 px render, as the curve histogram has. It is stable across window
  sizes, but it costs a render per state, waits for the pause, and is not what is on screen.

### 4. What "clipped" means, and the triangles

Per the user's decision: in the 8-bit picture in the output encoding, a pixel is
**shadow-clipped** if any of R, G and B is code 0, and **highlight-clipped** if any is code 255. No
tolerance is added beyond the 8-bit rounding itself: codes 1 and 254 are not clipped. Out-of-gamut
colours that the conversion clamps to 0 or 255 count, since they are clipped in the output.

- **Triangles**: a checkable tool button at each top corner of the plot (shadows on the left,
  highlights on the right).
  - **Unlit** (an outline) when nothing is clipped. **Lit** (filled) when the count is above 0,
    filled with the colour of the channels that clip: R, G or B alone, two channels as their sum
    (yellow, cyan, magenta), and white for all three, as in Lightroom.
  - The difference between lit and unlit is fill against outline, not colour alone.
  - The tooltip gives the share of pixels, for example "Highlights clipped: 1.2 % of pixels (R 1.1 %,
    G 0.4 %, B 0.2 %)".
- **Toggling.** A click on a triangle toggles painting that kind of clipping on the picture. `J`
  (View > Show Clipping) turns both on if either is off, and otherwise both off. The state is view
  state: not undoable, but remembered between sessions in `QSettings` under `histogram/clipping`
  (0 none, 1 shadows, 2 highlights, 3 both), and it starts off on a first run.
- *Alternative:* light a triangle only above a share of pixels (say 0.01 %). That would hide the
  few clipped pixels a photographer is looking for.

### 5. The clipping overlay is painted from the shown image, not rendered

`PhotoView` paints the overlay over the render. The overlay is a mask of that same image, drawn
through the same rectangle as the render, so no second render is needed and every mark lies on the
pixel it was made from, at any zoom.

- **Only the render is masked, never the fallback.** When zoomed in, the render is a region and the
  reduced whole-frame fallback shows around and beneath it. A mask of the fallback drawn under or
  over the render would put marks of a coarser picture, and during a drag of an older state (the
  fallback is refreshed only at the pause), on pixels that do not clip. The area outside the
  render's rectangle shows the fallback unmasked; the render covers the visible region once it
  lands. (The first version masked both and painted the fallback's mask across the whole frame; the
  review found it, and `test_MainWindowHistogram.cpp` now zooms in for real and checks that the
  fallback's clipping never shows through the render, and that a render that no longer clips drops
  the old marks.)

- `clippingMask(const QImage&, bool shadows, bool highlights)` (pure, `arraw-app-core`) returns an
  `ARGB32_Premultiplied` image that is transparent where nothing clips. Highlight-clipped pixels are
  **red**, shadow-clipped ones **blue**, and a pixel that is both is shown as highlights. These are
  Lightroom's colours, readable on any picture, and they are not themselves picture colours.
  *Alternative:* colour by the channels that clip (Camera Raw's Alt-drag), which the user allowed. It
  says more, but a yellow mask on a yellow flower cannot be seen. The triangle's colour and tooltip
  carry the channel instead.
- It is built **on the GUI thread, lazily**: only while an overlay is on, once per render (cached by
  `cacheKey()`, and dropped when both kinds are turned off), and drawn with the render's transform
  and no smoothing, so mask pixels stay crisp when magnified. This keeps the overlay in step with
  the picture: the two can never be shown a frame apart. Measured (release, 3840 × 2160, best of
  five): 13 ms for a picture that clips nothing and 20 ms for noise that clips about 3 % of its
  pixels, on a 2.6 GHz laptop core in a container. That is within a frame at 60 Hz only for the
  lighter case; if the mask must be cheaper on a 4K screen, it moves to the counter thread and the
  previous one is drawn stretched until it lands (not done).
- In the crop mode the overlay is not painted (the overlay there draws its own rotated image); the
  triangles still show the crop's counts.
- In the mask mode the overlay is painted beneath `MaskOverlay`, which is a child on top.

### 6. The pixel readout reads the shown image on the GUI thread

`PhotoView` maps the pointer through its `ViewTransform` to the image it paints at that spot (the
render if it covers the point, otherwise the fallback) and emits `pointerColourChanged`, an
optional `QRgb` that is empty off the picture. The panel shows it under the plot as
`R 45.1 %   G 40.2 %   B 38.0 %` (code / 255, one decimal), or dashes off the picture. The line
keeps its height either way, so the panel does not jump.

- These are the output encoding's values, the same picture the histogram counts.
- When the picture is fitted, a screen pixel averages several photo pixels, so the readout is of
  the picture as shown. At 1:1 it is per photo pixel.
- In the crop mode it reads the crop overlay's image, inside the frame only. Over the mask tint it
  reads the picture beneath.
- *Alternative:* sample the full-resolution render at the pointer. That is a render per pointer
  move, and a value the screen does not show.

### 7. The panel

- **Placement.** The develop dock's widget becomes a column: a `CollapsibleSection` with the stable
  id `histogram`, its open state kept under `developSections/histogram/open` (ADR 048's key,
  default open), then the tab widget. It is not inside either tab's scroll area, so it stays put
  while the tabs scroll. The dock's minimum width is unchanged; the plot follows the width.
- **Contents.** A painted `HistogramView` (Qt Widgets, as `CurveEditor`), about 110 px tall. It has
  the two triangles at its top corners, then two checkable tool buttons, **RGB** and **Luma**
  (exclusive, remembered under `histogram/channels`), and the readout line.
- **Drawing.** In RGB, each channel is a filled area in its colour, composed additively
  (`CompositionMode_Plus`) over the plot's own dark ground, so where they overlap the plot shows
  cyan, magenta, yellow and white, as in Lightroom. In Luma, one area is drawn in the text colour.
- **Scale (user decision): linear, with a soft cap.**
  - The plot's height is the **99th percentile of the interior bins** (1 to 254) of the channels
    drawn. That is the third tallest interior bin of each channel (`histogramScaleRank = 3`), and
    the largest of those over the channels.
  - Taller bins are cut at the top, with a one-pixel cap line in a brighter tone to show the cut.
  - An all-zero interior (a picture of pure black and white) draws only the end markers.
  - Bins 0 and 255 are **not** part of the area, and they do not set the scale. Each is drawn as
    its own **marker**: a 3 px column in a gutter outside the plot area, at its height on the same
    scale and cut at full, in the channel colours, hidden when empty.
  - No log switch in v1.
  - The curve editor keeps its square-root scale (ADR 036). The two plots show different things,
    and the question below asks whether to align them.
- **Theming.** The app has one dark theme (`Theme.h`). The channel colours, the plot ground and the
  overlay colours are new `ThemeColors` entries. The plot always draws its own dark ground, since
  additive blending needs one.
- **Accessibility.**
  - The plot has the accessible name "Histogram" and a description of its state ("Shadows clipped
    0.3 %, highlights clipped 1.2 %").
  - The triangles and switches are tool buttons with accessible names ("Show clipped shadows",
    "Show clipped highlights", "RGB", "Luma"). They take the focus by Tab only, never by a click
    (ADR 040).
  - The readout is a plain label that is not announced on each move.
  - Lit and unlit are told apart by shape as well as colour. Red and blue stay distinct under the
    common colour-vision deficiencies.
- **Empty and loading states.**
  - With no photograph, the dock is disabled (ADR 048) and the plot shows "No photograph" in the
    placeholder colour.
  - While a photograph opens, the last histogram is cleared and the plot shows nothing until the
    first count. A spinner would only duplicate the render indicator.
  - When the section is collapsed or the dock hidden, the plot is emptied, and counted again on
    return (the same as opening a photograph).
  - A failed render keeps the last count, since the picture beside it stays too; the plot is
    empty only before the first count. Triangles are unlit and disabled while there is no count.
- **Out of v1:** drag-to-adjust (dragging in a tonal region moves the matching Basic Tone
  control), recorded in `parked-ideas.md`.

### 8. GPU and CPU: one count, on the host

The counter reads the 8-bit picture, which both paths produce on the host: the GPU's render is read
back and converted by the same `toDisplayImage`. So the histogram is as close between the two as
the renders are (ADR 017's tolerances: the pointwise and resample bounds, one 8-bit code).

- **The parity test** (`tests/gpu`) renders one state and one request on both paths, converts and
  counts them, and allows at most 1 % of pixels per channel to move, by one bin only. It checks
  that on the pictures themselves, code by code (no sample differs by more than one code, and at
  most 1 % of the samples of a channel differ), as well as on the histograms. A clipped
  count may differ only by pixels within one code of 0 or 255 in the other render. ADR 035 set the
  same allowance.
- *Alternative:* count on the GPU with a reduction pass. The pixels are read back for display
  anyway, a reduction needs storage-buffer atomics that QRhi exposes unevenly, and the host count
  takes milliseconds.

### 9. Cost

The count runs over the shown whole-frame image, at most the viewport's device pixels (about 8 MP
fitted on a 4K screen), on a thread that holds nothing else up. It does a table lookup per
channel and one table lookup for luma. The original estimate was 2 to 3 ns a pixel, so up to
about 25 ms on a 4K screen, and a budget of **10 ms for 2560 × 1600**.

**Measured** (`[.bench]` cases in `tests/test_OutputHistogram.cpp` and `tests/test_ClippingMask.cpp`;
release build, `-O3`, best of five; Intel i7-10750H at 2.6 GHz in a container, so a desktop will be
somewhat faster, and the container's timings vary by about 30 % between runs):

| What | 2560 × 1600 (4.1 MP) | 3840 × 2160 (8.3 MP) |
|---|---|---|
| Count, luma by binary search (first version) | 128-165 ms, rainbow; 375 ms, noise | 234-272 ms, rainbow |
| Count, luma by lookup (this ADR) | 52-60 ms (rainbow, noise, photograph) | 105-118 ms |
| Clipping mask (section 5) | | 13 ms flat, 20 ms noise |

About 13 ns a pixel. The binary search over 255 floats mispredicts on most pixels and was most of
the cost; the lookup is 2 to 6 times faster and gives the same codes. The remaining cost is four
increments into random bins per pixel (a loop that counts only R, G, B and the clipping takes about
4 ns a pixel on this machine, and the luma conversion and its increment add as much again).

**The budget is not met.** 2560 × 1600 takes about 55 ms against 10 ms, five to six times over,
and a 4K screen about 110 ms. So the histogram updates at about 8 to 18 Hz during a drag, and the
counter thread keeps one core busy while the drag lasts. This is not hidden by the thread, but it is
not a problem for the picture (the count never delays a render). The budget is unchanged here. The
levers, for the user to choose:

1. **Count with several threads.** Split the rows into bands over N workers and add the bins
   (exact, because counts add). With 4 to 6 workers a 2560 × 1600 count is about 10 to 15 ms. It
   competes with the CPU renderer's bands during a drag, so the counter would take a share of the
   cores, not all of them.
2. **Relax the budget** to what is measured (about 60 ms at 2560 × 1600), and accept 8 to 18 Hz.
3. **Sample every second row and column above 2 MP** for the bins, as this section first proposed
   (a 4 times saving, so about 14 ms), and keep a full pass for the clipping counts, which must
   cover every pixel so that the triangles agree with the overlay. The full pass is then the same
   cost as today's R, G, B and clipping part (about a third of the count), so the saving is
   about 2.5 times, not 4, and a sampled distribution flickers a little between frames.

The **clipping counts are always taken over every pixel**, so the triangles always agree with the
overlay.

### 10. Python, small; the CLI unchanged

Per the user's decision, Python gets one function, and the CLI's `info` does not change:

```python
arraw.output_histogram(image, encoding=None) -> arraw.OutputHistogram
```

- `image` is either a developed buffer in the working encoding, which is converted to `encoding`
  (default `NamedEncoding.SRGB`) as `save` would convert it at 8 bits, or a buffer already in
  sRGB, Display P3 or Adobe RGB, which is counted as it is (`encoding` must then be `None` or equal
  to it).
- Any other encoding raises `ValueError`.
- `OutputHistogram` has read-only `uint64` numpy arrays `luma`, `red`, `green` and `blue`, and
  `pixels`, `shadows_clipped`, `highlights_clipped` and `encoding`. The constant
  `OUTPUT_HISTOGRAM_BINS` is bound as well.
- The size and filter are the caller's, through `develop(..., size=...)`. There is no
  `Photo` overload: `output_histogram(develop(photo, size=1024))` is one line.

## Consequences

- **The histogram describes the picture at the size it is shown.** Lanczos (the preview's filter)
  overshoots at hard edges, so a pixel next to a bright edge can show 255 on screen, and count as
  clipped, where a full-size export would not. ADR 035 forced Bilinear for the curve histogram for
  exactly this reason. Here it is accepted: the overlay paints those pixels too, so the triangles
  never claim what the picture does not show. Measured on a real photograph (`samples/IMG_2610.JPG`,
  3000 × 2000, developed with default settings; the `[.bench]` case "end-bin pixels that Lanczos
  adds over Bilinear"): at fit 1024 Lanczos adds 32 highlight-clipped and 2 429 shadow-clipped
  pixels of 699 392 (0.35 % of them shadows); at 1600, 34 and 2 197 of 1 707 200 (0.13 %); at
  2560, 73 and 1 692 of 4 369 920 (0.04 %). The shadow figures are dark edges of the photograph, and
  would be a visible overlay of a few thousand pixels beside edges. If that matters, the fix belongs in the resize (a clamp of the overshoot,
  on both paths), not in a different histogram source.
- The histogram depends a little on the window size, and between fitted and zoomed views, because
  the fallback is a 1024 px render. The differences are those of a resampled statistic.
- In the crop mode the counted picture has no vignette or grain (ADR 040), so the histogram
  shifts slightly on entering and leaving the mode.
- During a drag while zoomed in, the histogram lags until the pause.
- Output sharpening (ADR 026) is export-only, so it is never in the histogram.
- **ADR 048 is amended:** the develop dock holds the histogram section above its tab widget. The
  tab-follows-mode rule is unchanged.
- `J` moves from *Planned* to *Current* in `keybindings.md`. `View > Show Clipping` is a checkable
  action, added to the window as ADR 047 requires, so it works with the menu bar hidden.
- One more thread in the app (`HistogramCounter`), idle unless the panel is shown.
- `PreviewRenderer` and `CurveHistogram` do not change. The curve editor's histogram keeps its own
  source, scale and gating (ADR 036).

## What was rejected, and why

- **A second render for the histogram or the overlay.** Everything needed is already in the
  window's images.
- **The histogram of the visible region at 1:1.** The user decided on the whole cropped frame, and
  a histogram that changes with panning measures the view, not the photograph.
- **A log or square-root scale switch in v1.** The user chose linear with a soft cap.
- **A CLI histogram in `info`.** The user chose Python only.
