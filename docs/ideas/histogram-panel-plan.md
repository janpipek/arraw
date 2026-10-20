# Histogram panel: final picture, clipping, pixel readout — plan

Status: done, 2026-10-20 (steps 1 to 9, and the fixes of the review in `docs/reviews/`). Decision record:
[ADR 055](../adr/055-the-histogram-panel-counts-the-picture-as-shown-on-a-thread-of-its-own.md).

## Decisions (user, 2026-10-20)

| Question | Answer |
|---|---|
| What is counted | The final picture as on screen, in the output encoding (sRGB today) |
| Region | The whole cropped frame, independent of zoom and pan; in the crop mode, the crop being drawn |
| Channels | R, G, B overlaid, and luma, with a switch |
| Clipped | Any of R, G, B at code 0 (shadows) or 255 (highlights) in the displayed 8-bit picture |
| Indicators | Shadow and highlight triangles that toggle painting clipped pixels; `J` toggles both |
| Scale | Linear with a soft cap (high percentile); end bins as markers of their own; no log switch |
| Readout | RGB in percent under the pointer |
| Placement | Top of the develop dock, above the tabs, collapsible, remembered |
| Outside the app | Python `output_histogram` only; CLI `info` unchanged |
| Out of v1 | Drag-to-adjust |

## What the code does today (verified 2026-10-20)

- `CurveHistogram` (`include/CurveHistogram.h`, `src/core/CurveHistogram.cpp`) counts the curve
  input (`Tap::CurveInput`, perceptual Rec.2020). The work happens on the preview worker at a pause,
  gated by `PreviewRenderer::setCurveHistogramWanted`, and `CurveHistogramRefresh` decides when to
  count again. There is no output tap, and none is needed (ADR 055, section 2).
- Every preview result is converted on the worker by `toDisplayImage`
  (`src/app/DisplayImage.cpp`): Qt `convertedToColorSpace(SRgb, RGBA8888)` from a
  `RGBA32FPx4` view in linear BT.2020. `prepareExportImage` (`src/core/ImageExport.cpp`) makes the
  same Qt call for an unsharpened 8-bit export.
- `MainWindow::showResult` (`src/app/ui/MainWindow.cpp`) routes each result:
  - provisional stand-ins and region renders go to `PhotoView::setImage`;
  - refreshed fallbacks go to `setBackground`;
  - crop-mode renders go to `CropOverlay::setImage`.
  `PhotoView::wholeFrameImage()` gives the render if it covers the frame, else the fallback.
- `buildDevelopDock` sets the `QTabWidget` (`developTabs_`) as the dock's widget. Sections use
  `CollapsibleSection(id, title)`, which keeps `developSections/<id>/open`.
- `J` is listed under *Planned* in `docs/keybindings.md`. `Theme.h` has one dark theme, and its
  colours are in `ThemeColors.h`.
- `arraw-app-core` sees `src/core` (ADR 036), so it can include a private core header.

## Steps

Each step builds, passes `just test` (and `just format-check`), and is reviewed before the next
that depends on it. Sonnet implements and Opus reviews into `docs/reviews/`, as in the earlier
plans. Each step lists its tests by file and case name.

### 1. Core: `OutputHistogram` and the counting loop

- `include/OutputHistogram.h`: `outputHistogramBins`, `OutputHistogram` and both `outputHistogram`
  overloads (ADR 055, section 2).
  - `static_assert` that `OutputHistogram::Bins` and `CurveHistogram::Bins` are the same type.
- `src/core/OutputHistogramCount.h/.cpp` (private):
  - `RowSpan { std::uint32_t begin, end; }` is declared in the public `include/OutputHistogram.h`
    instead, since `arraw-app-ui` names it through `HistogramCounter.h` and does not see
    `src/core`; ADR 055, section 2, records why that is accepted;
  - `countOutputCodes(const std::byte* rows, ImageSize, std::size_t stride, PixelFormat,
    NamedEncoding, std::span<const RowSpan> spans = {}, const std::atomic<bool>* abandon =
    nullptr)`;
  - the luma tables per encoding (a 256-entry decode table, 255 boundaries and a 65 536-cell
    lookup of where the boundary search lands, then a short forward walk), built once.
- `src/core/ImageExport.cpp`: factor the Qt conversion out into a private
  `encodedForOutput(const ImageBuffer&, NamedEncoding, QImage::Format)` that `prepareExportImage`
  calls. Behaviour is unchanged, which the existing `test_ImageExport` cases check.
