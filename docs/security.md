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
  require an explicit replacement input. Checksums make replacement detectable to
  users who verify them. Build-provenance attestations are temporarily disabled
  while the repository is private (see "Build-provenance attestations disabled while
  private" below).
- **Intended resolution:** Enable GitHub release immutability, upload complete
  releases as drafts, and publish once. Treat corrections as new releases.

### Release environment protection is not configured

- **Status:** Open — [GitHub issue #38](https://github.com/janpipek/arraw/issues/38).
- **Surface:** The shared publish job for AppImage, Fedora RPM, and future platform
  artifacts.
- **Threat:** Naming an environment in workflow YAML does not itself require an
  approval. Until repository protection rules are configured, a maintainer who can
  dispatch the workflow can reach the write-enabled publication job directly.
- **Current mitigation:** Build jobs remain read-only, dispatch is restricted to the
  default branch, and same-named asset replacement requires an explicit input.
- **Required setup:** Create one `release` environment, require reviewer approval,
  prevent self-review where available, and restrict deployment to the default
  branch. All platform builds converge on this one publication boundary; separate
  build environments are unnecessary.

### Unsigned native packages

- **Surface:** Self-hosted Fedora RPM and future native packages.
- **Threat:** Package-manager workflows cannot authenticate an unsigned RPM through
  a project signing key.
- **Current mitigation:** HTTPS transport through GitHub Releases and published
  SHA-256 checksums. GitHub build-provenance attestations are temporarily disabled
  while the repository is private (see below).
- **Intended resolution:** Sign repository metadata and packages when COPR or
  another package repository is introduced. Document key rotation and revocation.

### Mutable third-party build inputs

- **Surface:** GitHub Actions, container images, Fedora repositories, Qt downloads,
  and linuxdeploy's continuous release.
- **Threat:** A compromised or unexpectedly changed build input can alter release
  artifacts without a source change.
- **Current mitigation:** GitHub Actions are pinned to full commit SHAs. Build jobs
  have read-only repository permissions. Fedora package builds are offline after
  declared build dependencies are installed. Release artifacts will receive provenance
  attestations again once the repository is public (currently disabled; see below).
- **Intended resolution:** Pin and verify downloadable tools by digest, assess
  snapshot repositories or dependency manifests for reproducible builds, and add
  controlled automation for dependency-pin updates.

### Non-reproducible release artifacts

- **Surface:** AppImage and native package release builds.
- **Threat:** Provenance identifies where an artifact was built but independent
  rebuilds cannot yet prove that the same source and inputs produce identical bytes.
- **Current mitigation:** Builds start from an exact tag, validate version agreement,
  and publish checksums. Provenance attestations are temporarily disabled while the
  repository is private (see below).
- **Intended resolution:** Normalize timestamps and archive ordering, pin complete
  build inputs, and compare independent rebuilds.

### Build-provenance attestations disabled while private

- **Status:** Open — deferred until the repository is public.
- **Surface:** AppImage, RPM, and SRPM release artifacts.
- **Threat:** Without attestations, consumers cannot cryptographically verify which
  workflow and commit produced an artifact; provenance claims elsewhere in this
  register are not currently enforceable end to end.
- **Reason disabled:** GitHub build-provenance attestations are only publicly
  verifiable for public repositories (or with GitHub Advanced Security). While
  `arraw` is private, the `actions/attest` step is gated off in
  `.github/workflows/release.yml`.
- **Current mitigation:** Published SHA-256 checksums let downloaders detect
  tampering; release dispatch and publication remain manually authorized.
- **Intended resolution:** Re-enable the "Generate build-provenance attestations"
  step and restore the `id-token: write` / `attestations: write` permissions in the
  `attest` job when the repository is made public.

## Resolved or actively enforced controls

- Release workflows are manually dispatched; each platform target is explicitly
  selected.
- Workflow permissions are read-only by default and publication is separately
  authorized.
- Third-party GitHub Actions are pinned to full commit SHAs.
- Fedora RPM tests do not download dependencies during the build.
