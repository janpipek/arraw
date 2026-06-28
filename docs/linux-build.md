# Building and deploying arraw on Linux

This guide covers the Linux **development build** and, in more depth, how the
**release AppImage** is produced and how to reproduce it locally. The short dev
setup in [AGENTS.md](../AGENTS.md) is enough to compile and run arraw; this document
adds the packaging machinery and the non-obvious deployment gotchas behind it. The
architectural rationale lives in [ADR 0014](adr/0014-linux-distribution-appimage-ubuntu-aqt.md).

> Everything here was verified by reproducing the full CI build in `ubuntu:24.04`
> containers (June 2026): build → `linuxdeploy` bundle → offscreen smoke test, plus
> a clean-room run on a stock 24.04 baseline.

---

## 1. Development build (Fedora)

The day-to-day dev setup uses the distro's system Qt. On Fedora:

```bash
sudo dnf install qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtshadertools-devel \
    qt6-qttools-devel LibRaw-devel lcms2-devel lensfun-devel cmake ninja-build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
./build/arraw
```

`qt6-qtbase-private-devel` is required: the viewport is a `QRhiWidget` and pulls the
semi-public RHI headers (`rhi/qrhi.h`) from Qt's private Gui module. See §6.1 for why
this package is requested differently across distros.

Other distros: install the equivalent `qt6-base`, `qt6-base-private`,
`qt6-shadertools`, `qt6-tools`, `libraw`, and `lcms2` development packages.

---

## 2. How the release AppImage is built

The release is a single self-contained **AppImage**, cut by a manually dispatched
GitHub Actions workflow (`.github/workflows/release.yml`) for an existing `vX.Y.Z`
tag and attached to the matching GitHub Release. The job:

1. Runs on a stock **`ubuntu-24.04`** runner (glibc 2.39) — this sets the AppImage's
   glibc floor, so the binary runs on every desktop from Ubuntu 24.04 LTS forward.
2. Installs **Qt 6.8 via `jurplel/install-qt-action`** (aqtinstall). The distro's own
   Qt on 24.04 is only 6.4, and `QRhiWidget` needs ≥ 6.7, so we bring our own. Do
   **not** restrict the `archives:` of the action — the default full qtbase carries
   the GuiPrivate CMake config the build needs (see §6.1).
3. Installs the X/xkb/Vulkan/fontconfig **`-dev`** packages that official Qt's
   imported CMake targets reference (see §6.2).
4. Verifies the pushed tag matches `project(VERSION …)` in `CMakeLists.txt`, then
   builds Release.
5. Stages an AppDir with `cmake --install`, bundles it with **`linuxdeploy` +
   `linuxdeploy-plugin-qt`** (see §6.3–§6.5), and produces
   `arraw-<version>-x86_64.AppImage`.
6. **Smoke-tests** the AppImage on a *clean* `ubuntu-24.04` runner — `--version`
   under `QT_QPA_PLATFORM=offscreen`, with only the host baseline installed — to
   prove self-containment before publishing.

### Identity and version

- App-id `io.github.janpipek.arraw` names the `.desktop`, the AppStream
  `metainfo.xml`, and the installed icons. `main.cpp` sets `organizationDomain` and
  `desktopFileName` to it; `applicationName`/`organizationName` stay `arraw` so Linux
  QSettings (`~/.config/arraw/arraw.conf`) don't move.
- The version is single-sourced from CMake: `project(VERSION …)` → the `ARRAW_VERSION`
  compile definition → `setApplicationVersion` and the `--version` flag. The CI guard
  fails the release if the tag disagrees.

### Cutting a release

Three declarations must agree, or the release workflow's *Verify tag, CMake, and RPM
versions* step fails: `project(VERSION …)` in `CMakeLists.txt`, `Version:` in
`packaging/fedora/arraw.spec`, and the pushed `vX.Y.Z` tag. The AppStream `metainfo.xml`
also wants a dated `<release>` entry per version (the guard doesn't check it, but
`appstreamcli validate` does). `just bump X.Y.Z` rewrites all three files in lockstep:

1. `just bump X.Y.Z` — edits `CMakeLists.txt`, `arraw.spec`, and `metainfo.xml`. The
   Windows installer, RPM, and smoke scripts read the version from CMake, so they need
   no edit.
2. Add a `%changelog` entry in `packaging/fedora/arraw.spec` (the prose stays manual).
3. Commit, then `git tag vX.Y.Z && git push origin vX.Y.Z`.
4. Dispatch `.github/workflows/release.yml` for the tag.

---

## 3. Reproducing the AppImage build locally

