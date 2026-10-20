# Keyboard shortcuts

The desktop application's shortcuts: those it has, and those the `main` branch has
and this rework will take over as each feature lands. When a feature lands, move
its rows from *Planned* to *Current* in the same change that wires them.

## Current

| Key | Action | Where |
|---|---|---|
| `Ctrl+O` | Open a photograph | File menu (`QKeySequence::Open`) |
| `Ctrl+Q` | Quit | File menu (`QKeySequence::Quit`) |
| `Ctrl+Shift+O` | Open folder | File menu |
| `Ctrl+S` | Save adjustments (`.xmp` sidecar) | File menu (`QKeySequence::Save`) |
| `Ctrl+E` | Export… | File menu |
| `Ctrl+Z` | Undo | Edit menu (`QKeySequence::Undo`) |
| `Ctrl+Shift+Z` | Redo | Edit menu (`QKeySequence::Redo`) |
| `Ctrl+Shift+C` | Copy settings… (asks which sections) | Edit menu |
| `Ctrl+Shift+V` | Paste settings (no dialog; off in the crop mode) | Edit menu |
| (platform) | Settings | Edit menu (`QKeySequence::Preferences`; no key on some platforms) |
| `C` | Crop & Straighten (toggle) | Photo menu |
| `Ctrl+]` / `Ctrl+[` | Rotate 90° clockwise / counter-clockwise | Photo menu |
| `Ctrl+Shift+D` | Debug log | Window-level action |
| `Ctrl++` / `Ctrl+-` | Zoom in / out | View menu (`QKeySequence::ZoomIn`, `ZoomOut`) |
| `Ctrl+0` | Zoom to fit | View menu |
| `F9` | Toggle Film Strip | View menu |
| `←` / `→` | Previous / next photograph in the folder | Culling actions |
| `1`–`5` | Star rating | Culling actions |
| `0` | Unrated | Culling actions |
| `X` | Reject (outside cropping) | Culling actions |
| `R` `Y` `G` `B` `P` | Red, yellow, green, blue, purple colour label | Culling actions |
| `F5` | Refresh Folder: walk the strip's folder again. F5 on every platform, plus the platform's own Refresh keys (`Ctrl+R` on GNOME, `Shift+Cmd+R` on macOS, the keyboard's Refresh key) | View menu (F5 and all of `QKeySequence::keyBindings(Refresh)`), the strip's context menu and its "not watched" notice |
| `F7` | Toggle History panel | View menu |
| `F8` | Toggle Adjustments panel | View menu |
| `F11` | Full screen | View menu |
| `F12` | Hide panels (lights-out) | View menu |
| `J` | Toggle the clipping overlay (highlights and shadows); remembered | View menu |
| `M` | Masks tab and its on-image controls (toggle) | Photo menu |

### Mask mode

Window-level shortcuts, on in the mask mode only; they fire whether the photograph or
another widget has the focus (a slider's value field keeps typed characters).

| Key | Action |
|---|---|
| `O` | Toggle the mask tint |
| `Space` (hold) with a drag | Pan the view |
| `Delete` / `Backspace` | Delete the selected mask |
| `Esc` | Cancel the gesture under way; else, on a selected brush, deselect it; else disarm the tool; else leave the mask mode |
| `[` / `]` | Brush smaller / larger (÷ / × 1.25) while the brush is armed |
| `{` / `}` (`Shift+[` / `Shift+]` on a US layout) | Brush softer / harder (∓ 10 % hardness) while the brush is armed |
| `E` | Flip the brush's Erase toggle while the brush is armed |
| `Alt` (held at the press) | Erase with this stroke (paint when Erase is on); on some Linux desktops `Alt` with a drag moves the window, so use `E` or the Erase button |

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

### View

| Key | Action |
|---|---|
| `S` | Toggle soft-proofing |
| `\` (hold) | Show the original (before/after) |
| `Esc` | Cancel the active tool, or leave full screen / lights-out |

Zoom also follows the scroll wheel; pan with middle-button drag (`Alt`+drag is taken by Alt-to-erase in the mask mode).

### Tools

| Key | Action |
|---|---|
| `Q` | Spots tab and its on-image controls |
| `Enter` | Commit the active tool |
| `Esc` | Cancel the active tool |
| `X` | Swap crop orientation (while cropping) |

Straighten and the White Balance picker have no key.
