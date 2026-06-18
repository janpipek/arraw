# DevelopSession owns active image state; widgets edit and present it

The active image now has one non-GUI owner: `DevelopSession`. It owns the loaded
path, load/sidecar state, decoded buffers, current develop settings, derived
spot-applied buffers, read-only EXIF metadata, user metadata, and dirty baselines.
Adjustment panels, the spot panel, the filmstrip, and the viewport are editors or
presenters of that state; they should not be asked to answer "what is the current
image?" for save/export/copy/paste decisions.

## Considered Options

- **Keep panels as state owners.** This matched the original code: global
  adjustments came from `AdjustmentPanel`, local adjustments from
  `LocalAdjustmentPanel`, spots from `SpotRemovalPanel`, and culling marks from
  `FilmStrip`. Rejected because save/export/undo/batch actions had to
  reconstruct one image state from several widgets, making it easy to clobber
  per-image edits when a global edit changed.
- **Introduce a controller class per workflow.** Rejected as too much structure:
  most transformation logic is still better as pure functions (`applyGroups`,
  spot application, crop geometry, etc.). `MainWindow` remains the composition
  layer that wires Qt signals to session mutations and editor synchronisation.
- **Make `DevelopSession` the active-image state object (chosen).** It is a
  narrow QObject with explicit setters and query methods, not a controller for
  every action. Undo commands and batch paste update it directly or via a small
  callback, then `MainWindow` mirrors the session out to widgets.

## Consequences

- Save/export/copy/paste and preset application read `DevelopSession`, not
  editor widgets.
- Programmatic editor sync must block editor signals where needed; otherwise
  repainting a panel can be mistaken for a user edit.
- `FilmStrip` may still own model state for non-active thumbnails and may write
  culling sidecars for non-active images. For the active image, `MainWindow`
  keeps `DevelopSession::userMetadata()` in sync and mirrors it back to the
  strip.
- Sidecar parse status is part of active image state. A malformed sidecar loads
  defaults and is visible to the user instead of being silently treated the same
  as a missing sidecar.