You rarely need to — CI produces the shippable artifact — but it is useful for
iterating on packaging. The cleanest local repro mirrors CI inside a container so the
glibc floor is correct:

```bash
# In an ubuntu:24.04 container (podman or docker), as root:
export DEBIAN_FRONTEND=noninteractive
apt-get update && apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build file wget ca-certificates \
  libraw-dev liblcms2-dev liblensfun-dev liblensfun-data-v1 appstream libdbus-1-3 \
  libgl1-mesa-dev libglu1-mesa-dev libvulkan-dev \
  libxkbcommon-dev libxkbcommon-x11-dev libfontconfig1-dev libfreetype-dev \
  libx11-dev libx11-xcb-dev libxext-dev libxfixes-dev libxi-dev libxrender-dev \
  libxcb1-dev libxcb-cursor-dev libxcb-glx0-dev libxcb-keysyms1-dev \
  libxcb-image0-dev libxcb-shm0-dev libxcb-icccm4-dev libxcb-sync-dev \
  libxcb-xfixes0-dev libxcb-shape0-dev libxcb-randr0-dev \
  libxcb-render-util0-dev libxcb-util-dev libxcb-xinerama0-dev libxcb-xkb-dev

# Qt 6.8 via aqt (or mount one you already have):
pip install aqtinstall   # or: uv run --with aqtinstall python -m aqt ...
python -m aqt install-qt linux desktop 6.8.3 linux_gcc_64 -m qtshadertools --outputdir /opt/qt
export QT=/opt/qt/6.8.3/gcc_64

# Build, stage, bundle:
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DARRAW_BUILD_TESTS=OFF \
  -DARRAW_WITH_LENSFUN=ON -DCMAKE_PREFIX_PATH="$QT"
ninja -C build arraw
DESTDIR="$PWD/AppDir" cmake --install build --prefix /usr

# linuxdeploy bundles liblensfun (+ glib) but not the lens database, so stage it
# into the AppDir; arraw loads it relative to the executable (usr/bin → ../share):
mkdir -p AppDir/usr/share/lensfun/db
cp /usr/share/lensfun/version_1/*.xml AppDir/usr/share/lensfun/db/

wget -q https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
wget -q https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
chmod +x linuxdeploy*.AppImage
APPIMAGE_EXTRACT_AND_RUN=1 NO_STRIP=1 EXTRA_PLATFORM_PLUGINS=libqoffscreen.so \
  QMAKE="$QT/bin/qmake" LD_LIBRARY_PATH="$QT/lib" \
  OUTPUT=arraw-0.1.0-x86_64.AppImage \
  ./linuxdeploy-x86_64.AppImage --appdir AppDir --plugin qt --output appimage

# Smoke test:
APPIMAGE_EXTRACT_AND_RUN=1 QT_QPA_PLATFORM=offscreen ./arraw-0.1.0-x86_64.AppImage --version
```

> **Do not ship an AppImage built on Fedora.** A Fedora 40+ build links glibc 2.40,
> so the resulting AppImage **will not run on Ubuntu 24.04 LTS** (glibc 2.39) — the
> floor is set by wherever you compiled, not by the bundle. Build the shippable
> AppImage on Ubuntu 24.04 (CI does). A Fedora-built one is fine only for testing the
> packaging *mechanism*, and even then it pulls Fedora system libs (`.relr.dyn`,
> different sonames) that differ from CI.

---

## 4. The AppDir layout

`CMakeLists.txt` (guarded `if(UNIX AND NOT APPLE)`) installs a standard AppDir that
`linuxdeploy` consumes:

```
usr/bin/arraw
usr/share/applications/io.github.janpipek.arraw.desktop
usr/share/metainfo/io.github.janpipek.arraw.metainfo.xml
usr/share/icons/hicolor/<size>/apps/io.github.janpipek.arraw.png   (16–256)
usr/share/lensfun/db/*.xml                                         (staged in CI, see §6.6)
```

The `.desktop` and `.metainfo.xml` sources live in `packaging/linux/`. Validate them
with `desktop-file-validate` and `appstreamcli validate` (the CI does the latter).

---

## 5. Why Ubuntu 24.04 + aqt, not the distro Qt or KDE neon

Short version: the AppImage's glibc floor is whatever you compile against, and we want
that as low as practical while still having Qt ≥ 6.7. Ubuntu 24.04 (glibc 2.39) is the
lowest-glibc base that is still current, but its distro Qt is only 6.4 — so we install
Qt 6.8 from aqt on top. KDE neon was the original plan (for "system Qt 6.8") but its
Docker image is actually Ubuntu 22.04 + Qt 6.7.2 with an empty private-dev, so it was
abandoned. Full reasoning and rejected options: [ADR 0014](adr/0014-linux-distribution-appimage-ubuntu-aqt.md).

