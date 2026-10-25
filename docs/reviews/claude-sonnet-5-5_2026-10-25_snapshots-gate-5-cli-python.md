# Review: snapshots Gate 5, CLI and Python

- Reviewer: claude-sonnet-5-5
- Date: 2026-10-25
- Lens: cli-python (ADR 056 decision 10 conformance; `--snapshot` parsing, conflicts, exit codes and messages;
  `info` text/JSON parity and JSON schema stability; export applying the exact recipe before overrides; Python API
  shape and consistency with the existing bindings; stub and docstring accuracy; exception mapping; GIL, threading
  and object lifetime).
- Scope: the uncommitted working tree against `docs/adr/056-*.md` (decision 10 and the new addendum 18-20),
  `docs/ideas/snapshots-architecture-proposal.md` and `docs/todo.md`. Read: `include/Snapshots.h`,
  `src/core/Snapshots.cpp`, the diffs of `src/cli/ExportCommand.cpp` and `src/cli/InfoCommand.cpp`,
  `src/python/BindSnapshots.cpp`, the diffs of `BindSession.cpp`, `BindPhoto.cpp`, `PyBindings.h`, `Module.cpp`,
  `__init__.py`, `stubs.pattern`, `_arraw.pyi`, and `tests/test_CliSnapshots.cpp`, `tests/python/test_snapshots.py`.
  Also the surrounding code they depend on: `EditSession.h/.cpp`, `LensOptions.cpp`, `SidecarWatch.h`.
- Method: reading only. Nothing was built or run, and no production file was touched.

## Verdict

Decision 10 is met. `info` lists ID, parent, name, creation time and saved base in text and JSON and never prints
JPEG bytes. `export --snapshot ID` goes through one core function, `withSnapshot`, which copies state and negative
verbatim; the lens options and the other flags run after it (`ExportCommand.cpp:1271-1292`), so "recipe before
overrides" holds, and nothing in the export path rematches a lens on its own (`applyLensOptions` touches the lens
only when `--lens` or a correction flag is given). An unknown ID throws before the pixels are decoded and before
anything is written; `--snapshot` with `--no-sidecar` is a usage error (exit 2); the ID grammar is strict and tested.
The Python surface is coherent: the three new exceptions subclass what the same failures surfaced as before, the
translator is registered ahead of nanobind's built-in mapping, and holding the GIL in the session methods is
defensible because `EditSession` is not thread-safe.

Nothing here corrupts a sidecar or renders the wrong look. The findings are one stub error, one cross-platform
edge, a few API rough edges, and test gaps around the claims the report makes (lens, batch, repeat).

## Findings

1. **Minor, stub accuracy: `Iterator` is used in `_arraw.pyi` but never imported.**
   `src/python/arraw/_arraw.pyi:2647` (`def __iter__(self) -> Iterator[Snapshot]`, from the `nb::sig` at
   `BindSnapshots.cpp:170`). The stub's imports are `Callable, Sequence, Set` from `collections.abc` only. A type
   checker reports an undefined name, and `for s in doc.snapshots` is typed as unknown. `test_stubs.py` checks
   the import allow-list and the declared names, not that every annotation resolves, so it passes. Fix: write the
   `nb::sig` as `collections.abc.Iterator[Snapshot]` (stubgen then adds the import), or drop the custom signature.
   Consider a stub test that parses annotations and checks each bare name is defined or imported.

2. **Minor, portability: `Snapshot.created` can raise on Windows for far-future times.**
   `BindSnapshots.cpp:74-79` builds the value with `datetime.fromtimestamp(secs, utc)`. The model accepts times to
   year 9999 (`Snapshots.cpp:92`), and a hand-edited sidecar can hold one. On Windows `fromtimestamp` goes through
   `gmtime_s`, which refuses years past 3000, so the property raises `OSError`. Fix:
   `datetime(1970, 1, 1, tzinfo=utc) + timedelta(seconds=secs)`, which is also faster.

3. **Minor, CLI validation: a repeated `--snapshot` is accepted and the last one wins silently.**
   `ExportCommand.cpp:1072-1079` reads `parser.value("snapshot")`. `export a.arw --snapshot 1 --snapshot 2` exports
   snapshot 2 only and prints nothing; a user who expects both gets one file with no hint. The lens options already
   reject a repeat ("is given twice", `LensOptions.cpp:78`). Fix: if `parser.values("snapshot").size() > 1`, give a
   usage error that says to run the command once per snapshot with its own `--output`. Not tested.

4. **Minor, message: an ID that exists only as an unreadable entry reports "no snapshot N".**
   `ExportCommand.cpp:1261-1264`. If entry 5 is damaged, `info` shows `unreadable snapshot entries: 1` and no 5;
   `export --snapshot 5` says "there is no snapshot 5 in its sidecar; `info` lists the snapshots it has". That is
   true but sends the user to `info` for an answer it cannot give. `document.unreadableSnapshots` is at hand in the
   same scope: append " (it holds N entries this arraw cannot read)" when it is above zero.

