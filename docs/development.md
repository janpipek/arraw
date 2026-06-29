# Development Tasks

Routine project-maintenance commands live in the `Justfile`. Run `just` to list
the available recipes.

## App Icon

`resources/icon.svg` is the source file for the app icon. The derived PNGs and
Windows `.ico` are committed because they are runtime and packaging assets, not
rebuilt as part of every normal build.

After changing `resources/icon.svg`, regenerate the derived files with:

```bash
just icons
```

On Windows, the icon renderer uses CairoSVG through `uv`, which still needs the
native Cairo library available on PATH. If `just icons` fails with an error about
`cairo-2`, `cairo`, or `libcairo-2.dll` not being found, install Cairo for Windows
or run the recipe from Linux/macOS/CI. The existing generated icon files can stay
unchanged until the SVG update is ready to be rasterised in an environment with
Cairo available.

This updates:

- `resources/icons/arraw-16.png`
- `resources/icons/arraw-24.png`
- `resources/icons/arraw-32.png`
- `resources/icons/arraw-48.png`
- `resources/icons/arraw-64.png`
- `resources/icons/arraw-128.png`
- `resources/icons/arraw-256.png`
- `resources/arraw.ico`

Do not hand-edit the generated PNGs or `resources/arraw.ico` or any of the PNGs.
