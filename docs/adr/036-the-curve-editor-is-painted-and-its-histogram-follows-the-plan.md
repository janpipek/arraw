# The curve editor is painted, and its histogram follows the plan

Phase 4 of the [global adjustments plan](../ideas/global-adjustments-plan.md)
puts the tone curves of [ADR 033](033-the-tone-curve-is-four-monotone-curves-in-the-perceptual-coordinate.md)
in the develop panel, over the curve-input histogram of
[ADR 035](035-the-curve-input-is-sampled-through-the-render-and-binned-on-the-host.md).
This ADR records how the editor is split, how it reports edits, when the
preview counts the histogram, and how the Colour Grading hue rows show their
hues.

## Decision

**The point logic is a model without Qt; the widget paints and routes input.**
`src/app/CurveEditing.h` (in `arraw-app-core`) holds `CurveChannel`, `curveOf`,
`pointNear`, `insertPoint`, `removePoint`, `clampedPosition`, `CurvePointDrag`
and `sampleCurve`. Every operation keeps a curve well formed, and the tests
check that over thousands of random edits. `src/app/ui/CurveEditor` is a
painted `QWidget` (Qt Widgets, no QML) that turns mouse and keys into those
operations.

- (a) **A model and a thin widget (chosen).** The constraints are tested
  without a widget, and the widget tests only check routing and the edit
  protocol.
- (b) Everything in the widget. Fewer files, but every constraint can then
  only be tested through synthetic mouse events.

**The editor draws what renders.** `sampleCurve` evaluates through
`curvePlanFor` and `evaluateCurve`, the engine's own table. It lives in
`arraw-app-core` because that library sees `src/core`, so the widget never
includes an engine-private header.

**Editing rules.**
- A click on the empty plot adds a point there (y clamped). A click close to a
  neighbour is nudged clear of it by the minimum spacing. It is refused when the
  gap cannot hold the point, or when the curve already has 16 points. Adding
  starts a drag of the new point.
- A dragged point stops `minimumCurvePointSpacing` short of either neighbour, so
  points never pass each other. An end moves only in y.
- A point dragged more than 24 px outside the plot is removed. Brought back, it
  is restored between the same neighbours, because `CurvePointDrag` works every
  step out from the curve as the drag began. A double-click or right-click on a
  point removes it, and so does Delete or Backspace on the selected point. Ends
  are never removed, so a curve never has fewer than two points. The press of a
  double-click on the empty plot adds a point, and the double-click that follows
  does not remove it again.
- With the editor focused, the arrow keys move the selected point by 0.01 (0.1
  with Shift). Page Up and Page Down select the previous and next point, and
  Esc or Enter hand the focus back to the photograph. The editor claims the
  arrows, Delete, Page Up, Page Down and Enter through `ShortcutOverride` while
  it has the focus, as a spin box does; the arrows and Delete only while a point
  is selected. Esc is not claimed: while the white balance picker is armed, the
  window's Esc shortcut cancels the pick (and the editor keeps the focus);
  otherwise no shortcut takes Esc and it reaches the editor, which hands the
  focus back.
- **The editor takes the focus on a click (`Qt::StrongFocus`), unlike
  `SettingSlider`'s `NoFocus`.** A slider row never takes the arrow keys because
  its spin box is the keyboard way to its value. The editor has no such
  companion: it is itself the keyboard way to a point, and a user who has just
  clicked a point expects the arrows to nudge it. The cost is that after a
  mouse edit Left and Right move the point rather than step photographs, until
  Esc, Enter or a click elsewhere hands the focus back (Ctrl+Left/Right still
  step). Alternatives considered: `Qt::TabFocus` (the arrows reachable only by
  Tab), or claiming the arrows only after a keyboard selection; kept as is by
  the user's decision.
- Under the plot a label reads "In 0.25 → Out 0.31" for the selected or dragged
  point, in the perceptual coordinate the plot shows, to two decimals, and is
  empty while no point is selected; it keeps its line either way, so the panel
  does not jump. The editor has an accessible name ("Tone curve") and an
  accessible description of its mouse and keyboard model; the points
  themselves are not exposed through `QAccessibleInterface`.
