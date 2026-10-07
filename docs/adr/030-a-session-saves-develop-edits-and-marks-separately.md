# A session saves develop edits and marks separately

[ADR 019](019-develop-settings-live-in-an-xmp-sidecar-and-in-json.md) left open
when a GUI writes the sidecar, and [ADR 021](021-a-photograph-is-developed-from-a-develop-state.md)
promised marks their own baseline. The [film strip plan](../ideas/filmstrip-plan.md)
fixes the behaviour, as main had it: Ctrl+S saves adjustments, and leaving a
changed photograph asks. This ADR records how `EditSession` supports that. It
is separate from ADR 022 because that one is about edits, and this is about
persistence.

## Decision

**The session holds `saved()`, the photograph as its sidecar holds it.** It
starts as the opened photograph, which is what `openPhoto` read from the
sidecar, or defaults when there was none. Only a successful write changes it.

**Dirty is a comparison, not a flag.** `hasUnsavedChanges()` is true when
`photo().state()` differs from `saved().state()`. An open edit counts through
its provisional state, so a drag is dirty while it moves, and undoing back to
the saved state is clean again. Marks never count.

**`save()` writes `photo()` and then makes it the saved one.** An open edit is
committed first. History is kept, since saving is not an edit. A failure
throws what `writeSidecar` throws and changes nothing, an open edit included.

**Marks are written at once, against the saved photograph.** `setMarks(marks)`
writes `saved().with(marks)`, so develop edits that are not saved stay out of
the file, and then updates `saved()` and `photo()`. It is not an undo step.
This is why a rating pressed in the middle of an edit does not save the edit,
and why Discard does not revert a rating. A failure, or invalid marks, change
nothing.

**`discardChanges()` returns to the saved state,** dropping history and any
open edit. It is what Discard does on leaving; the session is then usually
dropped, but it stays coherent when the window stays open.

**Leaving a photograph asks once, in one place.** `MainWindow::confirmLeavingPhoto()`
asks "Save changes to <name>?" with Save, Discard and Cancel. Save that fails
is shown and stays, as Cancel does. File > Open… calls it once the chosen file
is known to open, and the film strip's navigation will call it too. Closing
the window asks it before the running-exports question: unsaved work is the
user's, while exports are asked about only once the window is certain to go.
The title shows the file name with Qt's `[*]` modified marker.

## Alternatives

- **Writing on every edit.** Nothing to lose, but no Discard, and an
  experiment would overwrite the photographer's settings. Main chose explicit
  saving, and the plan keeps it.
- **One baseline for state and marks.** A rating would then either save
  unsaved edits with it, or count as unsaved and ask on leaving.
- **A dirty flag.** Cheap, but it drifts: undoing to the saved state would stay
  dirty. A comparison cannot disagree with the states it describes.

## Consequences

- Another program changing the sidecar while the session is open is not noticed
  here; the strip's folder watcher (plan, step 4) decides that, and reloads
  only when there are no unsaved changes.
- Discarding after a failed `openPhoto`-then-`loadImage` is possible: Open…
  reads the sidecar before asking, but decodes after.

## Note, 2026-10-05

Rating a photograph that has no sidecar creates one that holds the marks alone,
with no `arraw:version` and no settings. It records no develop state, so the
photograph still opens with what its kind starts from (a RAW's colour noise
reduction, [ADR 039](039-noise-reduction-is-the-first-pass-and-reads-the-as-shot-luminance.md)). A session's own mark writes go through `writeSidecar` of
the saved document, so they write its state too, as before.
