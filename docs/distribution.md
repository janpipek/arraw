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
  threaded into `setApplicationVersion` and a `--version` flag (via the
  `ARRAW_VERSION` compile definition and `QCommandLineParser`).
- **Build & release via GitHub Actions**, tag-triggered: one workflow, one matrix
  leg per OS, all attaching to the same Release.
- **x86_64** baseline everywhere (macOS additionally needs Apple Silicon).
- **No auto-update** for v0.x — users re-download from Releases.

## Linux — IMPLEMENTED (see ADR 0014)

Single-file **AppImage**, built in CI on a **stock `ubuntu-24.04` runner (glibc
2.39)** with **Qt 6.8 from aqtinstall**, bundling Qt + LibRaw + lcms2, reaching
every desktop from Ubuntu 24.04 LTS forward. Qt stays at 6.8 (the viewport needs
`QRhiWidget`, 6.7+; 24.04's distro Qt is only 6.4, so we bring our own). Carries a
`.desktop` launcher, the existing icons, and an AppStream `metainfo.xml`; file
associations and zsync auto-update are deferred. x86_64 only.

> The original plan built on a KDE neon container for "system Qt 6.8." That
> premise turned out false (neon's image is jammy / Qt 6.7.2 with an empty
> private-dev), so the build host pivoted to aqtinstall on Ubuntu 24.04 — see the
> correction section in ADR 0014.

What's wired:

- **`.github/workflows/release.yml`** — tag-triggered (`v*`). Installs Qt 6.8.3 via
  `jurplel/install-qt-action` on `ubuntu-24.04` plus the X/xkb/Vulkan/fontconfig
  `-dev` packages official Qt's CMake targets reference, builds, bundles via
  `linuxdeploy` + `linuxdeploy-plugin-qt` (`NO_STRIP=1`,
  `EXTRA_PLATFORM_PLUGINS=libqoffscreen.so`), then **smoke-tests the AppImage on a
  clean `ubuntu-24.04` runner** (`--version` under `QT_QPA_PLATFORM=offscreen`,
  with the host glvnd/fontconfig/X11 baseline installed) to prove self-containment
  before publishing.
- **CMake `install()` rules** (`UNIX AND NOT APPLE`) stage a standard AppDir:
  `usr/bin/arraw`, the `io.github.janpipek.arraw` `.desktop` + `metainfo.xml`, and
  the existing icons renamed into `hicolor/<size>/apps`. `find_package` resolves
  `Qt6::GuiPrivate` portably across official-Qt/vcpkg (target via Gui) and Fedora
  (separate component).
- **Packaging assets** live in `packaging/linux/`; a `LICENSE` (GPL-3.0) backs the
  metainfo's `<project_license>`.

The full build + smoke pipeline was reproduced locally in `ubuntu:24.04` containers
before merge (`arraw 0.1.0` under offscreen, on a clean 24.04 baseline). Open
follow-ups: screenshots in the metainfo and a zsync update feed are deferred to the
posture-B store path.

Full rationale and rejected options: [ADR 0014](adr/0014-linux-distribution-appimage-ubuntu-aqt.md).

## Windows — BUILD + PORTABLE ZIP DONE (installer deferred)

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
runtime DLLs + plugin folders). It does **not** bundle the MSVC/OpenMP runtime
(`vcruntime140*.dll`, `msvcp140.dll`, `vcomp140.dll`); the target needs the VC++
2015–2022 x64 redistributable, or those DLLs copied in app-local. This is the
posture-A portable artifact; an installer remains the open design below.

**Installer is not yet designed.** Open questions to resume on, with leanings:

- **Installer format** — Inno Setup / NSIS / WiX (MSI) / MSIX. Leaning Inno Setup
  (simple, scriptable, no MSI ceremony) for posture A; revisit MSIX/winget for
  posture B. The portable `.zip` above already covers the "no-install" case.
- **Qt/dependency deployment** — the dev/zip path deploys plugins manually because
  this vcpkg port set ships no `windeployqt`. For the installer, evaluate adding
  `windeployqt` (e.g. via an aqtinstall Qt) to bundle Qt + plugins + the CRT/OpenMP
  runtimes in one step, superseding the manual copy. Pin which DLLs (Release) ship.
- **Code signing** — unsigned installers trip SmartScreen ("unknown publisher").
  An Authenticode cert costs money/identity verification. Decision deferred; may
  ship unsigned with a documented "more info → run anyway" note initially.
- **CI** — GitHub Actions `windows-latest` + vcpkg. The slow Qt-from-source build
  makes **vcpkg binary caching** (e.g. GitHub Actions cache / NuGet feed) the key
  concern, or switch to aqtinstall to skip building Qt.
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
