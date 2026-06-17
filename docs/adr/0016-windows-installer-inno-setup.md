# Windows installer: a per-user Inno Setup setup.exe over the existing staging

We ship a Windows installer as a **per-user Inno Setup `setup.exe`**, compiled by
`ISCC` over the **same staged folder** `tools/package_windows.py` already assembles
for the portable ZIP. The install is **per-user** (`PrivilegesRequired=lowest`) into
`{localappdata}\Programs\arraw` — **no UAC, no admin** — with a Start Menu shortcut,
an optional desktop shortcut, a standard uninstaller, and **opt-in RAW file
associations**. It bundles the **MSVC/OpenMP runtime DLLs app-local**, and ships
**unsigned** for v0.x. This round is the **local artifact only**; CI integration and
code signing are deliberately deferred.

This resolves the "Installer is not yet designed" thread in
[distribution.md](../distribution.md) and sits beside the two packaging ADRs already
in place: it reuses the `.exe` icon embedded by
[[windows-native-icon-gui-subsystem]] (ADR 0015) as the setup and shortcut icon, and
shares the cross-platform identity fixed by
[[linux-distribution-appimage-on-neon]] (ADR 0014). The installer is **not a new
build path** — it is a second consumer of the staging that already produces the ZIP.

## Why per-user and unsigned reinforce each other

A per-machine Program Files install needs UAC elevation, and on an *unsigned* binary
that elevation prompt stacks **on top of** the SmartScreen "unknown publisher"
warning — two scary dialogs before the app runs. Installing per-user into
`{localappdata}` needs no elevation at all, so the unsigned posture costs the user a
**single** friction point (SmartScreen's "More info → Run anyway"), not two. This is
the same deferred-signing stance `distribution.md` already takes; per-user is its
natural partner. When a signing cert is eventually bought (posture B), per-machine
and MSIX both reopen — but neither is worth the double prompt today.

## Why reuse staging, and bundle the runtime app-local

The hard Windows work — per-config LibRaw DLL selection, manual Qt platform/
imageformats plugin deployment, the GUI-subsystem exe with its embedded icon — is
already solved and already staged. The installer therefore consumes the staged
folder verbatim; the only new packaging step is copying the CRT/OpenMP runtime
(`vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140.dll`, `vcomp140.dll`) from
`%VCToolsRedistDir%\x64\...` (available once the script imports `vcvars64.bat`, which
`package_windows.py` already does) into the stage.

Bundling those DLLs app-local makes the package **self-contained** — no dependency on
a system-wide VC++ redistributable, no admin. As a deliberate side effect it **also
closes a latent gap in the portable ZIP**, which until now shipped without the CRT
and silently required the target to have the redist installed. One fix, both
artifacts: the stage is the single source, so the ZIP and the installer both become
self-contained. This supersedes the "does not bundle the MSVC/OpenMP runtime" caveat
in `distribution.md`.

## Considered Options

- **NSIS.** A peer of Inno Setup with the same `setup.exe` outcome and smaller
  output, but more low-level scripting for no gain here. Inno wins on
  batteries-included ergonomics (shortcuts, uninstaller, associations, redist
  bundling out of the box). No unusual install requirement justifies NSIS's
  flexibility.
- **WiX / MSI.** Verbose XML authoring whose real payoff — transactional rollback,
  GPO/SCCM fleet deployment — targets IT departments a v0.x hobby photo editor does
  not have. High ceremony, zero user-visible benefit. Rejected.
- **MSIX.** The modern, sandboxed, Store-/winget-/auto-update-ready format and the
  genuine posture-B target — but it **mandates a trusted signing cert**; even
  sideloading will not run it unsigned. That collides head-on with the deferred-
  signing decision. Revisit at posture B, when buying an Authenticode cert is the
  trigger that reopens both MSIX and signed per-machine Inno.
- **Per-machine (Program Files) install.** Rejected for the UAC-plus-SmartScreen
  double prompt on an unsigned binary (see above). Reopens once signed.
- **Invoke the VC++ redistributable instead of app-local DLLs.** Machine-wide, needs
  admin, and adds a separate failure point — and would *not* fix the portable ZIP.
  App-local copies are self-contained and fix both artifacts at once.
- **Scoop / winget manifests.** Orthogonal posture-B *distribution* channels, not
  installer formats: Scoop rides the portable ZIP (hash-verified, no signing,
  per-user), winget wraps the `setup.exe`. Both deferred — but the app-local-DLL fix
  above makes a self-contained ZIP, so a future Scoop manifest becomes near-trivial.

## Consequences

- **`distribution.md` is updated:** the Windows section drops "installer deferred"
  and removes the "does not bundle the MSVC/OpenMP runtime" caveat — the ZIP is now
  self-contained too.
- **`ISCC.exe` must be on `PATH`** to cut the installer (`scoop install inno-setup`).
  `package_windows.py` gains an `--installer` flag that, after staging, shells out to
  `ISCC` and emits `dist/arraw-<version>-windows-x64-setup.exe`. The plain
  build/test flow and the existing ZIP path are untouched; the installer is additive.
- **`tools/installer/arraw.iss`** is parameterized via `/D` defines — `AppVersion`
  (from the existing single-source `project_version()`), `StageDir`, `OutputDir` — so
  it hardcodes no version or path.
- **Identity:** `AppId=io.github.janpipek.arraw` reuses the shared id from ADR
  0014; the per-user uninstaller registers under `HKCU`.
- **File associations** register an `arraw.RawImage` ProgID under
  `HKCU\Software\Classes` for the ten RAW extensions arraw's open dialog accepts
  (`cr2 cr3 nef arw dng raf orf rw2 pef srw`), behind an **unchecked** `Tasks`
  checkbox the user opts into. No admin; fully removed on uninstall. Standard image
  formats (jpg/png/tiff/…) are intentionally *not* claimed.
- **Verification is a documented manual checklist** (packaging has no unit tests):
  build → install on a clean profile → launch → open a RAW → confirm the app-local
  DLLs resolve (no "VCRUNTIME140 missing") → confirm the Start Menu shortcut and the
  Add/Remove Programs uninstaller → uninstall leaves nothing behind.
- **Still deferred:** code signing, CI integration, and any Scoop/winget manifest —
  each a separable follow-on, not a prerequisite.
- This is packaging, not domain language: `CONTEXT.md` is untouched.
