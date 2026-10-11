# The brush in the window — plan

Status: accepted, 2026-10-10 (user: every recommendation, Q1–Q9 (a)); built and
measured, 2026-10-10 (sub-steps 6.1 to 6.6; numbers in the ADR 044 note "the brush in
the window, as built"; Q6 decided 2026-10-11: (b), a coarser picture while stroking,
not at level 0; see the ADR note); revised the same day after the review
`docs/reviews/claude-opus-5-5_2026-10-10_brush-gui-plan.md` (every High and
Medium finding fixed, most Low ones too; the response is at the end, before
the open questions). Step 6 of `local-adjustment-plan.md`: "Brush
GUI: the tool, dirty-tile painting, cursor, history. Measure a long painting
session." Builds on step 5 (merged as 44e8282; plan
`brush-end-to-end-plan.md`, ADR 044 "What step 5 settled") and on the mask
mode of step 3 (`masks-gui-plan.md`, ADR 044 note of 2026-10-09, ADR 048).
Spec: ADR 044 §6 (stroke contract, "During a gesture") and §10 (brush tool),
ADR 022 (begin, update, commit), ADR 009 (strokes in the sensor frame).
Qt Widgets only; every string British English through `tr()`.

Scope: creating a brush, the brush tool and its settings, the cursor, the live
stroke (pointer to storage frame, the edit, the engine's tail method, render
pacing, GPU uploads of touched tiles), cancellation, budgets, keys, and the
measurement of a long painting session.

Out of scope: pressure (reserved, ADR 044 §6), tablet events beyond the mouse
events Qt synthesises from them, removing single strokes (undo only), an
"auto mask" (edge-aware brush), a background prefetch of full-size coverage,
restricting the pointwise pass to a dirty rectangle (open question 6), lens
corrections, Python (it already has `with_brush_stroke`).

## Decisions at a glance

Proposed; the open questions at the end say which ones the user should confirm.

