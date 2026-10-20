# Review: vignetting amount and session-recorded lens match

Reviewer: claude-opus-5-5, 2026-10-19. Branch `lens-corrections`, uncommitted working tree
(48 files, +2053/-280). Design: `docs/ideas/lens-vignetting-amount-plan.md` (with the addendum)
and the 2026-10-19 amendments of ADRs 022, 030, 045, 049 and 050. The user decisions listed in the
brief were not reviewed.

## Verdict

**Ready to commit after the small should-fix items.** There are no blockers. The scaling, the
cache keys, the resume rung, persistence, the CLI and the session rule do what the plan and the
ADRs say, and the tests cover them. Two should-fix items are about code: the `setState` backup
copy, and the Python stub for `lens_database`. The third should-fix item is stale documentation.
Everything else is a nit.

## Findings

### Should-fix

**S1. `setState` copies the whole session to get the strong guarantee, and the restore path is
never run.** `src/core/EditSession.cpp:92-104`.
- With an open edit, `backup = *this` copies `history_`, which holds every step's `DevelopState`
  and its brush strokes. That happens on every paste or `setNegative` made during an open edit.
- The `catch` branch (`*this = std::move(*backup)`) has no test, because matching only throws
  on an allocation or lensfun failure.
- Fix: compute both records before anything changes, then mutate. `withLensRecorded` uses
  `before` only for the inequality, and `recordLensMatch` does not depend on it:
  ```cpp
  std::optional<Photo> committed;
  if (editing()) committed = withLensRecorded(baseline_->negative.lens, photo_);
  const Photo& base = committed ? *committed : photo_;
  next = withLensRecorded(base.negative().lens, next);   // may throw; nothing changed yet
  if (committed) commitPhoto(std::move(*committed), EditOrigin::Edit, {});
  ```
  This removes the copy and the untested branch.
- The same reordering also removes N1.

**S2. The Python stub gives `EditSession.lens_database` as non-optional.**
`src/python/arraw/_arraw.pyi:2337` (`def lens_database(self) -> LensDatabase`).
- The property is `None` for a PNG, and `test_a_session_of_a_png_has_no_database` asserts that.
- A type checker therefore accepts `session.lens_database.loaded` on any session, and it fails at
  runtime for a PNG.
- The stub is generated, and `test_stubs.py` passes because it only compares the stub with the
  generator's output. The fix belongs in the binding (`BindSession.cpp:158`): use
  `nb::for_getter(nb::sig("def lens_database(self, /) -> LensDatabase | None"))`, or a getter
  that returns `nb::object`. Then regenerate the stub.
- Related nit: `with_lens_corrections` and `set_lens_corrections` type `vignetting_amount` as
  `object | None`. A `nb::sig` with `float | None` would match the docstring.

**S3. Stale statements that contradict the new behaviour.**
- `docs/todo.md:51`: "three switches (off by default)".
- `docs/todo.md:66-67`: "a paste before the database has loaded matches live and records on the
  next toggle". ADR 050 (2026-10-19) says this never matched the code, so todo.md now
  contradicts the ADR.
- `src/app/ui/DevelopPanel.cpp:650`: "three switches without sliders".
- `docs/ideas/lens-vignetting-amount-plan.md:3`: "Status: proposed". It is implemented.
- The todo has no entry for this work. Add one under the lens section, or as a "Done" line.

### Nits

**N1. A negative built before an open edit commits can drop the record that the commit just
made.** `EditSession.cpp:90`.
- `next` is built from the pre-commit `photo_`, which has no record yet. If that negative also
  turns every correction off, the step clears the record. That breaks "taking the corrections off
  keeps it" (`include/NegativeSettings.h:115`).
- Python repro: `begin()`, `update_negative(amount 60)`, then
  `set_negative(<negative read before begin, all off>)`.
- I found no way to reach this in the window. A checkbox cannot turn everything off while only the
  slider's correction is on.
- S1's reordering fixes it, provided `next` keeps the committed record whenever the caller's
  lens equals the pre-commit lens apart from the record. Otherwise, document it as "as given".

**N2. A drag while zoomed in throws away the whole-frame fallback on every move.**
`src/app/PreviewRenderer.cpp:866-869` resets the background whenever `backgroundLens != job->lens`,
and that comparison includes the vignetting table. Vignetting moves no content, so a lens that
differs only in `vignetting` could keep the fallback, as a non-geometry state change does. The
plan marked this optional and it was skipped. Visible only on region renders.

**N3. Fractional amounts are rounded where they are shown.**
- `src/app/HistoryModel.cpp:74` uses `QString::number(amount, 'f', 0)`. An amount in (0, 0.5) reads
  "Lens: vignetting 0%".
- The slider (`decimals = 0`) shows 37.5, set from Python, the CLI or a sidecar, as 38.
- Neither changes the data. A nudge then snaps the value to whole numbers, which is acceptable.
  Consider `'g'` with 3 significant digits in the history text.

**N4. Python on a non-RAW does not check `vignetting_amount`.** `BindLens.cpp:301-306` returns
before `convertValue`, so `png.with_lens_corrections(vignetting_amount=True)` (or `"x"`, or 200)
succeeds silently. A RAW refuses the same call. Convert first, then return early.

**N5. Python has no way to say "no database", and the constructor loads it eagerly.**
- `EditSession(raw, lens_database=None)` always means the process's database.
- It also loads that database in the constructor (about 66 ms the first time), even for scripts
  that never touch lens settings.
- The behaviour is as decided. It is listed under Open questions.

