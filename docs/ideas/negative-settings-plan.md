# Settings of the negative: demosaic now, lens corrections later — plan

Status: Phase 1 done, 2026-10-17 (proposed 2026-10-16). Decision record:
[ADR 049](../adr/049-settings-of-the-negative-sit-beside-the-develop-state.md).
Answers `docs/todo.md`, "Demosaic and lens corrections as an import group".

## Decisions (user, 2026-10-16)

| Question | Answer |
|---|---|
| Where the settings live | Option C: a `NegativeSettings` value held by `Photo` beside `DevelopState` |
| Delivery | Phase 1 demosaic; Phase 2 lens corrections (own ADR, 050) |
| Presets and copy/paste | They carry it: `CopySection::Demosaic` now, `Lens` later |
| Lens data source | Both lensfun and the file's embedded profile |

## What the code does today (verified)

- `rawimport::load` sets `user_qual = 3` (AHD) for every full decode
  (`src/core/RawImport.cpp:37,145`). `half_size` skips the demosaic, and a
  linear DNG is halved afterwards. **The Phase 1 default is AHD.** LibRaw here
  is 0.21.5, while ADR 005 was measured on 0.22.2.
- `DecodeOptions` holds only `halfSize`. `loadImage` is called by
  `PhotoLoader` (full), `ThumbnailWorker` (half), `arraw-cli export` (full,
  `loadImage(input)`, with no options) and Python's `load`, `Photo.load` and
  three `develop` paths in `BindPhoto.cpp`.
- The plan has no decode block. `CheckpointLadder` is bound to a source buffer
  by address, and `PreviewRenderer::setSource` drops ladders and the pyramid.
  `MainWindow::decodeLanded` installs the buffer, and `ExportJob` reuses
  `open_->decoded`.
- `EditSession` history steps hold only a `DevelopState`. Paste is
  `withLook` followed by `setState(…, Paste)` on the open photograph only.
  `PresetStore` (looks plan step 4) is not built yet.
- The sidecar keeps unknown `arraw:` attributes on write. `xmp:Rating` and
  `xmp:Label` are written only when they differ from what was read.
- `ThumbnailCache::developedKey` hashes `settingsToJson(state.settings)`, plus
  the masks when there are any.

## Phase 1 steps

Each step builds, passes `just test`, and is reviewed before the next.
Sonnet implements, and Opus reviews into `docs/reviews/`.

### 1. The type

- `include/NegativeSettings.h`, `src/core/NegativeSettings.cpp`:
  - `enum class Demosaic { Linear, Vng, Ppg, Ahd, Dcb, Dht, Aahd }`;
  - `demosaicTokens` (stable), `tokenOf(Demosaic)`, and
    `parseDemosaic(std::string_view) -> std::optional<Demosaic>`, which is
    case-sensitive and takes lower case only;
  - `struct NegativeSettings { Demosaic demosaic = Demosaic::Ahd; }` with `==`;
  - `validate(const NegativeSettings&)`, which checks that the enumerator is
    in range;
  - `defaultNegativeFor(const ImageMetadata&)`.
- `rawimport` maps the enumerator to `user_qual` in one `switch` and nowhere
  else. DCB's `dcb_iterations` and `dcb_enhance_fl` are set explicitly, and so
  are `four_color_rgb` and `med_passes`: ADR 005's rule is that every parameter
  is set.
- `ImageMetadata` gains `mosaic` (`Bayer | XTrans | None`), from LibRaw's
  `idata.filters` and `colors` at `open_file`. It decides whether the demosaic
  applies. *Alternative:* skip it, treat every RAW as mosaiced, and say nothing
  for sensors where LibRaw substitutes.
- **Tests:** token round-trip for every enumerator, unknown and upper-case
  tokens refused, the default is AHD, `validate` refuses a cast value, and
  `mosaic` is read on the fixtures.

### 2. Photo and decode plumbing

