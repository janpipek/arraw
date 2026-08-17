# Save Preset pre-checks only groups that differ from default — the group stays the unit, not the field

Saving a [[Develop Preset]] seeded its group checklist from `lastCopySelection`,
the sticky selection shared with [[Copy Settings]]/[[Paste Settings]] — in
practice "every group but Geometry," since that is `defaultCopySelection()`'s
starting value. A photographer who touched only Tone would, by default, save a
preset carrying nine groups, eight of which are pure resets: applying it later
silently wipes HSL, Detail, Effects, etc. on the target, because a carried
group **replaces every field, defaults included** ([[Develop Group]],
docs/adr/0023). The fix is in what gets pre-checked, not in what "carrying a
group" means.

## What we decided

Save Preset's checklist now pre-checks exactly the groups whose values differ
from `GlobalAdjustment{}`'s defaults on the photo being saved, computed fresh
each time the dialog opens. It no longer reads or writes `lastCopySelection` —
that variable stays Copy/Paste's alone. Scalar/bool/array fields compare by
`==`; the four tone curves compare via `CurvePoints::isIdentity()`, reusing the
tolerance that already exists for exactly this purpose rather than inventing a
new epsilon.

## Considered Options

- **Fix the default, keep group-level replace (chosen).** The checklist
  changes; `applyGroups` does not. A preset can still deliberately carry an
  all-default group — meaning "reset this on apply" — by hand-checking it, but
  that is now an opt-in action instead of the silent default. One apply path
  keeps backing both [[Paste Settings]] and preset apply, and the JSON format
  (docs/adr/0023) is untouched: a group's presence in the file is still the
  only flag that matters.
- **Field-level merge (rejected).** Store and apply only the fields that were
  actually edited, leaving everything else on the target untouched. This reads
  as "more correct" but changes what a preset *means*: applying "Punchy" would
  no longer guarantee the Punchy look, since the result would depend on the
  target photo's prior state. It also forks `applyGroups` from
  [[Paste Settings]] (or drags merge semantics into paste too, contradicting
  the glossary's "paste replaces each group, it does not merge"), and it
  removes the ability to express "reset this group" at all — a real, if
  narrow, use case. We rejected it as a bigger, harder-to-reverse change in
  service of a problem the checklist fix already solves.

## Consequences

- `saveCurrentAsPreset` gains its own selection function
  (`groupsWithNonDefaultValues(currentParams())`), independent of
  `lastCopySelection`; Copy Settings' sticky memory is unaffected.
- A preset's [[Manage Presets]] details view can reuse the same per-field
  default check to decide what to display, and to flag a carried
  all-default group as "resets to defaults" rather than showing it as empty.
- Reversing this later — to field-level merge — would mean reworking
  `applyGroups`, deciding whether [[Paste Settings]] follows it or diverges,
  and changing the JSON format to record per-field provenance instead of
  per-group presence. Not undertaken lightly.
