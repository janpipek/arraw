# Linux distribution: a CI-built AppImage on Ubuntu 24.04, Qt 6.8 via aqtinstall

We distribute arraw on Linux as a single self-contained **AppImage**, cut by
GitHub Actions on a tagged release and attached to a GitHub Release. The AppImage
**bundles Qt 6.8, LibRaw, and lcms2** and borrows the host's standard baseline
(glibc, the glvnd graphics stack, fontconfig, X11, dbus, glib — the libraries the
AppImage *excludelist* assumes present on any desktop). We build it in CI on a
**stock Ubuntu 24.04 runner (glibc 2.39)** and pull **Qt 6.8 from aqtinstall**, so
the one binary's glibc floor reaches every desktop from Ubuntu 24.04 LTS forward.
x86_64 only; updates are a manual re-download for now.

This is the first packaging work in the repo and picks up the thread left dangling
by [[app-icon-svg-source-runtime-only]] (ADR 0013), which built the runtime icon
and explicitly deferred "the OS-level native icons + packaging machinery." That
machinery starts here: an `install()` AppDir carrying the binary, the existing
icons, a `.desktop` launcher, and an AppStream `metainfo.xml`.

## Correction (2026-06-17): build host is Ubuntu+aqt, not KDE neon

This ADR originally chose a **KDE neon** Docker base on the belief that it shipped
*system Qt 6.8 at glibc 2.39 (Ubuntu 24.04)* with zero moving parts. Building it
proved that premise false on every axis. The only pullable stable neon image,
`kdeneon/plasma:user`, is **Ubuntu 22.04 (jammy), glibc 2.35, Qt 6.7.2**, and its
`qt6-base-private-dev` is an **empty "transitional" package** — so the GuiPrivate
RHI headers ADR 0006 depends on are not even available. No stable neon tag
(`user`/`testing`/`developer`) is on noble; only `unstable` (a moving Qt-dev base,
a poor CI foundation) might be.

So we promote what was already the documented fallback — **aqtinstall on a stock
`ubuntu-24.04` runner** — to the primary mechanism. It gives *official* Qt 6.8.3
(private headers included), the app compiled at the runner's glibc 2.39, and thus
the exact 24.04-forward reach this ADR always wanted. The neon idea is retained
below only as a rejected option, with the empirical reason it failed.

The pivot surfaced two more things, now fixed:

- **`find_package(Qt6 … GuiPrivate)` was not portable.** Official Qt binaries (aqt)
  and vcpkg expose `Qt6::GuiPrivate` as a target *inside* the Gui package (pulled
  in transitively by Widgets); Fedora's system Qt splits it into a separate
  `GuiPrivate` *component* with its own config and does **not** define the target
  via Gui. Requesting the component unconditionally breaks on official Qt; relying
  on the transitive target breaks on Fedora. `CMakeLists.txt` now takes the target
  if already present and only requests the component as a fallback — one file for
  all three packagings.
- **The bundle borrows more than "glibc + drivers."** Building against official Qt
  on Ubuntu needs the X/xkb/Vulkan/fontconfig `-dev` packages its imported CMake
  targets reference (e.g. `Qt6::GuiPrivate → XKB::XKB`), and the produced AppImage
  relies on the standard excludelist baseline (glvnd, fontconfig, X11, dbus, glib)
  being present on the target — true of any real 24.04 desktop, but not of a bare
  container. The CI smoke test installs that baseline explicitly so it actually
  tests bundle self-containment.

## Why not just lower the Qt requirement to reach older glibc

The tempting shortcut — "drop to Qt 6.4 so we can build on a low-glibc distro" —
is a dead end. The viewport (`ImageViewport`) **is** a `QRhiWidget`, the class that
hosts the backend-agnostic RHI pipeline ADR 0006 is built on, and `QRhiWidget` did
not exist until Qt **6.7**. Downgrading to 6.4 would force a rewrite back to
`QOpenGLWidget`/hand-driven `QRhi`, *reverting ADR 0006*. And Ubuntu 24.04 LTS
ships only Qt 6.4 anyway, so the distro Qt is a non-starter regardless — which is
exactly why we bring our own Qt 6.8 via aqtinstall.

The insight that still holds: **glibc reach is a build-environment problem, not a
code problem.** An AppImage's floor is the glibc its binaries were compiled
against; Qt/LibRaw/lcms2 ride inside the file. Compiling on Ubuntu 24.04 (glibc
2.39) sets that floor at 2.39; aqt's official Qt — built by Qt against an even
older glibc — rides along without raising it.

## Considered Options

- **aqtinstall on an Ubuntu 24.04 runner (chosen).** Official Qt 6.8.3 with the
  GuiPrivate headers, app compiled at glibc 2.39, reaching 24.04 LTS forward.
  Reproducible; the only "moving part" is an aqt download, pinned to a version.
- **Build on KDE neon (`kdeneon/plasma:user`).** Believed to give system Qt 6.8 at
  glibc 2.39; is actually jammy / Qt 6.7.2 with an empty transitional private-dev
  (see correction above). Rejected on evidence.
- **Build on Fedora 41 (glibc 2.40).** Matches the documented `dnf` dev flow, but
  its 2.40 floor silently excludes Ubuntu 24.04 LTS / Mint 22 — a failure you never
  see from a Fedora dev box. Rejected: Ubuntu 24.04 reaches one glibc step lower.
- **Flatpak / Flathub first.** The posture-(B) store path. Deferred until the app
  stabilises past 0.x: it imposes a sandbox + portal-based file access on a
  folder-of-RAWs workflow, plus runtime/manifest upkeep. The cheap door-openers
  for it are taken now anyway — the reverse-DNS app-id and AppStream metainfo.
- **Distro-native `.rpm`/`.deb` in repos.** Per-distro packaging and Qt-version
  skew, high upkeep, low payoff for a hobby release. Ruled out.

## Consequences

- **Identity is fixed across all platforms:** the app-id is
  `io.github.janpipek.arraw` (Flathub's sanctioned `io.github.<user>.<app>` form),
  reused verbatim as the Linux AppStream/desktop id, the macOS bundle id, and the
  Windows AppId. `organizationDomain` becomes `io.github.janpipek`;
  `applicationName` stays `arraw` so existing QSettings don't move.
- **Version is single-sourced from CMake** (per [[spot-for-algorithms]]):
  `project(VERSION …)` is the truth, threaded in via the `ARRAW_VERSION` compile
  definition to `setApplicationVersion` and a `--version` flag; a CI guard fails the
  release if the pushed `vX.Y.Z` tag disagrees.
- The Linux build bundles the **offscreen** platform plugin (via linuxdeploy's
  `EXTRA_PLATFORM_PLUGINS`) so the (future) headless CLI of ADR 0012 works over
  SSH; arg pass-through is automatic.
- linuxdeploy runs with **`NO_STRIP=1`**: its bundled binutils `strip` cannot parse
  the `.relr.dyn` (DT_RELR) section in libraries from modern distros and aborts.
- The release CI installs a broad set of X/xkb/Vulkan/fontconfig `-dev` packages
  (for the build) and the smoke test installs the glvnd/fontconfig/X11 runtime
  baseline (to prove self-containment). Both lists live in
  `.github/workflows/release.yml`.
- A future reader will find we *bring our own Qt* (aqt) rather than use a distro's.
  That is intentional and recorded here: dev on Fedora system Qt, ship Qt 6.8 from
  aqt on Ubuntu.
- This is packaging, not domain language: `CONTEXT.md` is left untouched.
