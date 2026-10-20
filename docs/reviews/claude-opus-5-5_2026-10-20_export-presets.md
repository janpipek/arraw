# Review: export presets (branch `export-presets`, `main..de1cbed`)

Reviewer: Claude Opus 5.5, 2026-10-20. Scope: commits e0e31b5..de1cbed, checked against
ADR 054 and `docs/ideas/export-presets-plan.md`. Tests were not re-run (green at HEAD per the
request). One compile check was run with `-fsyntax-only` to confirm finding M1.

**Counts:** critical 0, medium 3, low 11.

Overall the core is solid. The file format follows ADR 051: older files are read, a newer one
gives a warning and is never written, unknown keys and unusable entries survive a write, and a
fixture test is in place. The lock, re-read, change and `QSaveFile` cycle is correct. Moving
`ExportSettings` changes no behaviour: `saveExportSettings`/`restoreExportSettings` are
byte-for-byte the old code, with the same `export/*` keys and int encodings. CLI precedence
follows the ADR. The defects are at the edges.

---

## Medium

### M1. The two new notices have no name in the CLI JSON log or in the app debug log

- `src/cli/StreamDiagnostics.cpp:22-105` (`nameOf(Notice)`) and `src/app/DebugLog.cpp:33-118`
  (`noticeName`) were not given `ExportPresetUnusable` / `ExportPresetsUnreadable`.
- Confirmed with the compiler: `-Wswitch: enumeration value 'ExportPresetUnusable' not handled`
  appears in both files (and the same for `ExportPresetsUnreadable`). `DebugLog.cpp:34` says
  "Exhaustive and without a default, so that a new notice fails the build". It doesn't fail,
  because warnings are not errors, so the warning was missed.
- **Scenario:** `arraw-cli export-preset list --log-format json` with a hand-made `"format":
  "avif"` entry prints `"notice": "unknown"`, so a script that filters on notice names cannot
  see it. The app's debug log (Export with Preset menu, `MainWindow.cpp` fill) shows an empty
  notice column.
- **Fix:** add `export_preset_unusable` / `export_presets_unreadable` and the matching Qt names.
  Add a CLI JSON-log test for one of them. Consider `-Werror=switch` for these tables.

### M2. A preset hidden by a built-in cannot be renamed or deleted in the app (the known deviation)

- `src/app/ui/ExportDialog.cpp:319` adds a shadowed preset disabled, and `findPreset`
  (`:270`) skips shadowed presets, so it can never be chosen. Rename and Delete act only on
  `chosen_`. ADR 054 §8 says such a preset "is listed disabled ... and can be renamed or
  deleted". §9 calls rename and remove "the way out". The store allows both
  (`ExportPresetStore.cpp:222-263`).
- **Verdict:** this is not acceptable as it stands. The likely trigger is not a hand edit but
  an upgrade. A later arraw adds a built-in named, for example, "Instagram 1080". A user who
  already saved "instagram 1080" then sees their preset greyed out in both the dialog and
  Export with Preset. The app offers no way to recover it, and the CLI is read-only. Only
  Python or a text editor can rename it, and the user's settings are effectively lost from the
  UI.
- **Options:**
  (a) Make the shadowed item selectable for management only: choosing it applies its settings
  and enables Rename and Delete, Update stays disabled, and `chosenPreset()` returns nothing,
  so `export/preset` never stores a name that resolves to the built-in.
  (b) Keep the item disabled and add "Hidden presets ▸ Rename…/Delete…" to the More menu.
  (c) Keep the deviation and amend ADR 054 §8 and §9, and plan step 7, to say so. This is the
  least work, but it leaves the upgrade trap above.
  (a) is the closest to the ADR. No test covers either behaviour.

### M3. `setAside()` moves any file, including a newer or now-readable one, and does not re-check under the lock

- `src/core/ExportPresetStore.cpp:265-295` checks only that the file exists. ADR 054 §6 defines
  set-aside for an **unreadable** file. Python exposes it with no guard
  (`BindExport.cpp:245`).
- **Scenario 1:** a script built against an older arraw calls `store.set_aside()` after a
  `NEWER_FILE` error ("start fresh"). The newer arraw's file is renamed, and the next `save`
  writes a v1 file. The newer arraw then shows none of its presets. The data survives under
  `export-presets.unreadable-*.json`, but this defeats ADR 051's "never write over newer".
- **Scenario 2 (TOCTOU):** the dialog opened on an unreadable file, so it shows Set Aside…
  (`ExportDialog.cpp:394`). Meanwhile Python sets the file aside and saves a new, good file,
  or the user fixes the JSON by hand. Clicking Set Aside then moves the good file away.
