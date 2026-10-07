# Edit operations, presets, copy/paste and session history — plan

Status: accepted, 2026-10-07. Step (a) of
`architecture-roadmap.md`.

## Decisions (user, 2026-10-07)

| Question | Answer |
|---|---|
| The shape of `Edits.h` | A setter per setting, with its side effects beside it (§2) |
| A history click | Navigates, as `main` and Lightroom do |
| Groups for copying | Copy sections, finer than the panel's groups (§3) |
| Presets in the GUI | A list in the left dock, above History |
| The CLI | `preset list`, `show`, `apply` and `export --preset`; no `preset save` |

## Goal

- A photographer can copy settings from one photograph and paste them onto
  another, choosing which groups to carry.
- A look can be saved as a named preset and applied later.
- A History dock shows every edit of the session; clicking a row returns to
  that step.
- The GUI, the CLI and Python apply an edit by the same rules. Today they do
  not.

Out of scope here:
- pasting or applying a preset onto a multi-selection (step (b), with batch
  export);
- Snapshots (deferred, but the design leaves room for them, see below);
- the Import group of `docs/todo.md`. Its per-shot lens profile must never
  travel with a look; `SettingScope::Photo` already expresses that.

## What exists

**History.** `EditSession` (ADR 022):
- holds two stacks of whole `DevelopState` values;
- steps carry no labels;
- nothing lists the steps;
- Python does not bind it.

**The edit rules live in three places:**

| Rule | GUI | CLI | Python |
|---|---|---|---|
| Temperature or tint makes the white balance Custom | `app/WhiteBalanceChoice` (`withTemperature`, `withTint`) | its own code in `readEdits` | none |
| Grain turned on gets a seed | `chooseGrainSeed` (core) in `DevelopPanel` | `chooseGrainSeed` in `exportAll` | the caller's job |
| Straighten shown on screen versus stored before the flips | `DevelopPanel::applyEdit` | stored value | stored value |
| A turn or flip carries the crop; straightening shrinks the crop to fit | `app/CropEditing`, which reaches into the private `GeometryPlan.h` | **resets the crop**, with a warning | **neither** |
| Settings a render ignores are dropped | — | `withoutUnusedSettings` (private core) | — |

**Settings JSON.** `SettingsJson.h` already reads a partial document: an
absent key leaves the base alone. It writes every key.

**Groups.** `SettingGroup` has ten groups, one per panel. Two are coarse for
copying:
- *Color* holds the white balance together with Saturation and Vibrance;
- *Effects* holds the vignette and the grain.

**What `main` did:**
- ADR 0023: a preset is partial JSON, one file per preset in
  `AppDataLocation/presets`. A group present in the file means "apply it", and
  a carried group replaces every field of the group.
- One pure function, `applyGroups`, backs both presets and paste.
- ADR 0055: saving pre-checks only the groups that differ from the defaults.
- ADR 0051: the CLI has `preset list`, `show` and `apply`, reading the GUI's
  directory and nothing else. Names match case-insensitively and never fuzzily.
- ADR 0038 and `HistoryPanel`:
  - a left dock with Snapshots above History;
  - History is newest on top with "Load" at the bottom;
  - steps that were undone stay listed above the current one;
  - a click moves the stack's index;
  - the list is not persisted.

## Design

### 1. Geometry rules move into core

A new public header, `include/CropGeometry.h`, holds:
- `CropPoint` and `CropBox`;
- the resolved frame: the upright size, the crop as resolved, and the corners
  of the valid content;
- the state-to-state rules:
  - turn and flip in displayed axes, carrying the crop;
  - rotate about the crop's centre, shrinking only as far as needed;
  - straighten along a line;
  - set the aspect, swap the orientation, reset the crop;
  - fit a crop back into valid content.

They take the photograph's size and orientation from `ImageMetadata`, so
nothing needs decoding. The plan checks that `ImageMetadata::size` equals the
decoded size the GUI uses today.

`app/CropEditing` keeps only what a gesture needs:
- the handles, resize and image move from `beginGesture`;
- the run of rotations;
- the crop mode's own nested step history (ADR 040).

It calls the core rules for everything else. `test_CropEditing` passes
unchanged, and the geometry cases move to a core test.

