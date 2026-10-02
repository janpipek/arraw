# arraw

A RAW processing engine and application for photographers — a library first,
with a Qt desktop application and a command line over it.

> **This branch is a ground-up rebuild.** Much of what
> [`docs/desired-features.md`](docs/desired-features.md) describes is not
> implemented here yet. Today the engine reads images (including RAW), develops
> them from the camera's own colour into a linear Rec.2020 working space, and
> writes them back out. Exposure, tone controls and white balance are implemented;
> defaults include a gentle highlight roll-off. Camera orientation, arbitrary
> rotation, flips and cropping are applied during development.

## Building

Requires CMake 3.21+, a C++20 compiler, **Qt 6.10**, and **LibRaw** (`libraw-dev`
on Ubuntu, `libraw-devel` on Fedora). LibRaw is required rather than optional, so
that every build decodes a RAW identically — see
[ADR 005](docs/adr/005-raw-import-through-libraw.md).

```bash
just build     # configure and build (Debug)
just test      # build and run the suite
just format    # clang-format owns mechanical formatting
just fixtures  # regenerate the committed test fixtures (needs uv)
```

For containerized agent sessions, the [VibePod overlay](.vibepod/README.md)
provides the Linux build dependencies and developer tools automatically.

## Using

```bash
arraw-cli --help                          # the commands that exist
arraw-cli export *.arw -o out/            # render a shoot, JPEG by default
arraw-cli export photo.dng -o out/ --format png --bit-depth 16
arraw-cli export photo.arw -o out/ --exposure -0.5 --temperature 3200   # RAW only
arraw-cli export photo.arw -o out/ --rotate 2.5 --crop-aspect 3:2
arraw-cli export photo.arw -o out/ --rotate 90 --crop 0.1,0.2,0.8,0.9
arraw-cli export photo.arw -o out/ --device gpu --gpu-backend vulkan   # no CPU fallback
arraw-cli gpu-test                        # check the GPU backend works on this machine
```

On Linux the command line needs no display: it runs on its own headless Qt
platform, which reaches Vulkan through the driver alone but has no OpenGL.
`export` uses it whatever `QT_QPA_PLATFORM` says, unless `--gpu-backend opengl`
is given, which honours `QT_QPA_PLATFORM=xcb` or `wayland`; `gpu-test` honours
it always. `ARRAW_DISABLE_GPU=1` keeps it off the graphics stack entirely.

`export` develops on the GPU when it can. `--device auto` (the default) creates
one device for the whole batch; if there is none, or it is a software
rasteriser (llvmpipe, lavapipe, WARP; accept one with `--allow-software`), or
`ARRAW_DISABLE_GPU` is set, it warns once and exports the batch on the CPU. A
photograph the GPU fails on is retried on the CPU with a warning. `--device gpu`
never falls back: it fails instead, and combined with `ARRAW_DISABLE_GPU` it is
a usage error. With `--gpu-backend opengl`, `auto` is `gpu`: OpenGL needs a
display's platform, and a process cannot fall back from one that will not load.
`--device cpu` loads no graphics stack. `--gpu-backend` picks the API (`vulkan`, `opengl`, `d3d11`, `d3d12`, `metal`), and one line per batch says
which device was used (silenced by `--quiet`).

The desktop app (`arraw-ui`) renders its preview on the GPU when it can, uploading
the photograph once, and on the CPU otherwise; the status bar says which, and
why not the GPU when it fell back (tooltip). `ARRAW_PREVIEW_DEVICE=cpu` forces
the CPU, for comparison or a broken driver; anything else, or unset, means
"GPU when available". Software rasterisers are not used for the preview.

Inputs are files rather than directories; your shell expands the wildcards.
Every input is attempted, so one bad frame does not abandon an overnight batch.
See [ADR 006](docs/adr/006-the-command-line.md) for the full contract.

Tone flags are `--exposure`, `--contrast`, `--shadows`, `--highlights`,
`--blacks`, `--whites`, and `--filmic-highlights`. Colour flags are
`--white-balance as-shot|custom`, `--temperature`, and `--tint`. Specifying
temperature or tint implies custom white balance; combining either with an
explicit `as-shot` is an error. Custom with neither value retains the applied
white balance for RAW images. Temperature and tint require RAW input.

Geometry flags:

| Flag | Value |
|---|---|
| `--rotate` | Any finite clockwise angle in degrees, relative to camera orientation |
| `--flip-horizontal`, `--flip-vertical` | Boolean flags, in final upright axes |
| `--crop` | `auto` or normalised upright `left,top,right,bottom`, e.g. `0.1,0.2,0.8,0.9` |
| `--crop-aspect` | `free`, `original`, or `width:height`, e.g. `3:2` or `2:3` |

Resize flags, applied after the crop:

| Flag | Value |
|---|---|
| `--resize` | `N` is the long edge in pixels, `WxH` is a box to fit inside, `N%` is a scale factor (`12.5%` is fine). A bare number is the long edge. Default: full size |
| `--allow-upscale` | Let `--resize` enlarge past the photograph's own size; by default it only shrinks |
| `--resize-filter` | `lanczos` (default, sharper, with slight ringing at hard edges) or `bilinear` (softer, never rings) |

Rotation is split into the nearest quarter-turn and a straighten remainder
within ±45°: `--rotate 100` gives 90° plus 10°, and `--rotate -20` gives 0°
plus -20°. Full turns wrap. At exact half-quarter-turns, the reduced angle rounds away
from zero: 45° gives 90° minus 45°, and -45° gives 270° plus 45°.

Crop edges describe the final upright frame, regardless of argument order;
they are not a sequence of crop and rotation commands. Aspect is a remembered
constraint, not a substitute for crop edges. With an automatic crop, the renderer
chooses the largest valid rectangle at the requested aspect. An explicit crop
must already match a locked aspect; a mismatch fails that input. Crops that
would include empty corners shrink around their centre, moving only when no
positive-size rectangle fits there. Source pixels are never modified.

Quarter-turns, flips and pixel-aligned crops preserve developed samples exactly.
Fractional rotations and crops use bilinear interpolation in linear colour with
premultiplied alpha. Continuous crop dimensions are floored to whole output
pixels, with a minimum of one pixel per axis. Resizing is applied last,
after the crop (see `--resize`).

## Layout

- `include/` — the public API
- `src/core/` — the engine
- `src/app/` — the Qt application
- `src/cli/` — the command line
- `src/platform/headless/` — the command line's display-free Qt platform (Linux)
- `docs/adr/` — why each hard-to-reverse choice was made
- `docs/desired-features.md` — the feature brief, from a photographer's view

## Licence

Copyright (C) 2026 Jan Pipek

arraw is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version — **GPL-3.0-or-later**.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the [GNU General Public License](LICENSE) for more
details.

### Third-party components

arraw links libraries under their own terms, all compatible with GPL-3.0-or-later:

| Component | Licence | Used for |
|---|---|---|
| [Qt 6](https://www.qt.io/) | LGPL-3.0-or-later | Application framework, image codecs |
| [LibRaw](https://www.libraw.org/) | LGPL-2.1 / CDDL-1.0 | RAW decoding |
| [Catch2](https://github.com/catchorg/Catch2) | BSL-1.0 | Test framework (not distributed) |

A distributed binary must carry these licence texts alongside arraw's own.
