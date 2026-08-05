# Zoom presets come from one shared list, with the preset match extracted as pure logic

The View menu carried a single flat `Reset Zoom` action (Ctrl+0) while the status
bar's zoom button carried its own, separately maintained dropdown (`Fit`, `50 %`,
`100 %`, `200 %`). The two had **already drifted** — different labels for the same
action, different preset sets — and nothing structural stopped them drifting
further. Two hand-rolled action lists for one concept is the duplication
[[spot-for-algorithms]] exists to prevent, in its cheapest and most typical form:
not an algorithm, just a table someone has to remember to update twice.

## What we decided

**One ordered preset list, consumed by both menus.** `kZoomPresets = {0.25, 0.5,
1.0, 2.0, 4.0}` plus a special `Fit` (which calls `resetView()` rather than
carrying a fixed value). A private `MainWindow` helper,
`addZoomPresetActions(QMenu*, QActionGroup* = nullptr)`, appends the entries to any
menu; the View > Zoom submenu and the status bar dropdown are two callers of it
instead of two literal lists. Retiring the `Reset Zoom` label in favour of `Fit`
everywhere is part of the same move — one label per action.

The presets form an **exact doubling chain**, so Zoom In / Zoom Out (bound to
`QKeySequence::ZoomIn`/`ZoomOut`, multiplying and dividing by 2) always land back
on a preset when starting from one.

**The one piece of real logic is extracted and tested; the widget wiring is not.**
Matching a live pixel-zoom value back to a preset index — needed to drive the
submenu's checkmarks — becomes a pure, Qt-widget-free function in
`MainWindowZoom.h`, following the existing `MainWindowStatus.h` pattern:

```cpp
constexpr std::array<float, 5> kZoomPresets = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
int matchingZoomPresetIndex(float pixelZoom); // -1 when no preset matches
```

Its tolerance is an **absolute epsilon of 0.005** — half a percentage point:
comfortably above float round-trip error through `setPixelZoom()`/`pixelZoom()`,
and comfortably below the ~25-percentage-point gap between adjacent presets. The
menus then consume `matchingZoomPresetIndex` and `kZoomPresets` directly rather
than restating the values.

## Considered Options

- **One shared list + a pure matcher (chosen).** The action list cannot diverge
  because there is only one, and the only branching logic is unit-tested without a
  widget. Costs a helper method and a small header.
- **Keep two hand-maintained lists, just sync them once.** Cheapest edit, fixes
  today's disagreement and nothing else — the next person to add a preset has the
  same two-places problem. Rejected: this ADR exists because that already happened.
- **Unit-test the menu wiring too.** Would catch a miswired action, but this
  codebase deliberately does not unit-test Qt menu/action glue (see the testing
  strategy in [DESIGN.md](../../DESIGN.md#testing-strategy)); doing it here only for
  zoom would be an inconsistent island of widget tests. Rejected — extracting the
  logic is what makes the wiring dumb enough not to need tests.

## Consequences

- **Only the View menu's `Fit` gets `Ctrl+0`.** Giving the status bar's `Fit`
  action the same shortcut makes it ambiguous at runtime — both actions live in the
  same top-level window, and Qt reports the collision and fires neither. The shared
  helper therefore does *not* assign the shortcut; the caller does.
- The preset group is exclusive and checkable, driven from `zoomChanged`. When the
  current zoom is not a preset (after a wheel zoom, or Zoom In/Out landing
  off-chain) **nothing** is checked — Qt's exclusive `QActionGroup` permits an
  all-unchecked state when set programmatically.
- The whole submenu is disabled while no image is loaded, reusing the signal that
  already drives the status bar zoom button's visibility.
- Adding or changing a preset is a one-line edit to `kZoomPresets`; both menus
  follow. Adding a preset that breaks the doubling chain would silently make
  Zoom In/Out stop landing on presets.
- Out of scope, deliberately: the underlying zoom clamp range (0.05×–32×) in
  `ImageViewport`, Zoom In/Out in the status bar dropdown (presets only), and
  persisting the last-used zoom across image loads.

*Supersedes the standalone design note and implementation plan this decision was
originally written as (2026-07-01); the code is `src/MainWindowZoom.{h,cpp}`,
`MainWindow::addZoomPresetActions`, and `tests/test_MainWindowZoom.cpp`.*
