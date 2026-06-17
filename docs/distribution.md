# Distribution plan

How arraw ships to users, across platforms. This is a **status + plan** doc; the
*why* behind hard-to-reverse choices lives in `docs/adr/`.

## Posture

Self-hosted **GitHub Releases** are the source of truth (posture A): one tagged
release carries one artifact per OS. Store presence (Flathub / winget / Homebrew)
is a deliberate *later* milestone (posture B), unlocked once the app stabilises
past 0.x. We take the cheap posture-B door-openers now (reverse-DNS app-id,
AppStream metainfo) but build none of the store machinery yet.

## Cross-cutting decisions (shared by all platforms)

- **App-id / identity:** `io.github.janpipek.arraw` — reused verbatim as the Linux
  AppStream/desktop id, the macOS bundle id, and the Windows AppId. Display name
  stays "arraw". `organizationDomain` → `io.github.janpipek`; `applicationName`
  stays `arraw` (don't move existing QSettings).
- **Version is single-sourced from `CMakeLists.txt`** `project(VERSION …)`. A CI
  guard fails the release if the pushed `vX.Y.Z` tag disagrees. The value is
  threaded into `setApplicationVersion` and a new `--version` flag. *(Not yet
  wired — `main.cpp` currently sets no version.)*
- **Build & release via GitHub Actions**, tag-triggered: one workflow, one matrix
  leg per OS, all attaching to the same Release.
- **x86_64** baseline everywhere (macOS additionally needs Apple Silicon).
- **No auto-update** for v0.x — users re-download from Releases.

## Linux — DECIDED (see ADR 0014)

Single-file **AppImage**, built in CI on a **KDE neon (Ubuntu 24.04 / glibc 2.39)**
base, bundling Qt 6.8 + LibRaw + lcms2, reaching every desktop from Ubuntu 24.04
LTS forward. Qt stays at 6.8 (the viewport needs `QRhiWidget`, 6.7+). Carries a
`.desktop` launcher, the existing icons, and an AppStream `metainfo.xml`; file
associations and zsync auto-update are deferred. x86_64 only.

Full rationale and rejected options: [ADR 0014](adr/0014-linux-distribution-appimage-on-neon.md).

## Windows — BUILD + PORTABLE ZIP + INSTALLER DONE (signing/CI deferred)

The *build* is established and documented in [windows-build.md](windows-build.md):
MSVC 2022 + vcpkg (`qtbase qttools qtshadertools libraw[openmp] lcms`,
`x64-windows`). Several Windows-specific fixes live in CMake:

- per-config libraw DLL selection (`rawd` in Debug, so the right DLL is deployed);
- manual deployment of the Qt **platform** *and* **imageformats** plugins plus the
  `jpeg62.dll` codec next to the exe (vcpkg's `applocal` deploys Qt DLLs but not
  plugins — see windows-build.md §6.2);
- `libraw[openmp]` for a multithreaded demosaic (much faster RAW load);
- the executable is a **GUI-subsystem** app with an **embedded icon**
  ([ADR 0015](adr/0015-windows-native-icon-gui-subsystem.md)).

A **portable `.zip`** is implemented: `tools/package_windows.py` builds Release and
bundles the runnable app to `dist/arraw-<version>-windows-x64.zip` (exe + Release
runtime DLLs + plugin folders). As of ADR 0016 the stage also bundles the MSVC/OpenMP runtime app-local
(`vcruntime140*.dll`, `msvcp140.dll`, `vcomp140.dll`), so the ZIP is now
**self-contained** — no separate VC++ redistributable needed on the target.

**Installer — DONE (see [ADR 0016](adr/0016-windows-installer-inno-setup.md)).**
A per-user **Inno Setup** `setup.exe` is built by `python tools/package_windows.py
--installer` (needs `ISCC` on PATH — `scoop install inno-setup`) over the same
stage as the ZIP. No UAC (`{localappdata}\Programs\arraw`), opt-in RAW file
associations, app-local runtime. Still deferred:

- **Code signing** — ships unsigned; users click through SmartScreen ("More info →
  Run anyway"). Revisit with an Authenticode cert at posture B.
- **CI** — local build for now; a tag-triggered GitHub Actions leg is a follow-on
  (vcpkg binary caching / aqtinstall is the key concern).
- **MSIX / winget / Scoop** — posture-B channels; Scoop is near-free now that the
  ZIP is self-contained.
- **Arch** — x64 only.

## macOS — TENTATIVE LEANINGS (not decided)

Goal (per original ask): a `.dmg`, and eventually a Homebrew cask. Build is already
documented in AGENTS.md (Homebrew deps + CMake). Only lightly explored — **nothing
below is final**, no ADR yet:

- **Signing/notarization** — the decision that gates everything. *Leaning:*
  **ad-hoc `codesign` (free)** for the exploratory phase, shipping a `.dmg` that
  runs but is quarantined (users right-click → Open the first time, or
  `xattr -dr com.apple.quarantine arraw.app`); document this in the README.
  **Defer the Apple Developer Program ($99/yr) Developer ID signing +
  notarization** until there's a real mac audience — it's an additive CI step
  later, not a redo. Caveat: the quarantine warning is off-putting to
  non-technical users, so polish-from-day-one would mean paying the $99 up front.
- **Architecture** — *leaning* universal binary (arm64 + x86_64) so one `.dmg`
  covers all Macs; Homebrew strongly prefers universal. (Contrast Linux/Windows:
  x86_64-only.)
- **Bundle** — `.app` with `Info.plist` carrying the shared bundle id
  `io.github.janpipek.arraw`, packaged into a `.dmg`.
- **Homebrew** — *leaning* a **cask** (ships the prebuilt `.dmg`/`.app`), not a
  formula (build-from-source), as the right shape for a Qt GUI app. Posture-B,
  later.