- **Fix:** under the lock, re-read and refuse unless the state is `Unreadable`. A new
  `ExportPresetProblem`, or `NotFound` with a clear message, would do. Alternatively, allow
  `Newer` only behind an explicit `force`. Add a store test for each case.

---

## Low

### L1. The "(modified)" mark stays on items that are no longer chosen
`ExportDialog.cpp:371-378` rewrites only the current item's text. Choose A, change quality, then
choose B (or "No preset"): the drop-down still lists "A (modified)". Restore the plain name of
every item, or of the previous index, before marking the current one.

### L2. Tooltips on menu actions are never shown
Neither `ExportDialog.cpp:233` (the More menu) nor `MainWindow::fillExportPresetMenu` calls
`QMenu::setToolTipsVisible(true)`. So the reasons ADR 054 §8 requires are invisible: "newer
file" on Update/Rename/Delete, and "unknown format 'avif'" and "Hidden by the built-in…" on
disabled presets. The tests check `toolTip()` but not that it shows.

### L3. The app throws away the file's diagnostics
`ExportDialog.cpp:279` calls `store_->read()` with the default `discardedDiagnostics()`. Clamped
and malformed values, and unknown keys, are never reported in the app. A hand-edit typo such as
`"quality": "85"` (a string) silently becomes 90 the next time any preset is saved, because
`writeExportPresets` re-encodes every usable entry. Route the dialog's reads to the debug log,
as `fillExportPresetMenu` does, or show a one-line notice.

### L4. A case-only rename is refused when a duplicate entry exists
`ExportPresetStore.cpp:240-245` compares the target against **every** other entry, including
the unusable "duplicate name" ones. File `[{"name":"Web"}, {"name":"web"}]`: `rename("Web",
"WEB")` throws `NameTaken`, although ADR §5 says a change of case only is a rename. Skip entries
whose `reason` is "duplicate name", or compare only against usable entries plus the
first-of-fold.

### L5. `export --config-dir` without `--export-preset` is a usage error
`ExportCommand.cpp:889-891`. ADR §8 says `export` takes `--config-dir`, with no condition. A
wrapper script that always passes `--config-dir` fails with exit 2 on plain exports. Ignore the
flag silently, or add it to `ignoredResizeOptions`-style "ignored" warnings, rather than fail.

### L6. `arraw.save(settings=…)` silently ignores the size and overrides the file extension
`BindPhoto.cpp:833-836`: `optionsOf` always sets `format`. Two cases:
- `save(img, "x.tif", settings=web.settings)` writes JPEG bytes into `x.tif`.
- Unless the caller also passes `develop(size=settings.develop_size)`, a "Web 2048 px" preset
  exports at full size.

Both are documented in the docstring, but they are easy traps. Options: a convenience such as
`arraw.export(photo, path, settings)`, or a `ValueError` when the extension names another
format.

### L7. Public header style
- `include/ExportPresets.h:3` uses `#include "ExportSettings.h"`. It is the only quoted include
  in `include/`, and CLAUDE.md says to use `<ExportSettings.h>`.
- It relies on transitive includes for `DiagnosticLog`/`discardedDiagnostics`
  (`<Diagnostics.h>`), `std::span`, `std::filesystem` and `std::runtime_error`.

### L8. Doc comments that are now wrong
- `ExportPresets.h:49`: `presets` is "Usable presets, sorted by folded name", but `read()` puts
  the built-ins first and includes shadowed ones.
- `ExportPresets.h:27`: `operator==` "Compares the name as written and every setting", but it
  also compares `builtIn`/`shadowed`.

### L9. The docs step is not done, and statuses are stale
- Plan step 9 is missing: the ADR 006 note, `desired-features.md`, the looks plan's
  `develop-presets/` amendment, and committing `docs/todo.md`.
- `tests/cli/ExpectExit.cmake` lacks the exit-2 case for an unknown preset (plan step 5).
- ADR 054's status line still says "the front ends follow", and the plan says "steps 1 to 4
  implemented".