| Topic | Decision |
|---|---|
| Creating | A checkable **Brush** button beside Linear and Radial arms the tool; with no brush selected, the first stroke creates the mask: one step "Add Brush 1" (Q1) |
| When the tool paints | Exactly while Brush is armed in the mask mode; the button shows it. Choosing a brush in the list arms Brush; unchecking it returns a left drag to panning (Q9). While painting, pins are hidden and Alt with a left drag no longer pans |
| Where the logic lives | App-core `src/app/BrushPainting.{h,cpp}`: `BrushSettings`, `LiveStroke`, refusal wording. `MaskOverlay` drives it; `MainWindow` only routes (no `MaskModeController`, as step 3 decided) |
| Settings | Size, Hardness, Flow rows and an Erase toggle in the Masks tab, shown while the tool paints; kept in `QSettings` under `brush/` (Q4) |
| Size | A radius in long-edge units, shown as a percentage of the long edge on a new logarithmic slider scale; anchored to the photograph, so the cursor grows with the zoom (Q3) |
| Keys | `[` / `]` size ÷ / × 1.25; `{` / `}` (Shift+`[` / `]` on a US layout) hardness ∓ 10 %; Alt held at the press erases that stroke; `E` flips the Erase toggle (Q5). Window shortcuts only; the overlay does not claim them |
| Cursor | Ring of `radius * scale()` logical pixels, a dashed core at `hardness * radius`, a cross under 4 pixels, "−" when erasing, the forbidden cursor when the mask is full (a probe stroke one radius long does not fit) |
| Pointer to storage | `MaskViewMapping::correctedFrom` (view → developed → upright → corrected, existing), then a new public `sensorFrom(CorrectedPosition)` beside `DevelopedFrameMap`: the identity until lens corrections (ADR 009) |
| The edit | One stroke is one begin / update / commit (ADR 022); each update is `withStrokeAppended(base, id, partial)`; the press updates at once, later moves once per event-loop turn; points thinned to `max(1 logical px, 0.1 · radius)` apart |
| Engine | The tail method moves into `BrushCoverageCache` as a new lookup, `Grown`: a list whose last stroke grew repaints only the new dabs, and replaces its parent entry. Every draw that paints a list's last stroke (a `Miss` too) leaves a growable entry, so a new brush's first stroke grows. No public API change in the engine |
| Rendering while painting | Paced: `PreviewRenderer::request(..., Supersede::Queue)` while a stroke is under way and for its commit (`MaskOverlay::stroking()`, not the mode); no thumbnail until the commit. The whole shown level's pointwise pass reruns per render (ADR 025's limit). Decided 2026-10-11 (Q6 b): while stroking, the picture is at most 0.4 MP (coarser level, not at level 0), the commit's is full |
| GPU | Only the tiles a render's coverage changed are packed and uploaded (the step-5 residency, by serials); asserted by a counter |
| Coarser first render | Unchanged: `Grown` and `Extended` are not a miss, so a stroke over a cached brush never makes one |
| Tint (O) | Follows every update, on the GUI thread, through `maskCoverage` (a `Grown` lookup at the grid's level); shown automatically while the painted brush contributes nothing to the plan (Q2); repaints only the stroke's new area |
| Caps | At 10 000 points the stroke continues as a new stroke in the same step (Q7); past a mask budget the stroke stops where it fits, the status line says why, release commits what fits |
| Cancelling | Esc, the crop mode, a user's tool switch, a photograph switch, a sidecar reload, a lost pointer: the existing paths, which also drop the `LiveStroke`. Undo, Redo, History, rotate, flip and paste mid-stroke abandon the drag; the session commits the stroke first (ADR 022) |
| History | "Add Brush 1" and "Paint Brush 1", worded as step 5 already does (`MaskChange::strokes`) |
| Measuring | An engine bench (B9) and a window `[.timing]` session at device-pixel ratios 1 and 2, with GUI-thread time (overlay paint included), pointer-to-picture latency, renders per second, cache memory, evictions and misses, and undo cost (§11) |
| Sub-steps | 6.1 tail method in the cache; 6.2 paced requests; 6.3 app-core; 6.4 panel, overlay and cursor; 6.5 window, keys, tint, docs; 6.6 measurements and ADR note |

## 1. What exists (findings)

- **The mask mode** is owned by `MainWindow` (`setMaskMode`, `leaveMaskMode`,
  `selectMask`, `setMaskTool`, `syncMasks`, the shared slots `beginEdit`,
  `updateEdit`, `finishEdit`, `cancelEdit`). There is no `CropModeController`
  and no `MaskModeController` (ADR 044 note of 2026-10-09). The tool's widget is
  `MaskOverlay` (ui), its maths `MaskEditing` (app-core): the brush follows that
  split, with its own app-core file.
- **`MaskTool` is `{None, Linear, Radial}`** (`src/app/MaskEditing.h`). The
  Masks tab (`MasksPanel`: `linear_`, `radial_`, ...) offers no brush, and a
  step-5 test asserts it (`test_MainWindowMasks.cpp`). With a brush selected, a
  press on the photograph pans or clears the selection (`MaskOverlay::
  mousePressEvent`), as step 5 §7 specified. The panel's `maskToolChosen` is
  connected straight to `MainWindow::setMaskTool`; `MaskOverlay::setTool` only
  sets the tool, so a gesture under way survives it.
- **`syncMasks` disarms a tool when the list is full**
  (`overlay.tool() != None && !canAddLocalAdjustment(state)`), and it runs on
  every `updateEdit` through `refreshPanel`. A creation drag of the 16th mask
  passes through it after its first update.
- **Alt with a left drag pans** in the mask mode (`mousePressEvent` ignores the
  press so `PhotoView` takes it). ADR 044 §10 gives Alt to erasing, so the brush
  tool takes it back.
- **Mapping.** `MaskViewMapping` (app-core) composes `DevelopedFrameMap`
  (`include/DevelopedFrame.h`), `ViewTransform` and the pixel ratio:
  `correctedFrom(QPointF)`, `longEdgeFromWidget`, `scale()` (logical pixels
  per long-edge unit). The chain is a similarity, so a round brush stays round
  on screen under turns, flips and straighten.
- **Edit rules.** `withStrokeAppended(state, id, stroke)` normalises and
  appends (`StrokeList::appended`, sharing every earlier stroke);
  `canAppendStroke` answers the same question without throwing. Neither appends
  to a live stroke: each call appends a whole stroke to the list it is given.
  That is enough (§6): the gesture appends the partial stroke to the same base
  each time.
- **Budgets are public** (`include/BrushStrokes.h`): the caps,
  `budgetOf(Stroke)`, `StrokeList::budget()` and `accepts`. A one-point stroke
  has zero length, so zero swept area and zero dabs in `budgetOf`: it is
  refused only by the stroke-count or point caps, never by a spent budget.
- **The cache's `Extended` repaints the whole partial stroke** and is retained
  (`BrushCoverageCache::coverage`, called with `retain = true` by
  `CoverageResidency::update` and by the tint's `sampleBrush`). It answers
  `Extended` only from a *non-empty* held prefix (`held.empty() || ...
  continue`): the first stroke of a new brush, or a stroke on a brush whose
  list is not held at that size, is a `Miss` on every update. Fed a live
  stroke, every update would cost the "whole" column of report B3 (3.0 ms at
  1500×1000, 32.8 ms at 24 MP, 154 tiles dirtied) and leave an entry of its
  own: about 10 MiB of cloned tiles per update at 24 MP. 200 updates would push
  the other brushes' entries out of the 512 MiB budget, oldest first. The
  tail method (B3: 0.21 ms and 1.36 ms) exists only in the bench
  (`paintedOnto` in `tests/test_BrushBench.cpp`); its pieces are in core
  (`dabCentres(stroke, raster, endDab = false)`, `paintDab`). The drawing
  happens outside the cache's lock; inserting takes it again.
- **Serials already limit packing and uploads.** `CoverageResidency::update`
  repacks only tiles whose serial changed against the *latest* list its slot
  holds, and keeps their rectangles pending until a GPU upload completes;
  `tilesPacked()`, `cacheCalls()` and `pending(plane)` are there for tests. An
  undo is a cache `Hit` (nothing drawn) but still repacks and uploads the
  tiles the undone stroke touched (step 5 §2.6).
- **A newer request cancels the render in flight** (`PreviewRenderer::request`
  calls `inFlight_->cancel()`). Pointer moves at 60 to 120 Hz against a 50 ms
  CPU render (step 3's measurement at 1500×1000) risk showing nothing until
  the hand rests.
- **Every whole-frame request asks for a thumbnail**
  (`thumbnailEdge = ThumbnailCache::maxEdge` in `requestRender`); the worker
  reduces each such result and `showResult` → `followWithThumbnail` updates
  the strip for every non-provisional image.
- **`PreviewView::outputSize` is in device pixels.** At ratio 2 a 1500×900
  logical window asks for 3000×1800, which for 24 MP is the 3000×2000 level:
  four times the pixels of ratio 1.
- **A region render still develops the whole level** (ADR 025, "Not decided"):
  at 1:1, and at a fit view on a display as wide as the photograph, every update
  reruns the pointwise pass over the full source.
- **A brush that contributes nothing is left out of the plan**
  (`localPlanFor`: disabled, Opacity 0, or every delta zero), so a new brush
  paints nothing visible until a delta is set, unless the tint is on.
- **Keys.** `MaskOverlay` claims Esc, O, Delete, Backspace and Space
  (`claimedKey`) in `ShortcutOverride`, unmodified only; a claimed key goes to
  `MaskOverlay::keyPressEvent`, not to the window's `QShortcut`. That handler
  returns early for any modifier but Keypad (`if (event->modifiers() &
  ~Qt::KeypadModifier)`), which drops Shift-ed keys and `Key_Alt` itself.
  `Ctrl+[` / `Ctrl+]` rotate; plain `[`, `]`, `{`, `}` and `E` are free
  (`Ctrl+E` exports). `docs/keybindings.md` still lists M and O under
  *Planned* (step 3 did not move them), and F7, F8, F11, F12 (ADR 047) too.
- **Overlay repaints.** `setScene` (each `syncMasks`, so each update) and the
  tint's `updateCoverage` call `update()` on the whole overlay; `paintEvent`
  scales the full tint image with `SmoothPixmapTransform`. The overlay is not
  opaque (`WA_NoSystemBackground`), so `PhotoView` repaints underneath.
- **`EditSession`.** `setState` (rotate and flip through `editGeometry`, paste)
  begins, so it commits an open edit first; `updateEdit` then drops every
  later update (`!session.editing()`). `redo` commits an open edit first;
  `canRedo` is false for an open edit that changed the state.
- **Sliders.** `SliderScale` has `Linear` and `Reciprocal`; a brush size from
  0.05 % to 100 % needs a logarithmic scale. `SettingSlider` signals
  `editStarted`, `valueEdited`, `editFinished`, `focusReleased`.
- **Measuring.** `tests/test_MainWindowMasks.cpp` has a `[.timing]` case that
  reads `arraw.timing` spans between MARK lines; `test_MainWindowOpening.cpp`
  has a `StallProbe`; `src/app/QtMessageCapture` can collect the spans
  in-process; `tests/test_BrushBench.cpp` holds B1 to B8. There is no CPU span
  for `ProgressStep::Coverage` (only `gpu.coverage`, `gpu.update-coverage`).

## 2. Creating a brush, and when the tool paints

The Brush button *is* the painting state (Q9, review finding 12): checked
exactly while the tool paints.

- **Brush button** (`maskBrush`, checkable) in the Masks tab beside Linear and
  Radial, in their exclusive group. Checking it arms `MaskTool::Brush`
  through `maskToolChosen` → a new `MainWindow::chooseMaskTool` (§9: it
  cancels a stroke under way, then calls `setMaskTool`), which enters the mask
  mode. Enabled while a mask can be added (`canAddLocalAdjustment`) *or* the
  selection is a brush, so a full list can still be painted on.
- **The tool paints** = mask mode on and `MaskTool::Brush` armed.
  `MaskOverlay::painting()` answers it; the panel shows the brush rows (§3)
  exactly then.
- **Target of a stroke:** the selected mask when it is a brush ("Paint Brush
  1"); otherwise a new brush, created by the stroke: one edit whose updates
  are `withStrokeAppended(withLocalAdjustmentAdded(baseline, BrushMask{}),
  newId, partial)`. The new mask is announced as selected on the first update
  (`maskSelected(baseline.nextLocalAdjustmentId)`, as a Linear creation does).
  The step is "Add Brush 1" (`describeChange` sees an added id). Esc during
  it, or any cancel, leaves no mask. Brush stays armed after the release, so
  the next stroke paints the same brush.
- **Choosing a mask** in the list: a brush arms Brush (painting it); a linear
  or radial disarms Brush and shows its handles. Arming Linear or Radial
  disarms Brush (exclusive group); the mask they create is selected.
- **While painting**, no handles (a brush has none), and other masks' pins
  are hidden and not grabbable, so a stroke across a radial's centre paints.
- **Not painting** (Brush unchecked) with a brush selected: a left drag pans
  and a click clears the selection, as in step 5.
- **Esc order** (`MaskOverlay::escape`): the stroke under way; then, with
  Brush armed and a brush selected, the selection (Brush stays armed: the next
  stroke starts a new brush); then the armed tool; then the mode.
- **Panning while the tool paints:** Space with a left drag, a middle drag, the
  wheel (zoom), as the mask mode already allows. A left drag on empty canvas
  paints, it does not pan.
- **The full list (16 masks):** `syncMasks` disarms Linear and Radial, and
  Brush only when no brush is selected; it never disarms while
  `overlay.stroking()` (the commit's `refreshPanel` runs it again afterwards).
  So a drag that creates the 16th mask with the brush commits "Add Brush 16"
  and leaves Brush armed on it (review finding 2).
- **The hint** under the list reads "Select a mask, or add one with Linear,
  Radial or Brush."; while painting a selected brush, "Painting Brush 1. Esc
  starts a new brush."

## 3. Brush settings and where they live

```cpp
/// @brief Settings of the brush tool: view state, never in the document (ADR 044, section 1).
struct BrushSettings {
    float radius = 0.02F;   ///< Long-edge units, minimumBrushRadius to maximumBrushRadius.
    float hardness = 0.5F;  ///< 0 to 1.
    float flow = 1.0F;      ///< 0.01 to 1.
    bool erase = false;     ///< The Erase toggle; Alt at a press inverts it for that stroke.
};
[[nodiscard]] BrushSettings restoreBrushSettings(QSettings& store);
void saveBrushSettings(const BrushSettings& settings, QSettings& store);
[[nodiscard]] BrushSettings resized(BrushSettings settings, int steps);   // × 1.25^steps, clamped
[[nodiscard]] BrushSettings hardened(BrushSettings settings, int steps);  // ± 0.1 per step, clamped
```

- **Rows** in `MasksPanel`, in a "Brush" block between the list and the
  selected mask's Invert / Opacity / delta rows, visible while the tool paints:
  - **Size**: `SettingSlider` id `brush.size`, the radius × 100 as a percentage
    of the long edge, 0.05 to 100 %, two decimals, new `SliderScale::Logarithmic`
    (equal steps in `log(value)`), tool tip "Radius of the brush, as a share of
    the photograph's long edge.";
  - **Hardness**: `brush.hardness`, 0 to 100 %, tool tip "Share of the radius
    painted at full strength; the rest fades out.";
  - **Flow**: `brush.flow`, 1 to 100 %, "Strength of each dab; overlapping dabs
    build up.";
  - **Erase**: a checkable button (`brushErase`), "Erase (E; or hold Alt while
    pressing)".
- The rows are not edits: their `valueEdited` updates the settings; their
  `editStarted` and `editFinished` are not connected to the session; their
  `focusReleased` is connected like the other rows', so focus returns to the
  photograph. `MasksPanel::finishOtherEdits` (`findChildren<SettingSlider*>`)
  reaches them too, which is harmless: they have no edit to finish. The panel
  emits `brushSettingsChanged(BrushSettings)`; the window hands the settings
  to the overlay and saves them; `[`, `]`, `{`, `}` and `E` go the other way
  (`MasksPanel::setBrushSettings`, no signal).
- **Persisted** in `QSettings` under `brush/radius`, `brush/hardness`,
  `brush/flow` (Q4), restored at start like `AppSettings`. The Erase toggle is
  not saved: every application session starts painting.
- **Captured per stroke** (ADR 044 §6): radius, hardness, flow and erase are
  read at the press; changing them mid-stroke (a key) applies to the next one.

## 4. The cursor

Drawn by `MaskOverlay::paintEvent` while the tool paints and the pointer is
over the overlay; the system cursor is `Qt::BlankCursor` there.

- **Ring:** radius `settings.radius * mapping.scale()` logical pixels, white
  with the one-pixel dark rim the handles use (`markColour`), so it reads on a
  light sky.
- **Core:** a dashed circle at `hardness * radius` (the profile's opaque core,
  `inner = min(hardness · R_d, R_d − 1)` in raster pixels, is within a raster
  pixel of it); not drawn at hardness 0 or 1.
- **Small brushes:** below 4 logical pixels of radius, a cross of 9 pixels
  replaces the ring, with the ring still drawn if at least 1.5 pixels.
- **Erase:** a short horizontal bar at the centre while the next stroke would
  erase (toggle XOR Alt held). Alt is read from every mouse event's
  modifiers, from `Key_Alt` press and release (handled in `keyPressEvent` and
  `keyReleaseEvent` *before* the modifier guard), and from
  `QGuiApplication::queryKeyboardModifiers()` on `enterEvent`, since Alt may go
  down while another widget has the focus.
- **Full:** when the target brush cannot take the probe stroke (§6.4),
  `Qt::ForbiddenCursor` instead of the ring. Checked on enter, after each
  commit and on a settings change, not per hover move.
- **Repaint:** a hover move updates the union of the old and new ring
  rectangles only; a zoom (`PhotoView::transformChanged`) or a settings change
  repaints the ring at its new size. The ring follows the pointer during a
  stroke.
- **Considered, not taken now:** a `QCursor` built from a pixmap of the ring
  (rebuilt on zoom, size or hardness change) has no one-frame lag behind the
  pointer, noticeable on Wayland, and needs no hover repaints, but platforms
  cap cursor sizes (about 64 to 128 device pixels), so the painted ring would
  stay for large brushes. A follow-up if the lag bothers in use.

Test: at zooms 0.25, fit and 2, ratios 1 and 2, the ring radius equals
`radius * longEdge * zoom / ratio` within 0.5 pixel (`ringRadius()` exposed for
tests).

## 5. From the pointer to the storage frame

```text
widget (logical px)  MaskViewMapping::correctedFrom      → CorrectedPosition   (existing:
                     view → developed frame → upright → corrected, via ViewTransform and
                     DevelopedFrameMap)
corrected            sensorFrom(CorrectedPosition)        → SensorPoint         (new, public)
```

- `sensorFrom` is declared in `include/DevelopedFrame.h` beside
  `DevelopedFrameMap` (namespace `arraw`): "Gives the sensor-frame point under
  a corrected position: the same numbers until lens corrections (ADR 009), the
  one place that changes then." Once lens corrections land the map needs the
  lens model, which lives in core, and Python and the CLI will want the same
  function; declaring it public now keeps one place. Narrowed to float once.
  `SensorPoint` comes from `include/BrushStrokes.h`.
- Distances for thinning and the stroke's radius use the long-edge metric
  (`longEdgeFromWidget`, `scale()`), never screen axes, so a turned or flipped
  photograph needs no case of its own.
- Positions off the picture are kept (the contract allows −2 to 3, clamped by
  `normalised`); dabs there paint nothing.
- A zoom or pan between two moves of one stroke (keyboard zoom) changes the
  mapping, not the stored points: each point is mapped when it arrives. The
  wheel is ignored mid-gesture as today.

Test: under every orientation × flip × straighten {0, 7.5} × an explicit crop,
at zooms 0.25, fit and 4 and ratios 1 and 2, the dab centre of a one-point
stroke (`dabCentres` at the full source size, through `DevelopedFrameMap`
back to the widget) lies within 0.01 logical pixel of the press.

## 6. The live stroke (app-core `LiveStroke`)

```cpp
/// @brief Why a stroke cannot be painted, in the order the checks run.
enum class BrushRefusal { None, MasksFull, NotABrush, Strokes, Points, SweptArea, Dabs };

/// @brief Tells which cap or budget refuses @p stroke on mask @p id (None when it fits).
[[nodiscard]] BrushRefusal refusalOf(const DevelopState& state, LocalAdjustmentId id,
                                     const Stroke& stroke);
/// @brief Makes the stroke that stands for "one more stroke" at @p settings: two points one
/// radius apart at @p at, so it spends swept area and dabs as a real stroke does.
[[nodiscard]] Stroke probeStroke(const BrushSettings& settings, SensorPoint at);
/// @brief Words a refusal for the status line: three messages (see 6.4).
[[nodiscard]] QString describeRefusal(BrushRefusal refusal, const QString& maskName);

/// @brief One stroke in progress: the states a gesture reports, from its press to its release.
class LiveStroke {
public:
    /// @brief Starts at a press: on mask @p target, or on a new brush when it is empty.
    /// @return The stroke, or nothing with @p refused set when the probe stroke does not fit.
    [[nodiscard]] static std::optional<LiveStroke>
    begin(const DevelopState& baseline, std::optional<LocalAdjustmentId> target,
          const BrushSettings& settings, bool erase, QPointF press,
          const MaskViewMapping& mapping, BrushRefusal& refused);
    /// @brief Adds a pointer position; @return whether state() changed.
    bool extend(QPointF pointer, const MaskViewMapping& mapping);
    /// @brief Records that state() was reported, so a split at the point cap builds on it.
    void reported() noexcept;
    /// @brief The baseline with every finished part and the live part appended.
    [[nodiscard]] const DevelopState& state() const noexcept;
    [[nodiscard]] LocalAdjustmentId mask() const noexcept;
    /// @brief Set once a budget stopped the stroke; later positions are ignored.
    [[nodiscard]] BrushRefusal refusal() const noexcept;
};
```

### 6.1 States

- `base_` is the baseline (with the new empty brush when creating). Each change
  computes `state_ = withStrokeAppended(base_, id, live_)`, so consecutive
  states share every earlier stroke by pointer, and the live stroke's earlier
  points are a prefix of the next one's: exactly what the cache's `Grown` needs
  (§7). Points are appended in long-edge order of arrival; nothing is
  resampled or smoothed (the dabs are by distance, ADR 044 §6).
- The press gives a one-point stroke: a click paints one dab. The press's
  update is emitted at once, right after `editStarted` (not on the timer), so
  the session's state has changed before any key can arrive (§9, Redo).
- The release reports nothing new unless the pointer moved since the last
  update; commit follows.

### 6.2 Thinning

A position is kept when its long-edge distance from the last kept point is at
least `max(1 / scale(), 0.1 · radius)`: one logical pixel (no pointer is finer)
or a tenth of the radius (dabs are a quarter radius apart). The release
position is always kept. A 10 000-point stroke then spans at least 1 000 radii,
or 10 000 logical pixels: the cap is reached only by a long scribble, but a
careful outline with a small brush can get there.

### 6.3 One update per event-loop turn

`MaskOverlay` feeds every move to `LiveStroke::extend` and emits
`stateEdited(state())` from a zero-delay single-shot timer (`strokeTimer_`), so
a burst of moves in one turn costs one `updateEdit`, one `refreshPanel` and one
render request. The release flushes the timer, then emits `editFinished`.
Moves still queued on the timer when a stroke is abandoned or cancelled are
dropped (acceptable: they are at most one turn's worth).

### 6.4 Caps and budgets

- **Points per stroke (10 000):** when the live stroke would pass it, it is
  finished. If `state_` has not been reported since its last change, it is
  reported first (the timer is flushed); then `base_ = state_`, the reported
  state, whose last stroke is the finished one, and a new live stroke with the
  same style starts at its last point. So the finished stroke keeps the very
  pointer the cache saw, and the next update is an `Extended` by one stroke
  (one dab), not a repaint of 10 000 points (review finding 13). Same gesture,
  same step (Q7). The joint has one extra dab at the shared point: invisible at
  flow 1, a faint spot at low flow (documented).
- **A mask budget (2 000 strokes, 100 000 points, swept area 4, 2 000 000
  dabs):** checked with `canAppendStroke(base_, id, candidate)` before each
  change. When it refuses, the last accepted state stays, `refusal()` is set,
  further positions are ignored, the overlay emits `brushRefused(refusal)` once
  and shows the forbidden cursor; the release commits what fits.
- **At the press, and for the cursor:** `refusalOf` on `probeStroke(settings,
  press)`, two points one radius apart, so it spends `radius²` of swept area
  and 4 dabs. A one-point stroke would not do: its budget is zero, so a spent
  budget would never refuse it, and every press on a full brush would commit
  a one-dab step (review finding 6). When the probe is refused, no edit begins
  and the status line says why.
- **Messages** (review finding 25; the enum stays for tests):
  - Strokes, Points, SweptArea, Dabs: "%1 is full. Paint on a new brush."
  - MasksFull: "A photograph holds at most 16 masks."
  - NotABrush: not shown (the tool never targets one; a programming error
    cancels the stroke).
  - Mid-stroke, the first: "%1 is full: the stroke stops here."
- `refusalOf` uses the public caps: stroke count first, then the
  `StrokeBudget` sums against `maximumPointsPerMask`, `maximumSweptAreaPerMask`
  and `maximumDabsPerMask`.

### 6.5 Cost on the GUI thread

Per update: `budgetOf` and the content hash of the live stroke (linear in its
points, about 0.1 ms at 10 000), `StrokeList::appended` (a vector of shared
pointers), `EditSession::update`, `refreshPanel` (0.4 ms median in step 3's
measurement), and the overlay's repaint of the changed area (§8.3). Target
and measurement in §11.

## 7. The live stroke in the engine: the tail method in the cache

ADR 044 §6 ("During a gesture ... two tile states") is implemented inside
`BrushCoverageCache`, so that every caller of `coverage` (the ladder's
residency, the tint's `sampleBrush`, a direct render) gets it, and the app
needs no new public API.

- **New lookup `CoverageLookup::Grown`.** The wanted list W and a held entry H
  at the same size have the same number of strokes `n`, strokes `0 .. n−2` are
  pointer-equal, the last strokes have the same radius, hardness, flow and
  erase, and H's last points are a strict prefix of W's. Checked after `Hit`
  and `ContentHit`, before `Extended`.
- **Two grids per growable entry.** `Entry` gains `settled` (the coverage
  without the last stroke's end dab) and `settledDabs` (how many of that
  stroke's `dabCentres(stroke, raster, false)` it holds). **Every draw that
  paints the list's last stroke keeps them: a `Miss`, an `Extended` and a
  `Grown`.** Each draws the last stroke in two parts (its settled dabs, then
  the end dab onto copies of the tiles it touches, in the same parallel
  pass), so the two grids share every tile but those the end dab touched: one
  to four for a brush narrower than a tile, dozens for an everyday brush at
  24 MP and hundreds for a wash brush. A draw with `retain` false skips the
  copies. This covers the first
  stroke of a new brush (its base list is empty, and `Extended` needs a
  non-empty held prefix, so its first update is a `Miss`) and a stroke on a
  brush whose list is not held at that size (evicted, or a first 1:1 render):
  after one `Miss`, every later update is a `Grown` (review finding 1). The
  cost of keeping `settled` on every entry is at most four tiles (256 KiB at
  128 px).
- **Drawing a `Grown`:** paint `dabCentres(W.last, raster, false)` from
  `settledDabs` on onto clones of the tiles they touch in `H.settled`, then the
  end dab (if `placedStroke(W.last).dabs` has one more) onto clones of that.
  Bit-identical to `rasteriseBrush(W)`: the dabs before `settledDabs` are the
  same (placement by integer `k`, ADR 044 §6, asserted by B3 and checked in
  `BrushRaster.cpp`), and the end dab is the last thing painted.
- **The grown entry replaces H** when retained, so a 200-update stroke leaves
  one entry, not 200. An `Extended` never replaces its base, so the committed
  list before the stroke (what Undo returns to) stays a `Hit` when it was
  held.
- **`retain`:** every update is retained (the replacement keeps one entry);
  the comment on `coverage`'s `retain` parameter ("false for a live stroke's
  updates") is corrected in 6.1.
- **`peek`** answers `Grown` (with `cachedStrokes = n − 1`), and the residency's
  readiness maps it to `CoverageReadiness::Extended`. So `drawsBrushCoverage`
  is false and the progress weight counts only the last stroke.
- **Races.** Drawing happens outside the lock, so two threads can both grow
  from the same H. The replacement happens at insert, under the lock, by the
  identity of H's list pointer and size: if H is still there it is replaced;
  if another insert already replaced or evicted it, the grown entry is
  inserted plainly (two entries for a moment, no redraw, the older one ages
  out). The tint's grid (density 0.5) usually samples a different level from
  the render's at ratio 1, so the two chains rarely meet; they can, through
  the `reduced_` latch or `sampleBrush` falling back to a finer held level.
- **Counters for tests and harnesses:** `lookups(CoverageLookup)` (how many of
  each the cache answered) and `evictions()`, both atomic, never reset by
  lookups.
- **Serials:** new and cloned tiles get new serials, untouched tiles keep
  theirs, so the residency repacks and uploads only the touched tiles (step 5
  §2.6, unchanged).
- **Timing:** a `detail::TimingSpan("cpu.coverage")` around
  `CoverageResidency::update`, the CPU counterpart of `gpu.coverage`.

Expected cost (report B3, tile 128): 0.21 ms median per update at 1500×1000,
1.36 ms at 24 MP, 8 and 36 tiles touched, against 3.0 and 32.8 ms whole.

## 8. Rendering while painting

### 8.1 Paced requests

```cpp
/// @brief What a request does to the render in flight.
enum class Supersede {
    Cancel, ///< Cancel it (the default): the newest request is what matters.
    Queue,  ///< Let a render of an earlier request finish and be delivered; this one runs next.
};
std::uint64_t PreviewRenderer::request(DevelopState state, PreviewView view,
                                       Supersede supersede = Supersede::Cancel);
```

- `Queue` replaces the pending request as today, but does not cancel the work
  in flight when it is the render of a request (`Layer::Shown`, including its
  provisional stand-in). A fallback refresh or a histogram count in flight is
  still cancelled. A later `Cancel` request cancels as before.
- The window asks with `Queue` only while a stroke is under way, and for the
  request of the commit that ends it: `MaskOverlay::stroking()`, true from the
  press until its `editFinished` has been handled (the flag drops after the
  emit returns; the connection is direct). Not keyed on `painting()`: with
  Brush armed, a delta, Opacity or Invert drag in the Masks tab still cancels
  as every other drag does (Q8 (a); review finding 4). The result: one render
  after another, each of the newest state; latency at most two renders; no
  starvation.
- **No thumbnail while stroking:** requests made while `stroking()` and
  before the commit use `thumbnailEdge = 0`; the commit's request keeps
  `ThumbnailCache::maxEdge`, so the strip follows once per stroke.
- No duplicate-request skip (review finding 23): the commit's request differs
  from the last update's anyway (the release point, the thumbnail edge), and a
  skip would need resetting on every source, photograph, decode, resize and
  crop path.
- `RenderDelay` is not the tool: it holds a render back (noise reduction on the
  CPU); a stroke renders at once and paces instead. `renderDelayFor` gives 0
  for a strokes-only change (unchanged behaviour, tested).

Alternative (not taken): pace in the window (no request while one is
unanswered, request on its result). It needs the same bookkeeping in
`showResult` and is harder to test than the renderer's queue.

### 8.2 What a render redoes

- The stroke changes the plan's brush identity: Pointwise onwards reruns, from
  the Denoise checkpoint (step 5 §3.1); the Presence context is rebuilt from
  the pointwise input, as with every pointwise run (unchanged).
- Coverage: `CoverageResidency::update` → `Grown` → only touched tiles are
  quantised (`tilesPacked`) and, on the GPU, uploaded (`pending`).
- The pointwise pass covers the whole level the view renders (ADR 025). The
  level depends on the device-pixel ratio, since the request is in device
  pixels: at a fit view in a 1500×900 logical window, 24 MP renders the
  1500×1000 level at ratio 1 (about 50 ms on the CPU, step 3) but the
  3000×2000 level at ratio 2 (about 200 ms). At 1:1 it is 24 MP whatever the
  ratio: about 0.8 s on the CPU. Step 6 accepts this and measures it at both
  ratios (Q6).
- **Coarser first render:** unchanged. `Grown` and `Extended` are not a miss, so
  no stand-in during a stroke on a held brush. A brush whose list is not held
  (evicted, a new brush once deltas are set before painting) makes one `Miss`
  at the stroke's first update; its draw is one dab, so `drawsFromNothing`'s
  cost threshold normally keeps the stand-in away, and every later update is a
  `Grown` (§7).
- **Thumbnail:** none while stroking (§8.1); one for the commit. **Background
  refresh, curve histogram:** cancelled by each new request as today, so they
  run once the stroke ends.

### 8.3 The tint while painting

- With O on, the tint follows each update: `coverageInputs` changes with the
  list, `updateCoverage` runs on the overlay's zero-delay timer and
  `maskCoverage` → `sampleBrush` → `find` misses the new list at every level,
  then `coverage` at the grid's level is a `Grown` (§7). Expected: well under a
  millisecond to draw plus the sampling of at most 2^20 cells (2 ms median in
  step 3). The existing latch (`slowCoverageMilliseconds`, 8 ms) thins the
  grid for the rest of a stroke if it is slower. It stays on the GUI thread
  (decision 6b); moving it off is not needed if §11's numbers hold. The first
  stroke after an eviction can `Miss` and draw a heavy brush at the grid's
  level on the GUI thread once: decision 6b's known cost.
- The tint is drawn before the render arrives, so it is the most immediate
  feedback there is.
- **Repaints while stroking** (review finding 9): `setScene` and
  `updateCoverage` do not `update()` the whole overlay while `stroking()`;
  they update the ring rectangles and the widget bounding box of the dabs
  added since the last repaint (their points' box grown by `radius * scale()`
  plus one grid cell), the only places the tint changed. No other marks are
  drawn while painting. `paintEvent` is wrapped in an `overlay.paint` span,
  counted in §11's GUI time.
- **Auto tint (Q2):** while the tool paints a brush that contributes nothing
  to the plan (as `localPlanFor` decides: disabled, Opacity 0, or every delta
  zero), the overlay draws its tint whether O is on or not (the render would
  show nothing). The O state and the panel's button do not change; once the
  brush contributes, the tint follows O again.

## 9. Edits, history and cancelling

- **Signals.** `MaskOverlay` reports a stroke with its existing protocol:
  `editStarted` at the press, then the press's update at once, `stateEdited`
  per turn, `editFinished` at the release, `editCancelled` when lost.
  `MainWindow`'s `beginEdit`, `updateEdit`, `finishEdit` and `cancelEdit` need
  no change for the edit itself.
- **History:** origin `Edit`; "Add Brush 1" for a stroke that creates,
  "Paint Brush 1" for a stroke on a selected brush (`HistoryModel`,
  `tr("Paint %1")`, already there).
- **Undo** restores the list before the stroke. When that list is held at the
  render's size (the brush was rendered before, not evicted), the cache
  answers `Hit` and draws nothing; the residency still repacks, and on the GPU
  uploads, the tiles the undone stroke touched, since its slot holds the
  stroke's codes (review finding 3). Asserted that way in 6.1, B9 and §11.2.
- **Cancelled** (the edit and the `LiveStroke` dropped, the state before it
  shown, no step) by: Esc (§2's order); `setCropMode`, the straighten tool and
  arming the picker (through `leaveMaskMode` → `cancelGesture`); a tool the
  *user* chooses (`MainWindow::chooseMaskTool`, connected to the panel's
  `maskToolChosen` in place of `setMaskTool`, calls `cancelGesture` first;
  `setMaskTool` and `MaskOverlay::setTool` never cancel, so the program's own
  disarming, as in `syncMasks`, cannot end a stroke: review finding 2);
  switching or closing the photograph and a sidecar reload (the existing calls
  in `showPhoto`, `closePhoto`, `confirmLeavingPhoto`, `sidecarChangedOnDisk`);
  a lost pointer (`WindowDeactivate`, `Hide`, `focusOutEvent`, a move without
  the left button, as today).
- **Abandoned, then committed by the session** (ADR 022: `begin` and
  `setState` commit an open edit): Undo, Redo and a History click (existing
  `abandonDrag()` calls), and now also `editGeometry` (rotate, flip, swap,
  Ctrl+[ / Ctrl+]) and `pasteSettings` (review finding 10). The stroke so far
  lands as its own step, followed by the rotation or the paste. As a safety
  net, `updateEdit` and `finishEdit`, finding no open edit while
  `overlay.stroking()`, call `abandonDrag()`, so no other path can leave the
  overlay drawing a stroke the session no longer has. `abandonDrag()` now also
  drops the `LiveStroke` and stops `strokeTimer_`.
- **Redo mid-stroke:** disabled. The press's update changes the state at
  once, and `canRedo` is false for an open edit that changed it
  (`updateEditingActions` via `refreshPanel` runs in that update), so Redo is
  greyed from the press on and its key does nothing. (Were it reachable,
  `EditSession::redo` would commit the stroke, find nothing to redo and throw
  into `guarded`'s message box; review finding 19.)
- **`guarded`** already abandons the overlay's drag before cancelling; an edit
  rule's throw cannot happen (every append is checked by `canAppendStroke`
  first), and if it did, the stroke ends as a cancel.
- **Mid-stroke keys:** O toggles the tint; `[`, `]`, `{`, `}`, `E` and Alt
  change the next stroke; Space does nothing until the release; Delete does
  nothing (`deleteSelected` already refuses during a gesture).

## 10. Keys and `docs/keybindings.md`

New window-level shortcuts in `maskShortcuts_` (enabled in the mask mode only,
acting only while the tool paints). The overlay does **not** claim them in
`ShortcutOverride`: a claimed key goes to `MaskOverlay::keyPressEvent`, where
the modifier guard would drop the Shift-ed braces, and no other shortcut
competes for these keys, so the window shortcut fires whether the overlay or
`PhotoView` has the focus (review finding 5). A slider's value field keeps
typed characters, as `QAbstractSpinBox` accepts its own `ShortcutOverride`.

| Key | Action |
|---|---|
| `[` / `]` | Brush smaller / larger (÷ / × 1.25) |
| `{` / `}` (Shift+`[` / Shift+`]` on a US layout) | Brush softer / harder (∓ 10 % hardness) |
| `E` | Flip the Erase toggle |
| `Alt` (held at the press) | Erase with this stroke (paint when Erase is on) |

- Registered as `QKeySequence(Qt::Key_BracketLeft)`, `Key_BracketRight`,
  `Key_BraceLeft`, `Key_BraceRight` and `Key_E`, not as `Shift+BracketLeft`:
  Qt reports Shift+`[` as `Key_BraceLeft` on a US layout. Layouts where `[`
  needs AltGr (German) get the brackets by character, as Qt reports them; the
  panel rows remain.
- `Key_Alt` press and release are handled in `MaskOverlay::keyPressEvent` and
  `keyReleaseEvent` before the modifier guard (the cursor's "−", §4), and not
  accepted, so a menu bar's Alt handling is unchanged.
- **Risk:** on some Linux desktops Alt with a drag moves the window, so the
  press never arrives. `E` and the Erase button are the way round it (Q5).
- `docs/keybindings.md`: under *Current*, add the rows above, and move from
  *Planned* the ones that landed: `M` (Masks), `O` (mask tint, in the mask
  mode), Space with a drag (pan, in the mask mode), Delete / Backspace (delete
  the selected mask), Esc's mask-mode order (§2); F7, F8, F11 and F12 from ADR
  047 in passing (the doc's own rule says to move rows when a feature lands).

## 11. Measuring a long painting session

Two harnesses, both hidden tags, run by name in a release build
(`just release`).

### 11.1 Engine bench B9, "a painting session through a ladder" (`tests/test_BrushBench.cpp`, `[.bench][brush-bench]`)

- Source: a generated 6000×4000 frame; renders through `resumeOrDevelop` with
  one `CheckpointLadder`, as the window's shown layer does.
- State: 15 brushes from B2's realistic set plus one being painted, all with
  Exposure and Shadows deltas; one radial.
- Session A, 1500×1000 (the fit level at ratio 1): 100 strokes from B4's
  "painting" generator, each fed as growing partials of 10 points per update.
  Session A2, 3000×2000 (the fit level at ratio 2): 30 strokes.
  Session B, 6000×4000 (1:1): 10 strokes.
  Session C, 1500×1000: 10 strokes, each on a brush created inside the session
  (empty list first), then 10 strokes on a brush whose list was evicted
  (`clear()` of the cache before the stroke).
- Per update: render time; the Coverage step's time, from the progress
  observer's step transitions (the time from entering
  `ProgressStep::Coverage` to the next step); tiles packed
  (`LadderAccess::coverage(...)->tilesPacked()`); the cache's entry count,
  bytes (`memoryBytes`), `lookups` per kind and `evictions`. Every 25 strokes:
  10 undos and 10 redos, each rendered.
- Printed (B-style `Line`): median, p95, max per quarter of the session, so a
  slowdown over the session shows; evictions and `Miss` lookups per session.
- Asserted (not timing):
  - the last render equals a direct render of the committed state bit for bit;
  - the cache gains at most one entry per stroke (no partial-entry pile-up),
    also in session C;
  - each `Grown` update packs only the tiles its new dabs touch;
  - in session C, each stroke has at most one `Miss` (its first update) and
    the rest `Grown`;
  - an undo is a cache `Hit` (no `Miss`, `Extended` or `Grown` counted), and
    its `tilesPacked` delta equals the number of tiles the undone stroke
    touched;
  - no `Miss` during strokes in sessions A and A2.
- As built (differences from the above): the source is generated at the level
  size, not as a 6000×4000 frame; the base is 14 realistic brushes plus a
  radial plus the painted brush (the list holds 16); "packs only the new
  dabs' tiles" is checked by comparing tile serials, not dab geometry.
- Targets on this machine: Coverage step median ≤ 1 ms (A) and ≤ 3 ms (B);
  whole update render ≤ 60 ms median, ≤ 90 ms p95 (A), reported for A2; the
  last quarter's p95 within 1.2× the first's; cache ≤ 512 MiB throughout; an
  undo's render no slower than a delta edit's.

### 11.2 Window session (`tests/test_MainWindowMasks.cpp`, `[.timing]`, "Brush painting session")

- As the existing "Mask mode timing" case: `Window w(6000, 4000, QSize(1500,
  900), false)`, `ARRAW_PREVIEW_DEVICE=cpu` for the CPU run, the default for the
  GPU run. Run twice: at ratio 1 (offscreen), and with `QT_SCALE_FACTOR=2` set
  on the command line for a second run of the binary by name, so the request
  is 3000×1800 device pixels and the overlay paints at ratio 2.
- 15 masks first (B2-like brushes set with `setState` through the session, not
  painted), then the Brush button and 60 strokes of 120 moves each, events
  every 8 ms (120 Hz), wandering paths; at fit, then 10 strokes at 1:1; tint off,
  then 20 strokes with O on.
- Spans: `brush.update` (new, in `MainWindow::updateEdit` while
  `overlay.stroking()`, around the whole handling; it `note()`s
  `latestRequest_` after `requestRender`, so updates pair with results),
  `overlay.paint` (new, §8.3), `window.result` (existing, carries the request
  shown), `mask.coverage`, `cpu.coverage` (new, §7), `preview.render`.
  Collected in-process with `QtMessageCapture`, so the test computes and
  prints the metrics itself; `StallProbe` (moved to `tests/support/` from
  `test_MainWindowOpening.cpp`) gives the GUI thread's longest stall. The
  cache's `lookups` and `evictions` are read before and after each phase.
- **Metrics and targets** (fit view, CPU, this machine; ratio 1 unless said):

| Metric | Definition | Target |
|---|---|---|
| GUI time per update | `brush.update` plus the `overlay.paint` spans until the next update | median ≤ 1.5 ms, p99 ≤ 5 ms |
| Longest GUI stall | `StallProbe` during the strokes | ≤ 16 ms (tint off), ≤ 24 ms (tint on) |
| Latency | from an update's span start to the first non-provisional `window.result` with a request id at least the one it noted | median ≤ 100 ms, p95 ≤ 160 ms (ratio 1); reported at ratio 2 (expected about 300 ms median) |
| Renders shown | results per second during a stroke | ≥ 10 (ratio 1); reported at ratio 2 (expected about 4) |
| Tint | `mask.coverage` during strokes | median ≤ 4 ms, p95 ≤ 8 ms |
| Commit | release to the final result shown | ≤ one render |
| Undo | Ctrl+Z to result shown, 20 strokes back | ≤ one render; cache `Hit`s only; `cpu.coverage` packs only the undone stroke's tiles |
| Cache | `lookups` and `evictions` during strokes | no `Miss` and no eviction at fit; reported at 1:1 |
| Memory | resident set (`/proc/self/statm`) per phase (after the 15 masks, after fit, after 1:1), with the cache's `memoryBytes` | reported against decision 8's bounds (cache ≤ 512 MiB asserted; host planes up to 366 MiB at 24 MP; level-0 checkpoints), not a single pass mark |

  At 1:1 on the CPU only reported (expected about 1 s latency, §8.2), and the
  numbers at 1:1 and at ratio 2 decide Q6's follow-up. The ladder's residency
  holds full-size coverage regardless of `retainedCoveragePixels` (that limit
  is the direct path's), so 15 heavy brushes at 1:1 may evict: the eviction
  count says whether they did. GPU: reported where a hardware GPU exists;
  this machine has only llvmpipe, which the app refuses, so the GPU run
  (pointwise plus upload per update) is "not measured" here and lavapipe is
  used for correctness only.
- The results go into the dated ADR 044 note (6.6) with the machine, as step
  3's did.

## 12. Sub-steps

Each is separately reviewable and committable, leaves `just test`, the GPU
tests under lavapipe and `just py-test` green, and runs `just format` and
`just format-check`. The hidden digests (CPU and GPU) are unchanged by every
sub-step. Until 6.4 nothing creates a brush in the window; from 6.4 on, a
painted brush is one export renders (step 5). 6.4 and 6.5 both edit
`MainWindow.cpp` and `MaskOverlay.cpp`: 6.4 does the gesture, 6.5 the pacing
and keys. Between them strokes still cancel renders in flight; no timing
session runs before 6.5.

### Step 6.1: The tail method in the cache

- Files: `src/core/BrushCoverageCache.{h,cpp}` (`Grown`, `Entry::settled`,
  `settledDabs` kept by every draw of a last stroke, replacement of the parent
  by identity at insert, `peek`, `lookups`, `evictions`, the `retain` comment),
  `src/core/BrushCoverage.cpp` (`Grown` → `CoverageReadiness::Extended`, the
  `cpu.coverage` span), `src/core/RenderProgress.cpp` if the weight needs it;
  `tests/test_BrushCoverageCache.cpp`, `tests/test_BrushCoverage.cpp`,
  `tests/gpu/test_GpuBrush.cpp`; B9 in `tests/test_BrushBench.cpp`.
- Tests: random growth sequences (steps of 1 to 50 points, repeated points,
  a point jumping back, hardness 0 and 1, erase, radius at the minimum, a
  path leaving the picture) equal `rasteriseBrush` bit for bit after every
  step, at two tile sizes; growth from an empty brush (first update a `Miss`,
  every later one `Grown`); growth from an evicted base (same); a 200-update
  stroke leaves one more entry than before it; the list before the stroke
  stays a `Hit`; a style change in the last stroke or a non-prefix path is an
  `Extended`, never a `Grown`; `peek` agrees with `coverage`; through a
  ladder, a `Grown` update packs exactly the touched tiles, and an undo is a
  `Hit` that packs exactly the undone stroke's tiles; on the GPU (lavapipe) a
  `Grown` uploads only the touched tiles and the texture reads back equal to
  the host plane; `drawsBrushCoverage` is false during a growth; two threads
  growing the same chain give the reference bits, and an insert whose parent
  is gone adds a plain entry.
- Done when: the above pass and B9 runs (its numbers recorded in 6.6).

### Step 6.2: Paced preview requests

- Files: `src/app/PreviewRenderer.{h,cpp}` (`Supersede`, the in-flight kind),
  `tests/test_PreviewRenderer.cpp`, `tests/gpu/test_GpuPreviewRenderer.cpp`.
- Tests, deterministic: a progress callback that blocks on a gate the test
  opens holds the worker at a known step while `Queue` and `Cancel` requests
  arrive (review finding 22). With a render held, a `Queue` request lets it
  finish and deliver, then delivers its own; three `Queue` requests during one
  render deliver the first and the last; a `Cancel` request after a `Queue`
  one cancels as today; a fallback refresh or histogram count in flight is
  cancelled by a `Queue` request; a request with `thumbnailEdge = 0` delivers
  no thumbnail; the newest request always ends with a result.
- The class comment of `PreviewRenderer` states the exception to "a newer
  request cancels the render in flight".

### Step 6.3: App-core brush logic

- Files: new `src/app/BrushPainting.{h,cpp}` (`BrushSettings`, its `QSettings`
  functions, `resized`, `hardened`, `BrushRefusal`, `refusalOf`,
  `probeStroke`, `describeRefusal`, `LiveStroke`), `include/DevelopedFrame.h`
  and `src/core/DevelopedFrame.cpp` (`sensorFrom`), `src/app/MaskEditing.h`
  (`MaskTool::Brush`), `src/app/SettingPresentation.{h,cpp}`
  (`SliderScale::Logarithmic`, the three brush presentations),
  `src/app/ui/SettingSlider.cpp` (the logarithmic positions), CMake;
  `tests/test_BrushPainting.cpp`, `tests/test_DevelopedFrame.cpp`,
  `tests/test_SettingPresentation.cpp`, `tests/test_SettingSlider.cpp`.
- Tests: §5's mapping test; the radius stored equals the setting; thinning
  (positions closer than the rule are dropped, the release kept); Alt at the
  press captured for the stroke; consecutive states share the earlier strokes
  by pointer and the live points are a prefix; the split at 10 000 points
  gives two strokes and the finished one is pointer-equal to the last stroke
  of the state reported before the split; each refusal at the press (with the
  probe: a brush whose swept-area or dab budget is nearly spent refuses a
  press) and mid-stroke, with the state left at the last accepted one;
  creation adds a brush with the next id; `describeRefusal` for each case;
  settings round trip through `QSettings` (a temporary INI file), out-of-range
  stored values clamped; `resized` and `hardened` clamp; the logarithmic
  slider maps its ends exactly and is monotone.

### Step 6.4: Panel, overlay and cursor

- Files: `src/app/ui/MasksPanel.{h,cpp}` (Brush button and its enabling, the
  Brush block, `brushSettingsChanged`, `setBrushSettings`, the hints),
  `src/app/ui/DevelopPanel.{h,cpp}` (pass-through signals as for
  `maskToolChosen`), `src/app/ui/MaskOverlay.{h,cpp}` (`painting()`,
  `stroking()`, `setBrushSettings`, the `LiveStroke` gesture, `strokeTimer_`,
  the cursor and Alt tracking, hidden pins, Alt no longer passed through while
  painting, the Esc order, partial repaints while stroking, `brushRefused`,
  `ringRadius()`, `abandonDrag` dropping the stroke), `src/app/ui/MainWindow.cpp`
  (`chooseMaskTool`; choosing a brush arms Brush, another mask disarms it;
  `syncMasks`'s full-list rule; connect `brushSettingsChanged` to the overlay;
  `abandonDrag` in `editGeometry` and `pasteSettings`; the `updateEdit` /
  `finishEdit` safety net); tests `tests/test_MaskOverlay.cpp`,
  `tests/test_MasksPanel.cpp`, and the step-5 assertion "the Masks tab offers
  no brush creation" in `tests/test_MainWindowMasks.cpp` inverted.
- Tests: a press emits `editStarted` then at once a `stateEdited` with a
  one-point stroke; moves in one turn give one `stateEdited`; the release
  flushes and finishes; Alt at the press erases; Alt pressed with the focus
  elsewhere shows "−" on enter; Esc, `WindowDeactivate`, hiding, focus out and
  a move without the button cancel; Esc's order (stroke, brush selection,
  tool, mode); Space with a drag and a middle drag pan with no edit; the wheel
  is ignored mid-stroke; pins are not grabbable while painting; with Brush
  unchecked and a brush selected, a drag pans and a click deselects; the
  ring's radius (§4) at three zooms and two ratios; the forbidden cursor on a
  brush whose budget is spent (by the probe); the panel shows the Brush block
  exactly while Brush is armed, Brush is checked after choosing a brush in the
  list, and Brush is disabled at 16 masks unless a brush is selected; with 15
  masks, Brush then a drag gives "Add Brush 16", the stroke is not cut short,
  and Brush stays armed on it; a partial repaint while stroking covers the
  ring and the new dabs only.

### Step 6.5: Window, keys, tint and documents

- Files: `src/app/ui/MainWindow.{h,cpp}` (`Queue` requests while
  `stroking()` and for the commit, `thumbnailEdge = 0` while stroking, `[`,
  `]`, `{`, `}`, `E` in `maskShortcuts_`, saving `BrushSettings` on change and
  restoring them at start, `brushRefused` → `StatusLine::showMessage`),
  `src/app/ui/MaskOverlay.cpp` (auto tint, Q2), `docs/keybindings.md` (§10);
  tests `tests/test_MainWindowMasks.cpp`, `tests/test_HistoryModel.cpp`.
- Tests on the generated 640×480 PNG of step 3: Brush, then a drag: one step
  "Add Brush 1", the mask selected, Brush still armed, the render changes
  where painted once a delta is set; a second drag: "Paint Brush 1"; Esc,
  then a drag: "Add Brush 2"; Undo and Redo; C, the arrow keys and Esc
  mid-stroke leave no step; Undo mid-stroke commits and undoes it, Redo
  restores it; Redo is disabled from the press on; Ctrl+] and Ctrl+Shift+V
  mid-stroke give a "Paint" step then the rotation or paste, and the overlay
  stops stroking; a brush at its budget refuses a press with a status message
  and no step; a stroke stopped by a budget commits what fits; `]` three times
  grows the radius by 1.25³ and the next stroke has it, with the overlay
  focused and with `PhotoView` focused; `}` hardens; the settings survive a
  new `MainWindow` (same `QSettings`); during a stroke on a cached brush no
  provisional result arrives and no thumbnail is made; a delta drag with
  Brush armed still cancels the render in flight; the last render after the
  commit equals a direct render of the committed state; auto tint draws for
  a brush without deltas, and for a disabled one, while O is off.

### Step 6.6: Measurements and the ADR note

- Files: `tests/test_MainWindowMasks.cpp` (the `[.timing]` session of §11.2;
  the `brush.update` span in `MainWindow.cpp`, the `overlay.paint` span in
  `MaskOverlay.cpp`), `tests/support/StallProbe.h` (moved), ADR 044 (a dated
  note "the brush in the window, as built": tool, creation, settings and keys,
  the tail method in the cache, paced requests, the numbers of §11 with the
  machine and both ratios, departures from §10), `docs/todo.md` and
  `local-adjustment-plan.md` (step 6 done), `docs/ideas/brush-gui-plan.md`
  (status).
- Done when: both harnesses ran in a release build on the CPU at ratios 1 and
  2, every target of §11 is met or its miss is written down with the
  follow-up the user picks (Q6), and the GPU run is reported or marked "not
  measured" with the reason.
- Note from 6.2: a `Queue` request cancelling a histogram count in flight has
  no deterministic test; the count is too quick to be caught in flight, and
  the test only checks that the newest request ends with a histogram.

## Response to the review (2026-10-10)

All findings were checked against the code before changing the plan.

- **High 1** (new brush never grows): fixed, §7: every draw of a last stroke,
  a `Miss` included, keeps the settled grid; tests in 6.1, session C in B9.
- **High 2** (16th mask cancels itself): fixed, §2 and §9: only user tool
  choices cancel (`chooseMaskTool`); `syncMasks` never disarms mid-stroke.
- **Medium 3** (undo packs): fixed, §9, §11: a `Hit` that packs the undone
  stroke's tiles.
- **Medium 4** (pacing scope): fixed, §8.1: keyed on `stroking()`.
- **Medium 5** (keys): fixed, §10: not claimed by the overlay; braces as
  `Key_BraceLeft` / `Key_BraceRight`; `Key_Alt` before the guard; modifiers
  queried on enter.
- **Medium 6** (one-point probe): fixed, §6.4: a probe stroke one radius long.
- **Medium 7** (HiDPI): fixed, §8.2, §11: ratio 2 in B9 (A2) and the window
  session; per-ratio targets; Q6.
- **Medium 8** (memory): fixed, §11.2: RSS per phase against decision 8,
  eviction and `Miss` counters, no `Miss` at fit asserted.
- **Medium 9** (overlay repaints): fixed, §8.3: partial repaints and an
  `overlay.paint` span counted in GUI time.
- **Medium 10** (rotate or paste mid-stroke): fixed, §9: `abandonDrag` in
  those paths plus a safety net in `updateEdit` / `finishEdit`.
- **Medium 11** (thumbnails): fixed, §8.1: `thumbnailEdge = 0` while stroking.
- **Medium 12** (implicit painting): taken as the proposed design (a) with
  (b), §2; kept as Q9 for the user.
- **Low 13** (split pointer): fixed in the mechanism, §6.4 (`base_` is the
  reported state). Q7 still recommends (a), not the review's (b): a careful
  outline with a small brush reaches the cap at 1-pixel spacing, and stopping
  there would cut it.
- **Low 14** (race): fixed, §7. **15** (`retain` comment): fixed, §7, 6.1.
  **16** (Coverage span): fixed, §7 (`cpu.coverage`) and §11.1 (progress
  observer). **17** (span placement): fixed, §11.2. **18** (signals): fixed,
  §3. **19** (Redo): fixed, §9; the press updates at once. **20** (auto tint
  condition): fixed, §8.3, Q2. **21** (`sensorFrom`): fixed, §5. **22**
  (timing tests): fixed, 6.2. **23** (duplicate skip): dropped, §8.1.
  **24** (pixmap cursor): noted as considered, §4. **25** (refusal wording):
  three messages, §6.4.

## 13. Decisions (user, 2026-10-10)

All recommendations accepted: Q1 to Q9 are each answered (a), as below.

### The questions as asked

1. **How is a brush created?**
   (a) **Brush arms the tool; with no brush selected, the first stroke creates
   the mask** ("Add Brush 1", one step), as Linear and Radial are created by
   their drag.
   (b) Brush adds an empty brush at once ("Add Brush 1"), selected and ready;
   deltas can be set before painting; each stroke is then "Paint Brush 1".
   (c) Both: a click on Brush arms, a double-click adds an empty one.
   Recommended: (a); it leaves no empty masks from stray clicks, and Q2 (a)
   covers painting before any delta is set.

2. **What shows while painting a brush that contributes nothing** (no deltas,
   Opacity 0 or disabled: the render shows nothing)?
   (a) **Its tint, automatically, while the painted brush contributes
   nothing to the plan**, whatever O says.
   (b) The tint during every stroke, deltas or not.
   (c) Nothing: the photographer turns O on.
   Recommended: (a).

3. **What does Size mean when the zoom changes?**
   (a) **A share of the photograph's long edge**: the ring grows when zooming
   in, a stroke is the same whatever the zoom (Photoshop's way).
   (b) A size on screen: the ring stays put, and zooming in paints finer
   (Lightroom's way); the stored radius is worked out at each press.
   Recommended: (a); the stored radius is already in long-edge units, and `[`
   / `]` resize after a zoom.

4. **Where are the brush settings kept?**
   (a) **`QSettings`, across application sessions** (Size, Hardness, Flow; not
   Erase).
   (b) Only for the application session.
   (c) Per mask, as view state of the window.
   Recommended: (a).

5. **How does one erase?**
   (a) **Alt held at the press, an Erase button in the panel, and `E`.**
   (b) Alt and the button, no key.
   (c) The button only, leaving Alt with a drag to pan as today.
   Recommended: (a); `E` avoids the desktops where Alt with a drag moves the
   window.

6. **Painting where the shown level is large** (1:1 on the CPU: whole-level
   pointwise per render, about 0.8 s at 24 MP, so about one picture a second
   while the tint follows the hand; a fit view at device-pixel ratio 2: the
   3000×2000 level, about 200 ms, so about four pictures a second):
   (a) **Accept it in step 6, measure it at ratios 1 and 2, and decide a
   follow-up from the numbers.**
   (b) During a stroke, render through the background ladder at its coarser
   level and show it scaled, then the full level after the release.
   (c) Restrict the pointwise pass and geometry to the stroke's dirty rectangle
   in the ladder (ADR 025's "future work"; its own ADR and plan).
   Recommended: (a), with (c) the likely follow-up if the GPU numbers are good
   and the CPU stays the fallback.

7. **A stroke that reaches 10 000 points:**
   (a) **It goes on as a new stroke in the same step**, with an extra dab at
   the joint (the split builds on the reported state, so it costs one dab).
   (b) It stops at the cap, with a status message, until the release (the
   review's preference: simpler).
   Recommended: (a); a long outline with a small brush can reach the cap, and
   the fix that makes (a) cheap is a few lines.

8. **Paced requests beyond the brush:**
   (a) **Only for brush strokes now** (keyed on a stroke under way, not on
   the tool being armed).
   (b) For every drag (handles, sliders) as well, which may make slider drags
   on the CPU smoother.
   Recommended: (a); measure (b) separately, since it changes how every drag
   feels.

9. **How does one stop painting a selected brush?** (review finding 12)
   (a) **The Brush button is the painting state**: checked exactly while the
   tool paints; choosing a brush in the list checks it; unchecking it returns
   a left drag to panning and a click to deselecting; Esc first deselects a
   painted brush (Brush stays armed, so the next stroke starts a new brush),
   then disarms, then leaves the mode.
   (b) A selected brush paints implicitly; Brush only arms creation and
   disarms after it (the first draft: a stray click adds a "Paint" step, and
   the button shows unchecked while painting).
   (c) As (a), but choosing a brush in the list does not check Brush: painting
   always needs the button.
   Recommended: (a); the button says what a drag will do, and Esc gives the
   way to a new brush.
