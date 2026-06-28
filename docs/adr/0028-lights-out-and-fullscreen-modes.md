# Two composable focus modes: OS fullscreen (F11) and lights-out hide-chrome (F12)

`MainWindow` gains two **independent, composable** view modes for an image-first
workflow (issue #22, "Lights Out / Fullscreen"):

- **Full Screen (`F11`)** — OS-level borderless fullscreen via `showFullScreen()`,
  reversed by restoring the prior windowed state.
- **Lights Out / Hide Panels (`F12`)** — hides *all* application chrome so only
  the image remains, restoring each element to exactly its prior visibility.

The two are orthogonal: any of the four combinations (windowed/fullscreen ×
chrome shown/hidden) is reachable. `Escape` is a single "give me my UI back"
panic key that exits **both** at once. Neither mode persists across restarts.

## Context

Arraw had no fullscreen or distraction-free mode. Chrome consists of the menu
bar, the main tool bar, the Adjustments **reveal strip** toolbar (ADR 0012), the
status bar, the Film Strip dock, and the Adjustments dock. Photographers
reviewing pixels want all of it to vanish, then come back exactly as it was.

Two forces shaped the decision:

- **The menu bar is part of the chrome that hides.** So the modes cannot rely on
  visible menu items to toggle off — each needs a window-level shortcut that
  keeps firing when the menu bar is gone. `QAction` shortcuts register at the
  window, so they satisfy this even while the menu is hidden.
- **ADR 0012's collapse-to-strip invariant must survive.** The Adjustments dock
  and its reveal strip are mutually exclusive, driven by `CollapsiblePane`'s
  state machine. Hide-chrome must not perturb that machine, or restoring would
  desync the dock/strip pair.

## Design

### Modes and state

- `bool chromeHidden_` — whether lights-out is active.
- `bool wasMaximized_` — captured on entering fullscreen so exit returns to
  maximized-or-normal correctly (`showNormal()` alone would drop a prior
  maximized state).
- A visibility **snapshot** (one `bool` per chrome element) captured when
  hiding, consumed when restoring.

Fullscreen-ness itself is read from `isFullScreen()`; no separate flag.

### `setChromeVisible(bool)`

The hide path snapshots `isVisible()` for each of the six chrome elements, then
`hide()`s them. The show path restores each from the snapshot. It uses only raw
`hide()`/`setVisible()` — it never calls `CollapsiblePane` expand/collapse — so a
dock that was collapsed-to-strip beforehand comes back collapsed, preserving the
ADR 0012 invariant. The main tool bar and the Adjustments strip toolbar are kept
as members so they can be addressed directly rather than rediscovered.

### Shortcuts, menu, Escape

- The View menu gains two **checkable** actions: "Full Screen" (`F11`) and
  "Hide Panels / Lights Out" (`F12`); their check state tracks the modes.
- `Escape` is handled in `keyPressEvent` **only when** `chromeHidden_ ||
  isFullScreen()`: it restores all chrome, un-fullscreens, and consumes the
  event. When no mode is active, `Escape` passes through untouched, so existing
  uses (e.g. crop-cancel) are unaffected.

### Persistence

The modes are deliberately transient. Because `closeEvent` persists
`saveState()`, it first calls the restore path (un-hide chrome, un-fullscreen)
**before** saving, so the stored layout reflects the user's real arrangement and
never the transient focus mode.

## Considered options

- **One combined "maximal focus" toggle (rejected).** Simpler, but the user
  wanted to compose fullscreen and hide-chrome separately (e.g. fullscreen with
  panels still up for a slideshow-like review).
- **`Tab`/`Shift+Tab` like Lightroom (rejected).** ADR 0012 already ruled these
  out: the Adjustments panel is full of slider/spin-box fields where `Tab` means
  field traversal. `F11`/`F12` are unused function keys with no such conflict.
- **Persist mode state in `QSettings` (rejected).** Starting the app in a
  chrome-less or fullscreen state is disorienting and undiscoverable. Focus
  modes are session-transient.
- **Hide only toolbars/status, leave docks to `F8`/`F9` (rejected).** The user
  wanted a single gesture that clears *everything*; manual composition defeats
  the "image-first" purpose.

## Consequences

- Four reachable view combinations; `Escape` collapses any of them back to the
  user's normal layout in one press.
- A future reader must not "simplify" hide-chrome to drive `CollapsiblePane` —
  the raw snapshot/restore is deliberate, to protect the ADR 0012 invariant.
- `closeEvent` must keep restoring chrome before `saveState()`; removing that
  step would let a transient focus mode corrupt the persisted window layout.
- This is a UI/window concern only — no new domain term, so `CONTEXT.md` is
  unchanged.
