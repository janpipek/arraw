# Develop Presets are partial, arraw-native JSON — a clean break from the crs: sidecar

A [[Develop Preset]] is stored as one **JSON** file per preset under
`QStandardPaths::AppDataLocation/presets/`, and is **partial**: it serialises
only the [[Develop Group]]s chosen at save time, so a group's presence in the
file *is* the flag that says "apply me." This deliberately does not reuse the
`crs:` XMP writer that the develop sidecar uses.

## Context

The sidecar is `crs:` XMP on purpose — it is the Lightroom-compatibility surface
for global edits (`0010-parametric-local-adjustments`,
`0014-crop-aspect-lock-lightroom-flag`). Presets are the opposite: an internal
convenience, explicitly *not* a Lightroom interop surface, and now (Milestone 8)
they store only the groups the user picked rather than a whole settings snapshot.

## Considered Options

- **Partial JSON in AppDataLocation (chosen).** JSON expresses "only the present
  groups" naturally — an omitted group is simply an absent key, so the file is
  self-describing and the active-group set needs no separate flag. Human-readable
  and inspectable, and `QJsonDocument` makes save→load round-trip and known-good
  fixture tests trivial (mirroring the `XmpSidecar` test discipline). The cost is
  a second serialisation format in a codebase that is otherwise XMP.
- **Reuse the crs: XMP writer (rejected).** One serialisation codebase, but it is
  built around a full `crs:` snapshot with Lightroom-shaped field names — it
  fights both the "partial" and "arraw-native, not Lightroom-compatible" goals,
  and entangles presets with the sidecar's interop contract. A preset is not a
  sidecar; sharing the writer would couple two things that must evolve apart.
- **QSettings / INI (rejected).** No file management, but opaque, awkward to
  share or inspect, and clumsy for nested data like the per-channel tone curves
  and the eight HSL bands.

## Consequences

- Accumulated `.json` preset files become a small format-compatibility surface:
  loaders must tolerate unknown keys and missing groups (a forward/backward
  compatibility seam), which the partial design already assumes.
- The same `applyGroups(target, source, selection)` pure function backs both
  preset apply and copy/paste; a loaded preset reconstructs `(source, groups)`
  where `groups` is exactly the set of keys present in the file. One apply path,
  one test surface, per [[spot-for-algorithms]].
- Presets never carry [[Local Adjustment]]s — masks are pinned to one photo's
  framing and are not a [[Develop Group]] (see Milestone 8).
