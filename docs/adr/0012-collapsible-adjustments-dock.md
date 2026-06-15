# Adjustments dock collapses to a right-edge strip (Film Strip stays full-hide)

The right Adjustments dock collapses to a thin, always-visible reveal strip at
the window's right edge: collapsing `hide()`s the `QDockWidget` and `show()`s a
vertical `QToolBar` in `Qt::RightToolBarArea` carrying a single expand-chevron;
expanding reverses it. A chevron in the dock's title bar collapses it, the
toolbar chevron re-opens it, and `F8` (View menu) toggles. The bottom Film Strip
keeps its **existing** native full-hide (`toggleViewAction`, `F9`) — it does not
get a reveal strip. The two side panels deliberately hide differently.

## Context

Goal: let the user reclaim screen space for the image without losing track of
where the panel went. `QDockWidget` natively offers only full-hide and float —
there is no built-in "collapse to a thin edge with a reveal handle." So a strip
has to be built.

The Film Strip already had `F9` = native full-hide before this work. Giving the
Adjustments dock a reveal strip means the app now has two "make it go away"
gestures. Rather than retrofit the same mechanism onto both, we kept the cheap
existing behavior on the Film Strip and added the richer one only where it earns
its keep — the Adjustments dock, which is wide and worth reclaiming, and whose
strip sits naturally at a vertical edge.

## Considered options

- **Native full-hide everywhere (rejected).** Simplest, fully native, but the
  panel vanishes with nothing left at the edge — undiscoverable without knowing
  the shortcut.
- **Hover auto-hide, Lightroom-style (rejected).** Thin strip that slides the
  panel out on hover and re-collapses on leave, with a pin. Closest to
  Lightroom, but the most state and the most custom code.
- **`QSplitter` pane (rejected).** Drop `QDockWidget` for Adjustments and use a
  splitter with a custom collapsible pane. Full control, but loses float/move and
  is the biggest rewrite.
- **Hide dock + right-area `QToolBar` strip (chosen).** Persistent, discoverable
  chevron at the edge; no hover magic; keeps the dock floatable/movable; reuses
  Qt's own toolbar placement and — importantly — its state serialization.

## Consequences

- The two side panels hide via different mechanisms by design (Film Strip:
  full-hide/`F9`; Adjustments: collapse-to-strip/`F8`). A future reader must not
  "unify" them without re-deciding this trade-off.
- Collapse state persists for free: `saveState()`/`restoreState()` already
  serialize both `QDockWidget` and `QToolBar` visibility. First-run default is
  set in the constructor (dock shown, toolbar hidden); a restored state wins.
- The reveal strip is always anchored at the right edge. If the user floats or
  re-docks the Adjustments dock elsewhere, collapsing still shows the right-edge
  strip and expanding restores the dock to its last position — the floating case
  is left intentionally loose rather than special-cased.
- `F8` toggles the dock; `Tab` is deliberately **not** used, because the
  Adjustments panel is full of sliders/spin-boxes where `Tab` is field focus
  traversal.
- This is a UI/window concern only — no new domain term, so `CONTEXT.md` is
  unchanged.
