# Crop and rotation in the window — plan

Status: accepted, 2026-10-05. The engine already holds everything this needs:
quarter-turns, flips, straighten (±45°), an upright crop rectangle and an aspect
constraint (`include/GeometrySettings.h`, ADR 014). This plan is the user
interface for them, and it should feel like Lightroom's Crop tool.

## Decisions (user, 2026-10-05)

| Question | Answer |
|---|---|
| How cropping is entered | A crop **mode**: the R key or the Crop group's button. Enter, R or a double-click inside commits; Esc cancels |
| What the mode shows | The whole uncropped, straightened photograph, the outside of the crop dimmed, the crop frame with eight handles |
| Undo | A whole crop session is one undo step (ADR 022: begun on entering, committed on leaving, dropped on Esc) |
| Corners and edges | Resize; with the aspect locked, the ratio is kept, anchored at the opposite corner or edge |
| Drag inside the frame | Moves the **image** under a fixed frame, as Lightroom does |
| Drag outside the frame | Rotates (straighten) about the frame's centre |
| Staying inside the photograph | The crop always shrinks to stay inside rotated content (ADR 014's contract); it never shows empty corners |
| Straighten tool | Draw a line along a horizon or a vertical; the angle comes from it. Ctrl-drag in the mode does the same |
| Overlays | O cycles thirds, grid, golden ratio, diagonals; shown while dragging (a finer grid while rotating) |
| Aspect | Presets menu: Original, 1:1, 4:5, 5:7, 2:3, 16:9, Custom…, Free; a lock; X swaps portrait and landscape |
| Quarter-turns and flips | Ctrl+[ and Ctrl+] rotate; flips in the Photo menu; buttons in the Crop group beside the angle slider |
| Out of scope | Auto straighten (horizon detection) |

## Shape

- **Crop group in the develop panel**: the mode button, the aspect menu and lock,
  the angle slider (straighten), the straighten tool button, rotate and flip
  buttons, Reset. The six geometry keys leave `notShownYet`.
- **Interaction model, separate from painting** (as `CurveEditing` is for the
  curve editor): a Qt-free class that takes the geometry, the photograph's
  upright size and a drag, and gives the new geometry. All the rules live there
  and are unit tested: aspect lock, the inside-content constraint, image move
  under the frame, rotation about the frame's centre, the straighten line.
- **Overlay on the photo view**: in the mode, the view renders the uncropped,
  straightened frame (a render request whose geometry has no crop) and paints
  the crop frame, dimming, handles and overlay on top. Cursors change over
  handles, inside and outside.
- **Preview**: during a drag the view shows a fast render; a rotation drag may
  rotate the last render on the GPU or the widget while the next render
  arrives — decided in the ADR.

## Steps

One workflow: Opus for the interaction model, the mode and its rendering
(architecture), Sonnet for the Crop group, the menus and shortcuts, then an Opus
review with offscreen screenshots. Agents never commit; the reviewed result is
committed on its own. An ADR records the decisions that last.
