# Linear and radial masks in the window — plan

Status: proposed, 2026-10-09. Step 3 of `local-adjustment-plan.md` (linear
and radial GUI). Builds on step 2 (d9c0bfb: model, edit rules, engine,
sidecar, Python) and the History dock (87a855b). Spec: ADR 044 §1, §4 and
§10, the plan's §9 GUI bullets, ADR 040 (the crop mode, the precedent for a
mode over the photo view) and ADR 022 (an edit is begun, updated and
committed as one step). Qt Widgets only; every string British English through
`tr()`.

## Decisions (proposed; for the user to confirm)

| Question | Proposal | Alternative |
|---|---|---|
| Where the panel goes | A **Masks group in the develop dock**, right below Crop | A separate Masks dock, tabbed with Develop |
| Entering the tool | A **mask mode**: Photo > Masks, key **M**, exclusive with the crop mode; selecting a mask or pressing Add enters it | Handles shown whenever a mask is selected, no mode |
| Creating | **Arm, then drag on the photograph**: Linear from → to, Radial centre → radius; a click without a drag places a default-sized mask there | Create a default mask in view, then adjust |
| Handles | Linear: both ends, centre, both band lines. Radial: centre, four radius points, a rotation knob, a feather knob | — |
| Overlay | Red tint of the selected mask's weight, **O** toggles (mask mode only), from a public core function sharing `maskWeight` | A preview-renderer tap |
| Coordinates | A public `DevelopedFrameMap` in `include/` (corrected ↔ developed frame, affine), composed with `ViewTransform` in app-core | Expose `GeometryPlan` publicly |
| Undo inside the mode | Ordinary history: each gesture is a step (unlike the crop mode, the mask mode is not one edit) | — |
| Selection | View state in the window; kept across undo while its id exists, cleared otherwise | — |
| Reorder | Move Up / Move Down buttons | Drag and drop in the list |
| Where the wiring goes | The session hub stays in `MainWindow` (shared edit slots); shapes, hits, drags and the list model are app-core | A `MaskModeController` in ui |

## Goal

A photographer adds a graduated or a radial mask by dragging on the
photograph, moves and reshapes it with handles at any zoom, sees where it
applies as a red tint, and sets its opacity and thirteen deltas with the same
sliders as the global controls. Every gesture is one history step, worded as
the History dock already words it ("Add Linear 1", "Move Linear 1",
"Linear 1: Exposure +0.50"). Photographs without masks render exactly as
before.

Out of scope: the brush (steps 4 to 6), combining shapes, drag-and-drop
reordering, masks in the crop mode, Python bindings for the new public
functions.

## What exists (findings)

- **Edit rules** (`include/LocalAdjustmentEdits.h`) cover every list action:
  add (shape or full adjustment), duplicate, remove, reorder, rename, enable,
  invert, opacity, one delta, the shape (`withLocalShape`), and
  `canAddLocalAdjustment`, `maskOrdinal`, `defaultMaskName` (English only).
  Each normalises and **throws** for a degenerate shape (radius or end
  distance below `minimumMaskExtent`), so a drag must clamp before it calls
  them, or `guarded()` would show a message box mid-drag.
- **History wording** for masks exists (`src/app/HistoryModel.cpp`:
  `maskLabel`, `wordDelta`, `wordLocal`). Its `maskLabel` and the delta
  presentation are private to that file; the masks list needs the same
  names, so both move to a shared app-core file.
- **No public point mapping.** `GeometryPlan::toUpright` / `toSource` are
  private (`src/core/GeometryPlan.h`). `CropGeometry.h` speaks only the
  uncropped upright frame (`CropPoint`, `CropFrame`). The white balance
  picker maps developed → upright → source inside core
  (`WhiteBalance.cpp:341`), which is the precedent. The app has no way to
  place a `CorrectedPoint` on screen.
- **No public coverage.** `maskWeight` and the shape resolution live in
  `src/core/LocalPlan.{h,cpp}` (private). `localPlanFor` drops disabled masks
  and masks without deltas, so it cannot serve the overlay of a new mask.
- **The mapping is a similarity.** Corrected normalised → source pixels is a
  per-axis scale; source → upright is orthogonal (turns, flips, straighten);
  upright → developed frame is a translation; developed → view is a uniform
  scale and translation. In ADR 044's long-edge metric the whole chain is a
  rotation, maybe a reflection, a scale and a shift, so circles stay circles
  and a linear mask's bands stay perpendicular to its axis on screen. The
  preview's reduced level does not enter: everything is in fractions.
- **Keys.** C is the crop mode (ADR 040 and `crop-ui-plan.md` still say R;
  the code moved it to C so R is red). **M is free.** **O** is only a crop
  shortcut, enabled in the crop mode alone (`cropShortcuts_`), and
  Ctrl+Shift+O opens a folder: plain O is free outside the crop mode.
  Delete and Space are free at window level.
- **Space does not pan today.** ADR 044 and the plan say "pan stays on
  Space", but `PhotoView` pans with a left drag (or middle, or Alt with left)
  and knows no Space. The mask mode needs the left button, so Space with a
  left drag becomes the pan there (below).