---

## 6. Deployment gotchas baked into the build

These are already handled in `CMakeLists.txt` / the workflow; this section explains
*why*, because the symptoms are confusing.

### 6.1 `Qt6::GuiPrivate` is requested differently per packaging

The RHI headers come from Qt's private Gui module, exposed as the `Qt6::GuiPrivate`
target — but packagings disagree on how:

- **Official Qt (aqt) and vcpkg** ship the target *inside* the Gui package; it is
  pulled in transitively by `Widgets`. They have **no** standalone
  `Qt6GuiPrivateConfig.cmake`, so `find_package(Qt6 … COMPONENTS GuiPrivate)` *fails*.
- **Fedora** splits it into a separate `GuiPrivate` **component** with its own config
  file, and does **not** define the target via Gui — so you *must* request the
  component.

`CMakeLists.txt` reconciles both: it requests `Widgets Concurrent ShaderTools`, then
takes `Qt6::GuiPrivate` if already defined and only `find_package(... GuiPrivate)` as
a fallback. One file builds on Fedora, official Qt, and vcpkg.

### 6.2 Building against official Qt needs extra `-dev` packages

Official Qt's CMake config creates *imported targets* for system libraries it was
built against — e.g. `Qt6::GuiPrivate` lists `XKB::XKB` in its link interface. If the
matching `-dev` package is absent, configure fails at generate time with
`target "XKB::XKB" ... was not found`, even though the headers would compile. The
workflow installs the full set (xkb, Vulkan, fontconfig, freetype, X11, the xcb
cluster). The distro dev packages (Fedora `dnf`) already pull these in, which is why
the dev build in §1 doesn't list them.

### 6.3 `NO_STRIP=1` — linuxdeploy's bundled `strip` is too old

`linuxdeploy` ships an old binutils `strip` that cannot parse the `.relr.dyn`
(DT_RELR relative-relocations) section emitted by modern distros (Ubuntu 24.04,
Fedora 40+). Without `NO_STRIP=1` the run aborts with
`unknown type [0x13] section .relr.dyn`. Skipping strip makes the bundle slightly
larger but builds reliably.

### 6.4 The offscreen plugin needs `EXTRA_PLATFORM_PLUGINS`

ADR 0022's future headless CLI needs the **offscreen** Qt platform plugin in the
bundle. `linuxdeploy-plugin-qt` bundles the `xcb` platform by default; extra platform
plugins go in **`EXTRA_PLATFORM_PLUGINS=libqoffscreen.so`**. The generic
`EXTRA_QT_PLUGINS` variable is for plugin *groups* (svg, imageformats, …) and
silently ignores platform-plugin names — a value there does nothing.

### 6.5 What the AppImage bundles vs. borrows

`linuxdeploy` bundles Qt, LibRaw, lcms2 and their non-system dependencies, and
*excludes* a standard "excludelist" of libraries assumed present on any desktop:
glibc, the **glvnd** GL stack (`libGL`/`libGLX`/`libOpenGL`/`libEGL`), `fontconfig`,
X11, `dbus`, `glib`. So the AppImage runs on a real Ubuntu 24.04 desktop (and the CI
smoke-test runner, which installs that baseline) but **not** on a stripped-down
container that lacks those libs. This is normal AppImage behaviour, not a packaging
bug.

### 6.6 The lensfun lens database is staged, not auto-bundled

Lens corrections (ADR 0032) need two things at runtime: `liblensfun` and the lens
**database**. `linuxdeploy` carries the library (and its `glib` dependency) because
the binary links it, but the database is plain data under `/usr/share/lensfun/` that
`linuxdeploy` does not follow. So the build configures with `-DARRAW_WITH_LENSFUN=ON`
(a missing `liblensfun-dev` then fails the build instead of silently shipping without
corrections) and a *Bundle the lensfun database* step copies
`/usr/share/lensfun/version_1/*.xml` into `AppDir/usr/share/lensfun/db/`. At runtime
`LensfunSource.cpp` resolves the DB relative to the executable (`usr/bin` → `../share`),
never the host's `/usr/share`, so the AppImage stays self-contained. The smoke-test
job extracts the AppImage and asserts both the library and the DB are present.

---