- `Photo` gains `negative()`, `with(NegativeSettings)` and a constructor
  `Photo(path, metadata, NegativeSettings, DevelopState, PhotoMarks = {})`. The
  existing constructors give `defaultNegativeFor(metadata)`.
- Move `DecodeOptions` into `include/DecodeOptions.h`, so that `ImageBuffer.h`
  can name it without a cycle. Add `Demosaic demosaic = Demosaic::Ahd` and add
  `canonical(DecodeOptions, const ImageMetadata&)`. That function resets the
  demosaic for a non-RAW, a half-size decode or `mosaic == None`.
- `decodeOptionsFor(const Photo&, bool halfSize)` reads `negative()` and never
  reads `state()`.
- `ImageBuffer::decodedWith() -> std::optional<DecodeOptions>` is set by
  `loadImage` (canonical), copied by pyramid levels and `halved`, and empty for
  buffers a caller builds.
- Switch every `Photo` holder to `decodeOptionsFor`: `PhotoLoader::decode(path,
  DecodeOptions)`, `ExportCommand.cpp:1152` and the four Python call sites.
  `ThumbnailWorker` stays half-size, so the demosaic is moot.
- **Tests:**
  - two fixtures with Bayer detail decode differently under VNG and AHD, and
    identically for a half-size decode;
  - `decodedWith` is set, and is canonical for a JPEG;
  - the CLI exports a sidecar with `vng` differently from one with none.
  - The committed fixtures are synthetic, and flat fields demosaic alike, so add
    a small generated DNG with a fine pattern.

### 3. Plan decode block and ladder invalidation

- `ProcessingPlan::decode` (`std::optional<DecodeOptions>`) is filled by
  `planFor(buffer, …)` from `decodedWith()` and by `planFor(photo, …)` from
  `decodeOptionsFor`. `prefixMatches` compares it before any stage. The test
  that "the plan has no member outside `stagesOf`" names it as the one header.
- `staleReason` reports "decoded differently" for a mismatched block.
- **Tests:**
  - **a negative edit clears ladders:** render through a ladder from buffer A
    (AHD), then from buffer B (VNG, same size). B's render has
    `resumedFrom == nullopt`, and a rung of A put against B's plan is stale by
    `staleReason` even when the same buffer is reused (the by-value test);
  - **a develop edit never re-decodes, in core:** for every row of
    `developSettingDescriptors`, set a non-default value through `withValue`
    and check that `decodeOptionsFor(photo.with(edited))` and `plan.decode` are
    unchanged;
  - the render digests (`[.digest]`) are identical for the default.

*Alternatives:*

- (a) `Stage::Decode` as table row 0, as ADR 012 literally says. Rejected for
  now in ADR 049, because it shifts every stage index for a pass the driver
  cannot run.
- (b) No plan change: rely on `setSource` clearing ladders. This is the
  smallest change, but it leaves the GPU-cache hazard of ADR 012 open.

### 4. Persistence: sidecar and JSON

- `readSidecar` reads `arraw:demosaic` into `SidecarContents::negative`
  (`std::optional<NegativeSettings>`). An unknown token gives
  `Notice::SettingMalformed` (or a new `NegativeSettingUnknown`) and AHD.
  `openPhoto` uses it.
- `writeSidecar` writes `arraw:demosaic` only when the photograph's negative
  differs from what reading the file gives (the rating rule), so an unknown
  token survives. A new sidecar gets the key only when the value is not AHD.
  `sidecarVersion` stays 1.
- JSON: `negativeToJson` and `applyNegativeJson` for the object
  `{"demosaic": "vng"}`. `info --json` gains `"negative"`.
- **Tests:**
  - round-trip;
  - absent means AHD;
  - an unknown token is warned about and kept on a write that does not change
    the negative;
  - a sidecar with only `arraw:demosaic` records a negative and no state;
  - an existing sidecar of develop settings is unchanged byte for byte in
    meaning after a write with AHD.

