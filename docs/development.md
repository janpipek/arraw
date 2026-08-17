# Development tasks

Routine project-maintenance commands live in the `Justfile`. Run `just` to list
the available recipes.

Related: [README § Building](../README.md#building) for per-platform setup,
[AGENTS.md](../AGENTS.md) for the test/format/lint commands, and
[DESIGN.md § Source Layout](../DESIGN.md#source-layout) for the library and
executable targets.

## App icon

`resources/icon.svg` is the source file for the app icon. The derived PNGs and the
Windows `.ico` are committed because they are runtime and packaging assets, not
rebuilt as part of every normal build
([ADR 0013](adr/0013-app-icon-svg-source-runtime-only.md)).

After changing `resources/icon.svg`, regenerate the derived files with:

```bash
just icons
```

This updates `resources/icons/arraw-{16,24,32,48,64,128,256}.png` and
`resources/arraw.ico`. **Do not hand-edit any of the generated files.**

On Windows the icon renderer uses CairoSVG through `uv`, which still needs the
native Cairo library available on PATH. If `just icons` fails with an error about
`cairo-2`, `cairo`, or `libcairo-2.dll` not being found, install Cairo for Windows
or run the recipe from Linux/macOS/CI. The existing generated icon files can stay
unchanged until the SVG update is ready to be rasterised in an environment with
Cairo available.

## Packaging

| Command | Host | Produces |
|---|---|---|
| `just appimage` | Linux | AppImage in `dist/`, built in a podman/docker container |
| `just rpm` | Linux | Fedora RPM + SRPM in `dist/fedora/`, from a clean committed checkout |
| `just rpm-smoke` | Linux | Installs and verifies that RPM in a clean Fedora 44 container |
| `uv run tools/package_windows.py` | Windows | Portable ZIP in `dist/` |
| `just windows-installer` | Windows | Inno Setup `setup.exe` (needs `ISCC` on PATH) |
| `just bump <version>` | any | Rewrites the version single-sourced from `CMakeLists.txt` |

Each recipe is defined for one host OS and fails fast with a clear message on the
other. Release artifacts are also built by the manually dispatched
`.github/workflows/release.yml`. Full context, release workflow, and posture:
[docs/distribution.md](distribution.md), [docs/linux-build.md](linux-build.md),
[docs/windows-build.md](windows-build.md).
