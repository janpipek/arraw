# Review: negative settings, Phase 1 (steps 1-9)

Reviewer: claude-opus-5-5, 2026-10-16. Scope: the uncommitted work on branch
`negative-settings` (`git diff` plus untracked files), checked against
[ADR 049](../adr/049-settings-of-the-negative-sit-beside-the-develop-state.md) and
[the plan](../ideas/negative-settings-plan.md), steps 1-9. No production code was changed.

Ran: `arraw-tests "[negative]"` (40 cases pass), `arraw-widget-tests "[negative]"` (7 pass),
`arraw-tests "[.digest]"` (passes, so the default render is unchanged). Reproduced finding 1
in the real `arraw-ui` (see there).

## Findings, by severity

### 1. Critical: the window re-decodes forever when the negative canonicalises away

`decodeOptionsFor` returns the **canonical** options, so the demosaic is reset to AHD for a
non-RAW or a RAW with `mosaic == None`. `PhotoLoader::decode` then rebuilds the
`DecodedPhoto::negative` from those canonical options (`src/app/PhotoLoader.cpp:155`).
`decodeLanded` stores it as `decodedNegative` (`src/app/ui/MainWindow.cpp:2228`, `:2253`).
`syncDecode` compares it with the **uncanonicalised** `photo().negative()`
(`MainWindow.cpp:2272-2296`). When the two never agree, each landing calls `refreshPanel()`,
which calls `syncDecode()`, which starts another decode. Nothing sets `failedNegative_`,
because each decode succeeds.

Ways to get there, all of them ordinary:
- a PNG/JPEG/TIFF whose sidecar has `arraw:demosaic="vng"`. `openPhoto` takes the sidecar's
  negative whatever the file's kind is (`src/core/Photo.cpp`, `openPhoto`), and Python's
  `with_(negative=…)` plus a save writes such a sidecar;
- pasting a look with Demosaic from a Bayer RAW onto a linear DNG or a monochrome RAW.
  `sectionApplies(Demosaic, raw)` checks only `raw` (`src/core/Edits.cpp:263`), and the
  combo is disabled for that file but the paste is not.

**Reproduced:** `arraw-ui` (offscreen, CPU) opened `testcard-61x41-srgb8.png` with a
one-attribute sidecar `arraw:demosaic="vng"`. Under gdb, `arraw::loadImage` ran **4925 times
in 8 s**. Without the sidecar it ran 2 times. The status line stays on "Decoding with VNG…",
and every pass reinstalls the source, which clears the ladders, the pyramid and the GPU upload.

No test covers it. The only non-RAW app test opens a PNG with no sidecar.

Fix options:
- (a) **Recommended.** Compare decodes, not negatives. Keep the canonical
  `DecodeOptions` of the shown pixels (`open_->decoded->decodedWith()` already holds them).
  Compare them with `decodeOptionsFor(photo, false)` in `syncDecode` and in the export
  decision (finding 3). This is what plan step 7 says for the export queue. It also means a
  negative change that cannot alter the pixels never costs a decode.
- (b) Carry the requested `NegativeSettings` through `PhotoLoader::decode` as its own
  argument, instead of rebuilding it from the options. Smaller, but a JPEG with VNG then
  still costs one pointless decode, and Phase 2 adds more fields to keep in step.
- (c) Also make `sectionApplies(Demosaic, …)` look at `mosaic`. This is a nice-to-have
  beside (a) or (b), not a fix on its own: the sidecar route stays open.

### 2. Medium: a failed re-decode is never retried after the user moves away and back

`failedNegative_` is cleared only when a new decode starts (`MainWindow.cpp:2289-2292`).
Take this sequence: choose VNG, the decode fails (`:2216`), undo to AHD, then redo or choose
VNG again. `wanted == failedNegative_` returns early with no decode and no message. The
combo says VNG, the pixels are AHD, and an export (which decodes for itself, finding 3)
renders VNG. So the picture shown and the picture exported differ, and nothing says so.
Fix: reset `failedNegative_` in the branch where `wanted == open_->decodedNegative`, or keep
it only while the photograph stays on that step.

### 3. Medium: the export queue decides and decodes by its own rule, not by `decodeOptionsFor`

- `MainWindow.cpp:1809` shares `open_->decoded` only if `decodedNegative == photo.negative()`.
  The plan says it should compare the buffer's `decodedWith` with the job's options. Because
  of the canonicalisation in finding 1, every export of a file whose negative canonicalises
  away decodes a second time. The result is correct but costs time and memory.
- `src/app/ExportQueue.cpp:96` builds `DecodeOptions{.demosaic = job.negative.demosaic}` by
  hand. ADR 049 §3's rule is "one function builds it". Today the result is the same, because
  `loadImage` canonicalises. In Phase 2 (lens), the export would silently skip any new field.
  Suggestion: make `ExportJob` carry the canonical `DecodeOptions` from
  `decodeOptionsFor(photo, false)` in place of `negative`, and compare them with
  `decoded->decodedWith()`.
