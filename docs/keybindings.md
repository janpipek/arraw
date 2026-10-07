# Keyboard shortcuts

The desktop application's shortcuts: those it has, and those the `main` branch has
and this rework will take over as each feature lands. When a feature lands, move
its rows from *Planned* to *Current* in the same change that wires them.

## Current

| Key | Action | Where |
|---|---|---|
| `Ctrl+O` | Open a photograph | File menu (`QKeySequence::Open`) |
| `Ctrl+Q` | Quit | File menu (`QKeySequence::Quit`) |

## Wiring rules

Carried over from `main`, where each was learnt the hard way:

- Prefer a `QKeySequence::StandardKey` where one exists (Open, Save, Quit, Undo,
  Redo, ZoomIn, ZoomOut), so each platform gets its native binding.
- A `QAction`'s shortcut fires only while a widget hosting it is visible and
  enabled. An action that must keep working with the menu bar hidden
  (panel toggles, full screen, hide panels) is also added to the main window
  with `addAction`.
- Single-letter keys (culling, tools) are window-level actions, so they fire
  whether the film strip or the image has focus, with no modifiers.
- One key, one action per window: two actions with the same shortcut in one
  top-level window are ambiguous and neither fires. For example, only the View
  menu's Fit carries `Ctrl+0`, not the status bar's copy.
- `Esc` cancels the active tool first; only with no tool active does it leave
  full screen or bring hidden panels back.

## Planned

From `main`, grouped by the feature that brings them.

### File

| Key | Action |
|---|---|
| `Ctrl+Shift+O` | Open folder |
| `Ctrl+S` | Save adjustments (`.xmp` sidecar) |
| `Ctrl+E` | Export… |

### Edit (History)

| Key | Action |
|---|---|
| `Ctrl+Z` | Undo |
| `Ctrl+Shift+Z` | Redo |
| `Ctrl+Shift+C` | Copy settings… |
| `Ctrl+Shift+V` | Paste settings… |

### View

| Key | Action |
|---|---|
| `Ctrl++` / `Ctrl+-` | Zoom in / out (×2) |
| `Ctrl+0` | Zoom to fit |
| `F7` | Toggle History panel |
| `F8` | Toggle Adjustments panel |
| `F9` | Toggle Film Strip |
| `F11` | Full screen |
| `F12` | Hide panels (lights-out) |
| `S` | Toggle soft-proofing |
| `J` | Toggle clipping overlay (highlights and shadows) |
| `\` (hold) | Show the original (before/after) |
| `Esc` | Cancel the active tool, or leave full screen / lights-out |

Zoom also follows the scroll wheel; pan with `Alt`+drag or middle-button drag.

### Navigation

| Key | Action |
|---|---|
| `←` / `→` | Previous / next photograph in the folder |

### Tools

| Key | Action |
|---|---|
| `C` | Crop tool (toggle) |
| `M` | Masks tab and its on-image controls |
| `Q` | Spots tab and its on-image controls |
| `O` | Toggle the mask overlay (while masking) |
| `Enter` | Commit the active tool |
| `Esc` | Cancel the active tool |
| `Ctrl+]` / `Ctrl+[` | Rotate 90° clockwise / counter-clockwise |
| `X` | Swap crop orientation (while cropping) |

Straighten and the White Balance picker have no key.

### Culling

| Key | Action |
|---|---|
| `1`–`5` | Star rating |
| `0` | Unrated |
| `X` | Reject (outside cropping) |
| `R` `Y` `G` `B` `P` | Red, yellow, green, blue, purple colour label |