*Open:* write the key always or only when it changes, as recommended here.
Writing it always is more explicit, but every older arraw then warns about an
unknown key on every sidecar.

### 5. EditSession and history

- `HistoryStep::negative`.
- `setNegative(NegativeSettings, origin, detail)` commits an open edit first
  and leaves no step when the value is equal.
- `setState(NegativeSettings, DevelopState, origin, detail)` is one step for a
  paste or a preset.
- `begin`/`update`/`commit` keep the negative.
- `hasUnsavedChanges`, `save`, `discardChanges` and `goTo`/undo/redo cover the
  negative.
- `describeChange` gains `ChangeDescription::negative` (keys, `"demosaic"`).
  `HistoryModel` words it "Demosaic: VNG".
- **Tests:**
  - undo and redo of a demosaic step restore both parts;
  - `update` cannot change the negative;
  - a negative-only change counts as unsaved;
  - `discardChanges` restores it;
  - the step is named.

### 6. Looks, copy/paste and presets

- `CopySection::Demosaic` and its name `"demosaic"`, in `copyableSections` and
  `defaultCopySections`. `sectionApplies(Demosaic, raw)` is false for a
  non-RAW.
- `Look::negative`, `lookOf(const Photo&)`, and
  `withLook(const Photo&, …) -> AppliedLook { DevelopState state;
  NegativeSettings negative; skipped }`. The existing overload stays for
  callers without a photo; it refuses `Demosaic` with `invalid_argument`.
- The Copy Settings dialog gains a "Demosaic" row, ticked by default and shown
  as not applicable for a non-RAW. Remembered sections in `QSettings` that
  predate it get `Demosaic` added once (a migration key), or the first copy
  after the update would silently leave it out.
- Looks plan step 4 (`PresetStore`) is amended: the preset file's `"negative"`
  object, the carried sections being those whose keys are present, and the
  save dialog pre-checking Demosaic only when it is not AHD. If step 4 is not
  built when this lands, this becomes one of its requirements.
- **Tests:**
  - a paste with Demosaic changes the target's negative in one step;
  - one without it leaves the negative;
  - a RAW to JPEG paste skips it and reports;
  - an equal negative makes no re-decode (step 8);
  - a preset with and without `"negative"`.

### 7. App: placement and re-decode flow

**Placement (proposal):** a **"Negative" group at the top of the Adjustments
tab**, collapsible as in ADR 048, with a "Demosaic" combo. The group is
disabled for a non-RAW, and a note appears when `mosaic` says LibRaw
substitutes. Phase 2's lens switches join it.

*Alternatives:*

- a Photo ▸ Demosaic submenu of radio actions, which says "a property of the
  file" but is hard to find;
- a third dock tab, "Negative" or "Import", which sets the costly settings
  apart and is worth having once lens corrections add more rows.

**Flow:**

1. Picking a value calls `session.setNegative`. The window compares
   `photo().negative()` with the negative its source was decoded with, which
   `DecodedPhoto` gains. It makes the same comparison after every undo, redo,
   `goTo`, paste, preset and discard: this one check is the only re-decode
   trigger.
2. If they differ: `photoLoader_.decode(path, decodeOptionsFor(photo, false))`.
   The lane's newest-wins rule cancels any re-decode in flight. If the photo
   returns to the shown source's negative, `cancelDecode()` runs.
3. Meanwhile the **old source stays installed and shown**, unlike opening a
   photo, which clears it. The render pie reports `ProgressStep::Decode`, and
   the status line says "Decoding with VNG…". Develop edits keep rendering
   against the old source. *Alternative:* disable the develop dock until the
   decode lands, which is simpler to reason about but blocks the user for 1 to
   3 s.
4. `decodeLanded` with a matching request installs the buffer through
   `setSource`, which clears the ladders, the pyramid and the GPU upload, and
   requests a render.
5. Failure keeps the old source and shows a warning. The step stays so that
   undo takes it back. *Open:* revert it automatically instead.
