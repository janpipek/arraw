# The rewrite ships a Fedora RPM and opens from the desktop

The old application shipped a self-hosted Fedora RPM (`main`'s ADR 0030). The
rewrite had no packaging and no desktop entry, so a photograph could not be
opened with arraw from a file manager. This ADR carries that decision over,
adapted: same contract, smaller first scope, and the executables keep the
names they are built with.

## Decision

**A conventional spec builds one binary RPM.** `packaging/fedora/arraw.spec`
produces an SRPM, a binary RPM, and the debuginfo and debugsource RPMs with
stock `rpmbuild`. Fedora 44 x86_64 is the target. The RPM holds `arraw-ui`,
`arraw-cli`, the desktop entry, the AppStream metainfo and the icons
(`resources/icons/*.png` as hicolor sizes, `resources/icon.svg` as scalable).
There is no Python subpackage yet. Nothing else is installed: the shaders and
the window icon are Qt resources, and the headless Qt platform behind
`arraw-cli` is a static library.

**Installed names are build names.** The launcher runs `arraw-ui %f`; the GUI
takes one photograph or folder, and sets the desktop file name
`io.github.janpipek.arraw`, so a Wayland compositor matches its window to the
entry and its icon. CMake installs under `if(UNIX AND NOT APPLE)`; the rules
are the same ones that a distribution or `DESTDIR` staging uses.

**`just rpm` and `just rpm-smoke`** (`tools/package_fedora.sh`,
`tools/smoke_fedora_rpm.sh`) are ported unchanged in spirit:

- the archive, the spec and the version all come from committed `HEAD`
  (untracked files are ignored; uncommitted tracked edits only warn). A commit
  not tagged `v<version>` is a snapshot with the caret version
  `<version>^<date>git<sha>`, which sorts above the release it follows;
- `base_version` is literal in the spec and the script fails if it differs from
  CMake's `project(VERSION)`, which remains the authority;
- the build is offline: Catch2 3 is a system package (`catch-devel`) and
  `FETCHCONTENT_FULLY_DISCONNECTED` is on;
- the script checks the payload and that the RPM carries the generated Qt
  private-ABI dependency (`libQt6Gui.so.6(Qt_6.x_PRIVATE_API)`), which is how a
  Qt minor update that breaks the private headers is kept out; then it runs
  `rpmlint` and writes `dist/fedora/` with `SHA256SUMS`;
- missing build dependencies are reported, never installed through `sudo`;
- the smoke test installs the RPM with `dnf` in a clean `fedora:44` container
  (podman, or docker), runs `arraw-cli --version` and `arraw-ui --help`
  offscreen, validates the desktop file and asks `gio mime` that arraw handles
  a RAW type and `inode/directory`.

**`%check` runs the whole suite offscreen.** The tests that need a GPU (the
`arraw-gpu-tests` and `arraw-headless-tests` executables) skip rather than
fail when there is no Vulkan device: Catch2 exits 4 and CTest is told so with
`SKIP_RETURN_CODE`. A builder without a GPU therefore runs the CPU tests and
skips the device ones. Nothing is excluded by label.

## Desktop integration

The entry advertises the types arraw opens by name that Fedora's
shared-mime-info defines: CR2, CR3, CRW, NEF, NRW, ARW, SRF, DNG, RAF, ORF,
RW2, Panasonic `.raw`, PEF, MRW, KDC and X3F (`openedRawExtensions` in
`src/core/RawImport.h`), JPEG, PNG, TIFF, and directories. WebP and BMP, which
the old list had, are left out because the rewrite does not open them. A
handler only makes arraw available under "Open With"; it changes no default.

SRW, 3FR, IIQ, ERF and MOS open in arraw but are not advertised: the system
MIME database has no definition for them, and arraw does not install an
extension-only definition that could misclassify unrelated files. SR2 and DCR
have one but arraw does not list them by name.

The list lives in the desktop file and the metainfo `<provides>`; the smoke test
checks CR3, ARW and directories only. A change to the openable extensions has to
touch both files.

## Out of scope

No release CI: tagging and publishing stay manual until the rewrite has a
release. Windows would need installer registry entries for "Open With" and a
`ProgId`, and macOS `CFBundleDocumentTypes` in a bundle's `Info.plist`; neither
has an installer or a bundle yet. The packages are unsigned, as in ADR 0030.
Other distributions, COPR and older Fedora releases are follow-ups.

## Consequences

- Fedora users get dependency management and "Open With" without a repository.
- The package is tied to its Fedora release and to Qt's private ABI; a new
  Fedora needs its own build and smoke test.
- The rewrite starts at 0.4.0, above the old application's last release (0.3.1),
  so it upgrades the old package; the old `/usr/bin/arraw` goes away.
- The entry lists `inode/directory`, so on a desktop with no defaults list arraw
  may become the default folder handler. File managers keep their own default
  on GNOME and KDE.
- The dev sandbox image carries `rpm`, `rpmlint`, `desktop-file-utils`, `appstream`
  and `shared-mime-info` for lint-level checks of the spec, desktop file and
  metainfo; the real build and smoke test still need a Fedora host.

## What was rejected, and why

- **Renaming the binaries to `arraw` with subcommands, as the old app had.**
  The rewrite builds `arraw-ui` and `arraw-cli`; a wrapper would be a second
  entry point to keep in step for no gain.
- **CPack RPM generation.** Weaker Fedora metadata and no SRPM or COPR path.
- **Advertising every RAW extension.** It would claim files the system cannot
  name, and a global definition of our own would misclassify others.
- **Excluding the GPU tests in `%check` by label.** They already skip without
  a device; an exclusion would also hide them where a software device exists.
