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
                                     [--encoding] [--no-profile]
                                     [--overwrite] [--quiet]
```

- **Inputs are files, never directories.** The shell expands wildcards. A
  directory input would need a "which files count" policy involving RAW+JPEG
  pairing that the engine cannot answer yet, and approximating it would be worse
  than refusing it.
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
one is marked. Typing `info` gets "not implemented yet"; typing `develop` gets
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
file per command. `main.cpp` is reduced to constructing `QCoreApplication`,
collecting `argv`, and calling it. Adding a command is a row and a file.

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
- **`preset` and `info` are advertised but refuse to run**, exiting `2`. That is
  a deliberate trade: `--help` names two commands that do nothing, which is the
  cost of telling a reader following `desired-features.md` that the feature is
  coming rather than that they mistyped. It stops being a trade when they land.
- **`QCommandLineParser`'s lack of subcommands costs nothing here**, because
  dispatch happens before any parser is built and each command constructs its
  own. What Qt cannot do is put the command word in its usage line — it knows
  only `argv[0]` — so each command passes the word through its positional
  `syntax` argument.
- Qt claims `-v` for `--version`, so a future verbose flag needs another letter.
