# The command line: two binaries, a command table, and a contract scripts can rely on

`src/cli/main.cpp` was `int main() {}`. `desired-features.md` promises three
commands — `export`, `preset` and `info` — and **none is buildable as
specified**: `preset` needs presets, `info` needs the metadata reading deferred
in ADR 005, and `export` has no develop settings to apply, since
`DevelopSettings` and `Photo` are both empty structs.

That does not make a command line premature. What the engine can already do —
read any supported file including RAW, write PNG, JPEG or TIFF — is a useful
tool on its own, and the first user-facing proof the engine works end to end.

## Decision

**Two binaries**, `arraw-ui` and `arraw-cli`, as `desired-features.md` writes
them. (`main` later converged on a single `arraw` with `ui` as a subcommand,
driven by Windows packaging concerns that do not exist here yet.)

**The command is named `export`, not `convert`.** It applies no develop
settings today because there are none; once there are, it applies whatever an
image has, and an image with no sidecar has none. That is the *correct*
behaviour of `export` for an unedited file rather than a placeholder for it.
Following `main`'s rule that command names are reserved words forever, a rename
would be the expensive mistake.

```
arraw-cli export <input>... -o <dir> [--format] [--quality] [--bit-depth]
                                     [--sharpen] [--encoding] [--no-profile]
                                     [--overwrite] [--quiet]
```

*Added later:* `--device` (`auto`, `cpu`, `gpu`, or `gpuN` for the N-th adapter),
`--gpu-backend` and `--allow-software` choose where development runs; see
[ADR 017](017-the-command-line-prefers-the-gpu-and-says-when-it-does-not.md).

- **Inputs are files, never directories.** The shell expands wildcards. A
  directory input would need a "which files count" policy involving RAW+JPEG
  pairing that the engine cannot answer yet, and approximating it would be worse
  than refusing it. *(Superseded 2026-10-03: see the note at the end and
  [ADR 029](029-a-folder-is-a-list-of-shots.md).)*
- **`-o` names a directory, always**, which must already exist. The tempting
  alternative — a single input plus an image-shaped `-o` means "write exactly
  this file" — makes the meaning of `-o` depend on how many inputs were passed.
  That bites in a script the day a glob matches one file instead of three. A
  `--output-file` flag can be added later without ambiguity.
- **An existing output is refused unless `--overwrite` is given.**
  `exportImage` replaces a destination without a word, which is right for a
  library and wrong for a batch pointed at a folder of someone's negatives.

**Every input is attempted.** A failure is reported and the run continues, so
one corrupt frame cannot abandon an overnight batch (`desired-features.md:99`).

**Exit codes: `0` all exported, `1` at least one failed, `2` usage error.**
Usage is distinguished from failure because a script retrying on `1` would
otherwise loop forever on a mistyped flag.

**stdout carries only what was asked for** — `--help`, `--version`, and
machine-readable output when that arrives. Progress, warnings and errors go to
stderr. An export writes nothing to stdout at all; if progress went there now,
that channel could never be used for a `--json` listing later.

**Commands are rows in a dispatch table**, not a special case in `run()`. Three
are registered: `export`, and `info` and `preset` with a null `run`. A null
`run` is what "reserved but not built" means — no stub function, no apology
printed from a binary. The top-level help lists the commands *from that same
table*, so it cannot advertise a command that does not dispatch, and a reserved
one is marked. (When written, `info` was reserved; see the note at the end.) Typing `info` then got "not implemented yet"; typing `develop` gets
"unknown command", because someone who typed what the documentation promised did
not make the same mistake as someone who guessed.

A plain struct with a function pointer, not a `Command` base class — CLAUDE.md
is explicit about not overdoing polymorphism, and these are free functions.

**Parsing is `QCommandLineParser`, and the help is Qt's**, generated from the
options rather than hand-written. **Each command owns its own parser**, so
`arraw-cli export --help` lists export's options and no other command's, and
`help <command>` is routed to that same parser rather than being a second text
that can drift. `QCoreApplication` is required regardless —
Qt resolves image-codec plugin paths against an application instance, without
which every format beyond PNG fails to encode — so Qt Core is already paid for.
A hand-written help text and ANSI colour were built and then removed: they were
a maintenance surface with no user waiting for them, and generated help cannot
drift from the flags.

**`cli::run(arguments, out, err)` lives in a static library** that both
`arraw-cli` and `arraw-tests` link, alongside `Command.cpp` (the table) and one
file per command. `main.cpp` is reduced to collecting `argv`, calling it, and
constructing the Qt application a command asks for. A command asks once its
arguments are good — export for a `QCoreApplication`, the GPU probe for a
`QGuiApplication` — so help and usage errors never load a platform plugin, one
that cannot start aborting the process. Qt is shown only `argv[0]`: given the
rest it removes the options it takes for its own, even after `--`, so
`QT_QPA_PLATFORM` rather than `-platform` chooses a platform. Adding a command
is a row and a file.