- **No `CropModeController`.** ADR 044 §10 names one; the crop wiring is in
  `MainWindow` (`setCropMode`, `leaveCropMode`, `closeCropOverlay`, the
  `cropShortcuts_`, `guarded`).
- **Small fixtures.** The committed images are 32×24 or 61×41, and
  `ViewTransform::fitZoom` never enlarges, so the window shows them a few
  dozen pixels wide. Window tests of handles need a generated photograph of
  some hundreds of pixels (a PNG written into the test's temporary folder).
- `arraw-app-core` has `src/core` on its private include path, and three app
  files already use private core headers (`OklabHue.cpp`,
  `CurveHistogramRefresh.h`, `CurveEditing.cpp`). New code must not add to
  them; anything the app needs from core goes through `include/`.
- Stale wording to fix in passing: `CullingActions.h` says the labels are
  Shift with R Y G B P (they are plain keys), `PhotoView.cpp` and
  `CropOverlay.cpp` mention R as the crop key.

## Design

### 1. The Masks group

**Where.** A `Masks` group in the develop dock, below Crop and above White
Balance: the two tools that act on the picture's area come first, the global
controls after. Recommended over a separate dock because:
- ADR 044 §10 already put it there;
- the group joins `nonGeometryGroups_`, so the crop mode disables it for free;
- its slider rows share the panel's label column and edit protocol, so the
  window needs no new wiring for slider edits (below);
- the develop dock is fixed (`NoDockWidgetFeatures`), and a second fixed dock
  would cost width or a tab the photographer must switch.

The cost is length: with a mask selected the group adds about twenty rows
above the global groups. The list is capped at five visible rows (it
scrolls), and the delta rows show only with a selection. If this proves too
long, the alternative is a Masks dock tabbed with Develop, raised on entering
the mask mode; the group's widget moves there unchanged.

**Contents, top to bottom.**
1. A row of buttons: **Linear** and **Radial** (checkable: armed while
   checked, see §4), **Duplicate**, **Delete**, **Move Up**, **Move Down**,
   and **Show Overlay** (checkable, the O key's state).
2. The list (`QListView` over `MasksModel`, §8), in the state's order, which
   is the order masks sum in. Each row: a check box for Enabled, the
   displayed name ("Linear 2" when unnamed, translated), a dimmed "(inverted)"
   suffix when inverted, greyed text when disabled. Double-click or F2 renames
   in place; an empty name returns to the default.
3. With a mask selected: an **Invert** check box, an **Opacity** row (0 to
   100 %, stored 0 to 1), then the thirteen delta rows in the table's order
   (`localAdjustmentDescriptors`): Temp, Tint, Exposure, Contrast,
   Highlights, Shadows, Whites, Blacks, Texture, Clarity, Dehaze, Saturation,
   Vibrance.
4. With none selected: a hint in place of 3, "Select a mask, or add one with
   Linear or Radial.", and Duplicate, Delete, Move Up and Move Down disabled.

**The limit.** Linear, Radial and Duplicate are disabled when
`canAddLocalAdjustment(state)` is false, with the tool tip "A photograph
holds at most 16 masks." The group title stays "Masks"; a small "3 of 16"
label sits right of the buttons.

**Black & White.** As the global Colour group (ADR 027,
`visibleGroups`), the local Saturation and Vibrance rows hide under the
grayscale treatment; their values stay in the state. Temp and Tint show for
every photograph, JPEGs included (ADR 044's consequence: local Temp/Tint
works where the global one does not).

**Presentation of the local rows** (app-core `MaskPresentation`, §8):
`localPresentationOf(localKey)` gives a `SettingPresentation`:
- a row with a `globalKey` takes the global presentation's label, unit,
  decimals and step (Exposure " EV", two decimals; the rest whole numbers),
  and a tool tip of its own ("Brightens or darkens the masked area, added to
  the photograph's Exposure.");
- `relativeTemperature`: "Temp", no unit, no decimals, step 1, "Warms (right)
  or cools (left) the masked area, relative to the photograph's white
  balance."; `relativeTint` likewise, "Tint", towards magenta (right) or
  green;
- the range is always the local descriptor's (Exposure ±4, the rest ±100),
  never the global one;
- `maskOpacityPresentation()`: "Opacity", " %", no decimals, step 1, range 0
  to 100.

`SettingSlider` gains a constructor from an id, a range, a default and a
presentation; today's key constructor delegates to it. The local rows use ids
of their own (`local.exposure`), so they never collide with the global rows
in `DevelopPanel::rows_`. `HistoryModel::wordDelta` switches to
`localPresentationOf` so the list, the sliders and the history word a delta
alike.

**How edits flow.** `MasksPanel` (a `QGroupBox` in ui, owned by
`DevelopPanel`) builds the next state from the shown one with the edit rules
and reports through `DevelopPanel`'s existing signals:
- a slider drag or a run of keyboard and wheel changes: `editStarted`,
  `stateEdited(withLocalDelta(shown, id, key, value))` per change,
  `editFinished` (one step, origin Edit; the slider's double-click reset
  stays origin Edit as a per-row reset is today);
- a list action (add by button with the tool disarmed, duplicate, delete,
  rename, enable, invert, move): one complete edit, `editStarted`,
  `stateEdited`, `editFinished`, as `applyChoice` does.

`DevelopPanel::finishPendingEdit` and `finishOtherEdits` cover the masks
rows too. Selection requests go out as `DevelopPanel::maskSelected(optional
id)`; the window answers with `DevelopPanel::setSelectedMask(optional id)`,
which shows it without emitting.

### 2. The mask mode

**Entering and leaving.** A checkable action Photo > **Masks** (object name
`maskAction`, key **M**), enabled with an editable photograph. Also entered
by selecting a row of the list, by Linear or Radial, and by clicking a pin
(§3); left by M, the action, Esc (§7), entering the crop mode, or leaving the
photograph. Leaving keeps the selection, so the panel still shows the mask's
sliders; it hides the handles, pins and tint.

**Exclusive with the crop mode.**
- Entering the crop mode (C, the Crop button, the straighten tool, a
  geometry command that enters it) first cancels a mask gesture in progress,
  then leaves the mask mode.
- Entering the mask mode while cropping leaves the crop mode **keeping** the
  crop, as pressing C again does (`leaveCropMode(true)`), then enters.
- The crop mode disables the Masks group (it is a non-geometry group).

**Not one edit.** Unlike the crop mode (ADR 040), the mask mode opens no
session-long edit. Each gesture is its own step, so Undo, Redo, the History
dock, Paste, the zoom and the other groups keep working in the mode. This is
what makes masks feel like adjustments rather than a dialogue.

**The view stays live.** Zoom (wheel, View menu, status button) and pan keep
working; the overlay maps through the view as it is (§5).
- The wheel is ignored by the overlay, so it reaches `PhotoView` and zooms
  about the cursor.
- A middle drag, or Alt with a left drag, is ignored too, and pans.
- **Space held**, then a left drag, pans (the overlay ignores the press while
  Space is down; the cursor is an open hand). The overlay claims Space in
  `ShortcutOverride` so no focused button is pressed.
- A left drag on empty canvas with no tool armed pans as well (the overlay
  calls a new `PhotoView::panBy(QPointF)`), and a left click there without a
  drag clears the selection.

**The picker.** Entering the mask mode disarms the white balance picker;
arming the picker leaves the mask mode (keeping the selection).

**Keys in the mode**, wherever the focus is (window shortcuts enabled only in
the mode, as `cropShortcuts_`; the overlay claims them when focused, and is
the photo view's focus proxy, as ADR 040 does for the crop overlay):
- **Esc**: cancels a gesture in progress; else disarms an armed tool; else
  leaves the mode.
- **O**: toggles the overlay (Show Overlay).
- **Delete** and **Backspace**: delete the selected mask (one step).
- **M**: leaves the mode (the action's own shortcut).
X (Reject), the arrow keys (step photographs), the ratings and labels keep
their meaning: none collides.

### 3. Handles

Drawn by `MaskOverlay` for the selected mask only; every other mask shows a
**pin** (a small ring; hollow and dimmed when disabled) at its centre: the
midpoint of a linear mask's ends, a radial mask's centre. Clicking a pin
selects that mask. Marks are white with a one-pixel dark rim, as the crop
frame's, so they read on a light sky. Sizes are in logical pixels and do not
change with the zoom.

**Linear.**
- Three parallel lines across the whole view, perpendicular to from → to:
  through `from` (solid: weight 1), through `to` (dashed: weight 0) and
  through the midpoint (thin), and a faint axis segment from → to.
- Dots at `from`, at `to` and at the midpoint.
- Drags:
  - **an end dot** moves that end (angle and spread change); Shift snaps
    from → to to horizontal or vertical on screen;
  - **the midpoint dot** moves both ends;
  - **a band line** away from its dot moves that end along the axis only
    (spread changes, angle kept);
  - every result keeps the ends at least `minimumMaskExtent` apart. An end
    dot dragged past the other end reverses the gradient, so an end can turn
    it through any angle (decided 2026-10-09, after the review).

**Radial.**
- The ellipse (weight 0 beyond it) and, dashed, the feather ring at the
  inner distance `1 - feather` (weight 1 inside).
- Dots: the centre; four radius points at ±x and ±y of the mask's turned
  axes; a **rotation knob** on a short stem 24 logical pixels beyond the +x
  point; a **feather knob** on the feather ring at 45° between +x and +y.
- Drags:
  - **centre**: moves the mask;
  - **a ±x point**: `radiusX` becomes the pointer's distance from the centre
    along the x axis (both sides move: the centre stays); ±y likewise;
    Shift keeps `radiusX / radiusY` (scales both). Clamped to
    `minimumMaskExtent` to `maximumMaskRadius`;
  - **rotation knob**: `angle` turns by the pointer's turn about the centre;
  - **feather knob**: `feather = clamp(1 - d, 0, 1)`, `d` the pointer's
    elliptical distance (`sqrt((q.x/rx)^2 + (q.y/ry)^2)` in the mask's frame).
- All of it is worked out in the corrected frame's long-edge metric (ADR 044
  §4), never in screen axes, so a mirrored orientation or a flip shows the
  rotation the way the pointer turns, with no special case.

**Hit testing.** A handle is grabbed within **10 logical pixels**
(`maskHandleReach`, the crop overlay's `grabDistance`) of its dot; a band
line within the same distance of the line. The nearest wins; at equal
distance the order is rotation knob, radius points, feather knob, ends,
centre, band lines, then pins of other masks. On a mask smaller than its
handles the centre stays reachable because the rotation knob sits outside the
ellipse. A drag keeps the grab offset (the handle does not jump to the
pointer), as the crop overlay does.

Handles outside the frame (positions may lie from -2 to 3) are drawn and
grabbable where the view shows them; zooming out brings them in.

### 4. Creating

**Chosen: arm, then drag.** Linear or Radial in the group (checked) arms the
tool and enters the mode; the cursor becomes a cross.
- **Linear**: press at `from`, release at `to`.
- **Radial**: press at the centre, release at the radius (a circle:
  `radiusX = radiusY`, `angle` such that the x axis is horizontal on screen,
  `feather` 0.5).
- A press and release closer than 4 logical pixels is a **click**: a default
  mask at the click (linear: `to` a fifth of the view's shorter side straight
  below `from` on screen; radial: a circle of a sixth of that side).
- The tool disarms after one mask; the new mask is selected. Esc disarms
  without creating; Esc during the drag cancels it.
- The whole creation is one step: begin on the press, `update` with
  `withLocalAdjustmentAdded(baseline, shape)` on the first move and
  `withLocalShape` after, commit on release. The History dock says
  "Add Linear 1" (`describeChange` sees an added id).

**Why, over create-then-adjust.** Placing a gradient means choosing its
position, direction and extent at once; one drag gives all three, as
Lightroom and Capture One do, and a default that is almost always wrong costs
the photographer a second gesture on every mask. The extra code is small and
pure (`createdShape` in app-core, §8), and the click fallback keeps a
one-click path. Create-then-adjust remains what a click does.

### 5. Coordinates

The chain, and who owns each step:

```text
widget (logical px)  ×devicePixelRatio      → view (device px)        app-core MaskViewMapping
view                 ViewTransform::frameFromView → developed frame (fractions of the crop)
developed frame      crop left/top + x * width   → upright (source px) core, private GeometryPlan
upright              GeometryPlan::toSource      → source (edge px)
source               / source size               → corrected (fractions; no lens corrections yet)
```

**New public API**, `include/DevelopedFrame.h`, implemented in core over the
private `GeometryPlan`:

```cpp
/// @brief A position in the developed frame: the upright, straightened, cropped picture a
/// render shows, normalised per axis to the crop (0 to 1; outside is off the picture).
struct DevelopedPoint { double x = 0.0; double y = 0.0; };

/// @brief Rectangle of the developed frame, in the units of DevelopedPoint.
struct DevelopedRegion { double left = 0.0; double top = 0.0; double width = 1.0; double height = 1.0; };

/// @brief Placement of a photograph's corrected frame in its developed frame under a geometry.
///
/// An affine map both ways: turns, flips, straighten and crop compose to a similarity in
/// pixels (ADR 009, ADR 044 §4). Built from the full source's shape, never a reduced one.
class DevelopedFrameMap {
public:
    /// @throws std::invalid_argument as ::arraw::cropFrameFor does.
    DevelopedFrameMap(SourceShape source, const GeometrySettings& geometry);
    [[nodiscard]] DevelopedPoint developedFrom(CorrectedPoint point) const noexcept;
    [[nodiscard]] CorrectedPoint correctedFrom(DevelopedPoint point) const noexcept;
    /// @brief Gives the long-edge position of a corrected point: `(u W / L, v H / L)`.
    [[nodiscard]] std::array<double, 2> longEdgeFrom(CorrectedPoint point) const noexcept;
    /// @brief Gives the corrected point at a long-edge position.
    [[nodiscard]] CorrectedPoint correctedFromLongEdge(std::array<double, 2> position) const noexcept;
    /// @brief Gives the developed frame's width and height in source pixels (the continuous crop).
    [[nodiscard]] double width() const noexcept;
    [[nodiscard]] double height() const noexcept;
    /// @brief Gives one long-edge unit in source pixels: the source's long edge.
    [[nodiscard]] double longEdge() const noexcept;
    [[nodiscard]] const SourceShape& source() const noexcept;
};
```

Developed fractions are what `PhotoView` already speaks: the view's frame is
`croppedSize` of the full source, the picker sends fractions of it, and the
renderer places a region by them. A reduced preview changes nothing.

**App-core `MaskViewMapping`** composes a `DevelopedFrameMap`, the
`ViewTransform` and the device pixel ratio:
- `widgetFrom(CorrectedPoint)` / `correctedFrom(QPointF)`;
- `widgetFromLongEdge()`: the affine `QTransform` from the long-edge metric
  to the widget, used to draw an ellipse (the unit circle under the turned,
  scaled axes) and band lines, mirrored or not;
- `scale()`: logical pixels per long-edge unit, `zoom * longEdge / ratio`.

The window builds one from `shapeOf(*decoded)` and the session's geometry
whenever the state, the frame or the view changes (§6). `PhotoView` gains a
`transformChanged()` signal emitted from `adopt()` and `setFrameSize()` for
any change of zoom, centre, frame or view size, user or not, so the overlay
never draws with a stale transform. `refreshPanel` sets the frame size before
it hands the overlay the new state (today `requestRender` sets it, which a
render delay can hold back).

### 6. The overlay

**What.** The selected mask's weight `w` (its shape and Invert; not Enabled,
Opacity or the deltas, so a new mask with no deltas shows), painted red at
alpha `0.5 w`, under the handles. O and Show Overlay toggle it; off by
default; remembered for the application session, not saved. Only in the mask
mode.

**From core, sharing the maths.** New public function in
`include/DevelopedFrame.h`:

```cpp
/// @brief Weights of one mask over a grid laid on a region of the developed frame.
struct MaskCoverage {
    ImageSize size;                    ///< Grid size.
    std::vector<std::uint8_t> weights; ///< Row-major, `round(255 w)`.
};

/// @brief Evaluates a mask's weight at the centres of a grid over a region of the developed
/// frame, as development does (ADR 044 §4), with Invert; ignores Enabled, Opacity and deltas.
/// @throws std::invalid_argument if @p size is empty or above 4096 on a side, or the shape
/// cannot be normalised.
[[nodiscard]] MaskCoverage maskCoverage(const LocalAdjustment& adjustment,
                                        const DevelopedFrameMap& frame,
                                        DevelopedRegion region, ImageSize size);
```

Implementation: `resolveLinear` / `resolveRadial` in `LocalPlan.cpp` become
one internal `resolvedMask(const Mask&, bool invert, ImageSize)` declared in
`LocalPlan.h`, used by `localPlanFor` and by `maskCoverage`; each grid centre
goes developed → corrected → source pixels (`u W`, `v H`) and through the
existing inline `maskWeight`. Resolved against the full source size, so a
hard edge's one-pixel antialiasing is the export's (a preview at level k
differs only for feathers narrower than 2^k pixels).

**The grid.** The overlay asks for the region the widget covers (its corners
through `frameFromView`, so it may extend past the frame; the tint is clipped
to the frame when painted), at the widget's logical size halved, capped at
one megapixel; the image is painted scaled with smooth filtering. Recomputed
on a zero-delay single-shot timer when the selected mask's shape or Invert,
the geometry, the frame, the view or the widget size changes, or the overlay
is shown; not when only deltas, Opacity or other masks change. During a drag
it follows the session's current state, so the tint moves with the handle
before any render arrives.

**Performance bound.** One mask over at most 2^20 points: an affine map and
one `maskWeight` each, about 2 to 4 ms in a release build on one core. The
second task measures it (release and debug) with a timing span
`mask.coverage` and reports the numbers; above 8 ms in release the grid drops
to a quarter of the widget's logical size during a drag.

### 7. Gestures, history and selection

**Edits** (ADR 022), all through `EditSession`, all origin `Edit`:
- a handle drag: `begin` on the press, `update(withLocalShape(baseline, id,
  draggedShape(atPress, ...)))` per move (each step from the shape at the
  press, as `CropEditing` gestures do, so nothing accumulates), `commit` on
  release ("Move Linear 1"); a press that never moves commits nothing
  (`commit` of an unchanged edit leaves no step);
- a creation: §4 ("Add Radial 2");
- a slider or a list action: §1;
- Delete: one step ("Remove Linear 1").

**Cancelling.** A gesture in progress is cancelled (`session.cancel()`), as
the plan says, by:
- Esc;
- entering the crop mode, leaving the mask mode, arming the picker;
- leaving the photograph (`confirmLeavingPhoto`, `showPhoto`, `closePhoto`,
  a sidecar reload);
- losing the pointer: Qt Widgets has no event for a lost implicit grab, so
  the overlay treats as lost a `WindowDeactivate`, a `Hide`, a `FocusOut`
  not caused by its own mouse press, and a move whose `buttons()` no longer
  hold the left button.

**Undo, Redo and History clicks during a drag** follow ADR 022: the overlay
stops tracking without reporting (`abandonDrag()`), then the session's
`undo` commits the open edit and takes it back, leaving it to redo, as for a
slider drag.

**`guarded()`** additionally abandons a mask drag before it cancels the
edit, so a throw (which the clamps of §3 should make impossible) never leaves
the overlay tracking.

**Selection** is window state (`std::optional<LocalAdjustmentId>`, ADR 044
§1: view state stays out of the model).
- Set by: a list click, a pin click, a creation (the new id), Duplicate (the
  copy).
- Cleared by: Delete, a click on empty canvas, leaving the photograph.
- After every state change (`refreshPanel`: undo, redo, a History click, a
  sidecar reload, a paste), `reconciledSelection(state, selected)` keeps it
  when the id is still in the list and clears it otherwise. An id freed by
  undo and handed out again by a later add is a new mask, selected because it
  was just created, never because it was selected before.

### 8. Where the code goes

**Core (`include/`, `src/core/`).**
- `include/DevelopedFrame.h`, `src/core/DevelopedFrame.cpp`:
  `DevelopedPoint`, `DevelopedRegion`, `DevelopedFrameMap`, `MaskCoverage`,
  `maskCoverage` (§5, §6).
- `src/core/LocalPlan.{h,cpp}`: `resolvedMask` factored out; no output
  change.

**App-core (`src/app/`, testable without widgets).**
- `MaskPresentation.{h,cpp}`: `localPresentationOf`,
  `maskOpacityPresentation`, `maskDisplayName(state, id)` (translated "Linear
  2", own name otherwise), moved out of `HistoryModel.cpp`, which then uses
  them.
- `MaskEditing.{h,cpp}`:
  - `MaskViewMapping` (§5);
  - `MaskTool { None, Linear, Radial }`, `MaskHandle` (the handles of §3),
    `maskHandleReach`;
  - `handlePositions(const Mask&, const MaskViewMapping&)`,
    `pinPosition(const Mask&, mapping)`;
  - `handleAt(const Mask&, mapping, QPointF, reach)`,
    `pinAt(const DevelopState&, mapping, QPointF, reach)`;
  - `draggedShape(const Mask& atPress, MaskHandle, QPointF press, QPointF
    pointer, mapping, bool constrain)`, clamped so the edit rules never throw;
  - `createdShape(MaskTool, QPointF press, QPointF pointer, mapping, QSizeF
    widget)`, the click default included;
  - `linearMarks(const LinearMask&, mapping, QRectF bounds)` (the three lines,
    clipped) and `radialMarks(const RadialMask&, mapping)` (the
    unit-circle-to-widget transform and the inner distance);
  - `reconciledSelection(const DevelopState&, std::optional<LocalAdjustmentId>)`.
- `MasksModel.{h,cpp}`: a `QAbstractListModel` over a state's list. Roles:
  display (displayed name), edit (own name), check state (Enabled), `IdRole`,
  `KindRole`, `InvertedRole`. `setState` emits `dataChanged` when the ids and
  their order are unchanged (keeping the view's selection and an open rename
  editor) and resets otherwise. `setData` changes nothing itself: it emits
  `renameRequested(id, name)` and `enabledRequested(id, bool)`.

**Ui (`src/app/ui/`).**
- `MaskOverlay.{h,cpp}`: transparent child of `PhotoView` covering it (no
  autofill, not opaque), mouse tracking, strong focus. Holds a snapshot of the
  state, the selection, the armed tool, the overlay flag and the coverage
  image; paints tint, pins and handles; maps events through `MaskEditing`.
  Holds no edit; reports with the panel's protocol: `editStarted()`,
  `stateEdited(DevelopState)`, `editFinished()`, `editCancelled()`, plus
  `maskSelected(std::optional<LocalAdjustmentId>)`, `toolChanged(MaskTool)`,
  `overlayToggled(bool)`, `leaveRequested()`. Slots: `setScene(state,
  SourceShape)`, `setSelection`, `setTool`, `setOverlayShown`,
  `abandonDrag()`, `cancelGesture()`.
- `MasksPanel.{h,cpp}`: the group of §1, owned by `DevelopPanel`.
- `PhotoView`: owns the `MaskOverlay`; `setMaskMode(bool)`, `isMaskMode()`,
  `maskOverlay()`, `panBy(QPointF)`, `transformChanged()`.
- `SettingSlider`: the presentation constructor (§1).
- `DevelopPanel`: hosts `MasksPanel`; `setSelectedMask`, `maskSelected`,
  `maskToolChosen(MaskTool)`, `overlayToggled(bool)`; label column covers
  the masks rows.
- `MainWindow`:
  - the three panel lambdas (`editStarted`, `stateEdited`, `editFinished`)
    become private slots shared by the panel and the overlay, plus
    `cancelEdit()`;
  - `maskAction_` (M), the mask shortcuts (Esc, O, Delete, Backspace),
    `setMaskMode(bool)`, `leaveMaskMode()`, `selectMask(optional id)`,
    `syncMasks()` (scene, selection and mapping to the overlay and panel;
    called from `refreshPanel`);
  - exclusivity in `setCropMode`, `setStraightening`, `setPicking`; cleanup in
    `guarded`, `showPhoto`, `closePhoto`, `confirmLeavingPhoto`,
    `sidecarChangedOnDisk`; `updateEditingActions` enables M.

About two hundred lines in `MainWindow`. **Alternative**: a
`MaskModeController` QObject in ui owning the action, the shortcuts, the
selection, the tool and `syncMasks`, calling back into the window for the
session; worth it if the crop wiring moves out the same way later. The
recommendation keeps one session hub (`guarded`, `refreshPanel`) for now.

## Tests

**Core** (`tests/test_DevelopedFrame.cpp`, in `arraw-tests`):
- `DevelopedFrameMap` round trips to 1e-9 for every camera orientation ×
  quarter-turn × flips × straighten {0, 7.5, -30} × automatic and explicit
  crops;
- known points: identity; a camera orientation of 90°; a horizontal flip
  (`u → 1 - u`); an explicit crop's corner maps to (0, 0);
- agreement with `GeometryPlan::toSource` (private header, allowed in tests)
  and with the picker's mapping;
- the long-edge metric is preserved up to one scale under straighten and
  turns (distances and angles between three points);
- `maskCoverage` with an identity geometry and a grid the source's size
  equals `round(255 maskWeight)` of the `localPlanFor` mask exactly; Invert;
  a disabled mask and a mask without deltas are still covered; a region past
  the frame; a radial mask outside the frame; the size limits throw;
- **the overlay follows the render**: a flat grey source with one linear
  mask (Exposure +2) under turn, flip, straighten and crop: where coverage is
  at least 0.9 the CPU render is brighter than the render without the mask,
  and where it is at most 0.01 the two are equal bit for bit;
- the hidden digests (CPU and GPU) unchanged.

**App-core** (`arraw-tests`):
- `test_MaskEditing.cpp`: `MaskViewMapping` round trips at zoom 0.25, fit and
  4, panned, at pixel ratios 1 and 2; a linear mask's handles under a 90° turn
  sit where the turned picture puts them; each handle is hit within 10
  logical pixels at every zoom and missed beyond; each drag of §3 gives the
  expected shape; Shift constraints; a rotation drag clockwise on screen turns
  the ellipse clockwise on screen with and without a flip; random drags
  (collapsing ends, radii to zero, far outside) always pass
  `withLocalShape`; `createdShape` by drag and by click;
  `reconciledSelection`.
- `test_MasksModel.cpp`: rows and roles from a state; default names "Linear
  1", "Radial 1", "Linear 2"; `setData` emits requests and changes nothing;
  same ids give `dataChanged`, a reorder or add gives a reset; 16 masks.
- `test_SettingPresentation.cpp`: every local key has a presentation with
  the local range's labels, units and decimals; the opacity presentation.
- `test_HistoryModel.cpp` passes unchanged after the move of `maskLabel`.

**Widgets** (`arraw-widget-tests`, offscreen):
- `test_MaskOverlay.cpp` (like `test_CropOverlay.cpp`, events sent to the
  overlay over a `PhotoView` with a frame): a handle drag emits begin, one
  `stateEdited` per move with the expected shape, and finish; an armed drag
  creates; a click creates the default; Esc mid-drag and a move without the
  button emit `editCancelled`; `WindowDeactivate` and hiding cancel; an empty
  canvas drag pans the view and emits no edit; a click there clears the
  selection; Space with a drag pans; the wheel zooms the view; a pin click
  selects; O toggles the tint and the coverage image is made; a zoom
  recomputes the coverage region.
- `test_MasksPanel.cpp` (or in `test_DevelopPanel.cpp`): the hint without a
  selection, the fourteen rows with one; labels and ranges; a slider edit
  emits `stateEdited` of `withLocalDelta`; rename, enable, invert, duplicate,
  delete and move each emit one complete edit; Move Up and Move Down disabled
  at the ends; Linear, Radial and Duplicate disabled at 16 with the tool tip;
  grayscale hides Saturation and Vibrance; the crop mode disables the group.
- `test_MainWindowMasks.cpp`, on a generated 640×480 PNG in a temporary
  folder:
  - M enters the mode (action checked, overlay shown and focused); M leaves;
  - C from the mask mode enters the crop mode and leaves the mask mode; M
    from the crop mode commits the crop ("Crop" step) and enters the mask
    mode;
  - Linear, then a drag on the overlay: one step "Add Linear 1", the mask
    selected, the render changes;
  - dragging its `to` dot through synthesized mouse events: one step "Move
    Linear 1";
  - the Exposure row: "Linear 1: Exposure +0.50";
  - Undo keeps the selection (the id exists); Undo past the creation clears
    it and the panel shows the hint; Redo brings the mask back unselected;
  - Delete removes it in one step; at 16 masks Linear is disabled;
  - Esc order: cancels a drag (no step), then disarms, then leaves;
  - stepping to the next photograph mid-drag leaves no step;
  - the History dock and the zoom stay enabled in the mode; O toggles the
    tint.
- **Screenshots** (hidden `[.screenshots]`, written only when
  `ARRAW_SCREENSHOT_DIR` names a folder, as the digest's `ARRAW_DIGEST_OUT`
  does): `QWidget::grab()` of the window to PNG, on a generated 900×600
  photograph (gradient sky, a grey card, a few shapes):
  `masks-linear-selected.png`, `masks-radial-turned-straightened.png` (an
  ellipse on a photograph turned, flipped and straightened 7.5°),
  `masks-overlay-radial-inverted.png`, `masks-panel-none.png`,
  `masks-panel-selected.png`, `masks-panel-full.png` (16 masks, Add
  disabled), `masks-pins.png` (three masks, one selected). The task runs them
  into the scratchpad and lists the files for the reviewer.

## Tasks

Each task builds green on its own and ends with `just format`, `just
format-check`, the full `just test` and `just py-test` (counts reported), and
the hidden digests run before the first edit and after the last, CPU and GPU,
with the existing lines identical:

```sh
ARRAW_DIGEST_OUT=$SCRATCH/digest-cpu-before.txt build/${ARRAW_BUILD_PREFIX}debug/tests/arraw-tests "[.digest]"
ARRAW_DIGEST_OUT=$SCRATCH/digest-gpu-before.txt build/${ARRAW_BUILD_PREFIX}debug/tests/arraw-gpu-tests "[.digest]"
```

No commit, stage, stash, reset or checkout; `docs/reviews/` and `.idea/`
untouched; no private core header in `src/app` or `src/cli`.

**Task 1: core API and app-core.**
1. Digests before.
2. `src/core/LocalPlan.{h,cpp}`: factor `resolvedMask(const Mask&, bool
   invert, ImageSize)` out of `resolveLinear` / `resolveRadial`;
   `localPlanFor` calls it. No output change.
3. `include/DevelopedFrame.h`, `src/core/DevelopedFrame.cpp` (added to the
   `arraw` target): `DevelopedPoint`, `DevelopedRegion`, `DevelopedFrameMap`,
   `MaskCoverage`, `maskCoverage`, as §5 and §6, Doxygen per CLAUDE.md.
4. `tests/test_DevelopedFrame.cpp`: the core tests above, the render
   agreement test included.
5. `src/app/MaskPresentation.{h,cpp}`: `localPresentationOf`,
   `maskOpacityPresentation`, `maskDisplayName`; `HistoryModel.cpp` uses them
   (wording unchanged).
6. `src/app/MaskEditing.{h,cpp}`: everything listed in §8, clamped so no
   drag reaches an edit rule's throw.
7. `src/app/MasksModel.{h,cpp}`.
8. Add the new files to `arraw-app-core` and the tests to `arraw-tests`;
   app-core tests as listed (`test_MaskEditing.cpp`, `test_MasksModel.cpp`,
   the presentation cases in `test_SettingPresentation.cpp`).
9. Measure `maskCoverage` over a 1024×1024 grid in a release build (best of
   five) and report it.
10. Digests after; format; full tests; report counts, the timing and the
    files changed.

**Task 2: ui, window wiring and tests.**
1. Digests before.
2. `SettingSlider`: the constructor from an id, range, default and
   presentation; the key constructor delegates; existing tests unchanged.
3. `src/app/ui/MasksPanel.{h,cpp}` and its place in `DevelopPanel` (below
   Crop, in `nonGeometryGroups_`, shared label column, `finishPendingEdit`,
   `setSelectedMask`, `maskSelected`, `maskToolChosen`, `overlayToggled`).
4. `src/app/ui/MaskOverlay.{h,cpp}` (§2, §3, §4, §6, §7).
5. `PhotoView`: the overlay child, `setMaskMode`, `isMaskMode`,
   `maskOverlay`, `panBy`, `transformChanged`; the crop and mask overlays
   never shown together.
6. `MainWindow`: shared edit slots (and `cancelEdit`), Photo > Masks (M),
   the mask shortcuts, `setMaskMode`, `leaveMaskMode`, `selectMask`,
   `syncMasks` from `refreshPanel` (frame size set first), exclusivity with
   the crop mode and the picker, cancellation in `guarded`, `showPhoto`,
   `closePhoto`, `confirmLeavingPhoto`, `sidecarChangedOnDisk`; undo, redo and
   History clicks abandon a drag first.
7. Fix the stale key comments found above (`CullingActions.h`,
   `PhotoView.cpp`, `CropOverlay.cpp`).
8. Widget tests: `test_MaskOverlay.cpp`, `test_MasksPanel.cpp` (or cases in
   `test_DevelopPanel.cpp`), `test_MainWindowMasks.cpp`, added to
   `arraw-widget-tests`.
9. The hidden `[.screenshots]` case; run it with `ARRAW_SCREENSHOT_DIR` in the
   scratchpad and list the PNGs.
10. Measure slider and drag response with 16 masks (plan step 3): a release
    build, `QT_LOGGING_RULES="arraw.timing.debug=true"`, a 24 MP photograph
    with eight linear and eight radial masks; report `window.panel`,
    `mask.coverage` and render times for a handle drag and an Exposure drag,
    CPU and GPU.
11. A dated note at the end of ADR 044 recording the GUI as built (mode, keys,
    creation, the new public API) and where it departs from §10 (no
    `CropModeController`; Space pans in the mask mode only).
12. Digests after; format; full tests; report counts, timings, screenshot
    paths and the files changed.

## Open questions

1. The Masks group in the develop dock, or a Masks dock tabbed with
   Develop?
2. Should selecting a mask in the list enter the mask mode (proposed), or
   only Add and M?
3. Overlay off by default (proposed), on by default, or shown automatically
   while a handle is dragged?
4. After Delete: no selection (proposed), or the next mask?
5. A dated note in ADR 044 (proposed), or an ADR of its own for the mask
   mode, as ADR 040 is for the crop mode?
6. Should Space pan in the whole photo view, not only in the mask mode? It
   costs nothing elsewhere, and the plan already says "pan stays on Space".