6. Cancel means choosing again, undoing, or leaving the photograph. No
   separate button.
7. The demosaic is disabled in the crop mode, as history is.

**Export queue:** `ExportJob` gains `negative`. Its `source` is set only when
the shown buffer's `decodedWith` equals the job's. Otherwise the export thread
decodes for itself.

**Thumbnails:** `developedKey` adds `negativeToJson` when the negative is not
the default. Half-size thumbnails ignore the demosaic, so nothing refreshes in
Phase 1.

**Tests:**

- `test_MainWindow*` with a **counting `PhotoLoader::Decoder`** (already
  injectable): open is 1 decode; slider edits, a paste without Demosaic, undo
  and redo of develop steps all stay at 1; setting VNG makes 2, and undo makes
  3. **This is the "develop edit never re-decodes" test in the app.**
- Two quick choices give one delivered decode.
- An export queued during a re-decode uses the job's negative.

### 8. CLI

- `export --demosaic <token>` applies on top of the sidecar, as the develop
  flags do, and after `--preset` once that exists. A bad token is a usage error
  that lists the tokens.
- `info` prints "Demosaic: VNG" when it is not the default (all of them with
  `--all`), and `--json` prints `"negative"`.
- `preset apply` writes the negative with the sidecar and decodes nothing.
- **Tests:**
  - flag parsing;
  - a sidecar's `vng` is honoured by `export` (the export test of step 2);
  - `--demosaic` overrides the sidecar;
  - the `info` output.

### 9. Python

- `arraw.Demosaic` (enum; `str()` gives the token) and
  `arraw.NegativeSettings(demosaic=...)`.
- `Photo.negative`, `Photo.with_(negative=...)`, and `Photo.load()` and
  `develop` decoding with the photo's negative.
- `arraw.load(path, half_size=False, demosaic=Demosaic.AHD)`.
- `EditSession.set_negative`, `set_state(state, negative=None, …)`,
  `HistoryStep.negative` and `ChangeDescription.negative`. `.pyi` stubs are
  regenerated.
- `Photo.edited(**kwargs)` stays develop keys only, so `edited(demosaic=…)` is
  a `TypeError` by design.
- **Tests:** in `tests/python`, mirroring steps 5, 6 and 8.

### 10. Docs

- Amend ADRs 005, 012, 022, 030 and 045, as listed in ADR 049's consequences.
- Update `docs/todo.md`.
- Add the demosaic to `desired-features.md` if it is missing there.

## Risks

- **Silent wrong pixels in exports.** Any `loadImage(path)` left at a `Photo`
  holder ignores the demosaic. Step 2 replaces every one of them, and a test
  pins the CLI.
- **A stale shared source** in `ExportJob` during a re-decode. Step 7 covers
  it.
- **Synthetic fixtures** may not show demosaic differences. They need a
  patterned DNG, and a real camera file should be checked by hand once.
- **LibRaw substitutes for some sensors:**
  - for X-Trans it runs its own 1- or 3-pass method for most tokens;
  - for four-colour sensors it falls back to VNG;
  - monochrome and linear DNG files are not demosaiced at all.

  The token can then claim more than ran. Check the mapping against LibRaw's
  `dcraw_process` in step 1 and pin it in a test.
- **Undo churn:** each undo across a demosaic step costs a decode. Measure it,
  then decide on a one-deep source cache, which costs about 200 MB plus the
  pyramid for 24 MP.
- **Peak memory during a swap:** the old source, its pyramid and the GPU copy,
  plus the new decode.
- **Forward compatibility of tokens:** the read-gave rule keeps unknown
  tokens, but such a photograph renders with AHD and only says so in a warning.
- **The copy-dialog migration:** without it, users with a remembered selection
  never carry Demosaic.
- **Phase 2 moves content** under existing crops and masks when distortion is
  toggled, if the corrected negative is beneath them. ADR 050 must decide the
  frame that crop and mask coordinates live in.

## What to measure

