# Installing arraw

arraw ships via [GitHub Releases](https://github.com/janpipek/arraw/releases) —
one tagged release carries one artifact per OS. There is no store presence
(Flathub / winget / Homebrew) yet; that is a deliberate later milestone (see
[docs/distribution.md](distribution.md)). For v0.x there is **no auto-update** —
re-download from Releases to upgrade.

> ⚠️ arraw is heavily vibe-coded pre-1.0 software. Use at your own risk.

If you would rather build from source, see the README's
[Building](../README.md#building) section and the platform build guides:
[Linux](linux-build.md) · [Windows](windows-build.md).

## Linux (AppImage)

The Linux build is a single-file **AppImage** that bundles Qt, LibRaw, lcms2, and exiv2,
built for x86_64 and reaching every desktop from Ubuntu 24.04 LTS forward.

```bash
# Download arraw-x86_64.AppImage from the Releases page, then:
chmod +x arraw-*.AppImage
./arraw-*.AppImage
```

To integrate it into your menus, drop it somewhere on your `PATH` (e.g.
`~/.local/bin`) or use a tool like Gear Lever / AppImageLauncher.

If the image area renders black or arraw runs on the wrong GPU on a laptop with
switchable graphics, see the [FAQ](faq.md).

### Fedora (RPM)

A native RPM is also produced for Fedora. Install the downloaded package with:

```bash
sudo dnf install ./arraw-*.rpm
```

Building the RPM yourself is covered in the [Linux build guide](linux-build.md)
(`just rpm`).

## Windows

Download the Windows release artifact from the Releases page. Two forms may be
available:

- **Installer** (`.exe`, Inno Setup) — run it and follow the prompts.
- **Portable ZIP** — extract anywhere and run `arraw.exe` from the extracted
  folder. Everything Qt and the app need is bundled alongside the executable.

`arraw.exe` is a console-subsystem binary: run it bare to launch the editor
(`arraw-gui.exe`), or give it a command — see [Command line](#command-line)
below. The Start-menu / desktop shortcuts launch `arraw-gui.exe` directly, with
no console. See the [Windows build guide](windows-build.md) and
[diagnostics in AGENTS.md](../AGENTS.md#diagnostics).

## macOS

macOS packaging is not published yet. Build from source with Homebrew for now —
see [Building → macOS](../README.md#macos-homebrew).

## Verifying the version

Every build reports its version (single-sourced from `CMakeLists.txt`):

```bash
arraw --version
```

## Command line

`arraw` is also a command-line tool:

    arraw                  # open the editor
    arraw ui photo.arw     # open the editor on a file or folder
    arraw export *.arw -o developed/   # render files through their develop
                                       # sidecars, no window needed
    arraw export --help    # format, quality, size, profile, metadata flags

Exports use each file's `.xmp` develop sidecar exactly as the editor would —
no sidecar means the same defaults a freshly opened photo shows. On Windows
the installer's "Add arraw to PATH" option (on by default) makes the command
available in new terminals; the Start-menu entry launches `arraw-gui.exe`,
the editor itself. On a machine without a display, run with
`QT_QPA_PLATFORM=offscreen`.
