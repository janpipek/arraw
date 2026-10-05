# A photograph is developed from a develop state

[ADR 008](008-develop-settings-and-their-descriptors.md) made `DevelopSettings`
a plain aggregate with a descriptor table beside it: one row per leaf, each
with a key, range, group and stage. That suits global settings such as
Exposure or Straighten. It does not suit what is coming next.

The [feature brief](../desired-features.md) asks for local adjustments (masks),
spot heals and a grain seed. These differ from global settings in four ways:
- **They are lists.** A photograph has any number of masks and spots, so they
  have no fixed row in the descriptor table.
- **They stay with the photograph they belong to.** Presets and copy/paste
  carry global settings. A spot never travels, and a mask only when asked.
  The [reimplementation plan](../ideas/reimplementation-plan.md) says these
  "retain their existing treatment".
- **They can be large.** A brush mask holds strokes, possibly a raster.
- **They are edited with tools on the image**, not with panel rows.

They still belong to how a photograph is developed. A history step, a
snapshot and a render all need them together with the global settings. So
they need a home next to `DevelopSettings`, not inside it. The GUI's edit
history has to be designed around that home, so it is introduced now, before
history exists.

## Decision

**`DevelopState` is everything that says how one photograph is developed.**
For now it is an aggregate with one member:

```cpp
struct DevelopState {
    DevelopSettings settings{};
};
```

Masks, spots and the grain seed become members next to `settings` when they
arrive. (The grain seed did not: it is one number with a row of its own,
kept with its photograph by its `SettingScope`; see ADR 038.)

**The develop state is the unit the document, the session and the renderer
use:**
- `Photo` holds a `DevelopState`, reads it through `state()` and changes it
  with `with(DevelopState)`. There is no `settings()` shorthand, so there is
  one way to read it.
- `EditSession::setState` replaces it. The edit protocol
  (begin / update / commit) and its history store `DevelopState` values
  ([ADR 022](022-an-edit-is-begun-updated-and-committed-as-one-step.md)).
- `SidecarContents::state` is what a sidecar reads and writes, since a sidecar
  persists per-photograph edits too.
- `develop`, `developOnDevice`, `developOnGpu` and `planFor` take a
  `DevelopState`. Today they read only `state.settings`. Once a planner needs
  spots or masks, no caller has to change.

**`DevelopSettings` stays the global part, described by its table:**
- the descriptor table and `validate(const DevelopSettings&)`;
- the setting codec, which the command line and the sidecar use per setting;
- the JSON settings document, which is the shape presets will take.

`validate(const DevelopState&)` checks the settings and, later, each list.

**Culling marks are not part of the develop state.** Marks and metadata are
saved immediately and have their own baselines, so a rating never becomes an
undo step and never marks unsaved develop changes as saved.

**Rules for the lists, fixed now so the first one follows them:**
- **Every list item has a stable id.** The GUI selection, history labels, GPU
  mask caches and Python references all use it. Indices are not stable across
  insertion and deletion.
- **Large data is shared and immutable.** Strokes and rasters sit behind
  `std::shared_ptr<const T>`. Copying a `DevelopState`, for a history step or
  for each provisional update during a drag, then copies pointers rather than
  pixels.
- **Each list has its own presentation.** Typed values inside an item, such as
  a mask's exposure, may get their own small descriptor table. The list itself
  is never squeezed into the global table.

**Python follows the C++ API** ([ADR 018](018-python-binds-the-public-api-and-nothing-else.md)):
`arraw.DevelopState(settings=...)`, `Photo.state`, `Photo.with_(state=...)`,
`SidecarContents.state` and `develop(source, state)`. Flat keywords such as
`exposure=0.7` on `Photo.with_` still edit `state.settings`.

## Alternatives

- **Lists inside `DevelopSettings`.** One type everywhere, but the descriptor
  table, the drift test, validation, JSON and preset rules would all have to
  tell global leaves from per-photograph lists.
- **Lists as separate members of `Photo`.** No new type, but history,
  snapshots and the renderer would each have to gather several members and
  agree on which belong together.
- **Introducing the type with the first local feature.** Less work today, but
  `EditSession`'s API and its callers would change after the history built on
  it already exists.

## Consequences

- **Code that only edits global settings says so:** it reads
  `photo.state().settings`, and the Python equivalent is
  `photo.state.settings`.
- **Adding a local feature is a new member, its validation, its sidecar
  mapping and its planner input.** No signature has to change.
- **The renderers take more than they read for now.** The planner decides what
  each pass needs, which is where that belongs (ADR 012).
- **Equality of a develop state compares shared payloads.** Comparing pointers
  first keeps a history step and a "did anything change" check cheap. Whether
  equal content in different storage counts as equal is decided along with the
  first list.