- Full decode time per token on a 24 MP Bayer RAW, and on X-Trans if one can be
  had, with 8 threads, with and without film-strip load. Use ADR 043's
  `[.bench]` and generated DNG.
- Latency from choosing a token to the first new picture, and the GUI thread's
  longest gap during it, which must stay at ADR 043's few ms.
- Peak RSS during a swap, with and without the one-deep cache.
- Undo-toggle latency with and without the cache.
- Render digests before and after for the default. They must be identical.

## Phase 2 sketch: lens corrections (ADR 050)

- **Model:** `NegativeSettings::lens` holds:
  - `distortion`, `vignetting` and `lateralCa` (`bool`);
  - `source` (`Auto | Embedded | Lensfun`);
  - `manualLens` (`std::optional` lensfun maker and model).
- **Sidecar keys:** `arraw:lensDistortion`, `arraw:lensVignetting`,
  `arraw:lensLateralCa`, `arraw:lensSource`, `arraw:lensMaker` and
  `arraw:lensModel`.
- **Precedence (proposed):** `auto` uses the file's embedded data when arraw
  can read it, then lensfun matched from EXIF (ADR 028: camera, lens, focal
  length, aperture, distance), then nothing with a notice. The embedded data is
  first because it is the maker's own and the only one for some
  Micro Four Thirds lenses. `embedded` and `lensfun` force a source, and
  `manualLens` overrides the lensfun match.
- **What is recorded where:** the resolved profile (source, identity,
  coefficients) goes into the decode block, so a lensfun database update makes
  a new source. `info` shows it, and the sidecar does not.
- **Embedded readers, in order of value (to confirm):**
  - DNG `OpcodeList` `WarpRectilinear` and `FixVignetteRadial`;
  - Fuji, Sony, Panasonic and Olympus maker notes, through Exiv2 or LibRaw.
- **Copy/paste:** `CopySection::Lens` carries the switches and the source
  mode, and is ticked by default. `manualLens` is per shot and never copied.
- **Stage placement**, against `src/core/StageTable.h`:
  - (a) all of it in the decode block, before Denoise. This is the simplest
    and literally "a corrected negative", but Denoise then sees resampled,
    correlated noise.
  - (b) lateral CA and vignetting in the decode block, distortion folded into
    the Geometry warp. One resampling and best quality, but crop and mask
    frames then sit before the correction.
  - (c) a `Stage::Lens` between Denoise and Pointwise, cached by its rung.
  - Thumbnails need the correction too: the half-size path applies it.
- **Packaging:**
  - lensfun is LGPL-3 and its database CC BY-SA 3.0, both compatible with
    GPL-3.
  - Fedora `lensfun-devel` (the rpm requires `lensfun`), Ubuntu
    `liblensfun-dev` and `liblensfun-data-v1`, Windows via vcpkg (port to
    verify), macOS via Homebrew.
  - On Linux, use the distribution's database plus `lensfun-update-data`. On
    Windows, bundle the database with its attribution.
  - Database format: v1 (0.3.x) or v2 (0.3.95+).
  - Required or optional: ADR 005's argument against optional decoders
    suggests required.

## Open questions

1. **Demosaic ticked by default** in Copy Settings (proposed: yes, with the
   migration of remembered selections). The alternative is unticked, like
   geometry.
2. **The sidecar write rule** for `arraw:demosaic`: written only when changed
   (proposed), or always.
3. **Edits during a re-decode:** they render on the old source (proposed), or
   the dock is disabled.
4. **A failed re-decode:** keep the step (proposed), or revert it
   automatically.
5. **A one-deep source cache** for undo toggles: decide after measuring.
6. **`ImageMetadata::mosaic`:** add it now (proposed), or skip it.
7. **Phase 2 precedence detail:** whether `auto` may mix sources per correction
   (embedded distortion with lensfun vignetting), or picks one source for all.
8. **Phase 2:** stage placement, and the frame crop and mask coordinates live
   in. This is for ADR 050.
