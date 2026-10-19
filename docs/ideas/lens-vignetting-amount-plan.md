# Vignetting amount — plan

Status: implemented, 2026-10-19. Decision record: the amendment of 2026-10-19 in
[ADR 050](../adr/050-lens-corrections-are-a-stage-between-noise-reduction-and-the-chain.md).
Why: `docs/todo.md`, step 11 (the a6400 + E PZ 16-50 profile overcorrects).

## Decisions

| Question | Answer | By |
|---|---|---|
| Switch or amount | The amount replaces the switch; 0 off, 100 the profile | user, 2026-10-19 |
| Scaling | `g ^ (amount / 100)`, range 0 to 150 | user, 2026-10-19 |
| Scope | Everything the switch reached | user, 2026-10-19 |
| C++ field | `float LensSettings::vignettingAmount`, default 0 | proposed |
| Constants | `minimumVignettingAmount` 0, `profileVignettingAmount` 100, `maximumVignettingAmount` 150, in `NegativeSettings.h` | proposed |
| Where applied | `withVignettingAmount` on the resolved table, at the end of `resolveLens` | proposed |
| Stored names | Unchanged: `arraw:lensVignetting`, JSON `"vignetting"`, change key `"lensVignetting"` | proposed |
| Legacy values | `True`/`False` and `true`/`false` read as 100/0; no version bump | proposed |
| Dragging | `EditSession::updateNegative` (amends ADR 022) | proposed, **user to confirm** |
| Python bool | Refused (`TypeError`), as for every number | proposed |

## The API the packages build on

```cpp
// include/NegativeSettings.h
inline constexpr float minimumVignettingAmount = 0.0F;   // no correction
inline constexpr float profileVignettingAmount = 100.0F; // the profile as it is
inline constexpr float maximumVignettingAmount = 150.0F;
struct LensSettings {
    bool distortion = false;
    float vignettingAmount = 0.0F; // percent of the profile's stops
    bool lateralCa = false;
    std::optional<RecordedLens> lens;
    constexpr bool any() const noexcept { return distortion || vignettingAmount > 0.0F || lateralCa; }
};
/// Gives lens settings with the corrections of another and their own recorded lens.
[[nodiscard]] constexpr LensSettings withCorrectionsOf(LensSettings settings,
                                                       const LensSettings& from) noexcept;

// include/LensResolution.h
constexpr LensKinds kindsOf(const LensSettings& s) noexcept
    { return {s.distortion, s.vignettingAmount > 0.0F, s.lateralCa}; }

// include/LensCorrection.h (package 2)
/// Gives a correction with its vignetting gain scaled in stops: each node g becomes
/// g ^ (amount / 100).
[[nodiscard]] LensCorrection withVignettingAmount(LensCorrection correction, float amount);

// include/EditSession.h (package 6)
/// Changes the negative settings provisionally, inside the open edit.
void updateNegative(NegativeSettings negative);
```

`withVignettingAmount` rules, in order:

1. `amount == profileVignettingAmount`, or the table is empty: return the input
   unchanged. This keeps 100 bit for bit equal to the old "on".
2. `amount <= 0`: clear `vignetting` and `vignettingFrom`. If the result is no
   longer `active()`, return `LensCorrection{}`, so it equals what
   `resolveLens` gives at 0.
3. Otherwise set each node to `static_cast<float>(std::pow(double(g), double(amount) / 100.0))`.
   A node that is not finite and positive stays as it is. Provenance, centre,
   aspect and fill stay as they are.

`validate(NegativeSettings)` throws `std::invalid_argument` when the amount is
not finite or lies outside `[minimumVignettingAmount, maximumVignettingAmount]`.

## Work packages

Package 1 lands first. After it, packages 2, 3, 4 and 6 touch disjoint files and
can run in parallel. Package 5 waits for 6. Package 7 (docs) can run at any time.
Package 8 (UI) waits for 2 and 6. Each package ends with `just format`,
`just format-check` and the test command it names. A package that touches
Python also runs `just py-test`.

### 1. The type and every call site (serial, first)

