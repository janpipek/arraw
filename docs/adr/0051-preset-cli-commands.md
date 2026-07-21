# The preset command family: list, show, apply — the GUI's presets, headless

`arraw preset <verb>` brings Develop Presets (ADR 0023) to the command line:

```
arraw preset list
arraw preset show <name>
arraw preset apply <name> <paths>...
```

It is the first noun-grouped command in the grammar (`preset` joins `ui`,
`export`, `version`, `help` as a reserved word; ADR 0049 rule 2). All three
verbs follow the CLI-wide output and exit contract of ADR 0050: human tables
by default, `--json` for one valid JSON document on stdout, exit tiers
`0/1/2`. Parsing is CLI11, one `CLI::App` per verb, mirroring `ExportArgs`.

## One preset source: the GUI's

The CLI reads `QStandardPaths::AppDataLocation + "/presets"` through
`PresetStore` — precisely what the Presets menu sees. No `--presets-dir`
override, no environment variable: a second source of truth would enable "the
CLI applied a different preset than the menu shows", the worst failure mode a
mirror-of-the-GUI command can have. Preset syncing, if it ever comes, arrives
in both front-ends at once. Tests inject a temp directory into `PresetStore`
(the seam exists for exactly this) rather than needing a flag.

## Name matching: case-insensitive, never fuzzy

`<name>` is matched against display names case-insensitively — consistent
with `PresetStore::exists`, which already treats case variants as the same
preset because their storage filenames collide on case-insensitive
filesystems; store semantics guarantee at most one hit. On a miss, the error
lists the available preset names on stderr (exit 2), so the user never needs
a separate `list` round-trip. Prefix/fuzzy matching is rejected: `apply`
writes sidecars, and a fuzzy match on a write command is how someone applies
the wrong look to 300 files.

## list: one row per preset, groups as the scent

A preset's identity in practice is which Develop Groups it carries
(serialisation is partial — presence *is* the apply flag, ADR 0023), so
`list` shows `name` plus the active groups and nothing more; per-field detail
is `show`'s job.

- Table: display name + group *labels* (`developGroupLabel`).
- `--json`: `[{"name": ..., "groups": [<developGroupKey>, ...]}]` — the same
  stable keys the preset files use.
- No presets is not an error: exit 0, empty table (with a stderr hint) or `[]`.

## show: the details view, and the file format as the API

- Table: the Manage Presets details view, headless — each active group with
  its non-default fields via `describeGroupNonDefaults`, and a selected group
  whose fields are all default rendered as "resets to defaults" (applying a
  group replaces every field in it).
- `--json`: **the native preset JSON**, byte-for-byte what
  `serializeDevelopPreset` produces and the file stores. The on-disk format is
  already the stable machine representation (ADR 0023); exposing it means
  zero second schema to keep in sync, and `arraw preset show X --json >
  backup.json` is preset export for free. A bespoke report schema (groups +
  localised change lines) is rejected: scripts must not parse localisable
  strings, and everything structural in it is already in the native form.

## apply: byte-for-byte the GUI batch apply

Semantics are identical to the GUI's Apply Preset on a selection
(`MainWindow::applyPresetToPaths` → `writeBatchAfter`, ADR 0018 auto-save):

```
before = XmpSidecar::loadAdjustments(path)      // defaults if no sidecar
after  = applyGroups(before, preset.values, preset.groups)
XmpSidecar::saveAdjustments(path, after)
```

No image decode, no render — pure sidecar I/O through the same pure function
the GUI uses. `saveAdjustments` is namespace-scoped (ADR 0007), so ratings,
colour labels, and snapshots on disk are preserved.

- **Inputs are explicit files**, one or more, pre-flighted as a whole before
  any write: each path must exist, not be a directory, and carry a supported
  image extension (`apply` would otherwise happily write `notes.txt.xmp` next
  to a typo). Any bad path refuses the entire run, exit 2, zero sidecars
  touched. No folder mode: `export` deferred directory inputs for want of a
  "which files count" policy (ADR 0049), and `apply` growing one first would
  make the two commands disagree on what a path means — folder mode lands in
  both or neither.
- **Per-file write failures don't stop the batch** (exit 1, stderr line each,
  summary at the end) — files are independent and each write is cheap.
- Output: per-file `Applied:` lines + summary in table mode; with `--json`
  one document `{"preset": ..., "applied": [...], "failed": [{"path": ...,
  "error": ...}]}`, failures mirrored to stderr.
- **No `--dry-run` in v1**: pre-flight already refuses everything checkable
  up front, apply is cheap to redo from the GUI or by re-applying, and the
  GUI has no equivalent. It can be added compatibly later.

## Consequences

- `preset` is a reserved word; a file named `preset` needs `arraw ui preset`.
- `arraw preset` bare, `preset help`, or an unknown verb print the preset
  usage (unknown verb: exit 2, matching dispatch rule 3).
- The supported-extension check reuses the loaders' existing extension list —
  the CLI must not grow its own definition of "image file".
- Tests (GPU-free, `arraw_cli` static lib, injected `PresetStore` dir): name
  matching incl. case variants, `--json` validity and shapes, the apply
  pre-flight refusals, sidecar-preserving apply on files with and without
  sidecars, exit tiers.
- Future preset verbs (`save`, `delete`, `rename` — `PresetStore` already
  implements the latter two) slot into the same grammar but are deliberately
  not specified here.
