# Self-hosted Fedora RPMs with manual release builds

Arraw will ship a Fedora-native RPM alongside the existing AppImage. Fedora 44
x86_64 is the first target. The RPM and its SRPM are built from a conventional,
committed spec and distributed through GitHub Releases; COPR, the Fedora package
repositories, older Fedora releases, and Debian packages are follow-ups.

This partially supersedes ADR 0014, which ruled out native Linux packages. The
AppImage remains the broad, bundled Linux artifact. The RPM instead integrates
with Fedora's package manager and uses Fedora's Qt, LibRaw, lcms2, and image-format
plugins. Maintaining both artifacts is intentional: they serve different users
and expose different compatibility boundaries.

## Local packaging contract

- `packaging/fedora/arraw.spec` is a normal Fedora spec capable of producing an
  SRPM, binary RPM, debuginfo RPM, and debugsource RPM with stock `rpmbuild`.
- `just rpm` archives committed `HEAD` and builds without root access. Dirty
  working trees are rejected. Untagged development builds use an RPM snapshot
  release containing the date and abbreviated commit SHA; tagged builds use
  `Release: 1%{?dist}`.
- `Version` is necessarily literal in the spec because RPM evaluates metadata
  before unpacking sources. Tooling fails if it differs from CMake's
  `project(VERSION)`, which remains the project version authority.
- Package builds are offline. Fedora's Catch2 v3 package (`catch-devel`) is a
  build requirement; CMake's FetchContent fallback is disabled.
- `%check` runs the complete test suite under Qt's offscreen platform. Desktop,
  AppStream, RPM, and installed-payload validation are mandatory. Local
  dependencies are reported but never installed through `sudo` automatically.
- `just rpm-smoke` installs the result in a clean Fedora 44 container through
  `dnf`, runs `arraw --version` offscreen, and checks desktop/MIME visibility.
  Podman is preferred; Docker is accepted through the existing backend selection.

Qt private API use does not require a manually pinned package EVR. Fedora's RPM
dependency generator records the versioned private symbol capability (for example,
`libQt6Gui.so.6(Qt_6.11_PRIVATE_API)(64bit)`). Validation asserts that the binary
RPM contains that dependency, allowing compatible patch updates while preventing
an incompatible Qt minor update.

## Desktop integration

The desktop launcher advertises the formats arraw deliberately opens. These are
the nine RAW formats with MIME types supplied by Fedora (CR2, CR3, NEF, ARW, DNG,
RAF, ORF, RW2, and PEF) and JPEG, PNG, TIFF, WebP, and BMP. Declaring a handler
only makes arraw available through "Open With"; it does not change user defaults.

Samsung SRW remains supported through arraw's file chooser but is not advertised.
Fedora's shared MIME database has no SRW definition, and current Gwenview likewise
does not register one. Arraw will not install an extension-only system definition
that could globally misclassify unrelated `.srw` files.

## Release workflow

Release builds do not run on tag pushes. A `workflow_dispatch` invocation supplies
an existing `vX.Y.Z` tag and explicitly selects AppImage and/or Fedora RPM; no
target is selected by default. The workflow checks out the tag as build input and
fails if it disagrees with both CMake and RPM versions. CI publishes tagged release
builds only; snapshots stay local.

All selected builds and smoke tests finish before publication. The Fedora release
set contains the binary RPM, SRPM, and SHA-256 checksums. Debuginfo/debugsource are
retained as workflow artifacts without cluttering the public release. Existing
same-named assets are not replaced unless the dispatcher explicitly opts in.

Release jobs follow least privilege: read-only token permissions by default and
write permission only for publication. The workflow itself must be dispatched from
the default branch, and publication is placed behind a protected `release` GitHub
Environment. Third-party Actions are pinned to complete commit SHAs. Published
AppImage, RPM, and SRPM artifacts receive GitHub build-provenance attestations.

## Security posture

The first RPMs are unsigned. Local snapshots are also unsigned. GitHub-generated
SHA-256 digests, explicit checksum files, and provenance attestations establish
integrity and build origin, but are not substitutes for RPM repository signing.
Signing is deferred until a repository/COPR makes package-manager trust useful.

GitHub release immutability is also deferred. Replacement is explicit and audited,
but a mutable release still permits two different artifacts to exist under one
version over time. The desired future flow is build and validate everything,
upload to a draft, and publish once with repository release immutability enabled.
Security debt and mitigations are tracked in `docs/security.md`.

## Considered options

- **CPack RPM generation.** Less initial code, but weaker Fedora metadata and no
  credible SRPM/COPR path. Rejected in favour of a conventional spec.
- **COPR or Fedora repositories now.** Better updates and signing, but adds account,
  review, repository, and maintenance work before the package is proven. Deferred.
- **Fedora 43 in the first release.** Separate native builds are required per
  Fedora release. Deferred until the Fedora 44 path works; containers make the
  extension straightforward.
- **Debian package in the same change.** Dependency names, Qt private ABI handling,
  metadata, and target distributions require a separate design. Deferred.
- **Exact Qt package-version dependency.** Overly restrictive after normal Fedora
  updates. Rejected because generated private-ABI symbol requirements express the
  actual constraint.
- **Automatic dependency installation on the workstation.** Convenient but an
  inappropriate hidden privileged mutation. Rejected.

## Consequences

- Fedora users get native dependency management and desktop integration without a
  distribution repository.
- A Fedora RPM is tied to its Fedora release and Qt private ABI; new Fedora targets
  require their own builds and smoke tests.
- Two Linux delivery mechanisms and their security posture must remain documented.
- ADR 0014's AppImage choice remains valid; only its rejection of native packages
  and tag-triggered release assumption are superseded.