- **Tests** (`tests/test_OutputHistogram.cpp`):
  - `"each 8-bit code lands in its own bin"`;
  - `"a grey's luma is its own code in every output encoding"` (all 256 greys; sRGB, P3, Adobe RGB);
  - `"pure red's luma is the encoded luminance, not a sum of encoded channels"`;
  - `"a pixel with any channel at 255 counts once as highlight-clipped"`, and the same for 0 and
    shadows;
  - `"a pixel at 0 and 255 counts as both"`;
  - `"float and 16-bit samples round to the nearest code"` (the boundaries 254.5/255 and 0.5/255);
  - `"fully transparent pixels are not counted"`;
  - `"spans restrict the count to their pixels"`;
  - `"working, camera and perceptual encodings are refused by the counting overload"`;
  - `"the converting overload equals counting an unsharpened 8-bit export"`, against
    `prepareExportImage`, code for code;
  - `"an abandoned count stops within a band of rows"`.
- **Measurement** (`[.bench]`, not in the default run):
  - `"count cost at 2560x1600 and 3840x2160"`;
  - `"clipping mask cost at 3840x2160"`, from step 5's function once it exists;
  - `"end-bin pixels that Lanczos adds over Bilinear"` on the fixtures, at fit sizes.
  Record the figures in the ADR's sections 5, 9 and the Consequences. **Done:** the figures are
  there. In release the count takes 52-60 ms at 2560 × 1600 and 105-118 ms at 3840 × 2160 (after
  replacing the luma binary search, which took 128-165 ms and 234-272 ms, by a lookup), against the
  budget of 10 ms: the budget is not met, and the ADR proposes the levers (counting with several
  threads, relaxing the budget, or sampling) for the user to choose. The clipping mask takes 13 to
  20 ms at 3840 × 2160. Lanczos adds 0.04 to 0.35 % of the pixels as shadow-clipped on a real
  photograph.
