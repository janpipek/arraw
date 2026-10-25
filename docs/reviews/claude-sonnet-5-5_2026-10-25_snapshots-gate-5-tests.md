# Review: snapshots Gate 5 (CLI and Python), tests lens (uncommitted working tree)

Reviewer: Claude Sonnet 5.5, 2026-10-25. Scope: the uncommitted Gate 5 changes read against
[ADR 056](../adr/056-snapshots-are-per-photo-looks-with-independent-history-and-sidecar-storage.md)
(decision 10, the Verification list, addendum items 18 to 20), the Gate 5 entry of
[`todo.md`](../todo.md) ("Verify text/JSON parity, unknown IDs, option conflicts, repeated exports,
and Python parity") and the CLI/Python paragraph of the
[architecture proposal](../ideas/snapshots-architecture-proposal.md). Lens: do the tests prove the
verification list; vacuous assertions; export-equals-render proofs; the stubs test; documentation
drift; weakened existing tests.

Files read: `tests/test_CliSnapshots.cpp`, `tests/python/test_snapshots.py`,
`tests/python/test_stubs.py`, `tests/python/conftest.py`, `tests/CMakeLists.txt`, the diffs of
`src/cli/ExportCommand.cpp`, `src/cli/InfoCommand.cpp`, `src/core/Snapshots.cpp`,
`include/Snapshots.h`, `src/python/arraw/_arraw.pyi`, `stubs.pattern`, `__init__.py`, and the doc
diffs (`README.md`, `docs/todo.md`, the ADR addendum). I did not build or run anything, so every
"would still pass" below is by reading, not by mutation. The one exception is a read-only AST scan
of the committed `_arraw.pyi` for undefined names (S1).

**Counts:** major 4, minor 9.

Summary: the info tests are solid: text and JSON are each checked against the document read back
from disk (a shared oracle, not a copy of the formatter), the thumbnail test proves its premise
first (the thumbnail exists before `info` runs), and the unreadable-entry case is built from a
real forged entry. The repeated-export test checks the sidecar byte for byte. The Python
CLI-versus-render parity compares decoded 16-bit pixels with the device pinned the same way as
`test_cli_parity.py`. Existing tests are not weakened; the only edit to one is `datetime` joining
the stub import allow-list, which is legitimate. The gaps are in what the export tests can tell
apart. Every distinguishing assertion rests on one fact (snapshot 2 equals the saved look), the
"flags apply on top" test passes even if `--snapshot` is ignored, and nothing exercises the part
that makes `withSnapshot` special (the negative, and no lens rematching) through the CLI or
Python. Separately, the regenerated stub uses `Iterator` without importing it, and the stubs test
cannot see that.

---

## Major

### S1. `_arraw.pyi` uses `Iterator` without importing it, and the stubs test cannot notice

`src/python/arraw/_arraw.pyi:2647` (`def __iter__(self) -> Iterator[Snapshot]`), imports at
`:3`; `tests/python/test_stubs.py:22-34`.

The import line is `from collections.abc import Callable, Sequence, Set`; `HEAD` had no use of
`Iterator` at all, so this is new. A read-only AST scan of the file finds exactly one undefined
name that is not an enum member or class attribute: `Iterator`. mypy and pyright report
`Name "Iterator" is not defined` for every user of the stubs, and a user's `for s in
doc.snapshots` type-checks as `Any` or errors depending on the checker. At runtime nothing
breaks, so no test fails.

Why the tests miss it: `test_stub_imports_only_real_modules` checks the set of imported modules
against an allow-list (and now allows `datetime`), but nothing checks that each name the stub uses
is defined or imported. `test_stub_declares_every_exported_name_of_the_extension` checks classes
and functions only. The implementer's report says `_arraw.pyi` was "regenerated", so the
generator emitted the annotation without the import, or the import was lost afterwards.

Fix: add `Iterator` to the import (via `stubs.pattern` or the generator invocation), and add a
test that every `ast.Name` used in an annotation resolves to a class, import, builtin or
module-level name of the stub. That test would also guard the next `__iter__` binding.

### S2. "Flags apply on top of the snapshot" passes when `--snapshot` is ignored

`tests/test_CliSnapshots.cpp:282-288`.

The test exports `--snapshot 2` (`dark`) and `--snapshot 1 --exposure -1` (`overridden`) and
requires them equal. Snapshot 2 is the saved look (exposure -1, see the Rig at `:75-87`), and the
flag is also exposure -1. So an implementation that ignores `--snapshot` entirely renders the
sidecar look (-1) with `--exposure -1` on top, which is `dark`. The test does exclude the wrong
order (snapshot applied after the flags would give snapshot 1, +1, different from `dark`), but
the ADR sentence it is supposed to prove, "applies the snapshot recipe before explicit export
overrides", needs both halves: the snapshot is the base, and the flag wins over it.

Failing scenario: a refactor drops the `withSnapshot` call from `exportAll` for a code path (for
instance the `--export-preset` branch); this test and `Export --snapshot renders the snapshot's
look` still pass for snapshot 2, and only the snapshot 1 assertions in the latter would catch it.

Fix: override a different setting from the one the snapshots differ in, and compare against an
independent twin. For example `--snapshot 1 --contrast 20` must equal an ordinary export of a
photograph whose sidecar is snapshot 1's state with contrast 20 (the twin technique already used
at `:269-279`), and must differ from `--contrast 20` over the saved look. Keep the exposure case
as the "flag wins" half.

### S3. Nothing proves the negative is taken from the snapshot, or that no lens is rematched, through the CLI or Python

`tests/test_CliSnapshots.cpp:75-87, 347-363`; `tests/python/test_snapshots.py:346-355, 368-381`.

The point of `withSnapshot` (ADR addendum 18, header comment in `Snapshots.h`, CLI option help:
"its develop state and negative settings, exactly, with no lens matched again") is that the
snapshot's negative and lens settings are used verbatim. The Rig and the Python `made` fixture
create both snapshots with the photograph's default negative and a lens-less linear DNG, so:

- `made_photo.negative == snapshot.negative` (`test_snapshots.py:354`) is vacuous, as
  `snapshot.negative == photo.negative` already (the default negative on a fixture with no lens).
  A `with_snapshot` that forgot to copy the negative would pass.
- The CLI and parity tests cannot tell whether the export read the snapshot's negative or the
  sidecar's, because they are identical.
- The "no lens rematching" claim is exercised nowhere: the only fixture that names a lens
  (`lens-canon-32x24.dng`, with `lens_database_dir`) is not used, so a regression that matches
  the lens again on the way through `Photo::with` or `applyLensOptions` is invisible.

The C++ unit test at `:347-363` does set a different negative (`Demosaic::Vng`), but it tests
`withSnapshot` alone, not the wiring into `exportAll` (which does the lookup, the
`watch.unreadable` skip and the `document.photo =` assignment) and not the Python binding.

Failing scenario: `exportAll` is changed to build the snapshot photo from `document.photo.with(
snapshot.state)` (state only). Every test in both files stays green; the exported look silently
uses the sidecar's demosaic and lens settings.

Fix: in the Rig and in `made`, make snapshot 1's negative differ from the saved one (a Bayer
fixture with `--demosaic`-visible detail, `bayer-detail-32x24.dng`, exists for exactly this), and
export it through the CLI and compare with Python `with_snapshot`. For the lens, use the lens DNG
with a snapshot whose lens settings were chosen differently from what matching would give
(`lens_database_dir`), and assert the exported/rendered correction is the snapshot's.

### S4. The multi-input behaviour (open question 5) and the accepted ID spellings have no test

`src/cli/ExportCommand.cpp:1253-1270`; `tests/test_CliSnapshots.cpp:305-345`.

The implementer's report states: "`--snapshot` with several inputs looks the ID up per
photograph, and a photograph lacking it fails alone. Leading zeros are accepted." Neither is
tested. Every `--snapshot` test passes exactly one input.

- A photograph lacking the ID fails alone: the lookup throws inside the per-file loop, so the
  neighbours should still export and the exit code should be 1. A change that moves the lookup
  out of the loop (a natural "validate up front" cleanup) would turn this into an all-or-nothing
  failure, or into a usage error, and no test would object. The unknown-ID test checks only that
  the single output does not exist.
- The parser tests are all rejections. There is no positive case for `007` (reported as
  accepted), `18446744073709551615` (the largest ID) or an ID given twice (`--snapshot 1
  --snapshot 2`, where Qt's `value()` returns the last or first depending on version; the help
  says nothing).

Fix: add a two-photograph case (one with the snapshot, one without) asserting exit 1, the first
output present, the second absent, and the error naming the second file; add positive parsing
cases, and decide and test what a repeated option means (or reject it).

---

## Minor

### M1. The bad-ID test asserts only the exit code, so it cannot tell which check rejected the input

`tests/test_CliSnapshots.cpp:336-345`.

All twelve generated values require `UsageError` and no PNG. For `-1` and `+1` the rejection may
come from Qt's option parsing rather than from `parseSnapshotId`; for `"١"` (Arabic-indic digit)
it may come from the digit test or from `from_chars`. The test would pass if `parseSnapshotId`
accepted every one of them but `QCommandLineParser` or a later stage failed with a usage error
for some other reason. Also nothing is asserted about stdout, or that the error names `--snapshot`
(the conflict test does check that).

Fix: `CHECK_THAT(result.err, ContainsSubstring("--snapshot takes a snapshot ID"))` per case, and
call out the Qt-consumed forms separately.

### M2. Every snapshot-2 comparison is satisfied by the saved look

`tests/test_CliSnapshots.cpp:261-267, 290-303`; `tests/python/test_snapshots.py:368-381`
(`which=2`).

The Rig saves with snapshot 2 as the active look, so `dark == saved` and the `which=2` Python
parity case would hold even if `--snapshot` and `with_snapshot` were no-ops for snapshot 2. Only
the snapshot 1 assertions distinguish. The tests are not wrong, but half of the evidence is
weaker than it looks. Making the saved look a third state (an exposure of 0, or the unsaved
default) removes the coincidence and costs one line in each fixture.

### M3. Info cases that the text and JSON paths treat differently are untested

`tests/test_CliSnapshots.cpp:141-255`.

- Snapshots present but saved base `none` (a Create that was never saved): the text prints
  `saved base: none` only because the `!entries.empty()` branch fires (`InfoCommand.cpp:726`);
  the test has only "saved base 2" and "nothing at all".
- Only unreadable entries and no usable snapshot: text prints the unreadable line alone and
  JSON gives `[]`, `null`, `1`; the test always has two good entries beside the bad one.
- Several files: `firstFile` reads `files[0]` only, so a second file's snapshots cannot leak
  into, or be lost from, the first's report unnoticed.
- Names that need care: a control character or ANSI escape in a name (text goes through
  `cli::terminalText`, JSON through `jsonString`) and a multibyte name. Only `"` is covered. A
  sidecar is data from another machine, so this is the case `terminalText` exists for.

### M4. Text/JSON parity is shown through the disk document, not between the two outputs

`tests/test_CliSnapshots.cpp:141-181`.

Each output is compared with `openDocument` read back from disk, which is a sound oracle, and it
means the two outputs agree transitively. But there is no assertion that the text lists the same
number of snapshots as the JSON (the text check is `ContainsSubstring` per expected entry, so an
extra spurious text line, or a duplicated entry, would pass). Counting the `    N: ` lines in the
text and comparing with `listed.size()` closes it.

### M5. The held-lock test proves "raises", not "waited"

`tests/python/test_snapshots.py:323-343`.

It forges a Qt lock file and requires `SidecarLockTimeout` within 30 s. It would also pass if the
writer failed instantly (a lock-related error mapped to the same exception, or a timeout of 0).
The bound is loose in the useful direction only. Assert `>= 1.5 s` (the ADR says 2000 ms) to pin
the wait, so a regression to an immediate refusal, or to the 5000 ms stale takeover, is visible.
Failure mode if Qt's lock format changes: the forged file is treated as stale, the rename
succeeds and `pytest.raises` fails, so the test fails loudly rather than passing vacuously, which
is the right way round. It only covers `rename_snapshot`; `create`, `delete`, `discard` and
`save` map the same error through different call sites that are not tried.

### M6. `SnapshotId` strictness is tested in the constructor only

`tests/python/test_snapshots.py:38-52`; ADR addendum 20 ("no implicit conversion from `int`").

The tests prove `SnapshotId(True)`, `SnapshotId(1.0)` and the range errors. They do not prove the
claim as stated: `session.restore_snapshot(1)`, `snapshots.find(1)` and `1 in snapshots` should
raise `TypeError`, and nothing calls them with an `int`. If nanobind's implicit conversion were
enabled for the type (one line in `BindSnapshots.cpp`), the test file stays green and the ADR
sentence becomes false. The stub also binds `__index__`, which is not mentioned in the ADR; that
is a deliberate loosening (a `SnapshotId` can index a list) and deserves a test or a line.

### M7. The Python session surface has several unexercised corners

`tests/python/test_snapshots.py`.

- `EditSession(photo)` (no document) followed by `create_snapshot` is untested; the only
  assertion is that it "knows no snapshots" (`:108-111`).
- `discard_unreadable_snapshots` and `save` under a revision conflict or lock timeout are not
  tried (only create, rename and delete are in `:229-239`).
- `rename_snapshot` / `delete_snapshot` of an unknown ID on a stale session (conflict versus
  `ValueError` precedence) is untested.
- The documented deviation that these methods hold the GIL while waiting for the lock has no
  test and no mention in the ADR addendum; a user thread would block for up to two seconds.

### M8. `Export --snapshot` with a damaged sidecar is not covered

`tests/test_CliSnapshots.cpp:207-221, 314-324`; `ExportCommand.cpp:1257`.

`--snapshot` is documented as failing when the sidecar is unreadable (the lookup is skipped when
`watch.unreadable`, so the sidecar error wins). The likely real-world case, a sidecar with one
damaged snapshot entry, is `Notice::SnapshotDamaged`, which must not set `watch.unreadable`.
`addUnusableEntry` already exists in the test file, but no export test uses it. A test that
exports `--snapshot 1` from the damaged Rig (success, the good entry still reachable) and one
with a wholly unreadable sidecar (exit 1, the sidecar message, not "no snapshot N") would pin the
branch and the error precedence.

### M9. Documentation drift and process

- `docs/todo.md:382` marks Gate 5 `[x]` while the work is uncommitted and under review; the same
  list says not to begin a later gate "before the previous one is reviewed and committed", and
  gates 1 to 4 became `[x]` with their commits. Harmless for the last gate, but flip it after the
  review.
- ADR addendum 20 lists what Python binds but omits the public `write_sidecar(photo, saved_base)`
  overload (the report's first deviation), `Snapshot.has_thumbnail` as the only thumbnail
  surface, the UTC `datetime` for `created`, and the GIL behaviour. Python API additions that the
  ADR does not record are the usual way a later gate contradicts an earlier one.
- Addendum 19 and the CLI help say IDs are "plain decimal digits above zero". Leading zeros
  (`007`) and the repeated option are the two behaviours that sentence leaves open (see S4).
- `info`'s help ends the unreadable note with "the app can discard them"; Python now can as well
  (`discard_unreadable_snapshots`). Cosmetic.
- `docs/desired-features.md` lists the CLI examples under "Without opening the window"; Snapshots
  appear above it without a CLI line. `README.md:63-64` was updated. Optional.

---

## Does the test set prove the Gate 5 verification list?

| Item (todo.md, ADR decision 10) | Evidence | Verdict |
| --- | --- | --- |
| Text/JSON parity | `Info lists the snapshots ... in step` against the disk document; unreadable and empty cases | Proven for the main path; M3 and M4 list the holes |
| Never prints JPEG bytes | thumbnail premise checked, three substrings refused in both formats | Proven |
| Unknown IDs | CLI single input (`:305-324`), Python `ValueError` for restore/rename/delete | Proven for one input; S4 for batches |
| Option conflicts | `--snapshot` with `--no-sidecar` gives exit 2 and names both options; no output | Proven. No other combination is argued |
| Repeated exports | Three exports, distinct folders, sidecar byte-identical, snapshot list unchanged | Proven (an export to the same folder fails by the existing "already exists" rule, untested here but not Gate 5's) |
| Export equals render | CLI PNG pixels equal Python `develop(with_snapshot)`; twin photograph for the CLI | Weakened by S3 and M2: negative and lens unproven, snapshot 2 coincides with the saved look |
| Flags after the snapshot | One test | Not proven: S2 |
| Python parity | Info JSON against `EditSession.snapshots`; session operations against the sidecar | Proven, with M6 and M7 |
| Stubs | allow-list widened; no check of the new stubs | S1 |

## Existing tests

Not weakened. `tests/CMakeLists.txt` adds one line. `tests/python/test_stubs.py:33` adds
`datetime` to the allowed imports, required by `Snapshot.created`; the assertion remains a subset
check, so it still catches a stray import, but see S1 for what the module does not check.