- A smaller point: decode warnings now reach `ExportResult::warnings` only when the job
  decodes for itself (`log` moved above the decode). This is inconsistent, but harmless.

### 4. Medium: the tests miss the invariants that broke, and some of the plan's

- No app test for: a non-RAW or `mosaic == None` photo with a non-default negative
  (finding 1); the failure path; `goTo`; discard; reloading a sidecar changed on disk;
  **an export queued during a re-decode** (an explicit plan step 7 test). The ExportQueue
  unit test only shows that a null `source` decodes with the job's negative. It does not
  test the window's choice.
- `test_MainWindowNegative.cpp` checks `shown()` (the combo) and the decoder's log, never
  the pixels. One check that `wholeFrameImage()` differs between AHD and VNG would pin that
  the new source is really installed.
- "No develop edit changes how a photograph is decoded" (`test_SettingDescriptors.cpp`)
  writes sentinels and calls `Photo::with(DevelopState)`. That cannot touch the negative by
  construction, so the test is close to tautological. It is fine as a type pin, but it does
  not go through `withValue`, which the plan asked for. The real behavioural pin is the
  app's counting decoder, which covers exposure, undo/redo and a paste without Demosaic.
  That is enough, but it is thinner than the plan's list.
- Plan step 1 asked to check LibRaw's substitutions (X-Trans, four-colour) against
  `dcraw_process` and pin them in a test. This was not done (finding 5).

### 5. Low-medium: no X-Trans or four-colour handling (plan steps 1 and 7, ADR consequences)

`mosaicOf` sets `Mosaic::XTrans` (`src/core/RawImport.cpp`, `mosaicOf`), but nothing reads
it. The panel note covers only `!raw` and `Mosaic::None`
(`src/app/ui/DevelopPanel.cpp:731`). The ADR says "the UI says so where arraw can tell".
For X-Trans, LibRaw runs its own 1- or 3-pass method, and four-colour sensors get VNG, so
the combo can claim an algorithm that did not run. `canonical` also does not merge tokens
that give identical pixels on X-Trans, so changing between them re-decodes for nothing.
Either add the note and a mapping test, or record in the plan that this is deferred.

### 6. Low: a render of the old source can refill the crop mode's cached image

`decodeLanded` resets `lastCropImage` (`MainWindow.cpp:2230`). A render of the old source
that is already queued in the event loop can still land afterwards in the crop mode and
`emplace` it again (`:2064`). The key is the `DevelopState` alone, so the next entry to the
crop mode treats an image from the old demosaic as exact. The effect is cosmetic only. One
fix is to add the decode options to the key, or bump a generation in `decodeLanded`.

### 7. Low: the thumbnail key is not wired, and arguably should not use the negative

`developedKey` gained a `negative` parameter, but `ThumbnailWorker.cpp:258` does not pass
it. In Phase 1 that is harmless, since thumbnails are half-size, and in fact correct.
Passing the raw negative later would refresh thumbnails needlessly. Suggestion: key on
`decodeOptionsFor(photo, /*halfSize=*/true)`, which is empty in Phase 1 and picks up lens
corrections in Phase 2 by itself. Then amend the ADR's sentence about it.

### 8. Low: smaller points

- `toRgbaF32` drops `decodedWith` for a buffer that is not already F32
  (`src/core/SampleConversion.cpp:41-43`). Nothing plans from a converted buffer today, but
  it is a trap for the by-value rule.
- `info --all` prints "Demosaic: AHD" for a linear DNG (`src/cli/InfoCommand.cpp:533`),
  where the value means nothing. The CLI's upper-casing duplicates the app's
  `demosaicDisplayName`.
- Python's `ImageMetadata` does not expose `mosaic`, so scripts cannot tell whether the
  demosaic applies.
- Unknown-token rule: with a sidecar holding `"amaze"`, the photograph reads as AHD. The
  user cannot keep AHD and clear the token in one step, because AHD is already shown. This
  follows the documented rating rule, so it is fine, but the ADR could mention it.
- Nit: `src/core/NegativeSettings.cpp` includes `<ImageImport.h>` before its own header.
  CLAUDE.md wants the own header first, quoted, as `Photo.cpp` does.

## What is fine

- **The type boundary holds.** Nothing that takes or returns a `DevelopState` reaches the
  negative. `begin`/`update`/`commit`/`cancel` keep it. `Photo::with(DevelopState)` and
  the Python `edited` path carry it unchanged, and `edited(demosaic=…)` is a `TypeError`.
  In the app, develop edits, undo/redo of develop steps and a paste without Demosaic make
  no decode (counting-decoder test).
