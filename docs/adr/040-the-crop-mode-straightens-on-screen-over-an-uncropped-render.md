# The crop mode straightens on screen, over an uncropped render

The [crop plan](../ideas/crop-ui-plan.md) puts ADR 014's geometry in the
window as a Lightroom-like crop mode, with its decisions table settled by the
user. This ADR records how the mode is split, what it renders and how a
rotation stays responsive, how its gestures map to the geometry, and how it
meets the edit session.

## Decision

**The rules are a model without Qt; the overlay paints and routes input.**
`src/app/CropEditing.h` (in `arraw-app-core`) holds `CropEditing`, which
takes the decoded size, the camera orientation and a `GeometrySettings`, and
gives the geometry a gesture or a command leads to, as `CurveEditing` does for
the curve editor (ADR 036). Every operation keeps an explicit crop well
formed, in agreement with a locked aspect to ADR 014's 1e-6, and inside valid
rotated content; a fuzz test checks that over thousands of random operations
on several sizes and camera orientations. `src/app/ui/CropOverlay` is a
painted `QWidget` (Qt Widgets, no QML), a child of `PhotoView` that covers it
in the mode.
- (a) **A model and a thin overlay (chosen).** The constraints are tested
  without widgets; the widget tests check hit testing, routing and the edit.
- (b) Everything in `PhotoView`. Its zoomed, cropped-frame `ViewTransform`
  has nothing in common with the mode's rotated uncropped frame.

**The valid-content test is the engine's.** `GeometryPlan.h` gains
`UprightBox`, `GeometryPlan::crop()`, `contentCorners`, `isInsideContent`,
`fittedToContent` (the explicit-crop fit `geometryPlanFor` already did, now
shared), `shiftedIntoContent` (the nearest feasible centre at a kept size)
and `automaticCrop`. They are engine-private: `CropEditing.cpp` sees
`src/core`, as `CurveEditing.cpp` does, and `CropEditing.h` names no
engine-private type. Containment is the source-axis extent of the box about
its centre, which is exact for a rotated rectangle of content (ADR 014).

**What the mode renders: the photograph turned and flipped, with no
straighten, no crop and no effects.** The overlay rotates that image on screen
by the displayed angle, about the upright centre, and draws the crop frame,
the dimming, the handles and the guide over it.
- Straighten comes before the flips (ADR 014), so `R(θ)` conjugated by the
  flips is `R(±θ)`: the upright frame is that render rotated by
  `displayedStraighten` (negated after one flip). The render's size is the
  upright frame at zero straighten.
- The render's geometry stays the same through the session until a
  quarter-turn or a flip, so the preview's checkpoints (ADR 024, ADR 037)
  serve every crop edit: entering costs one geometry pass, a crop edit or a
  rotation costs no render at all, and `MainWindow` sends nothing when the
  uncropped state and the size it would request are the ones it last sent. A
  turn or a flip costs one geometry pass; until it lands, the overlay turns
  or mirrors the image it has (`reorientedImage`, exact for quarter-turns
  and mirrors), and a render that was on its way is turned to the present
  geometry on arrival, since `MainWindow` remembers the state of each mode
  request. Leaving costs one geometry pass from the
  pointwise checkpoint. A change of size (window, or a scale step) resumes
  from the geometry checkpoint.
- Effects are off in this render: the vignette and the grain follow the crop
  (ADR 037, ADR 038), which is what is being chosen. They return when the
  mode ends.
- The request is the whole frame at `CropOverlay::renderSize()`: the frame at
  the scale shown, rounded up to steps of an eighth of an octave (at most 9%
  wider than shown) and never above the photograph's pixels, so small changes
  of scale ask for nothing. Steps of √2, the first choice, left the image up
  to 1.41 times wider than shown, which doubled the paint time of a rotation
  frame (see the measurement below).
