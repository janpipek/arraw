# Export presets: one shared, versioned file — plan

Status: proposed, 2026-10-20. Decision record:
[ADR 054](../adr/054-export-presets-live-in-one-versioned-file-the-library-reads-and-writes.md).
Answers `docs/todo.md`, "GUI tasks", "Export presets".

## Decisions (user, 2026-10-19/20)

| Question | Answer |
|---|---|
| What a preset holds | A named, whole `ExportSettings` |
| Who sees presets | App, CLI and Python, through the library |
| Storage | `export-presets.json` in the configuration directory; one file per topic |
| Versioning | As ADR 051: read older for ever, warn on newer, never write over newer, frozen fixtures |
| Directory | Resolved by the library (`GenericConfigLocation/arraw`); `ARRAW_CONFIG_DIR`, an explicit path, `--config-dir` |
| Last used settings | Stay in `QSettings` |
| CLI | `export --export-preset NAME`, where flags win; a way to list and show presets |
| Develop presets | Separate, one file each; not designed here |

## What the code does today (verified)

- `ExportSettings` and its mapping functions are in `src/app/ExportSettings.{h,cpp}`, in
  `arraw::app`, together with `saveSettings`/`restoreSettings` (`QSettings`, `export/`, with
  enumerations stored as ints) and the naming helpers (`suffixOf`, `withSuffix`, `suggestedPath`,
  `targetPaths`). They are built into `arraw-app-core` (`CMakeLists.txt:531`).
- They are used by `ExportDialog` (`src/app/ui/ExportDialog.*`), `MainWindow::exportPhoto` and
  `MainWindow::exportShots` (`MainWindow.cpp:1946`, `:2032`), `ExportPrompts` (the test seam in
  `MainWindow.h:101`) and `tests/test_ExportSettings.cpp` / `test_ExportDialog.cpp`.
- `presetLongEdges` is {1080, 1600, 2048, 3840}. The dialog's size combo offers "Original",
  "Long edge N px" for each and "Custom…".
- The CLI's export flags are `--format` (`png|jpeg|jpg|tiff|tif`), `--encoding`
  (`srgb|display-p3|adobe-rgb`), `--bit-depth`, `--quality`, `--sharpen`, `--resize`
  (N, WxH or N%), `--allow-upscale`, `--resize-filter`, `--no-profile` and `--metadata`
  (`ExportCommand.cpp:713-811`). Without `--resize`, `--allow-upscale` is reported as ignored.
  `extensionFor` duplicates `suffixOf`.
- Core already uses Qt Core: `QJsonDocument` in `SettingsJson.cpp`, `QSaveFile` in `Sidecar.cpp`
  and `ImageExport.cpp`. Only the public headers are Qt-free.
- Only the app sets `QCoreApplication` names (`main.cpp:25`), so `QStandardPaths::AppConfigLocation` would differ
  between the app, the CLI and Python. `ThumbnailCache` uses `GenericCacheLocation/arraw`.