- **Every `Photo` holder decodes through `decodeOptionsFor`:** `PhotoLoader` (open and
  re-decode), `arraw-cli export` (`ExportCommand.cpp:1175`), and Python's `Photo.load`,
  `develop`, `sample` and `curve_histogram`. No `loadImage(path)` without options is left
  at a `Photo` holder. `ThumbnailWorker` is half-size by design. The one bypass is the
  export queue's hand-built options (finding 3).
- **Decode block:** `ProcessingPlan::decode` is filled from `decodedWith()`/
  `decodeOptionsFor` and compared first in `prefixMatches` and `staleReason`, with its own
  reason. `halved` and `clone` keep it, and level 0 of the pyramid is the source itself, so
  CPU and GPU rungs (both through `LadderAccess`/`staleReason`) are stale by value. The
  ladder test reuses one buffer and still detects the change. The field-count guard names
  the header. Default digests are unchanged.
- **RAW import:** one `switch` maps the enumerators to `user_qual` and throws on anything
  else. `dcb_iterations`, `dcb_enhance_fl`, `four_color_rgb` and `med_passes` are set
  explicitly. The new patterned DNG fixture really separates AHD from VNG, and half-size
  decodes stay identical.
- **The GUI flow** (apart from findings 1 and 2):
  - newest-wins works through the decode lane, and a stale landing is ignored by request id;
  - going back to the shown negative cancels the decode;
  - opening another photograph resets the re-decode state;
  - the old source stays on screen, and edits keep rendering on it;
  - the indicator is not ended by renders while a re-decode is awaited;
  - the Negative group is disabled in the crop mode, and so is `chooseNegative`;
  - a reload of a sidecar changed on disk keeps `decodedNegative` and lets `syncDecode` decide;
  - an export during a re-decode decodes the job's own negative.
- **Sidecar:**
  - the read-gave write rule follows the rating rule, and an unknown token is reported
    (`SettingMalformed`, not `SettingUnknown`) and kept;
  - AHD writes no key, and `writeSidecarMarks` leaves the key alone;
  - a sidecar holding only the demosaic records a negative and no state;
  - there is no version bump. Tests cover each case.
- **Copy/paste:**
  - `Demosaic` is in the default sections, and the overload without a photo refuses it;
  - RAW to non-RAW is skipped and reported;
  - a paste is one step through `setState(negative, state)`, and undo brings back both parts;
  - the QSettings migration adds Demosaic once and respects a later uncheck, with tests;
  - the dialog row is disabled from a non-RAW.
- **History and saving:** `HistoryStep::negative`, `goTo` moving both parts,
  `hasUnsavedChanges`, `discardChanges`, `SavedRole` and "Demosaic: VNG" wording are all
  correct and tested.
- **CLI and Python:**
  - `--demosaic` is applied over the sidecar, and a bad or upper-case token is a usage
    error that lists the tokens;
  - `info` and `info --json` gain `"negative"`;
  - Python has `Demosaic` (whose `str()` gives the token), a frozen `NegativeSettings`,
    `Photo.negative`/`with_(negative=)`, `load(demosaic=)`, `set_negative` and
    `set_state(…, negative=)`, plus `HistoryStep`/`ChangeDescription.negative` and stubs.
  - The CLI and Python export tests agree on the pixels.

## Verdict

**Not ready to merge.** Finding 1 is a real, reproduced runaway decode loop, reachable from
an ordinary sidecar or a paste. Fixing it as in 1(a), by comparing canonical decode options
rather than negatives, also resolves most of finding 3. Fix finding 2 alongside, and add app
tests for a non-mosaiced file with a non-default negative, the failure-then-return path, and
an export queued during a re-decode. Findings 5-8 can be fixed or explicitly deferred in the
plan. Everything else matches ADR 049 and is well built.

## Re-review, 2026-10-17

Reviewer: claude-opus-5-5. Scope: HEAD `14fbab6` (into which most fixes were folded: the
`decodedOptions`/`failedOptions_` rework, `ExportJob::decode`, the new app tests) plus the
uncommitted diff (crop-image floor, `toRgbaF32`, X-Trans note, four more app tests). No
production code was changed. Ran, in `build/container-debug` (up to date):
`arraw-widget-tests "[negative]"` (15 cases pass), `arraw-tests "[negative]"` (41 pass).

### The findings, one by one