- **Risks:** the luma boundary search is slower than estimated; Qt's 8-bit rounding differs from
  `round(v * 255)` for float input (test it, and match Qt's if it does).

### 2. Python: `output_histogram`

- `src/python/BindPhoto.cpp` (or a new `BindHistogram.cpp` if that file is crowded):
  - `OutputHistogram` (read-only numpy `uint64` arrays, `pixels`, `shadows_clipped`,
    `highlights_clipped`, `encoding`), `OUTPUT_HISTOGRAM_BINS`, and
    `output_histogram(image, encoding=None)` (ADR 055, section 10);
  - the stubs updated.
- **Tests** (`tests/python/test_output_histogram.py`):
  - `test_counts_a_developed_buffer_as_srgb`;
  - `test_counts_an_encoded_buffer_as_is`;
  - `test_encoding_mismatch_raises`;
  - `test_perceptual_buffer_raises`;
  - `test_arrays_are_read_only`;
  - `test_equals_histogram_of_saved_png`: save at 8 bits, load, count, compare.
  - `test_stubs.py` passes.
- **Risks:** none beyond step 1.

### 3. GPU parity

- **Tests** (`tests/gpu/test_GpuOutputHistogram.cpp`):
  `"GPU and CPU output histograms agree within one bin for 1 % of pixels"`. It covers the fixtures,
  states with default settings, an exposure that clips, a local adjustment, and vignette with seeded
  grain, and requests that fit 1024 (Lanczos) or a region. The renders are counted through
  `outputHistogram(developed, Srgb)`, and the clipped counts are compared with the tolerance of
  ADR 055, section 8.
- **Risks:** a driver whose `pow` is less exact moves more than 1 %. Read the result against ADR
  017's `illConditionedRelativeTolerance` before widening the allowance.

### 4. App core: `HistogramCounter`

- `src/app/HistogramCounter.{h,cpp}` (`arraw-app-core`):
  - `submit(QImage, std::vector<RowSpan>, NamedEncoding) -> serial`;
  - `clear()`, which bumps the generation and abandons the running count;
  - a result callback with `{serial, generation, OutputHistogram}`;
  - newest-wins pending, as in ADR 055, section 3;
  - for tests, a constructor that takes the counting function, so a test can block it on a gate
    (`tests/support/ProgressGate.h` style).
- **Tests** (`tests/test_HistogramCounter.cpp`):
  - `"a submitted image is counted and reported with its serial"`;
  - `"while one count runs, only the newest pending job is counted next"`;
  - `"a running count is not cancelled by a newer job"`;
  - `"clear abandons the running count and drops its result"`;
  - `"the destructor returns promptly during a count"`;
  - `"the submitted image is not detached"` (its `cacheKey` and `constBits` are unchanged);
  - `"spans reach the count"`.
- **Risks:** a callback that runs after `MainWindow` is gone. The counter is a member destroyed
  before the window's widgets, and it joins in its destructor, as `PreviewRenderer` does.

### 5. App core: presentation helpers

- `src/app/HistogramPresentation.{h,cpp}` (`arraw-app-core`, pure):
  - `histogramScale(bins..., HistogramChannels)`: the soft cap, `histogramScaleRank = 3`;
  - `clippedChannels(const OutputHistogram&) -> {shadows, highlights}`, as channel masks for the
    triangle colours;
  - `clippedShare` (percentages for tooltips);
  - `readoutText(std::optional<QRgb>) -> QString`.
- `src/app/ClippingMask.{h,cpp}`: `clippingMask(const QImage&, bool shadows, bool highlights)`.
- `src/app/CropSpans.{h,cpp}`: `cropSpans(QSize image, std::array<QPointF, 4> quad)`, the row
  spans of the pixels whose centres lie in a convex quad. The quad comes from `CropOverlay`'s
  frame, mapped into its image.
- **Tests:**
  - `tests/test_HistogramPresentation.cpp`:
    - `"spikes in bins 0 and 255 do not set the scale"`;
    - `"one interior spike is cut at the third tallest bin"`;
    - `"the scale of RGB is the largest of the channels' scales"`;
    - `"an empty interior gives a zero scale"`;
    - `"clipped channels name the channels whose end bin is non-zero"`;
    - `"readout shows one decimal and dashes off the picture"`.
  - `tests/test_ClippingMask.cpp`:
    - `"any channel at 255 is painted red"`;
    - `"any channel at 0 is painted blue"`;
    - `"a pixel at both is painted as highlights"`;
    - `"the other pixels are transparent"`;
    - `"turned-off kinds are not painted"`.
  - `tests/test_CropSpans.cpp`:
    - `"spans equal a brute-force point-in-quad test"` (random quads, including rotated ones at
      ±45°);
    - `"an axis-aligned quad gives whole rows"`;
    - `"a quad outside the image gives no spans"`.
- **Risks:** a pixel exactly on an edge of the quad. Pin the rule (centre inside, half-open
  edges), and the brute-force test uses the same rule.

### 6. Widget: `HistogramView` and the panel

- `src/app/ui/HistogramPanel.{h,cpp}`: `HistogramPanel` (the section body), which holds:
  - `HistogramView`, the painted plot with gutters for the end markers and the cap line;
  - the two triangle tool buttons;
  - the RGB and Luma buttons;
  - the readout label.
  Its API: `showHistogram(const OutputHistogram&)`, `clear()`, `setPointerColour(std::optional<QRgb>)`,
  `setClippingShown(bool shadows, bool highlights)`, and the signal
  `clippingToggled(bool shadows, bool highlights)`.
- `src/app/ui/ThemeColors.h`: the channel, plot ground, cap and overlay colours.
- **Tests** (`tests/test_HistogramPanel.cpp`):
  - `"the channel switch is remembered in QSettings"`;
  - `"triangles are unlit without clipping and lit with it, coloured by channel"`;
  - `"triangle tooltips give the clipped share"`;
  - `"clicking a triangle emits clippingToggled"`;
  - `"triangles are disabled while there is no histogram"`;
  - `"the readout keeps its height with and without a colour"`;
  - `"accessible names are set"`;
  - `"no button takes the focus on a click"`;
  - `"painting a histogram draws end markers outside the plot area"` (render to a `QImage`, then
    sample the gutters).
- **Risks:** `CompositionMode_Plus` on the platform's backing store. The view paints into its own
  `QImage` and blits it, which keeps the result independent of the backend.

### 7. Wiring: dock placement, feeding, gating, readout

- `MainWindow::buildDevelopDock`:
  - a column widget with `CollapsibleSection("histogram", tr("Histogram"))` holding the
    `HistogramPanel`, then `developTabs_`;
  - the minimum width unchanged.
- `MainWindow`:
  - owns `HistogramCounter`;
  - `measuredPicture()`;
  - submits after `setImage`, `setBackground` and `CropOverlay::setImage`, and on `geometryEdited`
    in the crop mode with `cropSpans`;
  - skips an unchanged `cacheKey`;
  - gates on the section being open, the dock visible and a photograph open;
  - calls `clear()` on opening a photograph;
  - marshals results with `QMetaObject::invokeMethod` and drops stale serials and generations.
- `PhotoView`: `pointerColourChanged(std::optional<QRgb>)` from mouse moves (and `leaveEvent`),
  reading the render or the fallback under the pointer. `CropOverlay` emits the same signal from its
  own image, inside the frame.
- **Tests** (`tests/test_MainWindowHistogram.cpp`, CPU renderer, offscreen):
  - `"the histogram section sits above the tabs and remembers being closed"`;
  - `"opening a photograph shows the histogram of the shown picture"`: equal to
    `outputHistogram` of `wholeFrameImage()`;
  - `"zooming to 1:1 and panning leave the histogram unchanged"`;
  - `"an exposure edit changes the histogram without waiting for a pause"` (fitted);
  - `"while zoomed in, the histogram follows the refreshed fallback"`;
  - `"a crop drag in the crop mode changes the histogram without a render"`;
  - `"the mask tint is not counted"`;
  - `"nothing is counted while the section is closed or the dock hidden"` (a counter of submits);
  - `"opening another photograph clears the histogram"`;
  - `"the readout shows the pixel under the pointer and dashes over the letterbox"`.
  - `test_DevelopPanel`'s width test still passes with the section present.
- **Risks:**
  - The crop-mode quad mapping (rotation sign after flips, ADR 040). Test with a flip and a
    negative angle.
  - Feeding from three call sites. All of them go through one `submitMeasured()`.

### 8. Clipping overlay and `J`

- `PhotoView`: `setClippingShown(bool shadows, bool highlights)`; it paints `clippingMask` of the
  render only, through the render's own rectangle, no smoothing, cached by `cacheKey`. The fallback
  is never masked (the review found its mask showing through the render when zoomed in). It does
  not paint in the crop mode.
- `MainWindow`:
  - `View > Show Clipping` (`J`), checkable, added to the window (ADR 047);
  - its rule: both on if either is off, else both off;
  - kept in step with the panel's triangles in both directions;
  - the state is remembered in `QSettings` under `histogram/clipping` and restored at start.
- `docs/keybindings.md`: `J` moves to *Current*.
- **Tests:**
  - `tests/test_PhotoView*` (or `test_MainWindowView.cpp`):
    - `"with highlights shown, clipped pixels are painted red at 1:1"`;
    - `"the overlay follows a new render"`;
    - `"no overlay in the crop mode"`.
  - `test_MainWindowHistogram.cpp`:
    - `"J turns both on, then both off"`;
    - `"J works with the panels hidden"`;
    - `"a triangle click and J agree"`;
    - `"the clipping overlay is remembered in QSettings"` (round trip).
- **Risks:** GUI time for the mask on a 4K screen. Use step 1's measurement; the fallback in
  ADR 055, section 5, is the counter thread.

### 9. Docs and close-out

Done:

- `docs/desired-features.md`: the histogram panel is described under Adjustments. `J` was already
  there, and `docs/keybindings.md` moved it to *Current* in step 8.
- `docs/ideas/parked-ideas.md`:
  - the clipping overlay is done;
  - "Drag-to-adjust on the histogram" is added.
- ADR 055 status set to accepted, with the measured figures. A note in ADR 048 that the dock holds
  the histogram section.
- `docs/todo.md` is the user's; it was not touched.

## Order and parallel work

```
1 ──┬── 2 (Python)
    ├── 3 (GPU parity)
    └──────────────┐
4 (counter) ───────┤
5 (helpers) ── 6 ──┴── 7 ── 8 ── 9
```

What parallel agents can do, each in its own worktree:

- **After step 1's header is merged** (land the header with a stub implementation first if that
  helps): steps **2**, **3** and **4** at once. Step 4 only needs `countOutputCodes`'s signature,
  and its tests inject the counting function anyway.
- **Step 5 is independent of 1 to 4** (it needs `OutputHistogram`'s fields only), so it can run
  beside step 1 against the header.
- **Step 6** needs step 5's `histogramScale` and `clippedChannels`, and can start on their headers.
- **Step 8's `PhotoView` painting** needs only `clippingMask` (step 5), so it can run beside steps 6
  and 7. Its `J` and triangle wiring waits for 7.
- Steps 7 and 9 are serial: they touch `MainWindow` and the docs that everything else feeds.

## Questions settled (user, 2026-10-20)

- **Overlay colours:** fixed red (highlights) and blue (shadows); the triangles keep the channels.
- **The curve editor's scale:** stays square root; not aligned with the panel.
- **Persisting `J`:** the clipping overlay IS remembered between sessions, in `QSettings` under
  `histogram/clipping` (the user chose this over the agent's reset-at-start). Step 8 stores and
  restores it, and tests the round trip.
- **Lanczos edges:** count what is shown (the triangles and overlay always agree with the picture).
- **The soft cap's rank:** the third tallest interior bin to start; tune on real photographs in step 6.
- **Before/after (`\`, planned):** the histogram follows the picture shown; decided when
  before/after is designed.