### 2. `include/Edits.h`: one way to edit a state

**A setter per setting (decided).** Every change goes through a library
function that knows its own side effects:

```cpp
template <typename T>
DevelopState withValue(const ImageMetadata& photo, DevelopState state, std::string_view key, T value);
```

- **Typed.** The value's type is checked against the descriptor with
  `visitField`. A wrong type, an unknown key, or a value out of range throws
  `std::invalid_argument`, and nothing changes.
- **A twin that copies.** `withValueFrom(photo, state, key, source)` copies the
  key's value from another `DevelopSettings`. The CLI's `addEdit` already
  builds such a source. No public "any value" type is needed.
- **Plain keys.** Most keys are a plain assignment and validation.

**Keys with side effects.** About ten keys carry rules, each written beside its
key:
- temperature or tint: the white balance becomes Custom. Clearing both returns
  it to As Shot, as `withTemperature` and `withTint` do today.
- whiteBalance set to As Shot: clears both values.
- grainAmount from 0 to above: the grain gets a seed (`chooseGrainSeed`).
- rotation or a flip: an explicit crop is carried with its content, and a ratio
  is reciprocated on an odd turn.
- straighten: an explicit crop shrinks about its centre as far as rotated
  content requires. The shrinking is computed from the state the caller passes,
  so a run of edits (a drag) passes the state from before the run each time.
  This is for the straighten only: a grain amount dragged that way would draw a
  new seed on every tick.
- cropRectangle: a rectangle frees the aspect and is then fitted back inside
  the valid content (`fittedCrop`); `nullopt` resets to automatic framing and
  keeps the aspect. Because the rectangle frees the aspect, the table's order
  (rectangle, then aspect) is enough for a pair, and a locked target aspect
  never makes `withValues` throw.
- cropAspect: a ratio fits the largest crop inside the explicit one
  (`withAspect`).
- **An unchanged value changes nothing.** A geometry key given the value it
  already has applies no fitting and no shrinking.
- **The frame.** `straighten` and the two crop keys need `photo.size` and a
  state whose geometry is valid for it, else `std::invalid_argument`.

**Several values at once.** Edits that come together are applied in the
descriptor table's order, where rotation and the flips come before straighten
and the crop comes last:
- `withValues(photo, state, edits)` takes the keys and their source;
- a pasted or preset crop is therefore placed on the final frame;
- a crop that the CLI names explicitly wins over the carried one.

**A drag starts from its baseline.** During a straighten drag the GUI applies
each new value to the edit's starting state, not to the last update. Turning
back then undoes the shrinking, as `CropEditing`'s run of rotations does today.

**Operations.** The few edits that are not one value are functions too:
- `turned(photo, state, clockwise)` and `flipped(photo, state, horizontal)`, in
  displayed axes;
- `withAspect`, `withLockedAspect`, `withSwappedOrientation` and
  `withCropReset`;
- `withLook` (§3);
- `displayedStraighten` and `withDisplayedStraighten`, the on-screen sign of
  the straighten.

**Who calls them:**
- **GUI:**
  - `DevelopPanel::applyEdit` calls `withValue` for its row's key.
  - `MainWindow::editGeometry` and `reconciledGeometry` call the operations
    instead of their own copies.
  - `WhiteBalanceChoice` keeps only the combo's names and its named lights.
- **CLI:** `applyEdits` passes its list of flag edits to `withValues`.
  **Behaviour change:** `--rotate` and the flip flags carry a sidecar's crop
  instead of resetting it. `Notice::CropReset` is retired, and ADR 006 and
  ADR 014 are updated.
- **Python:**
  - `Photo.edited(**settings)` calls the setter for each keyword, in the order
    given;
  - `arraw.turned(...)` and the other operations are bound;
  - `DevelopSettings.with_` stays the raw, rule-free setter, and its docstring
    says so.

**Rejected: settling.** One `settle(before, proposed)` after any edit would
cover raw edits too, but it guesses what was meant from the difference and
needs precedence rules for that guess. Setters state the intent.

### 3. Looks: copy/paste and presets

**One function backs both:**

