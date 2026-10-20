# Export presets live in one versioned file that the library reads and writes

Status: proposed, 2026-10-20. The plan is
[export-presets-plan.md](../ideas/export-presets-plan.md). Answers `docs/todo.md`, "GUI tasks",
"Export presets".

A photographer exports the same few ways: a 2048 px sRGB JPEG for the web, a full-size 16-bit
Adobe RGB TIFF for print, a small JPEG with no location for a client. Today the export dialog
remembers only the last choice (`saveSettings`/`restoreSettings` in `QSettings`, under `export/`).
The CLI has its own flags and Python its own keywords, and neither can see what the app
remembers. `ExportSettings` sits in `src/app/ExportSettings.h`, in `arraw::app`, beside its
`QSettings` code.

The user decided (2026-10-19 and 2026-10-20) that presets are named `ExportSettings`, shared by
the app, the CLI and scripts, and stored as JSON that the library reads and writes. They live in
one file per topic, `export-presets.json`, versioned as ADR 051 versions the sidecar. The library
finds the configuration directory itself, the same for every front end. GUI-only state stays in `QSettings`. The CLI gets
`export --export-preset NAME`, separate from the develop `--preset`. Develop presets are separate,
one file per preset, and are not designed here. This ADR decides the rest.

## Decision

### 1. `ExportSettings` moves to the library; its `QSettings` code stays in the app

`include/ExportSettings.h` (namespace `arraw`, no Qt) holds the struct as it is today, field for
field, plus:

- `operator==`, and `canonical(ExportSettings)`. The canonical form sets width, height and
  enlarging to 0/false when not resizing, and 16-bit to false for JPEG. Those are the values the
  dialog shows but does not use. Equality for "modified" and the written form both use it.
- `validate(const ExportSettings&)`: quality and sharpening 0 to 100, and a box of 1 to 99999 on
  each side when resizing. It throws `std::invalid_argument`.
- `requestOf`, `optionsOf`, `selectionOf` and `presetLongEdges`, moved unchanged.
- Stable tokens: `tokenOf(ImageFileFormat)` / `parseImageFileFormat` (`"jpeg"`, `"png"`,
  `"tiff"`) and `tokenOf(NamedEncoding)` / `parseOutputEncoding` (`"srgb"`, `"display-p3"`,
  `"adobe-rgb"`). These are the CLI's spellings, so a preset and a flag say the same word.

`saveSettings`/`restoreSettings` stay in the app, renamed `saveExportSettings` and
`restoreExportSettings` and placed in `AppSettings.{h,cpp}`, which already holds the other
`QSettings` readers. The file-naming helpers (`suffixOf`, `withSuffix`, `suggestedPath`,
`targetPaths`) move to `src/app/ExportPaths.{h,cpp}`, and `src/app/ExportSettings.*` is removed.
Two headers called `ExportSettings.h`, one in `include/` and one in `src/app/`, would make
`<…>` versus `"…"` the only thing telling them apart.

*Alternative:* keep `src/app/ExportSettings.h` as a thin app header that includes the public one.
That is less churn, but the name clash above remains.

### 2. The configuration directory is resolved by the library

`include/ConfigDirectory.h`: `std::filesystem::path configDirectory()`, which resolves in this
order:

1. `ARRAW_CONFIG_DIR` (`configDirectoryVariable`) when set and not empty, made absolute;
2. otherwise `arraw` under Qt's `QStandardPaths::GenericConfigLocation`: `~/.config/arraw` on
   Linux (it honours an absolute `XDG_CONFIG_HOME`), `~/Library/Preferences/arraw` on macOS and
   `%LOCALAPPDATA%\arraw` on Windows.

If neither gives a directory (no home, for example), it throws `std::runtime_error` that names the
variable to set. Reading never creates the directory; the first write does. The public header
stays free of Qt (the API speaks `std::filesystem::path`); only `src/core/ConfigDirectory.cpp`
calls Qt, which core links already. The rule itself is a pure function in a private header
(`resolveConfigDirectory`), so it is tested without the real environment.

