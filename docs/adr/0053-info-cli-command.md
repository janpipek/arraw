# The `info` command: per-file EXIF and edit-state report, read-only

`arraw info <paths>...` reports what arraw knows about one or more image
files — camera EXIF and the sidecar-derived edit state (rating, colour
label, descriptive metadata, non-default develop groups) — headlessly. It
follows the CLI-wide contract of ADR 0050 (human tables by default,
`--json` for one document, exit tiers `0/1/2`) and joins `ui`, `export`,
`preset`, `version`, `help` as a reserved top-level verb (ADR 0049 rule 2).

## Batch of explicit files, not a single-item view

Unlike `preset show <name>` (exactly one match, by construction), `info`
takes one or more paths in a single invocation, following `apply`/`export`'s
model rather than `preset show`'s: inspecting a folder's worth of files in
one script call is the common case, and a single path is just the
one-element case of the same input shape.

- **Pre-flight the whole list first**: each path must exist, must not be a
  directory, and must carry a supported image extension (the loaders'
  existing list, per ADR 0051's consequence — the CLI does not grow its own
  notion of "image file"). Any failure here refuses the entire run, exit
  `2`, nothing read.
- **Per-file read failures are a batch-1 tier, not a refusal**: a
  correctly-extensioned but corrupt file that LibRaw can't open fails that
  file only — stderr line, continue, exit `1` overall — mirroring `apply`'s
  per-file write-failure handling. Files are independent; one bad file
  shouldn't hide the report on the other 299. An unreadable sidecar is the
  same tier for the same reason: the report is missing something real, and
  a script that trusts exit `0` must not be told everything was fine.
- **No directory/recursive mode**, matching `apply`'s deferral of the same
  question (ADR 0051): a folder-mode policy answered once should serve both
  commands, or neither.

## What gets reported: EXIF plus the sidecar's edit state

`info` reports two independent things per file:

1. **EXIF**, read via the existing LibRaw-only `extractMetadata` path (no
   decode/unpack, no GPU — same cheap `open_file` used for FilmStrip
   tooltips). Standard-image formats (JPEG/PNG/TIFF/WebP/BMP) go through
   `StandardImageLoader`, which extracts no EXIF today; `info` reports an
   empty/near-empty EXIF object for those rather than growing new
   extraction — that gap is a pre-existing app limitation, not this
   command's to close.
2. **Sidecar-derived edit state**, via `XmpSidecar`: rating, colour label,
   descriptive fields (title/caption/creator/copyright/keywords), whether a
   sidecar exists at all, and — the reason this command exists rather than
   being redundant with any generic EXIF viewer — **which develop groups are
   non-default, and their actual field values**. A missing sidecar is not an
   error; it reads as all-default, distinguished from "edited back to
   default" only by the explicit `hasSidecar` flag.

The two halves are read **independently**, and neither failing blanks the
other. A RAW LibRaw cannot open still reports the rating and develop groups
its sidecar carries; only the `exif` half goes missing. Reporting less than
was successfully read would be its own kind of wrong answer.

A sidecar that exists but **won't parse** is a third state, not a synonym
for either of the first two: reporting `XmpSidecar`'s fallback defaults as
"no edits" would silently call an edited photo untouched. It reads as
`Sidecar: unreadable` / `"sidecarUnreadable": true`, warns on stderr, and
joins the per-file failure tier below — the same fact the GUI puts in the
status bar as "Sidecar unreadable; defaults applied".

Develop-group detail deliberately goes deeper than `preset list`'s
names-only summary: a preset is a reusable definition one command-hop away
from `preset show`'s detail, but a *file's* edit state has no such
companion to defer to — "what was actually done to this photo" is the
entire point of asking.

### Local Adjustments are reported too, and cannot come from the groups

Masks live on `GlobalAdjustment` but deliberately *outside* the
`DevelopGroup` enum: they are per-image state a preset never carries
(ADR 0023). So `groupsWithNonDefaultValues` structurally cannot see them,
and a heavily masked photo would otherwise report a near-empty Develop
section while the masks did most of the work — the exact failure that
prompted adding them.

`info` lists them separately: each mask's panel name, its kind, and the
deltas it applies, via `maskDisplayName` and a new
`describeLocalNonDefaults` — the local dual of `describeGroupNonDefaults`,
living beside `localDeltaFields()` in `LocalAdjustment.cpp` so the report,
the Masks panel and History all format a delta the same way
([[spot-for-algorithms]]). **Geometry is never reported**: a mask's
endpoints are not something a person reads, and a brush raster is a PNG.
The question `info` answers is *what does this mask change*, not *where*.

Mask kind names come from `maskKindName`, deliberately **not** shared with
the `arraw:MaskType` literals `XmpSidecar` writes: those are the on-disk
format, and a display rename must never silently change the file format.