```cpp
DevelopState withLook(const ImageMetadata& photo, const DevelopState& target,
                      const DevelopSettings& source, std::span<const CopySection> sections);
```

**Copy sections.** Copying uses its own sections, finer than the panel's
groups, as Lightroom's copy dialog does. The descriptor table gets a `section`
column, and a test requires one on every Look row:

| Section | Keys |
|---|---|
| White Balance | whiteBalance, temperature, tint |
| Exposure | exposure |
| Tone | contrast, highlights, shadows, whites, blacks, filmicHighlights |
| Presence | texture, clarity, dehaze |
| Color | saturation, vibrance |
| Tone Curve | the four curves |
| HSL | the HSL bands |
| Black & White | convertToGrayscale and the grey mix |
| Color Grading | the grading zones and their controls |
| Noise Reduction | the five noise reduction keys |
| Vignette | vignetteAmount, vignetteMidpoint, vignetteFeather |
| Grain | grainAmount, grainSize, grainRoughness, grainModel (not grainSeed) |
| Rotate & Flip | rotation, flipHorizontal, flipVertical, straighten |
| Crop | cropRectangle, cropAspect |

Exposure stands apart from Tone, as in Lightroom, because it usually differs
from frame to frame while the rest of a look does not.

`withLook` copies every `SettingScope::Look` field of the chosen sections from
`source` through `withValues`. As in `main`, a carried section replaces the whole
section, defaults included; it does not merge. The rules:
- **Photo-scoped fields stay** (the grain seed, and later the Import group's
  lens profile).
- **The white balance does not cross between a RAW and a non-RAW** (ADR 008).
  It is skipped and reported, and the dialog shows it as not applicable.