- Python's `arraw.save(image, path, *, format, encoding, bit_depth, quality, embed_profile,
  sharpening, metadata_from, metadata)` is in `BindPhoto.cpp:802`. `MetadataSelection`,
  `ImageFileFormat` and `NamedEncoding` are bound already.
- The sidecar fixture test (`test_Sidecar.cpp:991`, `tests/fixtures/sidecars/v1`) is the pattern
  to mirror. `test_Cli.cpp:113` already has an environment-variable guard.
- No test sets a configuration directory. `tests/main.cpp` only builds a `QCoreApplication`.

## Steps

Each step builds, passes `just test` and `just format-check`, and is reviewed before the next.
Test names are Catch2 `TEST_CASE` names; the tags follow the file's existing ones.

### 1. `ExportSettings` moves to the library (no behaviour change)

- New `include/ExportSettings.h` and `src/core/ExportSettings.cpp`, in namespace `arraw`:
  - the struct, unchanged;
  - `operator==`, `canonical` and `validate`;
  - `requestOf`, `optionsOf`, `selectionOf` and `presetLongEdges`;
  - `tokenOf(ImageFileFormat)` / `parseImageFileFormat` and `tokenOf(NamedEncoding)` /
    `parseOutputEncoding`. The parsers are lower-case only and take the CLI's aliases (`jpg`,
    `tif`); the token written is always the canonical one.
- `src/app/ExportPaths.{h,cpp}` takes `suffixOf`, `withSuffix`, `suggestedPath` and `targetPaths`.
- `AppSettings.{h,cpp}` takes `saveExportSettings`/`restoreExportSettings`, the old
  `saveSettings`/`restoreSettings`. The keys and their int encoding stay as they are, so last-used
  settings survive the update.
- `src/app/ExportSettings.*` is deleted. `CMakeLists.txt` moves the source from `arraw-app-core`
  to `arraw`.
- *Optional:* `ExportCommand.cpp` uses the token parsers instead of its own `if` chains, and
  `suffixOf` replaces `extensionFor`.
- **Tests:**
  - `tests/test_ExportSettings.cpp` keeps the mapping cases, now tagged `[export]`;
  - new cases: "Canonical settings drop what the format and size do not use"; "Validate refuses
    quality, sharpening and boxes out of range"; "Every format and encoding has a token that reads
    back"; "Format aliases read, and the canonical token is written";
  - the naming cases move to `tests/test_ExportPaths.cpp`;
  - the `QSettings` cases move to `tests/test_AppSettings.cpp`, with "Export settings stored by an
    earlier build are restored" (ints in, the same settings out).

### 2. The configuration directory

- `include/ConfigDirectory.h`: `configDirectoryVariable` (`"ARRAW_CONFIG_DIR"`) and
  `configDirectory()`.
- `src/core/ConfigDirectoryRules.h` (private): `resolveConfigDirectory(override, genericConfig)`, a
  pure function. `ConfigDirectory.cpp` calls it with `qEnvironmentVariable("ARRAW_CONFIG_DIR")` and
  `QStandardPaths::writableLocation(GenericConfigLocation)`. The generic location is used, not
  `AppConfigLocation`, because the latter adds the application name that only the app sets, so
  the CLI and Python would disagree with the app (ADR 054 section 2). Public headers stay Qt-free.
- `tests/main.cpp` sets `ARRAW_CONFIG_DIR` to a per-process temporary directory before
  `Catch::Session` runs, unless it is already set, and removes the directory at exit.
  `tests/python/conftest.py` does the same with an autouse session fixture, and the CLI tests
  inherit it.
- **Tests** (`tests/test_ConfigDirectory.cpp`, `[config]`):
  - "ARRAW_CONFIG_DIR wins, and is made absolute";
  - "An empty ARRAW_CONFIG_DIR is ignored";
  - "The generic configuration location is used, with arraw below it";
  - "The variable overrides the real lookup";
  - "No location is an error naming what is missing";
  - "The suite runs with a configuration directory of its own" (the variable is set and is not
    under `$HOME`).

### 3. The file format (pure: text in, text out)

- `include/ExportPresets.h`: `exportPresetsVersion`, `exportPresetsFileName`, `ExportPreset {name,
  settings}`, `UnusableExportPreset {name, reason}`, `ExportPresetFileState {Missing, Read, Newer,
  Unreadable}`, `ExportPresetCatalog {presets, unusable, state, version, problem}`,
  `normalizedPresetName` / `presetNamesEqual`, and `exportPresetToJson(const ExportPreset&)`, which
  `export-preset show` and Python use.
- `src/core/ExportPresetsJson.{h,cpp}` (private) holds `readExportPresets(std::string_view, log,
  subject) -> ParsedPresets` and `writeExportPresets(const ParsedPresets&) -> std::string`. The
  parsed form keeps each entry's `QJsonObject` beside the decoded preset, so unknown keys and
  unusable entries survive. It does Qt work behind a private header, as `LocalAdjustmentCodec` does.
- `Diagnostics.h` gains `ExportPresetUnusable` and `ExportPresetsUnreadable`, with `describe`
  sentences.
- Fixtures, never edited once committed: `tests/fixtures/config/export-presets/v1/complete.json`
  (every key, both size forms, all formats and encodings) and `v1/minimal.json` (names only, so
  every default applies).
- **Tests** (`tests/test_ExportPresetsJson.cpp`, `[presets][json]`):
  - "A catalog written and read back is the same, in name order";
  - "Formats and encodings are written as tokens";
  - "An absent key is the version 1 default";
  - "A number out of range is clamped with a warning";
  - "A value of the wrong type takes the default with a warning";
  - "An unknown key is reported and kept on write";
  - "An unknown format, bit depth or size shape makes the preset unusable, kept verbatim";
  - "Names that fold alike: the first is used, the rest are unusable";
  - "An invalid name makes the entry unusable";
  - "A newer file is read with a warning";
  - "Not JSON, the wrong shape, another topic, or over 1 MiB is unreadable";
  - "An empty or blank file is an empty catalog";
  - `[versions]` "Every stored version of the presets file is still read, and written as the
    current one", which also requires `v<exportPresetsVersion>` to exist.

### 4. The store: locking, atomic writes, errors

- `ExportPresets.h` gains `ExportPresetProblem`, `ExportPresetError` and `ExportPresetStore`:
  - `explicit ExportPresetStore(std::filesystem::path directory)`;
  - `static ExportPresetStore standard()`, which uses `configDirectory()`;
  - `file()`, `read(log)`, `load(name, log)`, `save(preset)`, `rename(from, to)`, `remove(name)`
    and `setAside() -> path`.
- `src/core/ExportPresetStore.cpp` implements ADR 054 sections 5 and 6: `QLockFile` with `tryLock`
  for 2 s, a re-read, one change, `QSaveFile`, and directories created on write only.
- **Tests** (`tests/test_ExportPresetStore.cpp`, `[presets][store]`; each uses its own `TempDir`):
  - "A missing file reads as an empty catalog and is not created";
  - "Save creates the directory and the file";
  - "Load finds a preset whatever the case of its name";
  - "Saving a name that folds alike replaces it and takes the new spelling";
  - "Rename refuses a name another preset holds; a change of case is allowed";
  - "Remove of a missing preset says NotFound";
  - "Save refuses to replace an unusable preset; remove deletes it";
  - "A write keeps unknown keys and unusable presets of other entries";
  - "A newer file is never written: save, rename and remove leave it byte for byte";
  - "An unreadable file is never written, and setAside moves it without replacing anything";
  - "Two stores on one directory do not lose each other's saves" (interleaved, and from two
    threads);
  - "A held lock makes a write Busy; a stale lock is broken";
  - "A symbolic link to the file stays a link after a write" (skipped on Windows without the
    privilege);
  - "An unwritable directory gives WriteFailed and leaves the old file".

### 5. CLI: `export --export-preset`, `--config-dir`, `export-preset`

- `ExportCommand.cpp`:
  - `--export-preset NAME`, `--config-dir DIR` and `--no-allow-upscale`;
  - `buildRequest` starts from `ExportRequest` defaults, applies the preset (format, options,
    `render.size`/`upscale` from `requestOf`, metadata), then each flag that was given;
  - a preset's size makes `--allow-upscale` meaningful, so it is no longer reported as ignored;
  - a preset's 16 bits with `--format jpeg` becomes 8, while an explicit `--bit-depth 16` with JPEG
    still fails.
- New `src/cli/ExportPresetCommand.{h,cpp}`, a row in `Command.cpp` placed after `export`, with
  `list`, `show NAME` and `path`. It is Core only and starts no application beyond
  `ApplicationKind::Core`.
- **Tests** (`tests/test_Cli.cpp`, `[cli][presets]`, with `ARRAW_CONFIG_DIR` or `--config-dir`
  pointing at a `TempDir`):
  - "export --export-preset applies the preset's format, size and metadata";
  - "An export flag overrides the preset's value and leaves the rest";
  - "--no-allow-upscale turns off a preset's enlarging";
  - "A preset's 16 bits become 8 under --format jpeg; --bit-depth 16 with jpeg is still an error";
  - "An unknown preset is a usage error that points at export-preset list";
  - "An unusable preset, or an unreadable file, is a usage error naming the file";
  - "export-preset list prints names on stdout and problems on stderr";
  - "export-preset show prints the preset as JSON that reads back";
  - "export-preset path prints the file even when it does not exist";
  - "--config-dir wins over ARRAW_CONFIG_DIR";
  - "The top-level help lists export-preset".
  - `tests/cli/ExpectExit.cmake` gains one exit-code case for an unknown preset (2).

### 6. Python

- `src/python/BindExport.cpp` (new) binds `ExportSettings` (frozen, with keyword construction and
  the fields in snake case), `ExportPreset`, `ExportPresetProblem`, `ExportPresetError` (a
  `RuntimeError` subclass with `.problem`), `ExportPresetStore` (`presets()`, `unusable()`,
  `load`, `save`, `rename`, `delete`, `set_aside`, `file`) and `config_directory()`.
- `arraw.save` gains `settings: ExportSettings | None`. Given together with `format`, `encoding`,
  `bit_depth`, `quality`, `sharpening` or `metadata`, it is a `TypeError`.
  `ExportSettings.develop_size` gives `develop`'s `size` (`None` or `(w, h)`).
- Regenerate `_arraw.pyi` and export the names from `__init__.py`.
- **Tests** (`tests/python/test_export_presets.py`):
  - "save, then presets() lists it, and load returns it";
  - "delete removes it, and delete of a missing one raises with NOT_FOUND";
  - "a newer file raises NEWER_FILE on save and leaves it";
  - "the store sees a preset the CLI's `export-preset show` prints" (parity, as
    `test_cli_parity.py` does);
  - "save(settings=...) writes what save(format=..., ...) writes";
  - "the session's ARRAW_CONFIG_DIR is used" (so no test touches `~/.config`).

### 7. App: the preset picker in the export dialog

- `ExportDialog(initial, frame, ExportPresetStore* store, std::optional<std::string>
  rememberedPreset, parent)`. With `nullptr` for the store, the picker is hidden, for callers and
  tests that do not care.
- Build the row as ADR 054 section 8 describes: combo, **Save As…**, and a menu with Update,
  Rename…, Delete…, Show File and, for an unreadable file, Set Aside…. The modified mark compares
  canonical forms. Add `chosenPreset()` beside `settings()`.
- `MainWindow` owns `std::optional<ExportPresetStore>`, built from `configDirectory()`. On failure
  it stays empty, with a line in the debug log. `exportPhoto` and `exportShots` pass the store and
  `export/preset`, and store the chosen name on OK. `ExportPrompts::settings` is unchanged, since
  the seam replaces the whole dialog.
- Errors from the store become a `QMessageBox::warning` with `what()`. For `NewerFile`,
  `UnreadableFile` and `Busy`, the buttons are disabled in advance where the catalog's state says
  so.
- **Tests** (`tests/test_ExportDialog.cpp`, `[app][dialog][export][presets]`; the dialog takes a
  store over a `TempDir`, and the prompts for names and confirmations are injected as
  `ExportPrompts` are):
  - "The picker lists presets in order, the unusable ones disabled";
  - "Choosing a preset sets every control";
  - "Changing a control marks the preset modified; changing it back clears the mark";
  - "The dialog opens on the remembered preset, modified when the last settings differ";
  - "A remembered preset that is gone shows No preset";
  - "Save As writes the file, and a name that folds alike asks before replacing";
  - "Update, Rename and Delete act on the file at once";
  - "A newer file disables every write, with the reason";
  - "An unreadable file offers Set Aside, which moves it after confirmation";
  - `tests/test_MainWindowExport.cpp`: "The chosen preset's name is remembered for the next
    export".

### 8. App: Export with Preset ▸ (optional, independent of step 7's UI)

- `FilmStrip` context menu and **File** menu: **Export with Preset ▸** lists the presets, read when
  the menu is about to show. Choosing one skips the settings dialog: `exportShots(primaries,
  settings)` goes to the folder prompt, and `exportPhoto(settings)` to the save prompt. Unusable
  presets are disabled. With none, the menu holds one disabled "No presets" item.
- The last used settings are not changed by such an export. *Open:* change them too.
- **Tests** (`tests/test_MainWindowExport.cpp`):
  - "Export with Preset exports several shots without the settings dialog";
  - "Export with Preset lists nothing usable as disabled".

### 9. Docs

- ADR 006: add a note for `export-preset`, `--export-preset`, `--config-dir` and
  `--no-allow-upscale`.
- Looks plan step 4: the store lives in `configDirectory()/develop-presets/` (ADR 054 section 2).
- `docs/todo.md`: mark the item done and replace its "`QStandardPaths` config location" with a
  pointer to ADR 054.
- `desired-features.md`: add export presets beside develop presets, and `export-preset` to the
  command examples.
- ADR 054 becomes "accepted" when step 4 lands.

## Files touched

| Step | Files |
|---|---|
| 1 | `include/ExportSettings.h`, `src/core/ExportSettings.cpp` (new); `src/app/ExportPaths.{h,cpp}` (new); `src/app/AppSettings.{h,cpp}`; `src/app/ExportSettings.{h,cpp}` (deleted); `src/app/ExportQueue.h`, `src/app/ui/ExportDialog.{h,cpp}`, `src/app/ui/MainWindow.{h,cpp}`; `CMakeLists.txt`; `tests/test_ExportSettings.cpp`, `tests/test_ExportPaths.cpp` (new), `tests/test_AppSettings.cpp`, `tests/test_ExportDialog.cpp`, `tests/CMakeLists.txt`; optionally `src/cli/ExportCommand.cpp` |
| 2 | `include/ConfigDirectory.h`, `src/core/ConfigDirectory.cpp`, `src/core/ConfigDirectoryRules.h` (new); `CMakeLists.txt`; `tests/main.cpp`, `tests/python/conftest.py`, `tests/test_ConfigDirectory.cpp` (new) |
| 3 | `include/ExportPresets.h`, `src/core/ExportPresetsJson.{h,cpp}` (new); `include/Diagnostics.h`, `src/core/Diagnostics.cpp`; `tests/fixtures/config/export-presets/v1/*.json`; `tests/test_ExportPresetsJson.cpp` (new) |
| 4 | `include/ExportPresets.h`, `src/core/ExportPresetStore.cpp` (new); `tests/test_ExportPresetStore.cpp` (new) |
| 5 | `src/cli/ExportCommand.{h,cpp}`, `src/cli/ExportPresetCommand.{h,cpp}` (new), `src/cli/Command.cpp`, `CMakeLists.txt`; `tests/test_Cli.cpp`, `tests/cli/ExpectExit.cmake` |
| 6 | `src/python/BindExport.cpp` (new), `src/python/BindPhoto.cpp`, `src/python/Module.cpp`, `src/python/PyBindings.h`, `src/python/CMakeLists.txt`, `src/python/arraw/__init__.py`, `_arraw.pyi`; `tests/python/test_export_presets.py` (new) |
| 7 | `src/app/ui/ExportDialog.{h,cpp}`, `src/app/ui/MainWindow.{h,cpp}`; `tests/test_ExportDialog.cpp`, `tests/test_MainWindowExport.cpp` |
| 8 | `src/app/ui/FilmStrip.{h,cpp}`, `src/app/ui/MainWindow.{h,cpp}`; `tests/test_MainWindowExport.cpp` |
| 9 | `docs/adr/006-…`, `docs/adr/054-…`, `docs/ideas/looks-and-history-plan.md`, `docs/todo.md`, `docs/desired-features.md` |

## Risks

- **Tests touching the user's real presets.** Step 2's global `ARRAW_CONFIG_DIR` must land before
  any test reads presets. A test pins it.
- **The move in step 1 is wide.** It is mechanical, but `arraw::app::ExportSettings` is named in
  the queue, the window and the tests. Doing it alone, with no behaviour change, keeps the review
  readable.
- **Windows file replacement.** `QSaveFile`'s rename can fail while another process (an antivirus,
  an editor) holds the file open. That surfaces as `WriteFailed` and the old file stays. A short
  retry may be needed; measure on Windows before adding one.
- **`QLockFile` on network or synced folders** (NFS, OneDrive-redirected AppData) is weaker. A
  lost lock degrades to last-writer-wins for one change; it never corrupts, because of the atomic
  rename.
- **Case folding and normalisation.** `QString::toCaseFolded` uses simple folding, so `ß` and `SS`
  stay distinct. NFC on write means a name typed in NFD shows as NFC. Both are accepted, and a
  test pins them.
- **Sorting is by code point**, so `Äpfel` sorts after `Zoo`. It is deterministic everywhere. A
  locale-aware display order is a later choice.
- **Bit depth under JPEG** has two rules (silently 8 from a preset, an error from a flag). They
  are documented in `export --help` and pinned by tests, but they can still surprise.
- **Key order.** `QJsonDocument` writes keys alphabetically, so a hand-made file is reordered on
  the first write. Contents are kept.
- **A hand edit while the dialog is open** is seen only when the dialog next opens. Writes
  re-read, so nothing is lost, but the combo can be briefly out of date.

## Open questions

1. **Built-in presets:** none in v1 (proposed), or read-only presets defined in code (ADR 054
   section 9, (b)).
2. **Order:** sorted by folded name (proposed), or the file's own order with new presets appended.
3. **Export with Preset ▸** (step 8): whether it should also update the last used settings.
4. **CLI writes:** whether `export-preset save|delete` are wanted in v1, or Python is enough.
5. **`--no-profile` and `--resize-filter` in presets:** they are left out of v1 because the dialog
   has neither control. Add them when the dialog gains the controls.
