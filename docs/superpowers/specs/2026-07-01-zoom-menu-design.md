# Zoom Menu Redesign

## Problem

The View menu has a single `Reset Zoom` action (Ctrl+0). The status bar's zoom
button carries its own, separately maintained dropdown (`Fit`, `50 %`, `100 %`,
`200 %`). The two lists already disagree (different labels, different preset
sets) and nothing stops them drifting further apart.

## Goal

Give the View menu a full `Zoom` submenu that mirrors the status bar dropdown,
add incremental Zoom In / Zoom Out, and generate both menus from one shared
list so they can't diverge again.

## Design

### Shared preset list

One source of truth, a small ordered list of (label, pixel-zoom value) pairs:

```
Fit (special: calls resetView(), no fixed value)
25 %, 50 %, 100 %, 200 %, 400 %
```

A private `MainWindow` helper, `addZoomPresetActions(QMenu*, QActionGroup* group = nullptr)`,
appends these to any menu: `Fit` calls `viewport->resetView()`, each
percentage calls `viewport->setPixelZoom(value)`. Both the View submenu and
the status bar dropdown call this helper instead of hand-rolling their own
action lists.

Note the preset values (25/50/100/200/400) form an exact doubling chain, so
Zoom In/Out (below) always lands back on a preset when starting from one.

### View menu: `&Zoom` submenu

Replaces the flat `Reset Zoom` entry. Contents, top to bottom:

- `Zoom In` — `QKeySequence::ZoomIn` — `viewport->setPixelZoom(viewport->pixelZoom() * 2.0f)`
- `Zoom Out` — `QKeySequence::ZoomOut` — `.../ 2.0f`
- separator
- `Fit` (shortcut `Ctrl+0`, moved from the old `Reset Zoom` entry), `25 %`, `50 %`, `100 %`, `200 %`, `400 %` — from `addZoomPresetActions`, added to an exclusive `QActionGroup`

The preset group is checkable. A slot wired to `viewport::zoomChanged` looks
up the preset matching the current pixel zoom (see below) and checks it; if
the current zoom isn't a preset (e.g. after a wheel zoom), nothing is
checked — Qt's exclusive `QActionGroup` permits an all-unchecked state when
set programmatically.

The whole submenu is disabled while no image is loaded, reusing the same
signal that drives `updateZoomStatus`/`zoomButton` visibility today.

### Status bar dropdown

Rebuilt via the same `addZoomPresetActions` helper (`Fit` + the five
percentages only). No Zoom In/Out, no checkmarks — unchanged from today
otherwise. `Reset Zoom`'s label is retired; both menus now say `Fit`.

### Testable core (for TDD)

Almost everything above is Qt widget wiring, which this codebase doesn't
unit-test directly (see `docs/adr` testing strategy). The one piece of real
logic — matching the current pixel zoom to a preset for the checkmark — is
extracted as a pure, Qt-widget-free function, following the existing
`MainWindowStatus.h` pattern (pure helpers pulled out of `MainWindow` purely
so they're independently testable):

```cpp
// MainWindowZoom.h
constexpr std::array<float, 5> kZoomPresets = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};

// Returns the index into kZoomPresets matching `pixelZoom` within tolerance,
// or -1 if none match (e.g. a non-preset zoom from the wheel or Zoom In/Out
// landing outside the chain).
int matchingZoomPresetIndex(float pixelZoom);
```

Tolerance: absolute epsilon of 0.005 (half a percentage point) — comfortably
above float round-trip error through `setPixelZoom`/`pixelZoom()`, comfortably
below the ~25-percentage-point gap between adjacent presets.

`tests/test_MainWindowZoom.cpp` (Catch2, tag `[zoom]`) drives this function
test-first:
- exact matches for all five presets
- a value roughly between two presets (e.g. from Zoom In landing off-chain, or a wheel zoom) returns -1
- values just inside/outside the epsilon boundary

`MainWindow::setupMenus`/`setupStatusBar` then consume `matchingZoomPresetIndex`
and `kZoomPresets` directly instead of duplicating the preset values.

## Out of scope

- Changing the underlying zoom clamp range (5%–3200%) in `ImageViewport`.
- Adding Zoom In/Out to the status bar dropdown (presets-only, per discussion).
- Persisting the last-used zoom level across image loads (unrelated to this change).