- `include/NegativeSettings.h`: the field, constants, `any()` and
  `withCorrectionsOf`, as above. Rewrite the briefs of `LensSettings` and
  `RecordedLens` ("when a correction is first turned on" now includes the
  amount leaving 0).
- `src/core/NegativeSettings.cpp`: the amount check in `validate`. Update its
  doc in the header.
- `include/LensResolution.h`: `kindsOf` as above.
- `src/core/EditSession.cpp` (`describeChange`): compare `vignettingAmount`,
  keeping the key `"lensVignetting"`.
- `src/core/Edits.cpp` (`withLook`): `result.negative.lens =
  withCorrectionsOf(result.negative.lens, look.negative.lens)`. In
  `include/Edits.h` and the `CopySection::Lens` comment in
  `include/SettingDescriptors.h`, "switches" becomes "corrections".
- `src/app/HistoryModel.cpp` (`wordLens`): the vignetting entry becomes an
  amount. At 0 it reads `tr("Lens: vignetting off")`, otherwise
  `tr("Lens: vignetting %1%").arg(QString::number(amount, 'f', 0))`. Several
  changed keys still read "Lens corrections".
- Interim shims, which later packages replace. Each keeps behaviour at 0/100:
  - `src/core/Sidecar.cpp`: read `lensVignetting` into a local `bool` with
    `flagIn`, then set 100 or 0. Write with
    `setFlag("lensVignetting", amount > 0, read > 0)`.
  - `src/core/SettingsJson.cpp`: write `flag(amount > 0)`. Read a bool as 100
    or 0.
  - `src/cli/LensOptions.cpp`: `vignettingAmount = kinds.vignetting ? profileVignettingAmount : 0`.
  - `src/cli/InfoCommand.cpp`: `settings.vignettingAmount > 0.0F`.
  - `src/python/BindLens.cpp`: `field("vignetting_amount",
    &LensSettings::vignettingAmount)`, which is final.
    `with_lens_corrections` keeps its `vignetting: bool | None` parameter
    mapped to 100/0 for now. Do the same in `src/python/BindSession.cpp`
    (`set_lens_corrections`).
  - `src/app/ui/DevelopPanel.cpp`: the vignetting checkbox gets its own
    handler, since `addSwitch` takes `bool LensSettings::*`. Checked sets
    `profileVignettingAmount`, unchecked sets 0, and it shows checked when the
    amount is above 0.
- Tests, mechanical: `vignetting = true` becomes
  `vignettingAmount = profileVignettingAmount`, and a test of a bool becomes a
  test against 100 or 0. Files: `tests/test_LensSettings.cpp`,
  `tests/test_ExportQueue.cpp`, `tests/test_LensStage.cpp`,
  `tests/test_HistoryModel.cpp`, `tests/test_MainWindowLens.cpp`,
  `tests/test_Cli.cpp`, `tests/test_DevelopPanel.cpp`,
  `tests/test_ThumbnailWorker.cpp`, and in `tests/python/test_lens.py` the
  `LensSettings(vignetting=…)` and `.vignetting` uses only. Regenerate
  `src/python/arraw/_arraw.pyi` (the `arraw-stubs` target).
- New tests:
  - `validate` refuses -1, 150.5 and NaN, and accepts 0, 37.5 and 150.
  - `any()` and `kindsOf` at 0, 0.5 and 100.
  - `withCorrectionsOf` copies the amount and keeps the lens.
  - A paste of `CopySection::Lens` carries 60 (`test_LensSettings.cpp`).
  - `describeChange` names `"lensVignetting"` for 100 → 60.
  - History reads "Lens: vignetting 60%" and "Lens: vignetting off".
- Verify: `just build`, `just test`, `just py-test`. A
  `grep -rn "\.vignetting\b"` over `src include tests` must show only
  `LensCorrection` tables and Python test code that package 5 owns.

### 2. The scaling, resolution and render identity (parallel, after 1)

