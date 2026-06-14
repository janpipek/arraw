# Culling marks live in the develop sidecar; saves are namespace-scoped and read-first

Rating and colour label (the [[Culling]] marks — `xmp:Rating`, `xmp:Label`) are
persisted in the **same `.xmp` sidecar** that already holds the develop settings,
for every file regardless of format — never embedded into the original. They are
modelled as a `UserMetadata` value type **separate from `AdjustmentParams`**
(they touch no shader uniform), paired with it as `SidecarData`. Because two
unrelated writers now share one file — `MainWindow` writing `crs:` edits on
Ctrl-S, and the filmstrip writing `xmp:` marks on a keypress, possibly for a file
that was never opened — `XmpSidecar` saves are **read-first and namespace-scoped**:
`saveAdjustments` reparses the file and replaces only `crs:`, `saveMetadata`
reparses and replaces only `xmp:`. The merge lives once, inside `XmpSidecar`.

## Considered Options

- **Embed marks into the file's own metadata** (DNG/JPEG/TIFF XMP packet): the
  primary targets are proprietary RAW, which cannot be written safely — so
  embedding could never be the only path, only a second one alongside the
  sidecar (split-brain over which wins) plus a new write-into-file dependency and
  corruption risk on the user's originals. Rejected; sidecar uniformly.
- **A single `save(SidecarData)` taking both halves**: forces every caller to
  load-merge the *other* half first, duplicating the merge at each call site.
  Rejected in favour of two namespace-scoped saves so the merge is SPOT.
- **Fold rating/label into `AdjustmentParams`**: pollutes the "params are the
  shader uniform" model and the CLAUDE.md add-an-adjustment ritual (a reader
  would hunt for `rating` in `image.frag`). Rejected; marks are their own type.

## Consequences

- Making `save` read-first also stops the pre-existing "save rewrites from
  scratch and drops everything it didn't write" data loss — *for the fields we
  model*. Truly foreign tags we don't model (e.g. `dc:subject` keywords from
  Lightroom) are still dropped; full unknown-tag round-tripping (retaining raw
  XML) is deliberately out of scope.
- Marks are written through to disk immediately on keypress and are **not** on
  the develop undo stack — metadata and develop edits stay separate concerns.
- A JPEG handed to a sidecar-blind app won't show its stars; acceptable for a
  RAW-first culling tool.
