# Linux distribution: a CI-built AppImage on a KDE neon base, Qt stays at 6.8

We distribute arraw on Linux as a single self-contained **AppImage**, cut by
GitHub Actions on a tagged release and attached to a GitHub Release. The AppImage
**bundles Qt 6.8, LibRaw, and lcms2** and borrows only the host's glibc and
graphics drivers. We deliberately build it in CI on a **KDE neon (Ubuntu 24.04 /
glibc 2.39)** base — *not* on Fedora 41 (glibc 2.40) and *not* by lowering the Qt
requirement — so the one file runs on every desktop from Ubuntu 24.04 LTS forward.
x86_64 only; updates are a manual re-download for now.

This is the first packaging work in the repo and picks up the thread left dangling
by [[app-icon-svg-source-runtime-only]] (ADR 0013), which built the runtime icon
and explicitly deferred "the OS-level native icons + packaging machinery." That
machinery starts here: an `install()` AppDir carrying the binary, the existing
icons, a `.desktop` launcher, and an AppStream `metainfo.xml`.

## Why not just lower the Qt requirement to reach older glibc

The tempting shortcut — "drop to Qt 6.4 so we can build on a low-glibc distro" —
is a dead end twice over. The viewport (`ImageViewport`) **is** a `QRhiWidget`,
the class that hosts the backend-agnostic RHI pipeline ADR 0006 is built on, and
`QRhiWidget` did not exist until Qt **6.7**. Downgrading to 6.4 would force a
rewrite back to `QOpenGLWidget`/hand-driven `QRhi`, *reverting ADR 0006*. And
Ubuntu 24.04 LTS ships only Qt 6.4 anyway, so the downgrade would buy a rewrite
*and still* not yield system Qt on the distro we downgraded for.

The insight that unlocks this ADR: **glibc reach is a build-environment problem,
not a code problem.** An AppImage's floor is the glibc its binaries were compiled
against; everything else (Qt, LibRaw, lcms2) rides inside the file. KDE neon is an
Ubuntu 24.04 base (glibc 2.39) that ships current Qt 6.8 from its own repo — so it
gives us system Qt 6.8 compiled at glibc 2.39, reaching 24.04 LTS with **zero code
change**. No Qt version in any *mainline* repo combines 6.8 with a glibc below
2.40: Fedora 41 = 2.40, Debian 13 / Ubuntu 25.04 = 2.41, and the only Ubuntu LTS
at 2.39 (24.04) is stuck on Qt 6.4.

## Considered Options

- **Build on Fedora 41 (glibc 2.40).** Matches the documented `dnf` dev flow
  exactly and is the lowest-glibc *mainline* Qt-6.8 distro — but its 2.40 floor
  silently excludes Ubuntu 24.04 LTS / Mint 22, the most common Linux desktop. A
  failure you never see from a Fedora dev box. Rejected: neon reaches one glibc
  step lower for no code cost.
- **aqtinstall on an Ubuntu 24.04 runner.** Same 2.39 reach as neon (Qt's own
  binaries are built at 2.39), but reintroduces the external Qt-download moving
  part we wanted to avoid; neon's system Qt is simpler. Documented fallback if
  neon's repo ever lags the Qt version we need.
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
  `project(VERSION …)` is the truth, a CI guard fails the release if the pushed
  `vX.Y.Z` tag disagrees, and the value is threaded into `setApplicationVersion`
  plus a new `--version` flag.
- The Linux build needs `QT_QPA_PLATFORM=offscreen` bundled so the (future)
  headless CLI of ADR 0012 works over SSH; arg pass-through is automatic.
- A future reader will find a CI base (neon) that is *not* the documented dev
  distro (Fedora). That is intentional and recorded here: dev on Fedora, ship from
  neon.
- This is packaging, not domain language: `CONTEXT.md` is left untouched.