Spots are the other per-image state outside the enum (ADR 0017), invisible
for exactly the same reason — and get **a count, not a list**. Every field
of a `Spot` is geometry (destination, source, radius, feather), so "how
many" is the entire reportable content; `Spots: 3` in the table, `"spots":
3` in `--json`, always present there so a script never tests for the key.

## `--json`: stable machine keys, not the GUI's display strings

Both `ImageMetadata::rows` (`{"label": "Focal length (35mm)", "value": "50
mm"}`) and `describeGroupNonDefaults` (`"Exposure +0.4"`) are
display-formatted for the GUI's `InfoPanel` and `preset show`'s table —
useful for `info`'s table mode, wrong for JSON per ADR 0050 ("JSON object
keys are stable machine identifiers, never localised strings"). `--json`
instead uses:

- A small new EXIF-to-stable-keys mapping (`make`, `model`, `lens`, `iso`,
  `apertureFNumber`, `focalLengthMm`, `shutterSpeedSeconds`, `dateTaken`,
  `width`/`height` — active area only, not raw sensor size) with numeric
  fields typed as JSON numbers, not formatted strings — a script filtering
  `iso > 1600` shouldn't have to parse `"f/2.8"` back into `2.8`.
- `groupToJson` (already used internally by `serializeDevelopPreset`,
  currently anonymous-namespace in `DevelopPreset.cpp`) reused for each
  non-default group, keyed by `developGroupKey` — the same native
  serialization `preset show --json` already exposes, not a bespoke report
  schema.
- `colourLabelToString`'s existing canonical strings for colour label — the
  on-disk XMP representation already, zero new mapping.
- A `masks` array, always present (empty when there are none, so a script
  iterating it never tests for the key first), each entry carrying its
  `type` and only the deltas it changed, keyed by `LocalDeltaField::key` —
  the same names `groupToJson` uses for the same quantities, so a local
  delta and a global one read alike.

Top-level shape is a **flat array**, one element per input path — a
listing of independent per-file reports (ADR 0050's "a listing emits an
array of objects"), not `apply`'s split-bucket batch-operation shape,
because `info` performs no single combined action: `[{"path": ..., "exif":
{...}, "hasSidecar": ..., "rating": ..., ..., "developGroups": {"tone":
{...}}}, ...]`. A file whose EXIF failed carries `"error"` inline (also
mirrored to stderr) in place of `"exif"` alone, keeping every sidecar key it
did read; `"sidecarUnreadable"` appears only when it applies, like every
other optional key.

## Table mode: one detail block per file, not a summary row

`preset list`'s one-row-per-item table works because a preset's identity
is a handful of group labels; a file carries ~15-20 EXIF rows plus sidecar
fields plus per-group changed values — too much for row/column shape.
Table mode instead prints one detail block per file (path header, then
key:value lines), reusing the same display formatting `InfoPanel` and
`preset show` already use. A file with nothing noteworthy still gets its
full EXIF block; there is no condensed "headline columns only" view in v1.
Blocks are separated by a blank line and **stream as each file is read** —
a folder's worth of files shouldn't sit silent until the last one opens.
`--json` is the one shape that must buffer, because it emits one document.

Blocks are seasoned with ANSI colour when — and only when — stdout is a
terminal that wants it (`src/cli/TextStyle.h`: `isatty` on stdout's own
descriptor, minus `NO_COLOR` and `TERM=dumb`, plus `CLICOLOR_FORCE` for
`| less -R`). The decision is made once in `main` and passed down, because
the stream a command writes to is a console in production and a `QString`
under test, and only the edge can tell those apart. Colour never carries
meaning alone: labels dim, the path bolds, a colour label prints in its own
colour, and every one of those is decoration over text that already said
it. `--json` is never coloured.

## Consequences

- `info` is a reserved word; a file named `info` needs `arraw ui info`.
- `groupToJson` needs exposing from `DevelopPreset.cpp`'s anonymous
  namespace (declared alongside `serializeDevelopPreset`) for reuse here —
  the only source-level dependency this ADR introduces.
- No field-selection flags (`--exif-only`, etc.) and no `--dry-run`-
  equivalent in v1 — `info` never writes, and the full report is the only
  report; both could be added compatibly later.
- The pre-flight `preset apply` already had becomes the shared
  `cli::preflightImagePaths`, and the CLI11 boilerplate every verb's parser
  repeated becomes `cli::parseArgs` — one definition each of "a usable input
  file" and "the exit tier a parse produces", rather than a third copy.
- Tests (GPU-free, `arraw_cli` static lib): pre-flight refusals (missing
  path, directory, unsupported extension), per-file read-failure handling
  alongside successes in one batch, `--json` array shape and per-file key
  stability, `hasSidecar` true/false/unreadable, non-RAW formats reporting
  empty EXIF without erroring, exit tiers, the parser's own contract, and
  the read-only promise itself (no sidecar appears, none is rewritten).