5. **Minor, API usability: a session built from a `Photo` hits a revision conflict on its first snapshot call, and
   there is no way to avoid it.** `BindSession.cpp:168-181` documents on `__init__` that `EditSession(photo)` knows
   no snapshots, but `create_snapshot`, `rename_snapshot` and `delete_snapshot` (`:289-319`) say nothing of it.
   For any photograph whose sidecar already lists snapshots, `EditSession(arraw.open(p)).create_snapshot("x")`
   raises `SnapshotRevisionConflict` (expected 0), which is the pattern every existing script and test uses. The C++
   `refreshSnapshots()` (`EditSession.h:545`) is not bound, so a script cannot re-read on purpose either; it can only
   provoke the conflict. Options: bind `refresh_snapshots()`; or give `EditSession` a `from_path` constructor over
   `open_document`; or make the `Photo` overload read the collection itself (changes gate-2 semantics, so ask first).
   At minimum, repeat the warning in `create_snapshot`.

6. **Minor, docs: the GIL is held for the whole lock wait and the sidecar rewrite, and the docstrings do not say so.**
   `create_snapshot`, `rename_snapshot`, `delete_snapshot`, `discard_unreadable_snapshots` (`BindSession.cpp:289-319`)
   are bound straight to the member functions, so another Python thread stalls for up to the lock timeout (about 2 s)
   plus an XMP parse of up to 128 MiB. I checked for a deadlock and found none: the only `withoutGil` code that
   takes the sidecar lock (`write_sidecar`) logs nothing, and `PythonLog` is used only by readers, which do not lock.
   Releasing the GIL is not a free fix, because `EditSession` is not thread-safe and the GIL is what keeps two Python
   threads off one session. Keep the deviation, say so in the docstrings ("blocks other Python threads"), and note
   that a per-session guard would be needed before releasing it.

7. **Minor, docstring accuracy.**
   - `Snapshot` (`BindSnapshots.cpp:123-125` and the stub) says "`SnapshotCollection` and `EditSession` make and
     change them". `SnapshotCollection` has no mutators and no constructor; only `EditSession` does.
   - `create_snapshot` and the siblings list `ValueError`, `SnapshotRevisionConflict`, `SidecarLockTimeout` and
     `RuntimeError` for a sidecar it cannot keep whole. Core also throws `std::logic_error` ("An open edit must be
     settled before a snapshot is prepared", `EditSession.cpp:307`), which nanobind turns into `RuntimeError`.
     State that an open edit (`begin_edit` without commit/cancel) is refused, or the caller cannot tell it from a
     damaged sidecar.
   - `stubs.pattern` declares `expected`/`actual`/`base` as always present, but `SnapshotRevisionConflict("x")` made
     in Python has none of them. Harmless; say "set when raised by arraw".

8. **Minor, API consistency.**
   - `SnapshotId` defines `==` and `<` only (`BindSnapshots.cpp:112-113`). `a > b` works by reflection, `a <= b` and
     `a >= b` raise `TypeError`. Add the other two (`nb::self <= nb::self`, `>=`) or `functools.total_ordering`
     semantics.
   - `SnapshotCollection.next_id` is a bare `int` while everything else is a `SnapshotId`; `revision` and
     `SnapshotRevisionConflict.expected/actual` are ints, which is right. Consider `next_id: SnapshotId`.
   - `2 in doc.snapshots` raises `TypeError` instead of returning `False`. That follows the stated strictness, but
     `__contains__` is the one place where a membership test conventionally never raises; return `False` for a
     non-`SnapshotId`.

9. **Minor, efficiency: every read of the snapshots copies every thumbnail.**
   `EditSession.snapshots` is `rv_policy::copy` (`BindSession.cpp:289`), `entries` copies again (`:150-152`), and
   `__iter__` copies a third time (`:169`). With 32 snapshots at up to 64 KiB each, `for s in
   session.snapshots` moves about 4 MiB, and `session.snapshots.find(id)` (used in a loop in the tests) copies the
   collection and then the found entry. Not a correctness issue at these limits. Options: have `Snapshot` hold the
   thumbnail as a shared pointer, or expose the collection by reference with `keep_alive` and accept that it can go
   stale, or leave it and note the cost in the docstring.

10. **Minor, docs: ADR 006 (the command-line record) is not updated.**
    ADR 006 documents the `info` fields and JSON keys one note at a time (`creatorTool`, `others`, `format`...), and
    the `export` usage line. The new `snapshots`, `savedBase`, `unreadableSnapshots` keys and `--snapshot` appear
    only in ADR 056's addendum and the `--help` text. A reader of the CLI record will not find them. Add a short
    dated note to ADR 006 pointing at ADR 056 item 19. Also, ADR 056 decision 10 itself still says "never JPEG
    bytes" and nothing about decimal strings; item 19 extends it, which is fine, but line 38 (the source of "decimal
    strings") is about the sidecar, not the CLI, so the addendum's "as the sidecar's JSON does" is the real
    justification and should stay.

11. **Minor, test gaps against the report's claims.**
    - Lens: the report says the snapshot's negative is used "exactly, with no lens rematching", and the CLI/Python
      parity test (`test_snapshots.py:369`) compares pixels. Both use a DNG fixture with no lens selected, so the
      negative's lens block and `lensCorrectionFor` are never exercised. Add one case with a snapshot whose negative
      names a lens (and a database that lacks it) and check CLI and Python agree and no rematch happens.
    - Batch: "a photograph lacking it fails alone" is not tested. Add two inputs, one with and one without the
      snapshot, and check exit 1, one file written, and one `InputFailed` line.
    - Repeat flag (finding 3), a JPEG input with `--snapshot`, and `saved base: none` with snapshots present are
      not covered. `info --no-sidecar` JSON gives empty `snapshots` and a null `savedBase`, the same as "no
      snapshots"; the test pins this but a consumer cannot tell "not read" from "none" (`sidecarRead` is false there,
      so it is recoverable; mention it in the help).
    - `test_stubs.py` does not catch finding 1.