- Files: `include/LensCorrection.h`, `src/core/LensCorrection.cpp`
  (`withVignettingAmount`), `src/core/LensResolution.cpp` (end of
  `resolveLens`: `return withVignettingAmount(std::move(*sampled),
  settings.vignettingAmount);`), `include/LensResolution.h` (doc),
  `tests/test_LensCorrection.cpp`, `tests/test_LensStage.cpp`,
  `tests/test_ThumbnailCache.cpp`, `tests/gpu/test_GpuLens.cpp`.
- Tests:
  - The math. At 50 each node is `sqrt(g)` (within 1 float ulp), and at 150 it
    is `g^1.5`. At 100 the result `==` the input. At 0 a vignetting-only
    correction gives `LensCorrection{}`, and distortion plus vignetting keeps
    distortion and drops `vignettingFrom`. An empty table is left alone. A node
    that is not positive stays as it is.
  - Identity with the fixture database (`[lensfun]`). `resolveLens` at 100
    `==` `sampleLensfun` with the vignetting kind. At 0 it `==` the old "off",
    meaning `resolveLens` with the vignetting kind false. For a in {25, 60, 150},
    `resolveLens(a)` `==` `withVignettingAmount(resolveLens(100), a)`.
  - Resume. A ladder rendered at 100, then a plan at 60: `canResumeFrom` gives
    the Denoise rung, as the toggle test does, and nothing is decoded. From 0
    to 60 also resumes from the Denoise rung.
  - The rendered pixels at 50 equal those of a correction with a
    `sqrt(g)` table built by hand (CPU).
  - `ThumbnailCache::developedKey` differs between 60 and 100 (the lens
    digest) and is the same at equal amounts.
  - GPU parity. A `"vignetting at 60 %"` case in "The lens pass matches the
    CPU" builds its table with `withVignettingAmount` and keeps the existing
    tolerance.
- Verify: `just test`, and `just test-lavapipe "[gpu][lens]"`.

### 3. Persistence and migration (parallel, after 1)

- `src/core/Sidecar.cpp`: replace the shim with `amountIn(descriptions,
  "lensVignetting", float&, log, subject)`:
  - `True` or `False`, in any case, reads as 100 or 0.
  - A finite `numberIn` value is clamped to the range. A clamp records
    `SettingClamped {key, read, limit}`, as `marksIn` does.
  - Anything else gets `reportUnreadable(key, "a number from 0 to 150, or True
    or False")` and reads as 0.

  To write, if the amount differs from what reading gave, write
  `shortestText(shortestDouble(amount))`, or remove the key at 0.
- `src/core/SettingsJson.cpp` (`negativeToJson`, `applyLensJson`): write
  `"vignetting": shortestText(shortestDouble(amount))`. To read:
  - A bool reads as 100 or 0.
  - A number is clamped, with `SettingClamped` and key `"lens.vignetting"`.
  - Anything else gets `reportMalformedList("lens.vignetting", "a number from
    0 to 150, or true or false")` and leaves the base.

  Update the docs in `include/SettingsJson.h` and `include/Sidecar.h`.
- Tests in `tests/test_LensSettings.cpp`:
  - The JSON and sidecar round-trip at 0, 37.5, 100 and 150.
  - Legacy `True`, `true`, `False` and `"vignetting": true` read as 100/0.
  - An untouched legacy `True` survives a save that changes something else.
  - 200 and -5 are clamped and logged. `"abc"` and `[1]` are reported, and the
    base is kept.
  - Amount 0 writes no key. A negative with no lens settings writes the same
    JSON as before.
- Verify: `just test`.

### 4. The CLI (parallel, after 1)

- `src/cli/LensOptions.h`: `corrections` becomes `std::optional<LensSettings>`.
  Its `lens` is always empty and is ignored.