- The channel buttons (Luma, Red, Green, Blue) and Reset sit above the plot in
  the panel's Tone Curve group, after Tone. Switching channel is view state, not
  an edit. Reset straightens the channel shown. The curve is drawn in the
  channel's colour (luma in the text colour), over its own histogram: luma for
  Luma, the channel for Red, Green and Blue. The R, G and B histograms are the
  channel curves' input only while the luma curve is the identity (ADR 035,
  "Red, green and blue are the channels at the tap"); with a luma curve set,
  they show the channels before it, which is not quite what the channel curve
  reads.

**Edits follow the slider's protocol (ADR 022).** The editor emits
`editStarted`, `curveEdited(channel, curve)` and `editFinished`. The panel
writes the curve straight into the state with `curveOf(next.settings.toneCurve,
channel)`: the channel names the curve, so there is no key to look up and no
ordering of keys to keep in step with `CurveChannel`. `toneCurveKeys()` stays
only so that the test of what the panel shows can count the four curves. The
edits then reach the session through the panel's existing `stateEdited`.
- A drag is one edit. A press that never moves is no edit, so selecting a point
  costs no history step. This is one deliberate difference from
  `SettingSlider`, which emits `editStarted` on `sliderPressed`: the editor
  begins an edit at the first change. Either is fine, as the session commits
  nothing for an unchanged edit.
- Arrow-key moves in a row are one edit, closed after 500 ms of quiet, on focus
  loss, or by `finishPendingEdit()`: the same interval as `SettingSlider`, on a
  timer of the editor's own.
- A removal and a reset are each one complete edit.
- One edit at a time: a row's edit beginning ends the editor's pending one, and
  the other way round.

The four keys leave the test's "not shown yet" list and count as shown by the
curve editor. They have no `SettingPresentation` row, because nothing about a
curve is a label, a unit or a step.

**The histogram is counted on the preview's worker, when requests pause.**
`PreviewRenderer` already owns the GPU context on its thread (ADR 023), and it
already waits for a pause (150 ms with no new request) before refreshing the
whole-frame fallback. The histogram uses the same pause: after the fallback, if
no request arrived meanwhile and the renderer is not being stopped (closing
the window must not wait for a count as well as a render). It is delivered as a
`PreviewResult` carrying only `curveHistogram`, which `MainWindow` hands to the
panel.
- On the GPU: `sampleOnGpu` at `Tap::CurveInput` with `curveHistogramRequest`
  (1024 px long edge), on the uploaded level, then `curveHistogram` on the host.
- On the CPU, or when the GPU sample throws: `curveHistogram(level, state,
  cpuCurveHistogramRequest)`, a 512 px long edge (`PreviewRenderer.h`).

**It is counted only while the editor is on screen.**
`PreviewRenderer::setCurveHistogramWanted(bool)` gates the count; nothing is
wanted until it is called. The panel works out `curveHistogramWanted()` as
`isVisible() && !visibleRegion().isEmpty()` of the editor, so it is false while
the dock is hidden or tabbed away (a hidden ancestor hides the editor), and
while the Tone Curve group is scrolled wholly out of the scroll area. It
re-evaluates on the editor's show, hide and paint events (a paint is how an
editor scrolled back into view first shows), on its own move and resize
(scrolling moves the panel inside the viewport), and on the viewport's resize,
and announces a change with `curveHistogramWantedChanged`, which `MainWindow`
passes to the renderer.
- Becoming wanted wakes the worker without a request. At the next pause it
  counts for the state last rendered, if that was of the current source, and
  only if `CurveHistogramRefresh` says the last count is not current. Showing
  the editor again with nothing changed counts nothing.
- While not wanted, an edit before the tap costs nothing; the editor keeps the
  last histogram it was given until a count replaces it.
- (a) **Gate on visibility (chosen),** with (b) below.
- (c) Make the host sample cheaper (ADR 035: tables for the encode and decode
  powers). The right long-term fix, but engine work; still open.

