# Settings of the negative sit beside the develop state

Status: accepted, 2026-10-17 (proposed 2026-10-16). The plan is
[negative-settings-plan.md](../ideas/negative-settings-plan.md).

A photographer will want to choose the demosaic and, later, turn lens
corrections on (`docs/todo.md`, "Demosaic and lens corrections as an import
group"). Both decide which pixels the decode hands to development, not what
development does with them. Today neither exists:

- `rawimport::load` hard-codes AHD (`user_qual = 3`, `RawImport.cpp`), and
  `DecodeOptions` has only `halfSize` ([ADR 005](005-raw-import-through-libraw.md),
  [ADR 031](031-thumbnails-start-from-embedded-previews-and-half-size-decodes.md)).
- The plan has no decode block yet ([ADR 012](012-a-render-resolves-from-a-photo-and-a-request.md)).
  A checkpoint ladder tells sources apart only by the address of the buffer it
  is bound to ([ADR 045](045-a-render-runs-one-stage-table-and-keeps-its-checkpoints-on-a-ladder.md));
  a caller that changes the pixels under it must clear it.
- Every edit goes through `DevelopState`: `EditSession::update`, `withValue`,
  `withLook`, the history steps and the sidecar's settings keys. A setting
  placed there would be one slider-shaped edit among many. Its cost would be
  different: a re-decode of 1 to 3 s for 24 MP ([ADR 043](043-the-gui-thread-never-decodes.md)).

`main` made the demosaic a develop setting that re-decodes through the load
path (its ADR 0036), and made lens corrections apply-once switches driven by a
profile (its ADR 0032). This ADR keeps both behaviours but chooses another
place for the settings. The user chose that place (Option C, 2026-10-16).

## Decision

### 1. A value of its own, held by `Photo` beside `DevelopState`

`NegativeSettings` (`include/NegativeSettings.h`) holds what shapes the
decoded negative. In Phase 1 it holds only the demosaic. `Photo` holds it as a
fourth part, beside `state()` and `marks()`: `negative()` reads it and
`with(NegativeSettings)` replaces it. `DevelopState` does not change.
`defaultNegativeFor(metadata)` gives the starting value, as `defaultStateFor`
does for the state ([ADR 039](039-noise-reduction-is-the-first-pass-and-reads-the-as-shot-luminance.md)).

The rule this makes structural is: **no develop edit can trigger a re-decode.**
Nothing that takes or returns a `DevelopState` can reach the negative.
`withValue`, the descriptor table, `EditSession::update` and the CLI's develop
flags never see it.

### 2. The demosaic is one of LibRaw's LGPL built-ins, stored as a token

`enum class Demosaic { Linear, Vng, Ppg, Ahd, Dcb, Dht, Aahd }`. These are
LibRaw's `user_qual` 0, 1, 2, 3, 4, 11 and 12, which are the algorithms left
in LibRaw since 0.21 dropped the GPL demosaic packs. Each enumerator is
persisted as a stable lower-case token (`"linear"`, `"vng"`, `"ppg"`, `"ahd"`,
`"dcb"`, `"dht"`, `"aahd"`) and never as a number, because LibRaw validates
nothing (ADR 005). **The default is `Ahd`**, which is what `loadImage` does
today (`RawImport.cpp`, `demosaicAhd = 3`), so every existing sidecar renders
as it did. An absent key always means AHD, whatever later becomes the default
for new photographs.

The setting applies only to a mosaiced RAW. For anything else, and for a
half-size decode (which combines 2x2 blocks and does not demosaic), the decode
does not read it. Section 4 makes that visible to the plan, so changing it
costs nothing there.

### 3. It is carried into the decode, and only from there

`DecodeOptions` gains `Demosaic demosaic = Demosaic::Ahd`. It is the one
options struct the decode takes; ADR 005's later `RawLoadOptions` is merged
into it rather than added beside it. One function builds it from a document:
`decodeOptionsFor(const Photo&, bool halfSize)`. It reads `negative()` and the
metadata and never reads `state()`. Every caller that holds a `Photo` (the
app's `PhotoLoader`, the export queue, `arraw-cli export`, Python's
`Photo.load` and `develop`) decodes through it. Without that rule, an export
would silently ignore the photographer's demosaic.

### 4. The plan's decode block describes it, so a new negative is a new source

`ProcessingPlan` gains `decode`, ADR 012's decode block, in its smallest form:
the `DecodeOptions` the source was decoded with, in canonical form. Canonical
means that for a non-RAW or a half-size decode the demosaic is replaced by the
default, because the decode never read it. The block is compared by every
prefix, before any stage. It is a **header of the plan, not a `Stage`**:
nothing runs it inside the stage driver, because the caller still supplies the
source. When the engine decodes for itself, it can become a stage.

The decoded buffer says how it was decoded. `ImageBuffer::decodedWith()` is
set by `loadImage`, kept by the pyramid's levels and empty for pixels a caller
built. `planFor(buffer, …)` copies it into the block. A rung from another
negative is therefore stale **by value** (`staleReason`), and does not rely
only on the ladder's buffer binding. The app also clears every ladder when it
installs a new source, as it does today. Both together mean a negative edit
clears every checkpoint ladder, and a develop edit never touches the block.

The file and its stamp, the rest of ADR 012's block, are not part of this. The
ladder's buffer binding still covers them.

### 5. History, unsaved changes and the sidecar cover it

- `HistoryStep` gains `negative`. `EditSession::setNegative(negative, origin,
  detail)` commits an open edit and then makes one step. An overload
  `setState(negative, state, origin, detail)` lets a paste or a preset change
  both in one step. `begin`/`update`/`commit` keep the negative as it is: a
  negative is chosen, never dragged. Undo, redo and `goTo` move it with the
  state, and the window re-decodes when the step it lands on has another
  negative. `hasUnsavedChanges` compares it, and `describeChange` names it
  (`ChangeDescription::negative`, keys such as `"demosaic"`).
- **Sidecar:** `arraw:demosaic="vng"` sits beside the settings keys. No version
  bump: an older arraw reports the key as unknown and keeps it on write, as
  `writeSidecar` already keeps unknown `arraw:` attributes. Reading an unknown
  token gives a warning and AHD. Writing follows the rule `xmp:Rating` already
  follows: the key is written only when the negative differs from what reading
  gave, so a token from a newer arraw survives until the photographer changes
  the demosaic. A sidecar with only `arraw:demosaic` records a negative and no
  state.
- **JSON:** a `"negative": {"demosaic": "vng"}` object goes beside
  `"settings"` in the documents that describe a photograph: `info --json`, the
  preset file and Python. It is not added inside `stateToJson`, which stays
  `DevelopState`'s.

### 6. Presets and copy/paste carry it, as their own section (user, 2026-10-16)

- `CopySection::Demosaic` (stable name `"demosaic"`) is copyable and is **in
  `defaultCopySections`**. Copying a look within one shoot is the usual case,
  and the same camera wants the same demosaic. A section is replaced, not
  merged, as every section is. `Look` gains `negative`, and `withLook` returns
  the target's new negative beside its state in `AppliedLook`.
- `sectionApplies(Demosaic, raw)` is false for a non-RAW. The section is then
  skipped and reported, by the mechanism White Balance already uses.
- A paste that leaves the negative equal costs nothing. One that changes it
  re-decodes the open photograph once, as one history step. Pasting or applying
  a preset to photographs that are not open writes sidecars and decodes nothing
  (`preset apply`, a later multi-photo paste). Their thumbnails are unchanged in
  Phase 1, since thumbnails are half-size decodes.
- A preset file carries the section when `"negative"."demosaic"` is present.
  The save dialog pre-checks the sections that differ from the defaults, so a
  look saved from an AHD photograph does not carry it.

### 7. Phase 2: lens corrections, sketched, for ADR 050

Distortion, vignetting and lateral CA become switches in
`NegativeSettings::lens`. They are applied once, on the CPU, to make a
corrected negative beneath spots, masks and the shaders. **The profile comes
from both the file and lensfun (user, 2026-10-16).** The proposed source mode
is `auto | embedded | lensfun`, and `auto` means:

1. the file's embedded correction data when arraw can read it (DNG opcodes,
   and the maker notes of the brands that store corrections there);