- **Rotate & Flip and Crop are not in the default selection.** When chosen
  they are carried by the setters' rules:
  - Rotate & Flip sets the look's rotation, flips and straighten through their
    setters, which carry the target's own crop. When Crop is chosen too, the
    look's crop then replaces it.
  - **Crop: normalised, then fitted** (ADR 014, 2026-10-07). The look's edges
    are fractions of its own upright frame; the same fractions are taken of the
    target's upright frame, after Rotate & Flip if that is chosen, with the
    aspect free. `fittedCrop` puts the rectangle inside valid content. The
    look's aspect is then set with `withAspect`: a ratio keeps its literal
    value, Original resolves against the target's frame, and an automatic crop
    stays automatic with the look's aspect.
  - **Consequence:** a free crop's proportions follow the target's frame. A
    free square crop from a landscape photograph is not square on a portrait
    one; a locked ratio keeps its shape.
  - **U1 (review of step 1) is resolved as option (a):** the earlier transfer
    rule (the crop's centre and long-edge fraction, its physical aspect kept)
    needs the source frame, which a Look does not carry.

**The preset file** is the existing settings document plus a name, holding only
the keys of the carried sections:

```json
{ "arraw": 1, "name": "Punchy", "settings": { "exposure": 0.5, "contrast": 25, ... } }
```

- The carried sections are the sections whose keys are present.
- It is read through `applySettingsJson`, so the rules for unknown keys,
  clamping and newer versions already exist and are tested.
- No second format, unlike `main`'s separate `"groups"` object.

**The store.** A `PresetStore` in core:
- one file per preset in `QStandardPaths::AppDataLocation/presets` (core links
  Qt Core already);
- the directory is injected for tests;
- `defaultPresetDirectory()` is the one place the path is made, so the GUI, the
  CLI and Python all see the same presets (`main`'s ADR 0051 lesson);
- it can list, read, save, rename and remove;
- file names are sanitised, and names collide case-insensitively.

**GUI:**
- **Edit ▸ Copy Settings… (Ctrl+Shift+C):** a checklist of sections, remembered
  in `QSettings`.
- **Edit ▸ Paste Settings (Ctrl+Shift+V):** pastes onto the open photograph, as
  one history step.
- **A Presets list in the left dock, above History** (as in Lightroom's left
  panel; Snapshots will sit between the two):
  - a click applies a preset as one history step;
  - a + button saves the current state as a preset; its dialog pre-checks the
    sections that differ from the defaults (`main`'s ADR 0055);
  - a context menu renames or deletes, and a button opens the folder;
  - the list follows the folder, so a preset saved or deleted elsewhere shows
    up;
  - a tooltip lists the preset's sections.

**CLI:**
- `arraw-cli preset list | show <name> | apply <name> <files>…`, under
  `main`'s ADR 0051 rules:
  - `apply` writes the sidecars, with no decode;
  - every path is checked before anything is written;
  - a failure on one file does not stop the rest;
  - names match case-insensitively, never fuzzily.
- `export --preset <name>` applies the preset before the flags.
- ADR 006 already reserves `preset`.

**Python:** `arraw.with_look(...)`, plus `arraw.presets()`,
`arraw.load_preset(name)` and `arraw.save_preset(...)` over the same store.

### 4. Session history

**`EditSession` keeps one list and a position**, instead of two stacks. Each
step is a whole state, as before, so nothing changes in what it costs or how it
behaves. New API:
- `history()`: the steps, the opening state first;
- `position()`: the current step;
- `goTo(index)`: an open edit is committed first.

**A click navigates (decided).** It moves the position, as `main`'s
`QUndoStack::setIndex` and Lightroom do: the steps above stay listed and can be
redone, and the next edit drops them. The roadmap currently says "returning to
a step is one undoable change". That should hold for restoring a Snapshot only,
and the roadmap is corrected.

**Step names: amending ADR 022.**
- The library describes a step from its two neighbouring states:
  `describeChange(before, after)` gives the keys changed and their group.
- The front end words it:
  - one key: "Exposure +0.50", localised in the GUI through
    `SettingPresentation`, and the key's own name in Python;
  - one group: "Tone";
  - several: "4 settings".
- Some edits mean more than their difference (a paste, a preset, a reset). For
  those, `commit` and `setState` take an optional **origin**: an enum (`Edit`,
  `Paste`, `Preset`, `Reset`, `Crop`, and later `Restore`) plus a detail such
  as the preset's name.
- It is not free text, so the GUI still localises it and the list can never
  claim a change the states do not show.

**The dock.** History sits in the left dock, under Presets:
- a `QListView` over a `HistoryModel` in app-core, which is testable without
  widgets;
- newest on top, and "Opened" at the bottom;
- the current step highlighted, and steps that can be redone dimmed;
- a click or Enter goes to the step;
- the step that matches the saved state carries a small mark (optional);
- View ▸ Presets and History shows or hides the dock.

While the crop mode is open, the list is disabled. The crop session has its own
undo (ADR 040), and it becomes one "Crop" step when the mode closes. During a
slider drag the list changes only when the drag commits.

**Python binds `EditSession`:**
- `begin`, `update`, `commit`, `cancel`, `undo`, `redo`, `history` and
  `go_to`;
- `with session.edit():` as the context-manager form ADR 022 anticipated.

The CLI is one-shot and has no session, so it gets no history command.

### 5. Room for Snapshots

- Steps stay whole states, so a snapshot is a named whole state.
- Restoring one is `setState` with the origin `Restore`, one undoable step.
- The dock's layout reserves a "Snapshots" section between Presets and History.
- Storing them waits for step (c)'s structured sidecar lists.

## Steps

Each step is one workflow: Sonnet implements, then an Opus review is written to
`docs/reviews/`. I fix what the review finds, run the full suite, and commit.
Nothing is pushed.

1. **Geometry into core:** `CropGeometry.h`, `CropEditing` thinned, tests
   moved, ADR 014 and 040 notes. Behaviour identical.
2. **`Edits.h`:** the setters and the operations. The GUI, the CLI and Python use
   them. The CLI carries the crop (ADR 006 and 014 updated), and Python gets
   `edited`.
3. **History:** the `EditSession` list and position, `goTo`, origins and
   `describeChange` (ADR 022 amended), `HistoryModel` and the dock, Python's
   `EditSession`.
4. **Looks:** copy sections, `withLook`, `PresetStore` and a new ADR for the
   preset file. The GUI's copy and paste and the Presets list, the CLI's
   `preset` and `export --preset`, and Python.

The roadmap's line "returning to a step … is one undoable change" is corrected
to say that a history click navigates and only restoring a Snapshot is a step.