**The directory name is `arraw`.** The app's `QSettings` uses organisation `arraw` and application
`arraw` (`src/app/main.cpp`), so on Linux it writes `~/.config/arraw/arraw.conf`, and the presets
land beside it in the same folder. On Windows `QSettings` uses the registry
(`HKCU\Software\arraw\arraw`) and on macOS a plist named after the domain; there the folder is
new, but named after the same organisation.

**Why `GenericConfigLocation` and not `AppConfigLocation`.** `AppConfigLocation` appends the
organisation and application names of the `QCoreApplication`. Only the app sets them
(`arraw`/`arraw`), so it would give `~/.config/arraw/arraw`, and the CLI and Python, which set
nothing, would each get a different folder: the failure that sharing presets must avoid. The
generic location does not depend on who set the application's identity, so the app, the CLI and
Python agree. The price is that Windows gives the Local rather than the Roaming AppData folder
(Qt's choice), so presets do not roam; `ARRAW_CONFIG_DIR` can point elsewhere.

Overrides: `ExportPresetStore` takes an explicit directory, which tests and the CLI's
`--config-dir` use. `ARRAW_CONFIG_DIR` covers everything else. `tests/main.cpp` and
`tests/python/conftest.py` set it to a temporary directory, so no test reads or writes the user's
files.

**Layout of the directory (reserved here, so topics do not collide):**

| Path | Topic | Owner |
|---|---|---|
| `export-presets.json` | export presets | this ADR |
| `develop-presets/<name>.json` | develop presets, one file each | looks plan step 4 (its own ADR) |
| `arraw.conf` | `QSettings`, GUI state, Linux only | the app |

The looks plan's "`AppDataLocation/presets`" is amended to `develop-presets/` under
`configDirectory()`, for the reason above. A bare `presets/` is not used, so it never matters which
kind of preset a path means. A later topic gets its own `<topic>.json` with its own version.

### 3. The file

```json
{
  "arraw": 1,
  "exportPresets": [
    {
      "name": "Web 2048",
      "format": "jpeg",
      "encoding": "srgb",
      "bitDepth": 8,
      "size": {"width": 2048, "height": 2048},
      "allowEnlarging": false,
      "quality": 85,
      "sharpening": 30,
      "metadata": {"capture": true, "location": false, "descriptive": true}
    },
    {
      "name": "Print TIFF",
      "format": "tiff",
      "encoding": "adobe-rgb",
      "bitDepth": 16,
      "size": null,
      "allowEnlarging": false,
      "quality": 90,
      "sharpening": 0,
      "metadata": {"capture": true, "location": false, "descriptive": true}
    }
  ]
}
```

- **`"arraw"` is the version** (`exportPresetsVersion = 1`), as in the state JSON. It is a constant
  of its own, independent of `sidecarVersion` and `settingsJsonVersion`. The content is named by
  its key, `"exportPresets"`, so a file of another topic placed here by mistake is unreadable
  rather than read as empty.
- **Enumerations are tokens, never numbers** (ADR 049): `format` and `encoding` use the tokens of
  section 1. `bitDepth` is a number (8 or 16) rather than the struct's `sixteenBit`, so a later
  32-bit float TIFF is a new value and not a second key.
- **`size`** is `null` for the original size, or a box. A long edge of N is an N by N box, as it
  is everywhere else (`RenderRequest::FitInside`).
- **Only what `ExportSettings` holds.** The CLI's `--no-profile` and `--resize-filter` have no
  control in the dialog and are not in a preset in v1. Either can later become an optional key
  (absent means "embed" and "lanczos") with no version bump.
- **Every key is written.** A preset is whole, never partial. On read, an absent key means the
  default of version 1, frozen: an absent `quality` is 90 for ever, whatever the default later
  becomes.
- **A list, not an object keyed by name.** `QJsonObject` sorts its keys, so it can keep no order of
  its own, and case-insensitive uniqueness would still need checking. The list is written sorted
  (section 4).
- **Unknown keys are preserved.** An unknown key in a preset or at the top level is reported
  (`Notice::SettingUnknown`, key `"<preset>.<key>"`) and kept on write, as an unknown `arraw:`
  attribute is kept in a sidecar. The public type does not carry it. Every write re-reads the file
  and changes only the known keys of the entry it touches (section 5), so preservation lives in the
  write path. Objects are written with Qt's key order, so a hand-made file's key order is not kept,
  but its keys are.
- **A preset this arraw cannot use is kept, not dropped.** That includes a token it does not know
  (a newer arraw's `"avif"`), a `bitDepth` other than 8 or 16, a `size` of another shape, or a
  missing or invalid name. Such a preset is listed as **unusable** with a reason
  (`Notice::ExportPresetUnusable`) and cannot be applied. Its JSON is written back unchanged. This
  lets a newer arraw add a format or a size form **without raising the version**: an older arraw
  shows the preset greyed out instead of quietly exporting a JPEG. A value of the right kind but
  out of range (quality 120) is clamped with `Notice::SettingClamped`, and a value of the wrong
  type takes its default with `Notice::SettingMalformed`. Both follow `applySettingsJson`.
- **Versions, as ADR 051:**
  - a newer arraw reads every older version, silently;
  - an older arraw reads a newer file with `Notice::NewerSettingsVersion` (the subject is the file)
    and uses what it understands;
  - it **never writes over a newer file**: every write throws `ExportPresetError` (`NewerFile`)
    and leaves the file alone;
  - the version is raised only for a change an older reader would get wrong. A new optional key
    whose absence means the old behaviour does not raise it, and with the unusable rule above
    neither does a new enumerator;
  - `tests/fixtures/config/export-presets/v<N>/*.json` are frozen files as that version's writer
    produced them. Each is read with no warning and written back as the current version with the
    same contents, and a directory must exist for the current version.

*Alternatives:* a general `config.json` holding every topic (the user chose one file per topic, so
a corrupt or newer file blocks one topic, not all of them); unknown keys dropped (simpler, but an
older arraw opening the dialog would erase a newer arraw's optional keys); a raised version for
every new format (a version bump then locks older builds out of all writes, for one preset).

### 4. Names, uniqueness, order

- A name is trimmed of white space at both ends and normalised to NFC. It must have 1 to 64
  characters and no control characters (C0, DEL, C1, U+2028/U+2029). An invalid name is
  `ExportPresetError` (`InvalidName`) on write. On read, the entry becomes unusable.
- **Names are unique case-insensitively** (Unicode case folding), and a lookup is exact after
  folding, never fuzzy. The spelling shown is the one stored. Saving `web` when `Web` exists
  replaces `Web`, and the name becomes `web`. In a hand-made file with two entries that fold
  alike, the first wins and the later ones are unusable ("duplicate name").
- **Order:** sorted by the folded name and then by the name as stored, in code-point order. That is
  the same order on every platform and in every front end, and the file is written in it, so diffs
  stay stable. Hand-made order is not kept. *Alternative:* keep the file's order and append new
  presets, which allows a chosen order but needs reordering in the UI to be of use. *Alternative:*
  locale collation (`QCollator`) for display, which sorts `Ä` next to `A` but makes the CLI and
  the dialog disagree.
- The names are not file names. That is why `/`, `:` and the rest are allowed, and why the
  develop-preset store, whose names are file names, has rules of its own.

### 5. Writing: one change at a time, under a lock, atomically

`ExportPresetStore(directory)` has `read`, `load`, `save`, `rename`, `remove` and `setAside`. Each
of `save`, `rename` and `remove` is one transaction:

1. take `export-presets.json.lock` with `QLockFile` (`tryLock` for 2 s; a stale lock, whose owner
   process is gone, is broken). If it is busy, throw `ExportPresetError` (`Busy`);
2. re-read the file. Refuse if it is newer, and refuse if it is unreadable (section 6);
3. apply the one change to the parsed document, keeping unknown keys and unusable entries
   verbatim;
4. write through `QSaveFile` (a temporary file in the same directory, then a rename), which
   resolves a symbolic link, so a dotfiles link stays a link;
5. release the lock.

Concurrent writers (the app, a Python script, a second window) therefore never lose each other's
changes. Each applies its change to the file as it is at that moment, not to a copy read earlier.
Readers take no lock: the rename means they see the old file or the new one, whole. The app
re-reads the file every time the dialog opens; nothing watches it.

- `save(preset)` inserts, or replaces the preset that folds alike. It refuses to replace an
  **unusable** preset (`Unusable`): an older arraw does not write over what it cannot read. That
  preset has to be removed first, and `remove` works on unusable ones.
- `rename(from, to)` refuses a `to` held by another preset (`NameTaken`). A change of case only is
  a rename.
- `remove(name)` throws `NotFound` when there is nothing to remove, so the CLI and Python can say
  so.

*Alternative:* no lock, with the last writer winning. That is simpler, and the CLI writes nothing
in v1 (section 8), but a script saving while the dialog saves would lose a preset without a
word. `QLockFile` is in Qt Core, which core already links.

### 6. A file that cannot be read is never written over

A file is **unreadable** if it is not JSON, is not an object with an integer `"arraw"` of at least
1 and a list `"exportPresets"`, or is larger than 1 MiB. A zero-length or white-space-only file
reads as empty, and can be written.

- Reading gives an empty list, `ExportPresetFileState::Unreadable`, the reason, and
  `Notice::ExportPresetsUnreadable`. It never throws for the file's content.
- **Every write refuses** (`UnreadableFile`), and the file stays as it was.
- `setAside()` renames the file to `export-presets.unreadable-<YYYYMMDD-HHMMSS>.json`, never
  replacing an existing file, and returns the new path. The next write then starts a new file. The
  app offers this behind a confirmation that names both paths. The CLI and Python only report the
  problem; Python can call `set_aside()` explicitly.

*Alternatives:* a `.bak` of the previous file on every write. Atomic writes already make a
half-written file impossible, so this would guard only against a user's own deletions, at the cost
of a second file that goes stale. Or set the file aside automatically on the first write. That is
not silent if it is reported, but the report would land in a status line nobody reads.

### 7. Errors and diagnostics

- **Content problems are diagnostics** sent to a `DiagnosticLog`, with the file as the subject:
  `SettingUnknown`, `SettingMalformed`, `SettingClamped` and `NewerSettingsVersion` (existing), and
  two new notices, `ExportPresetUnusable` (name, reason) and `ExportPresetsUnreadable` (reason).
- **Operations that fail throw `ExportPresetError`**, a `std::runtime_error` carrying an
  `ExportPresetProblem`: `InvalidName`, `NameTaken`, `NotFound`, `Unusable`, `NewerFile`,
  `UnreadableFile`, `Busy`, `WriteFailed`. The enum lets the dialog and Python react to the cause
  (`LensProblem` is the precedent). `what()` is a sentence naming the file.
- Settings that fail `validate` throw `std::invalid_argument`, as everywhere else.

### 8. Front ends

**App.** The dialog gains a row at the top: **Preset:** a combo, a **Save As…** button and a menu
button (**Update "Name"**, **Rename…**, **Delete…**, **Show File**).

- The combo lists the usable presets in order and the unusable ones disabled, each with its reason
  as a tooltip. With none, it shows the disabled item "No presets".
- Choosing a preset sets every control. Changing any control afterwards marks it **modified**: the
  item reads "Web 2048 (modified)" and Update becomes available. Changing the controls back clears
  the mark, since the comparison is between canonical forms. Nothing switches to another preset by
  itself.
- On opening, the controls show the last used settings (`QSettings`, unchanged), and the combo
  shows the remembered preset name (`export/preset`), modified when they differ. A remembered
  preset that is gone shows "No preset".
- Save As asks for a name and, when it folds like an existing one, confirms the replacement.
  Rename, Delete and Update act on the file at once, whether or not the export is then cancelled,
  as preset lists do in other editors.
- A newer file disables Save As, Update, Rename and Delete, with a tooltip saying why. An
  unreadable file disables them too and offers **Set Aside…** (section 6).
- OK stores the last used settings and the preset name in `QSettings`. Modified settings are never
  saved to the preset implicitly.

The film strip's multi-shot export (`MainWindow::exportShots`) already uses the same dialog once
for the batch, so it gets the picker at no cost, and every shot is exported with the one
`ExportSettings`. In addition, the strip's context menu and **File** gain **Export with Preset ▸
<names>**, which skips the settings dialog and goes straight to the folder prompt (or the save
prompt for one photograph). Unusable presets are disabled there.

*Alternatives:* a separate "Manage Presets…" dialog (more to build, and nothing to manage beyond
rename and delete in v1); auto-selecting a preset whenever the controls happen to match one
(surprising, and ambiguous when two presets are equal).

**CLI.**

- `export --export-preset NAME` loads the preset, and any export flag given overrides the value it
  names. The order is: defaults, then the preset, then the flags. `--resize` replaces the size,
  `--allow-upscale` and a new `--no-allow-upscale` set enlarging, and `--metadata`, `--format`,
  `--encoding`, `--bit-depth`, `--quality` and `--sharpen` each replace their own value.
- A preset's 16 bits under `--format jpeg` becomes 8, as `optionsOf` does in the app, while an
  explicit `--bit-depth 16` with JPEG stays an error.
- An unknown, unusable or unreadable preset is a usage error (exit 2) naming the file and
  suggesting `export-preset list`, because retrying will not fix it (ADR 006).
- New command `export-preset`, with `list` (names on stdout, one per line; unusable ones and
  warnings on stderr), `show NAME` (the preset as this arraw writes it, a JSON object on stdout)
  and `path` (the file's path, whether it exists or not). `export` and `export-preset` take
  `--config-dir DIR`, and `ARRAW_CONFIG_DIR` covers the rest.
- The CLI writes no presets in v1; scripts write through Python. *Alternative:*
  `export-preset save NAME [export flags]` and `delete NAME`, which reuse the export flag parser.
  They are easy to add later, since the command word is reserved now.

**Python** (ADR 018: only the public API): `arraw.ExportSettings`, `arraw.ExportPreset`,
`arraw.config_directory()`, and `arraw.ExportPresetStore(directory=None)` with `presets()`,
`unusable()`, `load(name)`, `save(preset)`, `rename(old, new)`, `delete(name)`, `set_aside()` and
`file`. Failures raise `arraw.ExportPresetError` (a `RuntimeError` with `.problem`). `arraw.save`
gains `settings=` (an `ExportSettings`) as an alternative to its separate keywords.

### 9. Built-in presets: none in v1

The size combo already offers the long edges (`presetLongEdges`), and the dialog remembers the last
choice, so an empty list leaves nothing out. A built-in name is reserved for ever, as a command
word is. Built-ins need rules for collisions with user names, for "cannot delete", and for what an
update does to one the user changed.

*Alternatives:*

- (b) a few presets defined in code ("Web 2048 sRGB", "Print TIFF 16-bit Adobe RGB"), read-only and
  listed in their own section. This is discoverable, at the cost of the rules above.
- (c) seeding the file on the first write. The user owns the seeded presets after that, but a read
  never writes, so they would appear only after the first save, which is odd. (b) is the better
  of the two if built-ins are wanted later. It needs no version bump, since they never enter the
  file.

## Consequences

- `arraw::app::ExportSettings` becomes `arraw::ExportSettings`. `ExportQueue`, `MainWindow`, the
  dialog and their tests change their includes. Behaviour does not change until the picker lands.
- Every test binary and the Python tests must set `ARRAW_CONFIG_DIR`. Otherwise a developer's own
  presets leak into the test results.
- `QSettings` keeps the last used settings, keyed as today, so nobody loses their last choice.
- ADR 006 gains a note: `export-preset` is a command word, and `--export-preset` and
  `--config-dir` are options. The looks plan's preset directory is amended (section 2).
- On a network home directory, opening the dialog reads a small file on the GUI thread. This is
  accepted, as `QSettings` already does the same.

## What was rejected, and why

- **`QStandardPaths::AppConfigLocation` in the library.** Its answer depends on the process's application name, and the CLI and Python do not set it (section 2). The generic location is used instead.
  the CLI and Python do not set it (section 2).
- **Keeping presets in `QSettings`.** The CLI does not read it, Windows keeps it in the registry,
  and the user decided against it.
- **Partial presets** (only the keys that differ). Flags already give the CLI partial overrides,
  and a partial preset in the dialog would need a "which fields" checklist for little gain.
- **Ints for enumerations**, as `saveSettings` stores them today. They are unreadable by hand, and
  they break when an enumerator is reordered.