2. else lensfun, matched from the camera and lens EXIF (ADR 028);
3. else none, with a notice.

A manual lensfun lens overrides the match. The sidecar records the switches,
the mode and any manual lens. It does not record the resolved profile, whose
coefficients go into the decode block, so a lensfun database update yields a
new source. The manual lens is per shot and is never copied; the switches and
the mode are. lensfun is LGPL-3 and its database CC BY-SA 3.0, both compatible
with this branch's GPL-3. ADR 050 must still settle:

- packaging (Fedora `lensfun-devel`, Ubuntu `liblensfun-dev`, Windows via
  vcpkg; whether lensfun is required, as LibRaw is);
- the database format version;
- stage placement against `src/core/StageTable.h`;
- whether `auto` may mix sources per correction.

## Consequences

- Changing the demosaic costs one decode of the open photograph, off the GUI
  thread, through `PhotoLoader`'s decode lane. A newer choice or an undo
  cancels it (ADR 042). The old picture stays on screen until the new source
  lands.
- Undo and redo across a demosaic step each re-decode. A one-deep cache of the
  previous source would make a toggle instant, at about 200 MB for 24 MP. That
  is for the plan to measure, not a decision here.
- The export queue must not reuse a shown source of another negative. A job
  carries its negative and decodes for itself when they differ.
