# Multi-select filmstrip: LR-style active/selected split for batch paste and batch export

Two batch operations — paste settings and export — arrive on the filmstrip. Both
require selecting multiple files without losing the single-image develop workflow.
This ADR records the selection model and the key behaviour decisions that follow
from it.

It deliberately **reverses an earlier decision**. The filmstrip began as a
left-side vertical icon grid, became a bottom-docked horizontal strip, and was
single-select throughout: one click, one loaded image, no selection concept at all.
That was right while every operation acted on the open photo; batch operations are
what make a target *set* necessary, so the reversal is a response to new
requirements rather than a correction.

## Considered Options

**Selection model**

- **Active = selected (simple):** clicking any item loads it and makes it the
  sole selection. Ctrl+click adds items but the viewport always shows the
  last-clicked file. Simpler API: `fileSelected` stays a single-path signal.
  Rejected: paste and export need a target set distinct from "which file am I
  editing right now" — collapsing them forces awkward workarounds.

- **LR-style active/selected (chosen):** last-clicked item = *active* (drives
  the viewport, always a member of the selection); `QListView`
  `ExtendedSelection` tracks the full batch target. Two distinct visual states
  in the delegate (active highlight vs. selection highlight). Navigation arrows
  move the active item and collapse the selection to it.

  **One deliberate deviation from Lightroom:** in LR a Ctrl/Shift+click makes
  the clicked photo *active* (it loads into Develop). Here Ctrl/Shift+click only
  edits the batch selection and leaves the active image pinned — only a plain
  single click changes the active image. The reason is cost: every active change
  triggers a full decode, an undo-stack reset, and a viewport swap, so making
  each batch-assembly click reload would be jarring and slow. `FilmStrip`
  intercepts modifier-clicks on the list viewport (`handleModifierClick`) and
  drives the selection model itself, never moving the current index; Qt's default
  `ExtendedSelection` would otherwise move current and fire `fileSelected`.
  Consequence: the active image can never be toggled *out* of the selection — a
  Ctrl+click on the active item keeps it selected — since it is by definition an
  edit target.

- **Right-click context target (chosen):** right-clicking a thumbnail opens a
  context menu for that thumbnail without making it active. If the thumbnail is
  already selected, context operations use the whole current selection; if it is
  not selected, they use only the right-clicked file. This keeps batch paste and
  batch export available from the strip while preserving the rule that only a
  plain left-click changes the active image. Copy Settings is intentionally
  single-source only: it is disabled when the current selection contains multiple
  images, or when the source has default develop settings. Apply Preset follows
  the same target rule as Paste Settings.

**Batch paste before-state capture**

- **Lazy / async:** load before-state from sidecar only if the user actually
  undoes. Complicates `BatchAdjustmentCommand` significantly (async undo).
  Rejected.
- **Synchronous at paste time (chosen):** read each non-active file's XMP
  sidecar immediately when paste is confirmed. XMP files are a few KB; 200
  of them is well under a second on any normal disk. `BatchAdjustmentCommand`
  is a plain value-holding object.

**Sidecar writes on batch paste**

- **Explicit Ctrl+S per file:** leaves non-loaded files in an unsaved state
  the user cannot reach without navigating to each one. Unusable.
- **Auto-save on paste (chosen):** all N sidecars are written immediately after
  paste is confirmed. Slider moves on a single image still require Ctrl+S —
  auto-save is not a global policy change, only a batch-paste necessity.

**Batch export output path**

- **Unify single and batch to directory + derived filename:** loses the ability
  to rename on single-file export.
- **Keep separate (chosen):** single-file export keeps "save as" (`getSaveFileName`);
  batch export uses a directory picker and derives `<basename>.<ext>`. May
  unify later.

**Long-operation progress UI**

- **Status bar + non-modal cancel:** lets the user keep editing while export
  runs, but export uses `renderToImage` on the main thread — concurrent editing
  would require queuing or locking the renderer.
- **Modal dialog with cancel (chosen):** honest about the GPU constraint; the
  same widget serves any future long operation (batch decode, etc.).

## Consequences

- `FilmStrip` gains a `selectionChanged(QStringList paths)` signal alongside
  the existing `fileSelected(path)` (renamed to `activeFileChanged`). The
  delegate must paint active and selected states distinctly.
- `BatchAdjustmentCommand` holds `QVector<{path, before, after}>` and calls
  `XmpSidecar::save` directly for non-active files on both redo and undo.
- All `QUndoCommand` subclasses must have `setText()` — batch command uses
  "Paste Settings (N files)"; existing single-file commands should be audited
  at the same time.
- Batch export dialog reuses the existing format/quality/profile/size controls
  with a `QFileDialog::getExistingDirectory` instead of `getSaveFileName`.
- `BatchProgressDialog` (modal, cancellable) is introduced as a reusable widget
  for long operations.