1. **Re-decode loop: fixed.** `OpenPhoto::decodedOptions` holds the canonical options of the
   shown pixels (`result.options`, which `PhotoLoader` passes through from
   `decodeOptionsFor`), and `syncDecode` compares them with `decodeOptionsFor(photo, false)`
   (`MainWindow.cpp:2281-2282`). Both sides go through `canonical`, so a VNG sidecar on a PNG
   or a pasted VNG on a linear DNG compares equal and decodes nothing. The sidecar reload
   carries `decodedOptions` across `open_.emplace`; `showPhoto` seeds it, but `syncDecode`
   ignores it until `decoded` is set, and `decodeLanded` then overwrites it with what really
   ran. No stale-options path found. Tests "A sidecar asking for a demosaic of a file that has
   none decodes once" and "A paste of the demosaic onto a file that has none decodes nothing"
   would loop (and fail on `started == 1`/`== 2`) under the old negative comparison.
2. **Failed re-decode retry: fixed.** `failedOptions_` is reset when the wanted options equal
   the shown ones (`:2284`), when a new decode starts (`:2297`), on open and on close. The
   test fails, undoes, redoes and expects a third decode; with the old rule the redo returns
   early and `settled(2)` times out. Choosing a third value in between also clears it.
3. **Export queue: fixed.** `ExportJob` carries `decode` from `decodeOptionsFor`, the window
   shares its source only when `decodedOptions == decode` (`:1810`), and `execute` decodes
   with `job.decode` (`ExportQueue.cpp:96`). A file whose negative canonicalises away now
   shares the shown source. During a re-decode `decodedOptions` is still the old value until
   `decodeLanded` replaces `decoded` and `decodedOptions` together on the GUI thread, so the
   shown source cannot be shared for the wrong options; a failed re-decode leaves the export
   decoding the chosen options itself (and failing the same way, reported). The new test
   "An export queued during a re-decode renders the chosen demosaic" pins the race through the
   real dialogs. Note that it would also have passed before the fix (the old rule also
   declined to share on a mismatch): it guards the invariant, not finding 3's efficiency point.
   Nothing tests that a PNG with a VNG sidecar *shares* the source.
4. **Tests: largely addressed.** Added: non-mosaiced file with a non-default negative (PNG
   and linear DNG), failure then return, `goTo` across a demosaic, discard, a sidecar changed
   on disk, export during a re-decode, and a pixel check (`wholeFrameImage()` differs).
   The `test_SettingDescriptors` tautology and the LibRaw substitution pin (step 1) remain.
5. **X-Trans / four-colour: partly.** The panel now notes X-Trans (`DevelopPanel.cpp:738`).
   Still open: four-colour sensors (VNG regardless) get no note; `canonical` does not merge
   tokens for X-Trans, so switching between them still re-decodes for identical pixels; no
   mapping test; the plan still says "pin it in a test" rather than recording a deferral.
6. **Crop-mode cache: fixed.** `cropImageFloor = latestRequest_` in `decodeLanded`, and
   `showResult` caches only `result.request > cropImageFloor`. Request ids are the same
   counter, so every render requested before the new source was installed is excluded; a
   render requested just before `setSource` but run on the new pixels is merely not cached
   (conservative). No test, acceptable for a cosmetic issue. Nit: the sidecar reload
   re-creates `OpenPhoto` with `cropImageFloor = 0` while carrying `lastCropImage` over;
   harmless today, since `leaveCropMode(true)` precedes it, but carrying the floor too would
   keep the invariant local.
7. **Thumbnail key: unchanged** (`ThumbnailWorker.cpp:258`). Still harmless in Phase 1;
   should be decided (or deferred in the plan) before Phase 2.
8. **Smaller points:** `toRgbaF32` now keeps `decodedWith` (fixed, untested). Still open:
   `info --all` prints a demosaic for a linear DNG, Python `ImageMetadata` lacks `mosaic`,
   and `NegativeSettings.cpp` still includes `<ImageImport.h>` before its own header.

### New observations

- **Low: discarding on close or on opening another photo starts a full re-decode.**
  `confirmLeavingPhoto` → `discardChanges()` → `refreshPanel()` → `syncDecode()` now
  decodes the saved negative at full size. On opening another photo `showPhoto` cancels it
  at once; on closing the window it runs (or is cancelled by the loader's destructor) for
  nothing. The new discard test drives it through `window.close()` and *asserts* that decode
  (`settled(3)`), so it pins the wasted work as behaviour. Options: skip `syncDecode` when
  the window is going (a flag set around the close path), or test discard through a path
  that keeps the window, e.g. a cancelled close or the reload prompt.
- **Nit: the history test** relies on the list being newest-first and on exact row counts;
  fine, but fragile if the "Opened" row ever changes.

### Verdict

**Merge-ready.** Findings 1, 2, 3 and 6 are resolved correctly, without new races or stale
options, and the critical loop is now covered by tests that would have caught it. Remaining
items are low: the four-colour/X-Trans canonicalisation and LibRaw pin (finding 5), the
thumbnail key (7), the small points in 8, and the re-decode started by discard-on-close.
Fix or record them as deferred in `docs/ideas/negative-settings-plan.md` before Phase 2.
