# XMP editor compatibility implementation plan

## Goal

Make `XmpSidecar` a safe shared-file module for any namespace-aware XMP editor,
with digiKam as an important compatibility fixture. Do not add catalogue,
keyword, caption, or face-management UI.

## Public contract

- Keep the existing `XmpSidecar` load/save interface.
- Preserve every property outside the ownership rules in ADR 0026 semantically.
- Select an existing stem-only or extension-specific sidecar; create stem-only
  sidecars by default.
- Report two existing naming variants as `ParseError` on load and `false` on
  save.
- Return failure without changing malformed or unreadable sidecars.
- Replace successful saves atomically.

## TDD slices

Each slice is one RED test followed by the smallest GREEN implementation. Tests
exercise only public `XmpSidecar` behavior and parse output semantically.

1. **Foreign metadata survives a User Metadata save.** Start with a packet
   containing a foreign namespaced attribute and an RDF collection; save Rating
   and Colour Label; verify all values survive and the marks change.
2. **Foreign metadata survives a develop save.** Verify an unknown `crs:`
   property and foreign RDF children survive while a modeled `crs:` property is
   replaced.
3. **arraw namespace ownership is complete.** Seed obsolete and current
   `arraw:` content; save adjustments; verify only the freshly serialized
   arraw-owned model remains.
4. **An existing extension-specific sidecar is selected.** Read and save
   `IMG.NEF.xmp` without creating `IMG.xmp`.
5. **New sidecars keep the stem-only convention.** Preserve the existing
   `pathFor` behavior when neither variant exists.
6. **Two sidecars are ambiguous.** Verify loading returns `ParseError`, saving
   returns `false`, and neither file changes.
7. **Malformed input is protected.** Verify both save paths return `false` and
   preserve the original bytes.
8. **Atomic replacement is used.** Exercise a successful save and a forced
   write failure where practical; verify the selected file is either the old
   complete packet or the new complete packet, never a truncated packet.

## Implementation shape

- Resolve both filename conventions once inside `XmpSidecar`.
- Parse existing packets with namespace processing enabled.
- Generate arraw-owned properties using the existing serializers, then merge
  them into the parsed document according to ADR 0026.
- Remove all `arraw:` attributes/elements during a develop save; remove only
  modeled `crs:` properties; remove only `xmp:Rating` and `xmp:Label` during a
  User Metadata save.
- Serialize to `QSaveFile` and commit only after the complete packet is written.
- Keep Qt XML details private to `XmpSidecar`; do not expand its public surface
  unless a failing behavior cannot be expressed otherwise.

## Verification and delivery

1. Run the focused `[xmp]` tests after every GREEN step.
2. Run `just format-check` and correct only touched files.
3. Run `ctest --test-dir /tmp/arraw-digikam-build --output-on-failure`.
4. Review the diff against ADR 0026 and this behavior list.
5. Commit the implementation and documentation on branch `digikam`.

## Current worktree state

The branch contains an uncommitted first-pass DOM merge, Qt Xml linkage, and
the first preservation/naming tests. Treat that code as disposable TDD work:
reconcile it against each slice above, keep only behavior justified by a GREEN
test, and do not commit until the complete plan passes.