**It samples the level that covers the histogram's own request, not the view's.**
`pyramidLevelFor(…, request)` picks the level that covers the request's long
edge: 1024 px on the GPU, usually the level the fallback already uses, and 512
px on the CPU. A zoom or pan therefore neither changes nor recounts the
histogram, and a zoomed-in view does not make the count read a full-resolution
source.
- (b) **A coarser request on the CPU only (chosen).** 256 bins do not need
  700 k pixels; 512 px is about a quarter of the host sample's cost. The GPU
  keeps 1024, where the cost is small. CPU and GPU histograms of the same photo
  therefore differ slightly, as they come from different levels, and the CPU
  path may build one more pyramid level than the preview needs.

**Whether to count again is the plan's to say.** `CurveHistogramRefresh` keeps
the level buffer and the `ProcessingPlan` of the last count: `planFor(level,
state, request)` with the request that count used, `curveHistogramRequest` on
the GPU and `cpuCurveHistogramRequest` on the CPU. It counts again unless the level is
the same buffer and `sameAtTap(previous, new, Tap::CurveInput)` holds. The app
has no list of its own of "settings before the curves". A curve drag, or any
control after the tap, never recounts, while exposure, Basic Tone, white
balance, geometry and a new source do. The record is cleared when the source
changes, which also releases the old level. A failed count is not retried
before the next render.

**Bars are square roots of the counts, scaled to the tallest interior bin.**
- (a) **Square root (chosen).** Linear counts let one dense tone range dwarf the
  rest. A logarithm lifts single stray pixels into visible bars. The square root
  sits between them, and is what many raw editors draw.
- (b) Logarithm. It reads clipping and sparse ranges well, but makes every
  histogram look full.
- (c) Linear. It is honest about proportions but unreadable beside a spike.

The tallest bin among bins 1 to 254 sets the scale, and the end bins are cut off
at the top. A spike of clipped black or white therefore cannot flatten
everything else.

**The Colour Grading hue rows paint the Oklab hue wheel in their groove.**
ADR 034's hues are Oklab angles (about 30 red, 110 yellow, 140 green, 260
blue), not Lightroom's, so a bare number cannot be read. `SettingPresentation`
has a `track` field (`SliderTrack::Plain` or `SliderTrack::OklabHue`), set for
the three hue rows. `SettingSlider` then uses a slider that paints a rounded
gradient band where the groove is, with the style's own handle drawn on top so
it stays whole and readable. The gradient runs from the handle's centre at the
minimum to its centre at the maximum, so the colour under the handle is the
colour of its value.
- The colours come from `oklabHueColour` (`src/app/OklabHue.h`): the tint
  `colorGradingPlanFor` resolves for that hue at full saturation (the grade's own
  angle convention and its largest chroma, 0.10), on an Oklab lightness of 0.65,
  through `fromOklab` and `workingToSrgb`, then the sRGB transfer function.
  Where that leaves sRGB the chroma is reduced, not a channel clipped, so the
  hue stays right. A test reads each colour back into Oklab and checks its hue.
- The flag is general: the HSL hue rows, or a later wheel, can use it.
- (a) **Gradient groove (chosen).** (b) A swatch beside the label showing the
  zone's current tint, and (c) landmarks in the tooltip were the alternatives.

## Consequences

- On the CPU fallback a histogram costs roughly 0.1 to 0.15 s at the 512 px
  level (a quarter of ADR 035's 0.3 to 0.6 s at 1024 px), and only while the
  curve editor is on screen. That cost comes after each pause that follows an
  edit before the tap, such as exposure. A render requested during the count
  waits for it, because a count cannot be interrupted. Curve drags pay nothing
  beyond building one plan per pause.
- With the editor off screen, the histogram it last showed may be stale until
  it is shown again and the next pause recounts it.
- The histogram may describe a state slightly older than the newest edit while
  a new pause is pending. It is always for a state that was rendered, and it
  never mixes photographs: results older than the open photograph's first
  request are dropped, and opening a photograph clears it.
- The GPU path of the count is not covered end to end here. The app refuses
  software rasterisers, so the CPU path is what the suite exercises.
  `sampleOnGpu` and its parity with the CPU are covered by `tests/gpu`.
- The Colour Grading group's rows are ordinary slider rows; only their hue
  track is part of this ADR.
