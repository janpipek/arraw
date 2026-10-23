# Review: histogram panel (branch `histogram`, plan steps 1-8)

Reviewer: Claude Opus 5.5, 2026-10-20. Scope: `git log main..HEAD` (c2f50c3..ce67c42), checked
against [ADR 055](../adr/055-the-histogram-panel-counts-the-picture-as-shown-on-a-thread-of-its-own.md)
and [the plan](../ideas/histogram-panel-plan.md). Step 9 (docs close-out) is not done, as expected.
No production code was changed. I built `arraw-tests` in `build/container-release` and ran the
`[.bench]` count case in release and debug. I also ran two standalone micro-benchmarks, described
under M1. I did not run the widget suite.

**Counts:** critical 1, medium 4, low 13.

The core counting is correct. Bins, luma, clipping on any channel, transparency, NaN, 16-bit and
float rounding, span validation and the abandon band all match the ADR. The counter's lifetime and
generation handling are sound. The serious problems are elsewhere: how the overlay is composed, the
cost in a release build, and tests that never actually zoom in.

---

## Critical

### C1. The clipping overlay shows the fallback's mask through the 1:1 render

`src/app/ui/PhotoView.cpp:269-277`. When the view is zoomed in, `paintEvent` first draws the mask of
`background_` over the **whole frame**, then draws the mask of `image_` over `target`. The mask is
transparent wherever nothing clips, so inside `target` the fallback's mask shows through every
render pixel that does not clip. The fallback picture itself is hidden there by the opaque render,
but its mask is not.

Failure scenarios:

