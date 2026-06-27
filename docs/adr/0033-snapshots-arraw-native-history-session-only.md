# Snapshots persist arraw-native; History is session-only

Issue #22 asks for a visible History Stack so photographers can A/B test
development paths. That request conflates two distinct Lightroom features, and
we split them deliberately.

**History** is the linear, session-only list of develop edit steps — the visible
face of the existing develop `QUndoStack`. It is *not* persisted: it resets on
load (as the stack already does), and the develop *state* is restored from the
sidecar while the step list is not. Persisting a full per-edit history would
mean inventing a per-file catalog, contradicting the no-database, sidecar-only
stance (ADR 0008/0015/0020).

**Snapshots** are named, persisted captures of the *complete* develop state,
used to A/B by switching between them (click-to-restore; one viewport, no split
view). They are whole-state — every field, including [[Local Adjustment]] masks
and spots — because a snapshot that dropped masks could not faithfully reproduce
a development path. Restoring a snapshot replaces the develop state as a single,
undoable History step (a plain `AdjustmentCommand`); creating, renaming, and
deleting snapshots edit the persisted list directly and are not History steps.

Snapshots are stored in arraw's own `arraw:Snapshots` RDF Seq, not Adobe's
`crs:Snapshots`. arraw owns the complete `arraw:` namespace (ADR 0026), and a
self-contained `arraw:`-named encoding of the full develop state is lossless for
arraw-only fields. Adobe's `crs:Snapshots` string format cannot represent
arraw's local adjustments or spots, so a faithful round-trip there would
silently drop masks — defeating the purpose for a niche interop benefit on a
solo-developer tool.

## Considered Options

- **Persist full history to the sidecar.** Rejected: a per-file edit catalog,
  large sidecars, and a divergence from Lightroom (which keeps history in its
  catalog, not XMP). Snapshots already cover persistent A/B paths.
- **`crs:Snapshots` (Lightroom-compatible).** Rejected: lossy — cannot carry
  masks/spots — for marginal interop value.
- **JSON blob inside XMP.** Rejected: would reuse one serializer but embeds
  opaque JSON inside an otherwise structured-RDF sidecar, inconsistent with the
  namespace-aware modeling of ADR 0026.
- **Side-by-side split-view A/B.** Deferred: a major RendererCore/viewport
  change (two live pipelines). Click-to-switch delivers A/B without it.