*Added 2026-09-30, with sidecars:* each input renders through its own sidecar
(see the sidecars plan, step 3). The flags are a partial edit, not a settings
value: the request keeps an ordered list of (descriptor, encoded value) for just
the flags given, and each input's settings are `openPhoto`'s, its sidecar's,
with those edits decoded on top through the same codec the sidecar uses. A range
is still refused as a usage error, never clamped. Cross-field rules are relative
to that base: `--temperature` or `--tint` makes white balance Custom, and the
half not named keeps the photograph's own value when its sidecar is Custom and is
as shot otherwise (ADR 008: a partial edit leaves what it does not name
untouched); `--white-balance as-shot` clears both. Whatever the flags leave
out stays as the sidecar has it. The geometry flags are setters, applied by the
rules core's `Edits.h` gives every front end (ADR 014), which read the
photograph's declared size: `--rotate` sets rotation and straighten,
`--flip-horizontal` and `--flip-vertical` set a flip and `--no-flip-horizontal`
and `--no-flip-vertical` clear it. A sidecar's explicit crop is carried with the
content it selects, composed as the editor composes it, a custom ratio is
reciprocated when the frame's sides swap, and a straighten shrinks it only as
far as rotated content requires. A flag that names the value the sidecar
already has changes nothing, so no crop moves and nothing is logged.
`--crop` with a rectangle replaces the crop and frees the aspect unless
`--crop-aspect` is given too, and `--crop auto` keeps the aspect;
`--crop-aspect` alone fits the largest crop of that ratio inside the sidecar's
rectangle, about its centre (an automatic crop stays automatic). Given
together, `--crop` and `--crop-aspect` that disagree are reshaped to the aspect,
as the rectangle is fitted and the aspect then applied, rather than failing the
file. There is no `CropReset` warning any more. `applyEdits` drops
the temperature and tint of a sidecar that is not Custom even with no flags,
which changes nothing a render reads. `--no-sidecar` ignores sidecars (defaults
plus flags). An unreadable sidecar fails that input (exit 1, the rest of the
batch still exports), because opening it bare would silently lose the
photographer's edits; `--no-sidecar` exports it anyway. The command never writes
a sidecar. Sidecar warnings and errors go through the same log, in text and
JSON, with the photograph as their subject.

## Consequences

- **The command line is tested in-process**, which departs from ADR 004's
  black-box stance deliberately. There the seam under test was the file-to-file
  path, so going through files was the point. Here the seam is
  arguments-to-exit-code, and `run()` is exactly that seam — injecting the two
  streams makes "stdout stays empty" a direct assertion rather than an
  inference. The cost is that roughly five lines of `main.cpp` are untested.
- **`process()` is unusable**, since it writes to the real stderr and calls
  `exit()`. `parse()` is used instead and Qt's help and version options are
  checked by hand rather than acted on for us.
- **An export currently looks disappointing.** A RAW rendered with no develop
  settings is a flat, faithful conversion, not a photograph. That is stated in
  the help text rather than left to be discovered, and it became less true when
  Exposure and white balance landed.
- **Everything the command says goes through a diagnostic log** rather than
  being written to the stream by hand, so `--log-format json` costs one writer
  and an overnight batch can be read by something other than a person. The
  summary line goes through it too, or a JSON reader would have one line to
  skip.
- **`preset` is advertised but refuses to run**, exiting `2`. That is
  a deliberate trade: `--help` names a command that does nothing, which is the
  cost of telling a reader following `desired-features.md` that the feature is
  coming rather than that they mistyped. It stops being a trade when it lands.
  (`info` was the second such command until 2026-09-30; see the note below.)
- **`QCommandLineParser`'s lack of subcommands costs nothing here**, because
  dispatch happens before any parser is built and each command constructs its
  own. What Qt cannot do is put the command word in its usage line — it knows
  only `argv[0]` — so each command passes the word through its positional
  `syntax` argument.
- Qt claims `-v` for `--version`, so a future verbose flag needs another letter.

## Note, 2026-09-30: `info` is implemented

`arraw-cli info <file>... [--all] [--json] [--no-sidecar] [--quiet]
[--log-format]` opens each file as a document (no pixels are read) and shows its
path, pixel size, orientation, colour encoding (`camera` for a RAW, else the
named encoding), its sidecar (the path, or `none`), its rating (`rejected` for
-1) and colour label when set, and the develop settings that differ from the
defaults, one `key: value` per line in table order, spelled by the same codec as
the sidecar and the JSON settings document. `--all` lists every setting. `--json`
prints one document, `{"files": [...]}`, with the settings as an object of the
same values. The report goes to stdout, diagnostics to stderr through the log
export uses. As in export, an unreadable input or sidecar fails that file
(exit 1) while the rest are shown, and `--no-sidecar` opens the file bare.
Nothing is ever written.