- **1:1 inspection, the main use of the overlay.** The fallback is a reduced render of about
  1024 px, and its mask is drawn without smoothing. Every clipped fallback pixel paints a red block
  several screen pixels wide over the detailed region. The overlay then marks pixels as clipped that
  the render beside them shows as unclipped. This breaks ADR 055 section 5 ("lines up pixel for
  pixel at any zoom").
- **A drag at 1:1.** The fallback is refreshed only at the pause (150 ms), so during an exposure
  drag that removes clipping, the red marks of the *previous* state stay painted over the live
  region.
- **The coarser stand-in while fitted** (ADR 044 section 8). The renderer sends
  `coarser.background = background`, which is the previous whole-frame render
  (`PreviewRenderer.cpp:902`). Its mask, from the old state, shows through the coarse frame until
  the full render lands.

Fix options:

- (a) Paint the background mask only outside `target`, using a clip region of the frame minus
  `target`.
- (b) Skip the background mask when `background_.cacheKey() == image_.cacheKey()`, or when the
  render covers the frame.
- Do both. Add a `ClippedView` test where the background clips and the render does not.

---

## Medium

### M1. The count is 10-15× over the ADR's budget in release, and the planned fallback cannot close the gap

The ADR estimates 2-3 ns a pixel and sets a 10 ms budget for 2560×1600 (section 9). It records no
measured figures yet. Sections 5 and 9 and the Consequences still say "step 1 measures it".

The `[.bench]` case `"count cost at 2560x1600 and 3840x2160"` gives these times on this container,
over three release runs and one debug run:

| Build | 2560×1600 | 3840×2160 |
|---|---|---|
| Release | 128-165 ms | 234-272 ms |
| Debug | 2133 ms | 4265 ms |

That is about 30 ns a pixel in release, on a smooth rainbow input that favours branch prediction.
Debug is about 15× slower again, so any figure measured in debug says nothing.

A standalone copy of the loop at 2560×1600 with random pixels separates the cost:

| Part of the loop | Time |
|---|---|
| R, G, B bins and clipping | about 16 ms |
| Adding the `std::ranges::upper_bound` luma search | about 350 ms |

The binary search over 255 floats mispredicts on most pixels. A 65 536-entry `uint8` table indexed
by `lin * 65536`, plus a fix-up compare against `boundaries[l]`, gave identical bins to the search
in about 28 ms total. That is roughly 12× faster, and it stays exact. One caveat: at the dark end of
Adobe RGB two boundaries fall within 2⁻¹⁶ of each other (about 1.1e-6 and 1.25e-5), so the fix-up
there must be a short `while` loop, or the table finer.

The ADR's fallback, counting every second row and column above 2 MP, gives only 4× and is not
implemented. Sampling also cannot reach the "clipping always over every pixel" rule unless it keeps
a full pass anyway.

Effect today:

- On a 4K screen (6-8 MP fitted) a count takes about 250-300 ms in release, so the histogram updates
  at 3-4 Hz during a drag.
- The counter thread keeps one core busy for as long as the drag lasts, competing with the CPU
  renderer's bands.

**What must be measured in release before step 9 records figures:**

1. The count at 2560×1600 and 3840×2160 on a real fixture render, not only the rainbow. Random and
   smooth inputs differ by about 3× because of branch prediction.
2. The clipping mask at 3840×2160. Its `[.bench]` case from the plan is missing.
3. The end-bin pixels Lanczos adds over Bilinear. Its `[.bench]` case from the plan is missing.
4. GUI-thread time per frame with the overlay on during a fitted drag (see M2).

**Verdict on the budget:** not credible with the binary search. It is credible with a lookup table:
about 1.5-2 ns a pixel for the whole loop, so 2560×1600 fits in about 10 ms on a desktop. Record
the release figures in sections 5 and 9, and either change the luma implementation or relax the
budget explicitly.

### M2. The clipping mask is built twice per frame on the GUI thread when fitted, and its cost is unmeasured

`PhotoView.cpp:269-277` and `PhotoView.h` (`imageMask_` and `backgroundMask_`).

- When fitted, the renderer delivers `result.background = result.image`: the same `QImage` with the
  same `cacheKey`. The two caches are separate, so `clippingMask` runs twice for each new render, and
  the result is drawn twice.
- A standalone copy of the loop, including the 33 MB allocation, takes 35-65 ms at 3840×2160 on this
  container.
- With the overlay on during a slider drag on a 4K screen, that is about 70-130 ms of GUI time per
  frame. This goes against the spirit of ADR 043, and ADR 055 section 5 names this exact risk.
- Both masks (up to about 32 MB each) stay cached after the overlay is turned off.

Fix options:

- Share one cache when the keys are equal; C1(b) does this for free.
- Release the caches in `setClippingShown(false, false)`.
- Measure the cost. If it exceeds a frame budget, take the ADR's own fallback and build the mask on
  the counter thread.

### M3. The "zoomed in" window tests never zoom in

`tests/test_MainWindowHistogram.cpp:186-222`. The fixtures are 32×24 px, and
`ViewTransform::fitZoom` never exceeds 1. The fitted view is therefore already at 1:1, and
`zoomTo(4.0)` shows a 128×96 picture that still fits the 1400×500 window. The render region stays
the whole frame, `panBy` does nothing, and `wholeFrameImage()` returns the render, not the fallback.

As a result these two cases never exercise the fallback path:

- `"Zooming to 1:1 and panning leave the histogram unchanged"`
- `"While zoomed in, the histogram follows the refreshed fallback"`

The final `CHECK(histogramSubmitCount() >= submits)` in the first is vacuous. None of these cases
would catch C1.

Fix options:

- Shrink the window so that the frame overflows it at zoom 4, and `REQUIRE` that the region is not
  the whole frame before asserting anything.
- Or use a larger fixture.

### M4. ADR 055's promised display-versus-export check is missing, and the ADR no longer matches the code

- **Missing check.** ADR §2 says "`toDisplayImage` is checked against [`encodedForOutput`] (a test)".
  No test compares the two. They agree today: both use `QColorSpace::SRgb` (`DisplayImage.cpp:37`,
  `QtImage.cpp:49`) and the same Qt call. Nothing guards that agreement, yet it is the whole basis
  for "the Python and app histograms agree code for code".
- **`RowSpan` moved.** It went to the public `include/OutputHistogram.h` in ee462d9 so that
  `arraw-app-ui` (via `MainWindow.h` → `HistogramCounter.h`) can see it. ADR §2 and plan step 1
  still put it in the private header. It is now public API that no public function takes. Options:
  - (a) Accept it, and document it in the ADR.
  - (b) Move it to an app-core header and have `countOutputCodes` take
    `std::span<const std::pair<std::uint32_t, std::uint32_t>>`.
  - (c) Keep `HistogramCounter.h` free of `RowSpan`, for example by hiding the job behind a pimpl.

  (a) is the cheapest. Record it in step 9.

---

## Low

1. **The readout freezes or goes stale.** `PhotoView.cpp:356` emits only on mouse moves, so the
   readout keeps an old value in these cases:
   - after a wheel zoom, a keyboard pan or a new render under a still pointer;
   - during a mask gesture, because `MaskOverlay` accepts the move (`MaskOverlay.cpp:766`);
   - after leaving the crop mode, until the next move.

   Re-emit from `setImage` and `adopt` with the last pointer position.
2. **Readout coordinates are fine but untested.** Under zoom, HiDPI, rotation and flip, the readout
   maps device to frame fraction to region correctly (`PhotoView.cpp:323-340`). The crop overlay
   inverts the painter's rotate about the middle exactly. But the readout test covers only the
   fitted 1:1 case, and the crop test has no flip, although the plan's risk list asked for "a flip
   and a negative angle".
3. **No "No photograph" text.** ADR §7 says the empty plot shows "No photograph" in the placeholder
   colour; `HistogramView::paintEvent` draws nothing.
4. **A failed render keeps the last histogram.** ADR §7 says a failed render leaves the plot empty;
   `showResult`'s failure path does not call `histogramPanel_->clear()`. Arguably the current
   behaviour is right, since the picture also stays. Decide, and fix either the ADR or the code.
5. **A closed gate keeps the stale plot.** When the section closes or the dock hides,
   `histogramGateChanged` (`MainWindow.cpp:2661`) clears the counter but leaves the plot. On reopening
   it shows the old count until the recount lands. That is harmless, but `clear()` there would match
   the "opening" rule.
6. **The parity test does not enforce "by one bin only".** `test_GpuOutputHistogram.cpp` uses the
   earth mover's distance, which allows 0.5 % of pixels moving two bins.
   - The clipped bound `colourBin(…, 254)` sums three channels, so it is loose.
   - Being histogram-level, the test cannot see compensating moves in the pixels.

   A per-pixel code difference on the two 8-bit pictures would match the ADR's wording.
7. **The C++ converting overload accepts a perceptual source.**
   `outputHistogram(developed, encoding)` (`OutputHistogram.cpp:23`) checks only the target
   encoding, although its doc says "any named encoding other than the perceptual one". Python guards
   against this itself; C++ callers do not get the guard.
8. **Python's "as is" branch cannot be tested from Python.** Python cannot make an
   output-encoded buffer, so the branch in `BindPhoto.cpp` is unreachable from Python today.
   `test_counts_an_encoded_buffer_as_is` tests something else, and its comment says so. Either drop
   the branch from the docstring until a constructor exists, or test it in C++.
9. **Missing benchmark cases.** The plan's `[.bench]` cases for the clipping mask and the Lanczos end
   bins do not exist (see M1).
10. **Theming assumes a dark palette.** `HistogramView` draws luma and the triangle outlines in
    `QPalette::Text` on the always-dark plot ground, which reads only with the one dark theme. Use
    `theme::brightText` or a fixed light tone.
11. **Style:**
    - `HistogramPanel` stores its triangles as `QToolButton*` and casts them with `static_cast`
      (`HistogramPanel.cpp:102`). Store them as `TriangleButton*` instead.
    - `ClippingMask.cpp` and `HistogramPresentation.cpp` (in `arraw-app-core`) include
      `ui/ThemeColors.h`. There is precedent in `DebugLog.cpp`, but it is still a layering leak.
12. **The window counts while minimized.** `histogramWanted()` uses `isVisible()`, which stays true
    when the window is minimized, so counting goes on. Renders do too, so this is consistent but not
    free.
13. **The flakiness risk is acceptable.** The window tests wait on conditions with generous timeouts.
    Fixed `qWait(300-700)` calls appear only before assertions that do not depend on them, except
    the "nothing is counted" cases. Those would pass vacuously on a slow runner rather than fail.

---

## Checked and found correct

- **Counting** (`OutputHistogramCount.cpp`):
  - 256 bins per code.
  - 16-bit rounding `(s·255+32767)/65535` is the nearest code.
  - Float: NaN and negative values go to 0, values ≥1 go to 255, the rest round half up.
  - Alpha 0, or a float alpha ≤0 or NaN, is skipped.
  - Luma comes from the decoded codes, weighted by the normalised Y row, then searched over the
    half-code boundaries, so a grey of code g has luma g.
  - Clipping is "any channel", counted once.
  - Span count and bounds are validated.
  - Abandon is checked at every 64-row band.
- **`cropSpans`**: pixel centres, half-open edges, either winding, clamped to the image.
- **`HistogramCounter`**:
  - Newest-wins, without cancelling the running count.
  - `abandon_` is raised and lowered under the lock, so an old-generation job can never be counted
    with the flag lowered.
  - A result whose generation changed is dropped on the thread; one already being delivered is
    dropped on the GUI thread (`MainWindow.cpp:2652`).
  - The destructor abandons, stops and joins. The member is destroyed before `~QObject`, so
    `invokeMethod(this, …)` can never target a dead object, and posted calls are removed with it.
  - `QImage` shallow copies are read through `constBits` only, so nothing detaches. Display images
    own their pixels (`DisplayImage.cpp:39`).
- **Stale results for another photograph**:
  - `showPhoto` and `closePhoto` call `resetHistogram()` (a generation bump).
  - `resetView()` drops the old images, and the stand-in is never in `wholeFrameImage()`.
- **Render paths**:
  - Every `setImage`, `setBackground` and crop-mode `setImage` submits, including provisional coarse
    renders and stroke renders.
  - `geometryEdited` and `reconciledGeometry` submit with spans.
  - `CropOverlay::changed()` reorients synchronously, so the spans and the image agree.
- **QSettings**:
  - `histogram/clipping` holds 0-3 and is tested round trip.
  - `histogram/channels` holds `rgb` or `luma`.
  - `developSections/histogram/open` cannot collide with other section ids.
- **The `J` action**: added to the window, it applies "both on if either is off" and stays in step
  with the triangles.
- **Python**:
  - The views keep `self` alive and are read-only.
  - `__hash__` is `None`, and the stub matches.
  - Errors map to `ValueError`.
- **CMake**: the sources are placed in the right libraries. `arraw-app-core` sees `src/core`
  privately, as ADR 036 allows.

---

## Fixed (2026-10-20, Claude Sonnet 5.5)

- **C1, fixed.** `PhotoView::paintEvent` now masks only the render, through the render's own
  rectangle. The fallback is never masked, so its marks cannot show through the render, and a stale
  fallback cannot leave old marks during a drag. Tests (bare `PhotoView`, 400 x 300 frame at 1:1 on a
  100 x 100 view, fallback present): the fallback clips and the render does not (no marks); the
  render clips on one half (marks only there); a render that stops clipping drops the marks, also
  when the fallback is then refreshed; a render smaller than the view marks only its rectangle. I
  confirmed that they fail against the old `paintEvent`.
- **M1, fixed in code, budget not met.** The luma search is a 65 536-cell lookup plus a forward walk
  (exact; a test compares it with a binary search over every red and green with nine blues in three
  encodings). Release figures (best of five): 52-60 ms at 2560 x 1600 and 105-118 ms at 3840 x 2160
  (was 128-165 / 234-272 ms; 375 ms on noise). That is five to six times over the 10 ms budget; ADR
  055 section 9 says so plainly and lists the levers (several counting threads, relaxing the budget,
  sampling). The budget is unchanged. The missing `[.bench]` cases (clipping mask, Lanczos end bins,
  real photograph) now exist; figures are in ADR 055.
- **M2, fixed.** One mask per render (the fallback has none), dropped when both kinds are turned off.
  The mask loop was reading the image's width on every pixel (the compiler could not rule out
  aliasing); fixing it took 4K from 65 ms to 13-20 ms. Not moved to the counter thread.