- `ThumbnailCache::developedKey` adds the negative when it is not the default,
  so existing keys stay as they are.
- ADR 012 is amended: its decode block exists, first as a plan header. ADR 045
  is amended: a rung's source is compared by value as well as by binding. ADR
  022/030 are amended: history and unsaved changes include the negative. ADR
  005 is amended: demosaic is no longer baked in; highlight handling still is.
- Default renders do not change: the render digests stay identical.
- LibRaw substitutes for some sensors (X-Trans, four-colour, monochrome,
  already-linear DNG). The token is what was asked for. What ran may differ, and
  the UI says so where arraw can tell from the metadata.

## What was rejected, and why

- **Option A: an Import group inside `DevelopState`** (a `SettingGroup::Import`
  with descriptor rows). It was the cheapest to build, because the descriptor
  table, JSON, sidecar, `withValue` and history would all have covered it for
  free. That is the problem: every path that edits a `DevelopState` could then
  re-decode, so "no develop edit re-decodes" would be a discipline in each
  caller rather than a property of the types. A slider-style `begin`/`update`
  would invite a re-decode per update. Presets, the CLI's develop flags and
  Python's `edited(**kwargs)` would all accept it as one more key. The plan
  would also have to look into the develop state to build its decode block,
  which is the dependency ADR 012 set out to remove.
- **Option B: settings stored on the decoded source** (an app-side choice
  passed to the loader, not part of the document). It would not persist, undo
  or travel with presets, and exports and the CLI would not see it.
- **A separate `RawLoadOptions` struct beside `DecodeOptions`.** Two structs
  would describe one decode, and the plan block would need both. One struct,
  built by one function from a `Photo`, is smaller.
- **Decode as a `Stage` row now.** The driver would gain a pass it cannot run,
  because the caller supplies the source, and every `Stage` index shifts. A
  plan header gives the same staleness rule without either cost.
- **Relying on the ladder's buffer binding alone.** It works for the app,
  which installs a new buffer, but it leaves ADR 012's GPU-cache hazard open
  and makes correctness depend on every caller clearing ladders. Kept only as a
  second line.
- **The GPL demosaic packs (AMaZE and others).** Licensing is not the obstacle
  (ADR 005); packaging is. They can be added later as tokens.

## Amendment, 2026-10-18: Phase 2 as decided (section 7)

[ADR 050](050-lens-corrections-are-a-stage-between-noise-reduction-and-the-chain.md)
changes section 7: lensfun is the only source in v1, there is no source field,
the sidecar records the automatic match, and the resolved profile goes into the
Lens block of the plan, not the decode block. The corrections are applied by a
cached stage after noise reduction, not in the decode.

## Amendment, 2026-10-19: section 7 has two switches and an amount

[ADR 050](050-lens-corrections-are-a-stage-between-noise-reduction-and-the-chain.md): of the lens settings, only distortion and lateral CA are switches.
Vignetting is an amount, 0 to 150 percent of the profile's stops (`NegativeSettings::lens`,
`LensSettings::vignettingAmount`), kept outside the descriptor table like the
switches.
