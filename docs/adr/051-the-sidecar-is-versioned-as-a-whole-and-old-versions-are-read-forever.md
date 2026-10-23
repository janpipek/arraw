# The sidecar is versioned as a whole, and old versions are read forever

Status: accepted, 2026-10-19.

The sidecar (`Sidecar.h`) is the one file of ours that outlives a build of arraw: a photographer
opens years-old edits in a newer arraw, and may open a newer arraw's edits in an older one. Until
now the format had `arraw:version`, a second `arraw:localAdjustmentsVersion` for the mask list
(ADR 044), a namespace URI spelled `http://ns.arraw.org/develop/1.0/` ("a placeholder until
confirmed") and no test that an old file is still read. These decisions are made by the user:

- **One version, `arraw:version`** (`sidecarVersion`), for everything arraw puts in a sidecar:
  the settings, the negative, the local adjustments. The list is the same format as the rest; a
  version of its own only gave two numbers to keep in step. `arraw:localAdjustmentsVersion`,
  `localAdjustmentsVersion` and `Notice::NewerLocalAdjustmentsVersion` are gone. A sidecar still
  carrying the key reads as before and loses the key when written. The state JSON likewise has
  only its `"arraw"` version; a `"version"` inside `localAdjustments` is ignored.
- **The namespace is frozen without a version**: `http://ns.arraw.org/develop/`. A version in the
  URI would orphan every file at each bump. The legacy `.../develop/1.0/` is read as the new URI
  (`parse` in `Sidecar.cpp`) and so written back as the new one; no file of an earlier build is
  lost.
- **Arraw content with no `arraw:version` is version 1.** Hand-made files, and sidecars that
  hold only the negative or other keys written without it, read with no warning, and a write
  adds `arraw:version` when it writes develop state.
- **A newer arraw reads every older version.** Always, with no warning. A file is overwritten as
  the current version when the photographer saves (it already was, key by key).
- **An older arraw may try a newer version, and may fail.** It reads what it understands and
  reports `Notice::NewerSettingsVersion`; it is free to read less than the file says.
- **An older arraw never writes over a newer sidecar**, a marks-only write included (a rating
  would otherwise rewrite the file under the old version and drop what it did not read). The
  write throws and the file stays as it was. This is the rule of ADR 044 extended to the whole
  file.
- **When the version is raised.** Only for a change an older reader would get wrong: a key
  changing meaning, type or range, or a new key that must not be ignored. A new optional key whose
  absence means the old behaviour does not raise it (the old arraw reports it as unknown and
  keeps it on write, as `arraw:futureKnob` in the foreign-sidecar fixture).
- **Every version stays tested.** `tests/fixtures/sidecars/v<N>/*.xmp` are frozen files as that
  version's writers produced them. A test reads each, requires no warning and the same
  contents, then writes it back and requires the current version, the current namespace and the
  same contents again. It also requires a directory for the current `sidecarVersion`, so raising
  it forces a fixture. A fixture is never edited; one for a bug of an older writer is added.

## Consequences

- `settingsJsonVersion` (the state JSON, `"arraw"`) is a separate constant: it versions a
  different serialisation of the same model, and may move on its own. The same policy holds for it
  (read older, warn on newer).
- ADR 044's `localAdjustmentsVersion`, and the brush plans' "rasteriser 2 comes with list version
  2", now mean `sidecarVersion` 2.
- The first real bump needs a migration only when old data cannot be read as it is; the reader
  keeps one code path per rule, not per version, until then.

## Amendment: version 2 for snapshots (2026-10-23)

ADR 056 deliberately raises the sidecar to version 2 on **every successful Arraw sidecar
write**, including marks-only and thumbnail writes. This supersedes the earlier rule above that
an optional key alone does not justify a bump, and the earlier marks-only behavior that left
`arraw:version` unchanged. The project is not yet at a release where preserving older writers
matters to its single user. Older Arraw builds may read what they understand, but cannot write a
sidecar once this build has written it. The v1 reader and frozen v1 fixtures remain so existing
photographs open correctly.

Version 2 also separates version from active-state presence through
`arraw:activeStatePresent`. A marks-only or snapshot-only v2 write must preserve whether the
sidecar had saved develop settings; `arraw:version` alone continues to imply an explicit state
only for v1 files. The version-2 writer determines presence using the pre-write version's rule.

Every Arraw sidecar write also takes the sibling `<sidecar>.lock` (`QLockFile`, 2000 ms), rereads
the file, patches only its own fields and replaces it through `QSaveFile`. Frozen version 2
fixtures live in `tests/fixtures/sidecars/v2/`; the version 1 fixtures are unchanged.