**N6. Stale "switch" wording.**
- `include/LensResolution.h:42,55,67,73` ("Switches and the recorded lens", "no switch on").
- `src/app/LensSupport.h:60,67,84`.
- `src/app/ui/MainWindow.h:499` (lensView "resolved when the switches … change"; it is now keyed
  on the amount reduced to 0 or 100 and rescaled otherwise) and `:563`.
- `DevelopPanel.cpp:838` ("no switch can be set").

**N7. Style.**
- `src/core/LensCorrection.cpp:1-2`: `<NegativeSettings.h>` sits in the own-header group, with no
  blank line after `<LensCorrection.h>`. CLAUDE.md puts the own header in a group of its own.
  clang-format cannot tell the difference, since both use angle brackets.
- `include/EditSession.h:385`: the `withLensRecorded` brief says "Records … in @p after", but the function
  returns a copy. It should read "Gives @p after with the lens match recorded when …".
- `LensOptions.cpp`: the amount error does not go through the `fail` lambda, so it lacks the usage
  prefix. This is deliberate, judging by the test, but it is inconsistent.
- `NegativeSettings.cpp:44`: the message hard-codes "0 to 150" instead of using the constants.

**N8. Test gaps** (none hides a known bug):
- No GPU case at 150 (gains above the profile). The 60 % case is in `test_GpuLens.cpp:245`.
- No window test that a drag does not re-resolve. The `window.lens` timing span could be counted.
  The identity it relies on, `resolveLens(a) == withVignettingAmount(resolveLens(100), a)`, is
  tested in core (`test_LensCorrection.cpp`, the `[lensfun]` case).
- The `setState` restore path is untested (S1).
- The UI's cancel-on-failure for a negative drag is untested.
- Covered after all: export at 0 through the CLI (`exportWith("vignetting=0") == plain`).

## What I verified, and how

- **Full CPU suite.** `flock /tmp/arraw-build.lock just test` gave 1967/1968. The one failure is
  "In the window a stroke repaints only where it painted", the known pre-existing flake.
- **Lavapipe run.** `flock … just test-lavapipe "[gpu][lens]"` gave 1968/1968 passing. The recipe
  ran the whole suite, the GPU lens cases included.
- **Python.** `flock … just py-test`: 725 passed, 2 skipped. `test_stubs.py` passes.
- **Formatting.** `just format-check` is clean.
- **Scaling (read).** `withVignettingAmount` (`LensCorrection.cpp:99-115`):
  - Exactly 100, or an empty table, returns the input unchanged, so 100 is bit for bit the old "on".
  - `amount <= 0` clears the table and `vignettingFrom`, and gives `LensCorrection{}` when nothing
    is left active.
  - Non-finite or non-positive nodes are kept.
  - Distortion, the fill scale and `distortionFrom` are untouched.
  - A NaN amount would turn every node into NaN, but `validate` keeps NaN out of `LensSettings`.
  - `resolveLens` applies it last (`LensResolution.cpp:174`), so the CPU, the GPU, the thumbnail
    worker and the CLI export all get the scaled table.
- **Cache keys (read).**
  - `LensPlan` compares the whole correction by value (`src/core/Lens.h:22`), so another amount
    resumes from the Denoise rung. The test is in `test_LensStage.cpp`.
  - `developedKey` digests the table and the negative's JSON.
  - `brushFrameOf` reads distortion only, so the brush cache is unaffected. This matches the ADR.
- **Session (read and tests).**
  - The baseline holds both the state and the negative.
  - `cancel`, `canUndo` and `canRedo` compare or restore both.
  - The record is made before the "anything changed" check, so a reset that matches the same
    lens leaves no step (tested).
  - The record is made only when `before.lens != after.lens`.
  - `recordLensMatch` never replaces a recorded lens and does nothing when `!any()`.
  - `save` records before it writes and commits the same photograph.
  - Navigation, `cancel` and `discardChanges` never match.
- **Window (read).**
  - Sessions get the database on open, on reload from disk and on `lensDatabaseLoaded`.
  - The slider is disabled while the database loads and in crop mode (it is in
    `nonGeometryGroups_`).
  - The paste refusal is kept.
  - The `lensView` cache is per `OpenPhoto`, so switching photographs cannot serve a stale view.
    The key is the settings with the amount reduced to 0 or 100, plus the database.
  - `syncDecode` compares decode options only, so a drag never decodes again.
- **Tests that fail without the change (read).**
  - "A dragged negative is one history step": with a state-only baseline, the commit makes no step.
  - "Cancelling an edit restores the negative": the negative would stay at 90.
  - The sidecar round trip at 37.5 needs the number to be written.
  - The legacy `True` survival test checks the "differs from what reading gave" rule.
- **Persistence (read and tests).**
  - Photographs without lens settings write the old JSON and no sidecar key.
  - A sidecar at 0 writes no key.
  - Clamping and malformed values are logged under the right keys.
- **ADR truth.**
  - 022's amendment matches `EditSession.cpp`.
  - 045's matches the ladder test.
  - 050's session section matches the code: where the database is set, when it records, the paste
    refusal, the CLI unchanged.

## Could not verify

- Hardware GPU (lavapipe only), and the GPU at 150.
- Real-time behaviour of the slider in the GUI: smoothness, and whether lensfun is called once per
  drag. I did not launch the app.
- The strong-guarantee restore in `setState` (no fault injection).
- Whether `nb::keep_alive<1, 3>` with a `None` argument is a no-op in this nanobind version. The
  PNG test passes, which suggests it is.

## Open questions for the user

1. Should Python get a way to have no database (for example `lens_database=False`), and should
   the process's database load lazily, at the first lens step, instead of in the constructor?
2. Should the history text and the slider show fractional amounts (N3), or are whole percent
   values enough?
3. Should N2 (keeping the zoomed fallback during a vignetting drag) be done now or recorded in
   todo.md?
