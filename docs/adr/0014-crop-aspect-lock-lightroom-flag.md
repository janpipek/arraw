# The crop aspect-ratio lock persists as a Lightroom flag, ratio re-derived from the rectangle

When a [[Crop]] is constrained to a ratio, we persist only *that it is
constrained* — the Lightroom-compatible `crs:CropConstrainAspectRatio` boolean —
and re-derive the locked ratio from the stored crop rectangle on load. We do
**not** store which preset (1:1, 16:9, …) the user picked. The internal lock is
therefore a single raw ratio, not a named preset, so a constraint read back from
a file — which carries only the rectangle — can re-engage at that rectangle's
proportions.

## Context

The crop rectangle's proportions are already persisted as the `crs:Crop*` edges,
so a locked crop's *ratio* is never lost regardless of this decision. The only
thing the file lacks is whether the crop should stay locked while the user drags
its handles again. Lightroom records exactly that one bit. The
[[Aspect Ratio Lock]] began life as transient tool state (forgotten on tool
close); making the constraint survive a reload is what this ADR settles.

## Considered Options

- **Lightroom flag, ratio from the rectangle (chosen).** One boolean in the
  existing `crs:` block we already read and write. Round-trips with Lightroom,
  adds no fields beyond the standard one, and never lets the stored ratio and the
  stored rectangle disagree (there is only the rectangle). The cost is that the
  preset *name* is not preserved: on reload we run `matchPreset` to label the lock
  (named ratios win over Original), and a ratio matching no preset — e.g. an
  exotic crop authored in another editor — round-trips as a nameless "locked"
  state. That cost is paid in a label, never in the geometry.
- **arraw-native preset (rejected).** Store the exact `AspectPreset` +
  orientation in arraw's own XMP namespace. Restores the menu choice precisely,
  but is not Lightroom-compatible and writes into the same arraw-native namespace
  the parametric Local Adjustments own (`0010-parametric-local-adjustments`),
  deepening that surface for a cosmetic gain (the ratio is already in the
  rectangle). Rejected: it trades real compatibility for a remembered label.
- **In-session memory only (rejected).** Remember the last ratio while the app is
  open but write nothing. Zero file-format change, but the constraint is gone the
  moment the image is reopened — which is the thing the user asked to keep.

## Consequences

- The lock is **not transient** (reversing the original tool-state design): the
  flag rides the develop sidecar with the rest of the `crs:` globals, consistent
  with the Lightroom-compatibility promise for global edits in
  `0010-parametric-local-adjustments`.
- The internal lock is a raw `lockedRatio` (0 = free). Presets compute it;
  `matchPreset` inverts it for the menu/readout. A future "Custom W:H" entry drops
  in for free, since the lock already speaks raw ratios.
- The pure ratio maths (`cropPixelRatio`, `matchPreset`, `presetRatio`,
  `fitRatioInside`, `lockedResize`) live in `arraw_core` and are unit-tested
  headlessly, per [[spot-for-algorithms]]; the persisted size readout shares
  `cropPixelSize` with the export path so the number shown can't drift from the
  number written.