- (a) **Rotate a straighten-free render on screen (chosen).** A rotation drag
  never waits for a render, and there is no render per step to coalesce. The
  cost is that the mode shows a bilinear screen-resolution resample
  (QPainter's smooth transform) rather than the engine's own geometry pass,
  slightly softer; outside the mode the engine's render is shown as always.
- (b) Render the uncropped upright bounding box in the engine, with
  transparent wedges. Exact, but it needs a request option and an outside-of-
  content alpha in the CPU and GPU geometry passes, and every rotation step is
  a geometry pass.
- (c) (b) for the result, with (a) while a new render is on its way. The best
  of both, at the cost of both; worth revisiting if (a) looks soft on a large
  screen.

**The mode opens on the photograph, never on an empty frame.** What the
mode renders (uncropped, unstraightened) is not what the view showed, so the
first render of the mode takes a geometry pass. Until it lands the overlay
shows a placeholder, chosen in this order:
- The mode's own last render of this photograph, if the uncropped state is
  unchanged since (`OpenPhoto::lastCropImage`): exact, so leaving and
  entering again is instant.
- Otherwise two layers. Beneath, the camera's embedded preview (from the
  thumbnail cache, else read from the file, once per photograph), turned and
  flipped to the geometry: uncropped and immediate, but the camera's look and
  soft. Inside the crop frame, the view's whole-frame image
  (`PhotoView::wholeFrameImage`), which is the developed, straightened,
  cropped frame: exactly what the frame holds, upright on screen as the
  frame is. The outside is dimmed, so the camera look shows little there.
  The inner layer is dropped at the first change of geometry; both at the
  first render.
- A file with no embedded preview (a PNG) shows the inner layer on a flat
  ground.
