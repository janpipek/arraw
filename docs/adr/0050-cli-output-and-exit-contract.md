# CLI output and exit-code contract: default tables, explicit --json, three exit tiers

The preset command family (ADR 0051) is the first CLI surface that serves two
readers at once: a person at a terminal and a script in a pipeline. `export`
already answered pieces of this implicitly — per-file stdout lines, stderr
failures, `0/1/2` exit codes — but only inside its own ADR. This ADR names the
contract once, CLI-wide, so every future command can cite it instead of
re-deciding it.

## Two output modes, chosen by an explicit flag

Human-readable output (tables, per-file progress lines, summaries) is the
default. Machine-readable output is opt-in via a `--json` flag on the
subcommand.

**No TTY detection.** Auto-switching structure on `isatty` (table when
interactive, JSON when piped) matches the "interactive vs scripted" intuition
but makes the same command produce different bytes in a terminal, a pipe, and
a cron job — `arraw preset list | less` would suddenly show JSON, and a script
tested interactively would break redirected. TTY state may influence
*decoration* (color, column widths) but never *structure*. A script that wants
JSON says so, which also makes the JSON contract testable in ctest without
faking a terminal.

## The --json contract

- With `--json`, **stdout is exactly one valid JSON document** — parseable by
  `jq` with no framing, no banners, no progress lines. Commands emit it once,
  at the end; NDJSON/streaming is rejected because the contract "stdout is a
  document" and "stdout is a stream" cannot both be true, and the commands
  that exist are either instant (preset operations do no decoding) or already
  have human progress lines in the default mode.
- Diagnostics and per-file errors still go to **stderr in both modes**, so
  `2> errors.log` behaves identically with and without `--json`.
- The JSON shape is uniform in kind across commands: a listing emits an array
  of objects, a single-item view emits one object, a batch operation emits one
  result document with per-item status inside it.
- JSON object keys are stable machine identifiers, never localised strings
  (e.g. Develop Group keys come from `developGroupKey`, the same keys the
  preset file format uses — the CLI vocabulary and the on-disk vocabulary are
  the same by construction).

## Exit codes: 0 / 1 / 2

Adopted from `export` (ADR 0049) and now binding CLI-wide:

- **`2` — refused up front.** Usage errors, unknown names, failed pre-flight,
  broken environment. The run performed no work: nothing rendered, nothing
  written. Commands with multiple inputs pre-flight the whole list before
  touching anything — failing at file 300 of 400 is the worst version.
- **`1` — ran, with per-item failures.** Batch commands continue past a
  failing item (one stderr line each, summary at the end) because items are
  independent; aborting mid-run protects nothing and hides how many more
  would have failed.
- **`0` — clean.**

Pre-flight checks only what is reliably checkable (existence, type, name
collisions). **No writability probing** — testing write access up front is
racy and platform-flaky; the actual write reports honestly and lands in the
exit-`1` tier.

## Consequences

- Future commands adopt this contract by reference; deviating requires its own
  ADR.
- `export` already conforms on exit codes and stderr discipline. It has no
  `--json` yet; adding one is mechanical under this contract and deliberately
  out of scope here.
- Tests can pin the contract per command: `--json` output parses as one
  document, exit codes hit the right tier, stderr carries the failures.