- **M3, fixed.** The two window tests zoom to the largest zoom, wait for a region render and
  `REQUIRE` that it is a region (new `PhotoView::renderRegion()`) before asserting.
- **M4, fixed.** Test "The display image equals the sRGB output of an export, code for code"
  (`test_DisplayImage.cpp`). ADR 055 section 2 now says `RowSpan` is public and why (option a).
- **Low 1, fixed:** the readout is re-emitted from `setImage`, `setBackground`, transform changes and
  when the crop mode ends, with the last pointer position. Not done during a mask gesture:
  `MaskOverlay` takes the moves, and giving it the readout needs a signal of its own.
- **Low 2, partly:** tests for the readout under zoom (and while a render lands or the view pans
  under a still pointer) and in the crop frame with a flip and -12 degrees. HiDPI is not tested.
- **Low 3, fixed** ("No photograph" while disabled and empty, with a test). **Low 4:** the code
  stays (the picture also stays); ADR 055 section 7 now says so. **Low 5, fixed** (the plot is
  emptied when the gate closes, tested). **Low 6, fixed:** the GPU parity test also compares the two
  8-bit pictures pixel by pixel (no sample more than one code apart, at most 1 % differ). **Low 7,
  fixed** (the converting overload refuses a non-working source, tested). **Low 8:** the Python
  docstring and stub say the as-is case cannot be reached from Python yet; a C++ test counts an Adobe
  RGB picture as is. **Low 9, fixed** (see M1). **Low 10, fixed** (fixed light tones, not the
  palette).
- **Skipped:** low 11 (the `static_cast` to `TriangleButton`: the class is private to the .cpp, so
  the header cannot name it; the `ThemeColors.h` include in app-core has precedent in
  `DebugLog.cpp`); low 12 (a minimised window keeps counting, as it keeps rendering); low 13
  (accepted by the review).