- ADR 054 lines 366-367 duplicate a sentence fragment ("the CLI and Python do not set it
  (section 2).").

### L10. Test isolation of the configuration directory is weaker than the plan says
- `arraw-widget-tests` has its own `main` (`tests/test_SettingsDialog.cpp:14`) and sets no
  `ARRAW_CONFIG_DIR`. Any `MainWindow` built outside `test_MainWindowExport.cpp`'s
  `ConfigScope` uses `~/.qttest/config/arraw` (Qt test mode). That location is not the user's
  real one, but it persists across runs.
- `tests/main.cpp:17` and `conftest.py` keep a pre-set `ARRAW_CONFIG_DIR`. A developer who
  exports it for daily use runs the suite against their real presets.

The plan's risk section asked that "every test binary" set the variable. Set it
unconditionally, or refuse to run when it points under `$HOME` outside a temporary directory.
The concurrency test (`test_ExportPresetStore.cpp:248`) uses two threads of one process.
Adding a cross-process case (two `arraw-cli`-free Python processes, or a helper executable)
would test what the ADR actually promises.

### L11. Small UX and portability points
- Set Aside's confirmation (`ExportDialog.cpp:537`) names only the source. ADR §6 says both
  paths, and the dialog never reports where the file went.
- Save As offers a built-in's name as the default text and then fails with `BuiltIn`. Suggest
  "<name> copy" instead.
- `MainWindow.cpp` logs a missing store with `qWarning`, while the plan says the debug log.
- On Windows, `path.string()` in error messages (`ExportPresetStore.cpp:89, 94, 104, …`,
  `ExportPresetCommand.cpp:161, 230`) throws for paths that the ANSI code page cannot
  represent. Prefer a `u8string`- or `QString`-based conversion.

---

## Checked and fine

- **Versioning:** a newer file gives `Newer` plus `NewerSettingsVersion` and every write throws
  `NewerFile`, tested byte for byte. Unreadable covers non-JSON, a non-object, a missing or
  non-integer `arraw`, a missing list and size over 1 MiB. A blank file reads as empty and is
  writable. The `v1` fixtures are frozen and the test requires `v<current>`.
- **Unknown keys:** kept at the top level, per entry, and in `size`/`metadata`. Unusable entries
  are written verbatim.
- **Names:** trim, NFC, 1-64 code points, control characters including U+2028/2029. Folding is
  `toCaseFolded`. Sorting is folded first, then by code point (UTF-8 byte order). The first of
  a fold wins and later ones are unusable.
- **Store:** `QLockFile` waits 2 s, and its stale-PID breaking is tested. The file is re-read
  under the lock. `QSaveFile` without direct-write fallback keeps a symlink a link (tested).
  Directories are created only on write.
- **Built-ins:** four, in order, never written. `save`, `rename`-to and `remove` throw
  `BuiltIn`. A shadowing file preset is kept, flagged, and the built-in wins in `load` and in
  the CLI.
- **Config directory:** `GenericConfigLocation + "/arraw"`, and a non-empty `ARRAW_CONFIG_DIR`
  is made absolute. The rule is a pure function under test.
- **CLI:** the order is defaults, then preset, then flags. `--no-allow-upscale` exists and
  contradicts `--allow-upscale`. A preset's 16 bits under JPEG become 8, while `--bit-depth 16`
  stays an error. Unknown, unusable and unreadable presets give exit 2 with the file and an
  `export-preset list` hint.
- **Python:** bindings and stubs agree, `ExportPresetError` is a `RuntimeError` with
  `.problem`, and `save(settings=…)` with a clashing keyword raises `TypeError`.
- **Export with Preset:** it leaves `export/*` last-used settings and `export/preset` alone,
  which is tested.

---

## Fixed (2026-10-20)

- **M1:** `export_preset_unusable` / `export_presets_unreadable` in `StreamDiagnostics.cpp`, `ExportPresetUnusable` / `ExportPresetsUnreadable` in `DebugLog.cpp`. CLI JSON-log test added; the dialog test checks the debug log names. (`-Werror=switch` not added.)
- **M2:** a hidden preset is selectable in the dialog as "name (hidden by a built-in)". Rename and Delete work; Update and OK are disabled with a tooltip, and `chosenPreset()` returns nothing for it. Dialog test added.
- **M3:** `setAside()` re-reads under the lock and moves only an unreadable file. New `ExportPresetProblem::NotUnreadable` (Python `NOT_UNREADABLE`, stub updated); a newer file gives `NewerFile`. C++ and Python tests added.
- **L1:** the "(modified)" mark is only on the chosen item (test). **L2:** tooltips visible on the More menu and the Export with Preset menu. **L3:** the dialog reads through the debug log (`setDiagnostics`). **L4:** a duplicate-name entry no longer blocks a case-only rename (test). **L5:** `export --config-dir` without a preset is ignored (test; the old usage-error test was removed). **L7, L8:** header includes and doc comments. **L9:** ADR 006 note, ADR 054 status, text and sections 6, 8, plan status, looks plan, `desired-features.md`; the exit-2 CTest already existed; `docs/todo.md` left to the user. **L10:** every test main sets a temporary `ARRAW_CONFIG_DIR` unconditionally (the cross-process test was not added). **L11:** Save As suggests "<name> copy" for a built-in or hidden preset, the Set Aside confirmation names both folder and file pattern, and the missing-store notice goes to the debug log.

**Skipped:** L6 (needs a design choice: `arraw.export()` or a `ValueError`), the cross-process lock test in L10 (needs a helper executable), the `path.string()` Windows conversions in L11 (cannot be checked here), and the Set Aside result message in L11 (the app does not report the new path afterwards).