The EXIF of the camera is shown from 2026-10-03 (ADR 028): after the
encoding line, one `label: text` line for each of `camera`, `lens`, `exposure`
(`1/250 s  f/2.8  ISO 400  35 mm (52 mm equivalent)`), `exposure bias`, `flash`,
`taken` (with the UTC offset), `GPS` (latitude, longitude and altitude, signed)
`artist` and `copyright` that the file records; a file that records none shows
none of them and prints no notice. `--json` gives an `exif` object per file with
the fields present, named as in `ExifInfo` (`make`, `lensModel`, `exposureTime`,
...), rationals as `{"numerator": n, "denominator": d}` and `gps` as
`{"latitude", "longitude", "altitude"}`. A file whose EXIF exiv2 failed to read
is still shown, with a `exif_unreadable` warning.

`info` also says which other tools left information in the sidecar, from what
reading it already finds (ADR 019): an `other tools:` block with
`written by: <xmp:CreatorTool>` and one line per foreign namespace,
`<owner or unknown> (<prefix>:, N properties)`. `--json` gives `creatorTool`
(string or null) and `others` (`uri`, `prefix`, `properties`, `owner`) per file.
Nothing is shown for a sidecar only arraw wrote, or with `--no-sidecar`.

## Note, 2026-10-03: folders and the marks filter

Inputs of `export` and `info` may be folders (ADR 029). A folder expands to the
primary files of its shots, in natural order, not recursively; a RAW+JPEG pair is
one shot, and only the RAW is exported or listed as a file of its own. Files
given directly behave as before, and so does everything else in this record.
`info` shows, for a shot found in a folder, a `format:` line (`ARW+JPEG`) and a
`companions:` line with the names; `--json` gives every file `format` and
`companions` (an array of paths, empty for a file given directly).

```
arraw-cli export <input>... -o <dir> [...] [--min-rating N] [--rejected] [--label NAME]...
arraw-cli info   <input>... [...]          [--min-rating N] [--rejected] [--label NAME]...
```

`--min-rating N` (1 to 5) wants at least N stars; `--rejected` wants rejects
only and is a usage error with `--min-rating`; `--label` takes red, yellow,
green, blue or purple in any case, and several labels mean any of them. The
dimensions combine by AND. A photograph is tested by the marks in its sidecar,
the default marks (none) when it has none or under `--no-sidecar`, whether it
was given as a file or found in a folder. A photograph left out is not a
failure: one summary line, `N left out, as their marks do not match the filter`
(notice `filtered_out`, informational, so `--quiet` drops it), and the exit
status is unchanged. A bad rating, an unknown label or the conflict is a usage
error (2) before any file is touched. An empty folder reports `no photographs`
(notice `no_photographs`) and is not an error; an unreadable folder fails as an
input (1), and a sidecar that cannot be read fails its photograph under a
filter, since its marks are unknown. The batch summary counts folders that could
not be read among the failed inputs.

## Note, 2026-10-03: exported metadata

`export` takes `--metadata LIST` (ADR 032): `all`, `none`, or a comma list of
`capture`, `location` and `descriptive`, in any case. The default is
`capture,descriptive`; location (GPS) is carried only when named. The photograph's
camera, lens, exposure and time, its rating and label (from the sidecar's marks)
and its title, caption, keywords, creator and rights (from its own XMP and its
sidecar, the sidecar winning) go into the JPEG, PNG or TIFF, and arraw's version
into its `Software` tag. `--no-sidecar` also ignores the sidecar for this. An
unknown name is a usage error (2). A photograph whose metadata exiv2 cannot read
fails as an input (1) with a hint to pass `--metadata none`; nothing is written
for it.
```
arraw-cli export <input>... -o <dir> [...] [--metadata LIST]
```

## Note, 2026-10-03: terminal styling and untrusted text

Human-readable output strips terminal control sequences from filenames,
metadata, diagnostic messages, arguments reported in usage errors, and GPU
report values. These fields can come from outside arraw: a filename or camera
tag must not clear the screen, change the clipboard, or inject report lines.

`cli::terminalText` in `src/cli/TerminalText.cpp` removes ANSI sequences,
including CSI and OSC sequences and control-string payloads, plus C0/C1
controls and DEL. It strips the sequences entirely rather than displaying
escape notation. An unterminated control string consumes the remainder of
that field. Ordinary Unicode text is preserved; embedded line breaks and tabs
are removed, while the report writer owns indentation and line breaks.

`cli::accented` cleans its text before adding arraw's own ANSI styling. Other
text fields use the same helper directly. This keeps the existing colours and
layout while preventing supplied text from contributing terminal controls,
whether colour is enabled or output is redirected. The earlier removal of ANSI
colour described above is superseded by this shared styling helper; help still
comes from the parser.

JSON reports and diagnostics keep the original field values through their JSON
serialization; terminal cleaning belongs to human-readable output, not the
engine's metadata or paths. CLI regression tests cover injected sequences in
filenames, sidecar metadata, diagnostics and usage errors, Unicode preservation,
and cleaning before trusted styling is added.