- Rejected: the film strip's thumbnail (it is cropped, by the saved
  settings); a reduced-level render first (still a render to wait for); the
  decoded pixels (linear, undeveloped); an uncropped render kept up to date
  in the background (a second request stream in the renderer for one
  frame's sake).

**The mode always fits; the view holds still during a move or a rotation.**
The upright frame is fitted with a 24 px margin; the wheel and the zoom
actions do nothing in the mode, and the zoom of the photo view beneath is kept
for when it ends.
- Dragging inside moves the image under a fixed frame: the placement keeps the
  frame where it was on screen and the image follows the pointer, stopping
  where content ends.
- Dragging outside rotates about the frame's centre: the placement keeps that
  centre where it was, at the fitted scale, so the image turns about it and
  the frame visibly shrinks as content requires.
- After either, the view eases back to the fitted placement in 160 ms. Handle
  drags and drawn lines use the fitted placement throughout.

**Gestures.**
- **Handles**, drawn as Lightroom draws them: corner brackets with 16 px
  arms and 16 px edge bars, 4 px thick, on a thin frame line one logical
  pixel wide, all snapped to device pixels so they stay sharp at any pixel
  ratio, over the outside dimmed by 60 %. A corner is grabbed along both
  arms of its bracket and up to 10 px around them, before any edge; an edge
  anywhere along its length within 10 px, so every hit area covers what is
  drawn (a test checks it). Brackets and bars shrink on a small frame. The
  handle keeps its offset from the pointer, so grabbing it off centre does
  not make it jump. They resize with the opposite corner or edge fixed. A
  free corner is the pointer projected onto the intersection of half-planes
  that keeps the three moving corners inside content and each side at least
  `minimumCropFraction` (2 %) of the photograph's shorter side: exact for
  convex content, and it slides along an edge rather than sticking. A free
  edge, a locked corner (the pointer projected onto the ratio's diagonal) and
  a locked edge (the neighbours grow about the middle of the opposite edge)
  have one parameter, clamped by where each moving corner's ray leaves the
  content. A crop never turns inside out.
- **Inside** moves the crop the opposite way, projected to the nearest centre
  that fits in source axes (an orthogonal map keeps distances).
- **Outside** rotates by the pointer's turn about the frame's centre, clamped
  to ±45°.
- **Ctrl with a drag, or the straighten tool,** draws a line; on release the
  photograph rotates so that the line becomes horizontal or vertical,
  whichever is nearer. The tool disarms after one line.
- Every step of a handle drag or a move is worked out from the geometry as the
  gesture began, so moving back restores. A grid of thirds, a 6 × 6 grid, the
  golden ratio or diagonals (O cycles) shows while a handle or the image is
  dragged, and a finer 10 × 10 grid while rotating; the angle shows above the
  frame.
- Cursors: diagonal and straight resize cursors on handles, an open or closed
  hand inside, a drawn curved arrow outside, a cross while drawing a line.

**Rotation turns about the crop's centre, not the image's.** The decisions
table says "rotates about the frame's centre". ADR 014 instead carried the
crop's offset from the image centre. `CropEditing` keeps the content under the
crop's centre there (mapping it through the old frame into the source and back
through the new), keeps the crop's size, and then fits as ADR 014 does:
shrink about that centre keeping the aspect, and move only when nothing fits
there. For a centred crop the two rules agree. The same rule serves a slider
or a typed angle (`adopt`). Every rotation in a run works out from the
geometry before the run, so turning back undoes the shrinking, as ADR 014
asks of intermediate drag updates; any other operation ends the run. An
automatic crop stays automatic. The core resolver is unchanged: it fits a
stored rectangle, it does not move one.

**Commands.**
- **Aspect.** Free keeps the rectangle. A ratio, or Original, makes the largest
  crop of that ratio inside the present explicit crop, about its centre; an
  automatic crop stays automatic and resolves at the new ratio. Lock takes the
  present ratio, computed from the stored edges exactly as the resolver checks
  it.
- **Swap (X)** reciprocates a locked ratio and swaps the crop's sides about its
  centre, shrinking about it if they no longer fit. A swapped Original
  aspect is a plain ratio, so the menu no longer says Original; swapping again
  gives the photograph's own ratio back, and with it the Original aspect.
- **Quarter-turns and flips** compose in displayed axes: a turn swaps the two
  flips (`R90 · diag(a, b) = diag(b, a) · R90`) and steps the enum; the
  rectangle is remapped by ADR 014's swaps and complements, and a custom ratio
  reciprocated. The stored straighten is untouched; after a flip it appears
  reversed, which is the mirror image of the same tilt.
- **Reset** (the Crop group's button) returns the whole geometry to its
  defaults: turns, flips, angle and crop. `CropEditing::resetCrop`, the crop
  alone with the aspect kept, has no button yet.

**The whole session is one edit (ADR 022), and undo walks its gestures.**
`MainWindow` finishes a pending panel edit and opens an edit on entering,
updates it on each `geometryEdited`, and commits it on Enter, R, a
double-click inside or the Crop action, or cancels it on Esc, which restores
the state the mode began with, whatever was undone or redone.
- Inside the session, as in Lightroom, Undo and Redo (Ctrl+Z, Ctrl+Shift+Z or
  Ctrl+Y, the window's actions) step through the session's gestures:
  `CropEditing` keeps a history of steps, one per drag (press to release),
  command (turn, flip, aspect, lock, swap with X or its button), or panel edit
  (a slider drag, a typed angle, Reset). Steps nest, so a command inside a
  slider's step joins it, and a step that changed nothing is not kept. Undo
  never goes past the session's start: there it does nothing, rather than
  leaving the mode or reaching into the photograph's history, and the Undo
  action is disabled. Leaving still records one step in the photograph's
  history.
  - (a) **A history of gestures in the model (chosen).** Lightroom's
    behaviour; tested without widgets.
  - (b) Disable Undo in the mode; (c) Undo as Esc. Simpler, but Ctrl+Z is
    what a user reaches for after a bad drag.
- The other develop groups, and the Treatment row, are disabled in the
  mode: a panel edit there would join the crop's one edit and be thrown away
  by Esc. Lightroom keeps them live; reopening them would need the session
  to commit and reopen around each such edit. The Crop group stays: its
  edits go through `CropOverlay::adopt` (a straighten slider therefore
  shrinks the crop live), with the panel's `editStarted` and `editFinished`
  opening and closing a step of the session. Outside the mode a panel
  geometry edit goes through a `CropEditing` made at its first change, so
  the same rules apply.
- `MainWindow::editGeometry(command)` applies a `CropEditing` command (turn,
  flip, aspect, reset) into the session's edit in the mode, or as one step of
  its own outside it; `setStraightening` arms the tool, entering the mode.
  These are what the Crop group and the menus call.
- Save, leaving the photograph (Left and Right included, as in Lightroom:
  the crop is kept, the next photograph opens outside the mode) and a
  sidecar changing on disk first leave the mode, keeping the crop. The white balance picker is
  disarmed, and cannot be armed, in the mode.
- Renders of the mode are the overlay's only from the first request the mode
  made; on leaving, those still on their way are dropped. They never feed the
  film strip's live thumbnail, the curve histogram or the photo view's
  background.

**Keys and focus.** R toggles the mode (Photo > Crop & Straighten), only
with a photograph open. In the mode Enter accepts, Esc disarms the
straighten tool or cancels, O cycles the guide and X swaps orientation,
wherever the focus is in the window:
- The overlay claims those keys through `ShortcutOverride`, as a spin box
  does, when it has the focus, and it keeps it: it is the photo view's focus
  proxy in the mode, so whatever gives the view the focus back (Enter or Esc
  in a spin box) gives it to the overlay; the Crop group's buttons and menu
  take no focus; nor does the panel's scroll area, which a click on a
  focusless button would otherwise focus. Leaving the mode gives the view the
  focus before the overlay hides, so it does not pass to a spin box.
- Window shortcuts for the same keys are enabled only in the mode, for
  focus elsewhere (the film strip). Reject is disabled in the mode, so X has
  one meaning; the picker's Esc is off, since picking is.
- A spin box claims Enter and Esc while typing, so typing an angle and
  pressing Enter ends the typing, not the mode.
- R was the red label's key. All five labels move to Shift with the colour's
  initial (Shift+R, Y, G, B, P), alike, rather than red alone.
- Left and Right keep stepping between photographs: the crop is committed,
  as in Lightroom, and the next photograph opens outside the mode.

**The develop dock fits its panel.** The dock opens 21 lines of the panel's
font wide (`DevelopPanel::defaultDockWidth`, about 360 px at a 17 px line),
and never narrows below the panel's minimum and a vertical scroll bar, so the
panel never scrolls sideways. Rows of several buttons (the Crop group's
tools, aspect lock and swap, the tone curve's channels) are tool buttons, as
wide as their text, rather than push buttons, whose style minimum of about
80 px made those rows the panel's widest. A test checks that the panel's
minimum and a scroll bar fit the default width.
- (a) **Compact button rows (chosen).** The label column stays one width,
  so the grooves line up, and nothing is elided.
- (b) Cap the label column and elide long labels with a tooltip. Not needed
  once the button rows shrink; the longest label sets the column today.
- (c) Widen the dock. Costs the photograph room, and the rows would still
  set the minimum.

**Painting a rotation frame, measured.** A throwaway test (offscreen platform,
debug build, software raster, a 3840x2160 widget at 1x and a 1920x1080 one at
2x, which are the same device pixels; a 6000x4000 photograph with a centred
crop; 30 mouse moves of a rotation drag; time of `QWidget::render`, which runs
`paintEvent`) gave these medians. The host was noisy, so the figures were
repeated and only comparisons made in one process are trusted.
- Before: the render at 4243x2829 as a `QImage` (RGBA8888), smooth transform.
  27 ms in the first run, 41 to 62 ms in later ones. Drawing the image alone
  in a separate loop: 19 to 28 ms.
- Cause, by elimination: the draw of the rotated image, not the dimming
  (2 ms), the background (2.4 ms) or the guides. It also depends on the
  source's width against what is shown (3168 px here): a source up to about
  3500 px drew in 15 to 19 ms, 3650 px in 34 ms and 4243 px in 42 ms.
- After: the render at 3272x2182 (step of an eighth of an octave), an
  RGB32 `QPixmap` made once per image, no smooth transform while a rotation
  is dragged. The image draws in 6 to 7 ms, a whole frame in 18 ms at both
  pixel ratios (the rest is the fixed cost of the 8 MP surface). Smooth
  resampling at the end of the drag costs 15 ms for the image, once.
- Of the three changes, the pixmap and the rounding each helped; the draw hint
  gave 10 to 20% (non-smooth 42 ms against smooth 48 to 49 ms with the pixmap
  at the old size). Antialiasing the image edge costs 4 ms and is kept.
- Not measured: a real window with a GPU-composited backing store, and a
  release build. A frame of 18 ms on a 4K surface is near, not under, 16 ms;
  if a rotation proves to stutter on real hardware, the next step is to draw
  the drag from a half-size pixmap.
- After review (2026-10-06): those 18 ms still included converting the image
  to a pixmap on every mouse move, because each edit dropped the pixmap. Now
  only a turn or a flip, which changes the image, drops it. Not measured
  again.

**A failed geometry change leaves the mode.** If handing a geometry to the
session throws, `MainWindow::guarded` closes the overlay before it cancels the
session's edit, so what the user sees is never ahead of the session.

## Consequences

- Nothing in the engine's render path changed; `geometryPlanFor` now calls the
  shared fit, with identical results.
- The mode's preview is a screen-resolution resample, slightly softer than the
  engine's; option (c) above is the way to sharpen it if needed.
- Lightroom resizes a crop into empty corners and snaps it back on release
  with an animation; here the crop simply stops at content, which is ADR 014's
  contract at every step.
- The colour labels need Shift, unlike main's film strip.
- Open: an aspect chosen for an explicit crop shrinks
  into it, so switching ratios back and forth ratchets smaller (the
  alternative, the largest crop of the ratio in the content near the present
  centre, does not, but discards a tight framing).