## 7. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `find_package(Qt6 ...)`: *Failed to find required Qt component "GuiPrivate"* | Building against official Qt/vcpkg, which has no standalone GuiPrivate config | Already handled in `CMakeLists.txt` (§6.1). If you reintroduced `COMPONENTS GuiPrivate`, revert to the target-or-fallback form. |
| Configure error: *target "XKB::XKB" ... was not found* (or Vulkan/fontconfig) | Missing the `-dev` package an official-Qt imported target references | Install the X/xkb/Vulkan/fontconfig dev packages (§3, §6.2). |
| `qsb: error while loading shared libraries: libdbus-1.so.3` | aqt Qt's shader tool links dbus; minimal image lacks the runtime lib | Install `libdbus-1-3` (§3). |
| linuxdeploy: `unknown type [0x13] section .relr.dyn`, *Strip call failed* | linuxdeploy's old `strip` vs modern relocations | Set `NO_STRIP=1` (§6.3). |
| AppImage runs, but `QT_QPA_PLATFORM=offscreen` segfaults / `no Qt platform plugin` | offscreen plugin not bundled | Set `EXTRA_PLATFORM_PLUGINS=libqoffscreen.so` (§6.4); confirm `usr/plugins/platforms/libqoffscreen.so` is inside the AppImage. |
| AppImage: `error while loading shared libraries: libGLX.so.0` / `libfontconfig.so.1` | Running on a host missing the excludelist baseline (e.g. bare container) | Install the host baseline: `libgl1 libglx0 libopengl0 libegl1 libglvnd0 libfontconfig1 …` (§6.5). Any real desktop already has it. |
| AppImage built on Fedora won't start on Ubuntu 24.04 | glibc 2.40 floor from the Fedora build host | Build the shippable AppImage on Ubuntu 24.04 (§3 warning). |
| Release workflow fails at *Verify tag, CMake, and RPM versions* | Selected tag `vX.Y.Z` disagrees with CMake or the RPM spec | Run `just bump X.Y.Z` (§Cutting a release) before tagging, or select the correct tag. |
| `appstreamcli validate` fails | Edited `metainfo.xml` | Fix per the validator output; `metadata_license`/`project_license` must be SPDX ids. |

---

## 8. Fedora RPM packaging

Fedora 44 x86_64 has a native package in addition to the AppImage. It uses Fedora's
system libraries and therefore targets one Fedora release, unlike the bundled
AppImage.

Build an RPM and SRPM from clean, committed `HEAD`:

```bash
just rpm
```

The command never installs packages. If build dependencies are absent, it prints
the exact `sudo dnf install ...` command and exits. Untagged commits get a snapshot
release containing the commit date and SHA; an exact `vX.Y.Z` tag produces the
normal `Release: 1.fc44` package. Results and `SHA256SUMS` are written below
`dist/fedora/`.

The package build is offline and runs the full test suite plus desktop and AppStream
validation. Run the clean installation test separately:

```bash
just rpm-smoke
```

That command uses Podman by default (Docker is also accepted), installs the RPM in a
clean `fedora:44` container, runs `arraw --version` offscreen, and checks its desktop
MIME registration. See [ADR 0030](adr/0030-self-hosted-fedora-rpm.md) for the design
and [the security risk register](security.md) for unsigned-package and release risks.

## 9. Quick reference

```bash
# Dev build (Fedora system Qt)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && ninja -C build && ./build/arraw

# Native Fedora package from committed HEAD
just rpm
just rpm-smoke

# Release artifacts: manually dispatch .github/workflows/release.yml for a tag
# and explicitly select AppImage and/or Fedora RPM.
```

## 10. Manual release workflow security

The release workflow must be selected from the repository's default branch. Its
`release_tag` is the source input; AppImage and Fedora RPM booleans default to off,
and dispatch fails when neither is selected. Every selected build and smoke test
must pass before the publish job runs.

This single environment protects the shared publish job for every selected platform;
individual build jobs remain read-only and do not need separate environments. Only
the environment-gated publish job receives `contents: write`. Existing assets are
not replaced unless `replace_existing_assets` is explicitly selected. GitHub Actions
are pinned to full commit SHAs and Dependabot proposes pin updates. Creating and
verifying the protected environment is tracked in
[GitHub issue #38](https://github.com/janpipek/arraw/issues/38).

Build-provenance attestation is temporarily disabled while the repository is private
(attestations are only publicly verifiable for public repos); the `actions/attest`
step in the release workflow is gated off. Once the repository is public the step
will be re-enabled, and a downloaded file's provenance can be verified with:

```bash
gh attestation verify arraw-0.1.0-1.fc44.x86_64.rpm --repo janpipek/arraw
```

RPM signing and immutable releases remain deferred security work; see
[the security risk register](security.md).