12. **Info, not a defect: ID spelling differs between surfaces by design.** JSON uses decimal strings
    (`"id": "2"`), the CLI argument is digits, and Python is an `int`-backed `SnapshotId` that prints as digits.
    A script that does `int(entry["id"])` and `SnapshotId(...)` works. The implementer's conservative reading of
    open question 1 is the right one; plain numbers would lose precision in any JSON reader that uses doubles.

## Verified

- **ID grammar.** `parseSnapshotId` (`ExportCommand.cpp:258-268`) accepts only ASCII digits, rejects empty text, a
  sign, space, prefix, fraction, exponent, a non-ASCII digit, zero, and 2^64 (`from_chars` out of range). Leading
  zeros are accepted ("007" is 7), which the report states and I find harmless. The conflict with `--no-sidecar` is
  tested before the ID is parsed, so `--snapshot x --no-sidecar` reports the conflict; both are usage errors (2).
- **Exit codes.** Usage errors give 2; an unknown ID gives `Failed` (1) through the normal per-input path, and the
  rest of the batch continues. The failure is raised before `loadImage`, so no pixels are decoded for it.
- **Order of application.** `withSnapshot` runs inside the sidecar branch, then `applyEdits`, then `--demosaic`,
  then `applyLensOptions`. That is the same order as for an ordinary sidecar, so a flag replaces only what it names,
  and the test (`Export flags apply on top of the snapshot`) shows an override equals the other snapshot's look.
- **Unreadable sidecar.** The snapshot lookup is skipped when `watch.unreadable` is set, so the existing "sidecar
  could not be read" error wins, as for any export.
- **Marks and metadata.** `withSnapshot` keeps the photograph's marks, so the marks filter and the `xmp:Rating`
  carried into the file are the photograph's, not the snapshot's. The descriptive metadata group reads the sidecar
  per `ExportMetadata::useSidecar` (`ImageExport.h:51`); that is described as descriptive properties, so I found no
  sign that the saved look is embedded under a snapshot's pixels (not traced into the Exiv2 writer).
- **Info parity.** Text and JSON read the same `FileReport` fields, use the same time function (ISO 8601, UTC, `Z`)
  and the same decimal IDs. Text prints nothing without snapshots, entries or a base; JSON always carries the three
  members, placed between `others` and `settings`, so existing keys are unmoved and the document stays valid (the
  comma placement at `InfoCommand.cpp:858` is right). Names go through `terminalText` in text and `jsonString` in
  JSON, and names cannot hold control characters in the first place (`Snapshots.cpp:87`).
- **Exception mapping.** The translator is tried before nanobind's built-ins, rethrows what it does not handle (so
  the next translator sees it), builds the instance with `expected`/`actual`/`base`, and the classes are held once
  by the module and once by the translator (`defineError`). The GIL is held when a translator runs, including after
  an exception crossed a `withoutGil` block.
- **Lifetimes.** `OpenDocument` and the session constructor copy, so no reference outlives its owner;
  `keep_alive<1, 3>` is still the lens database. `Snapshot.state`/`negative` are `def_ro` references into a
  `Snapshot`, which nanobind ties to it. The `EditSession(OpenDocument)` overload is distinct from the `Photo` one,
  and the existing `EditSession(photo)` call keeps its meaning.
- **`SnapshotId`.** Refuses `bool`, `float`, `str`, negative, zero and over-range values (`idFrom`); `__hash__` is
  consistent with `==`; `Snapshot`, `SnapshotCollection` and `OpenDocument` set `__hash__ = None` as the other
  value types do.
- **Core.** `withSnapshot` is four lines and has no path to disk or to a lens database. It drops anything the input
  `Photo` had beyond file, metadata and marks, as documented.

## Questions for the author

1. Should `EditSession(Photo)` stay snapshot-blind (finding 5), or should Python have a way to start from the path?
2. Is a repeated `--snapshot` an error (finding 3), or is last-wins the intended rule? I did not check what the
   other single-valued export options do on a repeat.
3. Do you want `SnapshotCollection.next_id` as a `SnapshotId`, or kept as a count-like `int`?
