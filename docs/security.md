# Security risk register

This document tracks known product and software-supply-chain risks that are not yet
fully resolved. It is not a vulnerability-reporting policy; a future `SECURITY.md`
will define private reporting and supported-version policy.

## Open risks

### Mutable GitHub releases and tags

- **Surface:** GitHub Release assets for every platform.
- **Threat:** A compromised maintainer token or workflow could replace a published
  artifact or move its tag, leaving different binaries under the same version.
- **Current mitigation:** Publication is manual, routed through the `release`
  environment, and write permission is limited to the publish job. Repository
  maintainers must configure that environment to require review. Existing assets
  require an explicit replacement input. Checksums and provenance attestations make
  replacement detectable to users who verify them.
- **Intended resolution:** Enable GitHub release immutability, upload complete
  releases as drafts, and publish once. Treat corrections as new releases.

### Unsigned native packages

- **Surface:** Self-hosted Fedora RPM and future native packages.
- **Threat:** Package-manager workflows cannot authenticate an unsigned RPM through
  a project signing key.
- **Current mitigation:** HTTPS transport through GitHub Releases, published
  SHA-256 checksums, and GitHub build-provenance attestations.
- **Intended resolution:** Sign repository metadata and packages when COPR or
  another package repository is introduced. Document key rotation and revocation.

### Mutable third-party build inputs

- **Surface:** GitHub Actions, container images, Fedora repositories, Qt downloads,
  and linuxdeploy's continuous release.
- **Threat:** A compromised or unexpectedly changed build input can alter release
  artifacts without a source change.
- **Current mitigation:** GitHub Actions are pinned to full commit SHAs. Build jobs
  have read-only repository permissions. Fedora package builds are offline after
  declared build dependencies are installed. Release artifacts receive provenance
  attestations.
- **Intended resolution:** Pin and verify downloadable tools by digest, assess
  snapshot repositories or dependency manifests for reproducible builds, and add
  controlled automation for dependency-pin updates.

### Non-reproducible release artifacts

- **Surface:** AppImage and native package release builds.
- **Threat:** Provenance identifies where an artifact was built but independent
  rebuilds cannot yet prove that the same source and inputs produce identical bytes.
- **Current mitigation:** Builds start from an exact tag, validate version agreement,
  and publish checksums plus provenance.
- **Intended resolution:** Normalize timestamps and archive ordering, pin complete
  build inputs, and compare independent rebuilds.

## Resolved or actively enforced controls

- Release workflows are manually dispatched; each platform target is explicitly
  selected.
- Workflow permissions are read-only by default and publication is separately
  authorized.
- Third-party GitHub Actions are pinned to full commit SHAs.
- Fedora RPM tests do not download dependencies during the build.