- `src/cli/LensOptions.cpp`: `parseCorrections` takes the items
  `distortion`, `ca`, `vignetting` (= 100) and `vignetting=N`. N is a decimal
  in [0, 150] read with `std::from_chars`, and 0 is allowed. `all` is
  distortion, vignetting=100 and ca. `none` turns everything off.

  Usage errors:
  - an unknown item;
  - an empty item;
  - `vignetting=` with no number, a value that is not a number, or one out of
    range ("--lens-corrections: vignetting takes an amount from 0 to 150, as in
    vignetting=60");
  - a name given twice;
  - `all` or `none` combined with anything.

  `applyLensOptions` uses `withCorrectionsOf`. Update the help text.
- `src/cli/InfoCommand.cpp`: the text line reads `vignetting 60%`, or bare
  `vignetting` at 100. Leave the JSON code alone: `"negative"` gets the number
  from package 3. Whichever of packages 3 and 4 lands second adds the
  `test_Cli.cpp` check that `info --json` shows `"vignetting": 60`.
- Tests in `tests/test_Cli.cpp`: each accepted form, each usage error and its
  exit code, `export` at `vignetting=50` against a sidecar at 50, and the
  `info` text.
- Verify: `just test`, `just py-test` (`test_cli_parity.py`).

### 5. Python (after 1 and 6)

- `src/python/BindLens.cpp` (`with_lens_corrections`) and
  `src/python/BindSession.cpp` (`set_lens_corrections`): replace the
  `vignetting: bool | None` keyword with `vignetting_amount: float | None`.
  - Take an `nb::object` and convert it with `convertValue<float>`, so a
    `bool` is refused.
  - None keeps the value.
  - An amount out of range raises `ValueError` through `Photo::with` /
    `validate`.
  - `set_lens_corrections` records the match when `any()` becomes true, as
    before.
- `src/python/BindSession.cpp`: bind `update_negative(negative)`, with a
  docstring.
- Regenerate `src/python/arraw/_arraw.pyi`. Nothing changes in `__init__.py`.
- Tests in `tests/python/test_lens.py`:
  - `LensSettings(vignetting_amount=60)`.
  - `vignetting_amount=True` raises `TypeError`, as does `vignetting=`.
  - 200 raises `ValueError`.
  - `with_lens_corrections(vignetting_amount=50).lens_correction(db).vignetting`
    is the square root of the value at 100.
  - `begin`, then `update_negative`, then `commit` makes one step.
- Verify: `just py-test`.

### 6. A dragged negative in the session (parallel, after 1)

- `include/EditSession.h` and `src/core/EditSession.cpp`:
  - `baseline_` becomes `std::optional<Baseline>`, where `Baseline` holds
    `DevelopState state; NegativeSettings negative;`.
  - `begin` stores both.
  - Add `updateNegative(NegativeSettings)`. It throws `std::logic_error` with
    no open edit, and `std::invalid_argument` (with nothing changed) if the
    negative is invalid.
  - `commit` adds a step when either part differs, and `cancel` restores both.
  - `canUndo` and `canRedo` compare both.

  Update the class docs and the ADR 022 sentence (package 7).
- Tests in `tests/test_EditSession.cpp`:
  - A drag 0 → 30 → 60 is one step, and undo returns to 0.
  - `cancel` restores the negative.
  - A drag that ends where it began leaves no step.
  - `canUndo` is true during a drag.
  - State and negative changed in one open edit make one step.
  - An invalid negative changes nothing.
- Verify: `just test`.

### 7. One-line amendments (docs, any time)

ADR 022, 030, 045 and 049, with the wording at the end of ADR 050's 2026-10-19
amendment. ADR 009, 012, 031 and 044 need nothing. Their text holds, because
vignetting moves no frame and the thumbnail digest already covers the table.

### 8. The UI slider (after 2 and 6; not in the model work)

What it can rely on:

- `LensSettings::vignettingAmount` and the three constants;
- `EditSession::begin`, `updateNegative` and `commit(EditOrigin::Edit)`;
- `withMatchRecorded` (`src/app/LensSupport.h`), called once before the commit
  when `any()` and no lens is recorded;
- `withVignettingAmount` and `resolveLens`;
- the history wording from package 1.

Work in that package:

- `DevelopPanel` replaces `lensVignettingCheck` with a slider (0 to 150, step
  1, unit `%`, default 0, double-click resets to 0). It is enabled as the
  switches are. It emits begin, change and finish signals for the negative,
  which `MainWindow` maps to the session.
- `LensView` gains `profile`, the resolution at 100. `MainWindow::lensView()`
  keys its cache on the settings with the amount reduced to 0 or 100, and sets
  `correction = withVignettingAmount(profile, amount)`, so a drag never asks
  lensfun.
- Optional: `PreviewRenderer` may keep its background fallback when only the
  vignetting table changed, since vignetting moves no content.

## Risks

- **A bool assigned to the float** compiles silently (bool to float is not
  narrowing for a constant), which is why the field is renamed. Package 1's
  grep is the check.
- **The interim state between 1 and 3** writes `True` for any amount above 0.
  Only Python can set another amount then. Land 3 before anyone edits real
  sidecars.
- **The one-time change of thumbnail keys** for photographs with lens settings.
  No pixel changes.
- **`pow` on the host** is deterministic per platform. CPU and GPU read the same
  float table, so parity is unaffected. Two platforms may differ by an ulp,
  which no golden test compares.
- **One amount per photograph** while the profile's error depends on the
  aperture (1.26× at f/3.5 against 1.07× at f/7.1). A pasted amount may suit
  one aperture and not the other. A flat-field user profile is the real fix
  (`docs/todo.md`, step 11).

## Addendum 2026-10-19: the session records the lens match

Decided by the user (2026-10-19), after packages 1 to 7 landed: **the session
owns the rule**, not its callers. `EditSession` holds a non-owning
`const LensDatabase*`, set by whoever owns the database, and records the match
itself when it makes a step. One place enforces the invariant, so no caller can
forget it. This addendum supersedes the plan above wherever they differ:
package 5's "`set_lens_corrections` records the match … as before", and package
8's `withMatchRecorded`.

### The rule

A step that changes the lens settings (`before.negative.lens !=
after.negative.lens`), and leaves a correction on (`lens.any()`) with no lens
recorded, records the match (`recordLensMatch`) in that same step. Then:

- **It is applied where a step is made.** That is `commit` (and so `begin`,
  `goTo`, `undo`, `redo` and `save`, which commit an open edit first),
  `setNegative`, and the two-part `setState`. The match is recorded *before*
  the "did anything change" comparison. So a "Reset to automatic" that matches
  the same lens again, with the same stamp, leaves no step.
- **It is not applied on a move.** `update` and `updateNegative` never match.
  During a drag `photo()` carries the negative with no lens recorded. The
  window resolves it live, once per drag (package 8 keys `lensView` on the
  amount reduced to 0 or 100). One match costs about 0.8 to 0.9 ms (release
  build, the full system database of 1304 lenses, the 80D's CR2, from the
  `[.lensbench]` case). That is cheap enough to run on the commit path, on the
  interface thread. Nothing is deferred.
- **It is never applied by navigation or by restoring.** `undo`, `redo` and
  `goTo` move to a step as it was recorded. `cancel` and `discardChanges` go
  back to a baseline. Undo of the step that turned a correction on takes the
  correction and the record away together.
- **Develop-only steps never record**, and neither do steps that change only
  the demosaic. A develop edit cannot reach the negative (ADR 049). A step
  worded "Exposure" must not also pin a lens, because the history would then
  read "2 changes" (`HistoryModel`).
- **A chosen lens is never overwritten.** Any recorded lens (`matched` or
  `chosen`) makes the rule a no-op, as `recordLensMatch` already is. Only
  clearing the record lets the session match again. "Reset to automatic" in the
  window, and `reset_lens` in Python, do that. A paste keeps the target's own
  record (`withCorrectionsOf`).
- **`save` writes what the commit leaves.** It writes first, so that a failure
  leaves an open edit open. With an open edit that is due a record, it writes
  the photograph with the match recorded, then commits that photograph. Without
  this, a save in the middle of a drag would show unsaved changes straight
  after it.
- **Strong guarantee.** The recorded photograph is computed before anything is
  mutated. If matching throws, the edit and the history stay as they were.

### No database, and a database that arrives later

- **Null (not loaded, or none attached).** The step is made without a record,
  exactly as the caller gave it. Renders match live, and the step that next
  changes the lens settings records the match.
- **Arrival.** `setLensDatabase` changes only what the *next* step uses. An
  open edit therefore records at its commit if the database arrived during it.
  Committed steps and the current photograph are not touched: there is no
  retroactive record and no step that the photographer did not make. A record
  added later pins the lens that the live match chose at that moment, so it
  cannot change which lens an edit uses (§1). It would still be a change that
  nobody asked for.
- **The window still refuses** a paste that turns a correction on while the
  database is loading ("paste again once it has"). This is the code since
  2150c30 and its test, "A paste that needs the lens database waits for it…".
  ADR 050's 2026-10-18 note, which says such a paste "records the lens on the
  next toggle", was already wrong about the code. The switches and the slider
  stay disabled until the database loads. So in the window the session always
  has the database when a lens step is made.

### The API

```cpp
// include/EditSession.h
/// Starts editing a photograph; lensDatabase as setLensDatabase().
explicit EditSession(Photo photo, const LensDatabase* lensDatabase = nullptr);
/// Sets the lens database the session records matches in (non-owning; null for none).
void setLensDatabase(const LensDatabase* database) noexcept;
/// Gives the lens database the session records matches in, or null.
[[nodiscard]] const LensDatabase* lensDatabase() const noexcept;
```

The database must outlive the session, or be detached first. The session is
not thread-safe, so whoever calls it (the interface thread, or Python under the
GIL) sets the pointer too. `LensDatabase` lookups lock internally. Matching on
the interface thread while a worker samples a profile waits on that mutex,
exactly as `withMatchRecorded` did. Copying a session copies the pointer.

### What stays and what goes

- **Stays.** `recordLensMatch` (core, public): the primitive the session calls,
  and Python's `record_lens_match` for scripts that write a `Photo` without a
  session. `withChosenLens` stays as well.
- **Goes.** `app::withMatchRecorded` and `app::withLensReset`
  (`src/app/LensSupport.h/.cpp`). The window's reset clears
  `negative.lens.lens` and calls `chooseNegative`.
- **The CLI does not change.** It has no session and writes no sidecar, and
  `applyLensOptions` never recorded a match: an export resolves live, or
  through `--lens`. The brief that started this addendum said otherwise; the
  code says this.
- **`withLook`** does not change. Its doc in `include/Edits.h` now says that the
  session records the target's match when it commits the paste.
- **`describeChange` and `HistoryModel`** do not change. A toggle that records
  already reads "Lens: distortion on" (`lensOnly`), and a record alone names the
  lens.

### Python

- `EditSession(photo, lens_database=None)`. Here None means the process's
  database, as everywhere else in the Python API. It is loaded with the GIL
  released, and only for a RAW (for any other file the session holds none).
  `keep_alive` keeps a passed database alive as long as the session.
  `lens_database` is a read-only property.
- `set_lens_corrections` and `reset_lens` lose their `database=` keyword (an
  unmerged API). They build the negative and call `set_negative`, and the
  session records. `choose_lens(database=None)` looks the lens up in the
  session's database, else the process's.
- Session methods keep the GIL while they match (about 1 ms). Releasing it
  would let a second Python thread mutate the same session.

### Work packages

A first (alone). B and C after A, in parallel: their files are disjoint. D (the
slider, package 8) after A and B. Every package uses
`flock /tmp/arraw-build.lock just <recipe>` and
`uvx clang-format@22.1.8 -i <files>`.

- **A, core**: `include/EditSession.h`, `src/core/EditSession.cpp`,
  `include/Edits.h` (doc), `tests/test_LensSettings.cpp`. Old callers still
  build and pass. Their own record makes the session's a no-op.
- **B, window**: `src/app/LensSupport.h/.cpp`, `src/app/ui/MainWindow.cpp/.h`,
  `tests/test_MainWindowLens.cpp`.
- **C, Python**: `src/python/BindSession.cpp`, `src/python/BindLens.cpp` (doc),
  `src/python/arraw/_arraw.pyi` (target `arraw-stubs`),
  `tests/python/test_lens.py`.
- **D, slider**: as package 8, but it records nothing itself. It calls
  `begin`, `updateNegative` on each move, and `commit(EditOrigin::Edit)` or
  `cancel`. It never calls `recordLensMatch` or `setNegative` per move.
