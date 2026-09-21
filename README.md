# arraw

A RAW processing engine and application for photographers — a library first,
with a Qt desktop application and a command line over it.

> **This branch is a ground-up rebuild.** Much of what
> [`docs/desired-features.md`](docs/desired-features.md) describes is not
> implemented here yet. Today the engine reads images (including RAW), develops
> them from the camera's own colour into a linear Rec.2020 working space, and
> writes them back out. Exposure and white balance are the develop settings
> that exist; with none of them set, an export is a faithful conversion rather
> than a rendered photograph.

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

## Using

```bash
arraw-cli --help                          # the commands that exist
arraw-cli export *.arw -o out/            # render a shoot, JPEG by default
arraw-cli export photo.dng -o out/ --format png --bit-depth 16
arraw-cli export photo.arw -o out/ --exposure -0.5 --temperature 3200   # RAW only
```

Inputs are files rather than directories; your shell expands the wildcards.
Every input is attempted, so one bad frame does not abandon an overnight batch.
See [ADR 006](docs/adr/006-the-command-line.md) for the full contract.

## Layout

- `include/` — the public API
- `src/core/` — the engine
- `src/app/` — the Qt application
- `src/cli/` — the command line
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
