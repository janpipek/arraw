# The arraw command front-end: one grammar, a Windows pair, and the export CLI surface

Implementing ADR 0022 surfaced decisions its "second executable `arraw-cli`"
sketch had not made. The delivery shape is now a **command grammar** — `arraw
[command] [args]`, with `ui` and `export` as the v1 commands — carried by **one
binary on Linux/macOS** and an **executable pair on Windows**. ADR 0022's
architectural core is unchanged and still governs: one shader as the single
source of truth, Qt stays, the engine/UI library split, `renderOffscreen` reuse.
This ADR records the delivery shape, the v1 export contract, and the one
refactor 0022 underestimated.

## The Windows pair

A Windows executable's subsystem is fixed at link time, so a single binary
cannot both behave in a shell (console subsystem: the shell waits, stdio
attaches) and launch clean from Explorer (GUI subsystem: no console flash) —
the reason python/pythonw and devenv.com/devenv.exe exist. arraw ships the
split only where the OS forces it:

- **Windows:** `arraw.exe` (console subsystem) is the front-end carrying the
  grammar; it links only the engine and never QtWidgets. `arraw-gui.exe` (GUI
  subsystem, today's `main.cpp`) is the editor; shortcuts, file associations,
  and the `arraw.rc` icon (ADR 0015) move to it. `arraw ui` spawns
  `arraw-gui.exe` detached. An old taskbar pin aimed at `arraw.exe` degrades
  gracefully — bare invocation dispatches to `ui` — with only a console flash.
  The Inno installer (ADR 0016) gains a default-checked task appending the
  install dir to the *user* PATH (HKCU, no elevation), removed on uninstall.
- **Linux/macOS:** the single `arraw` binary keeps shipping; the AppImage
  (ADR 0014), the Fedora RPM (ADR 0030), and the `.desktop` file are untouched
  except that `Exec=arraw %f` becomes `Exec=arraw ui %f`. The `ui` command runs
  the GUI in-process (argv is parsed before the `QApplication` is constructed,
  so the `export` path never instantiates Widgets).

## Dispatch: strict grammar with one AppImage-shaped exception

1. **No arguments → `ui`** (GUI, last-dir restore). Required: the AppImage entry
   point execs the binary bare on double-click, so bare invocation printing
   help would make the AppImage silently do nothing.
2. **`argv[1]` is a known command → dispatch to it.** Command names are
   reserved words forever.
3. **Anything else → error with a suggestion** (`unknown command 'x.arw' — to
   open it in the editor: arraw ui x.arw`), never a silent GUI launch, so a
   typo'd command fails loudly. Checking whether the stray argument exists on
   disk may improve the error's phrasing but never changes the dispatch.

The rejected implicit-`ui` fallback (unknown argument → open it in the GUI,
git-style path pun) kept old `arraw <path>` habits working but turned every
typo into a GUI launch. Every launcher that passes bare paths is a file this
repo ships, so the strict form costs only the one-line `.desktop` change.

## The v1 `export` contract

`arraw export <input>... -o <dir> [flags]`

- **Inputs are explicit files, many at a time, processed sequentially.** No
  self-made globbing (the shell expands on POSIX; a `--glob` can come later for
  Windows), no directory inputs (folder mode would need a "which files count"
  policy the GUI answers with pairing/companion rules — deferred rather than
  approximated), no parallelism (one Headless Render Context, one image at a
  time).
- **`-o` is required and always a directory**, created recursively on demand.
  Output names are always `<stem>.<ext>` — per-file renaming is a single-file
  concern the GUI owns. No default destination: rejected both
  next-to-source (pollutes the shoot folder) and CWD (collides when inputs
  span directories).
- **Intra-run stem collisions abort the run pre-flight** — failing after 300
  renders is the worst version, and auto-suffixing invents names nobody asked
  for. **Existing outputs are overwritten silently**: exports are derived
  artifacts and re-export after tweaking the sidecar is the tool's main loop.
- **Per-file errors don't stop the batch.** One stderr line per failure, one
  stdout line per success (`in.arw -> out/in.jpg`), a final summary. Exit
  codes: `0` all exported, `1` at least one file failed, `2` usage or
  environment error. A metadata-embed failure stays a warning (the pixels
  were written — the CLI is exactly as strict as ADR 0043's definition of a
  successful export).
- **Flags mirror `ExportOptions` 1:1** — `--format jpeg|png|tiff`, `--quality`,
  `--width`/`--height`, `--profile srgb|p3|adobergb`, `--bit-depth 8|16`,
  `--sharpen`, and three asymmetric metadata toggles (`--include-location`,
  `--no-capture-info`, `--no-descriptive`) that each flip a field *away* from
  its product default, so no flag restates a default. Defaults are the
  default-constructed struct — one source of truth shared with the export
  dialog. Where the GUI disables an inapplicable field, the CLI errors
  (`--quality` with TIFF): a silently ignored flag is a lie.
- **No develop-setting overrides.** The sidecar (resolved identically to the
  GUI, embedded-XMP merge included) is the only authority on what the image
  looks like. `--exposure` or `--preset` would need the whole develop surface
  and would break the "byte-identical to a GUI export" golden claim; if that
  feature ever comes, it is its own ADR.

## Parsing: CLI11, vendored

`QCommandLineParser` has no subcommand concept, no typed validation, and help
that cannot express a grammar — the flag surface above is exactly what it is
bad at. CLI11 (BSD-3) provides subcommands, range/enum validators, and
per-command help, and is vendored as the amalgamated single header at
`vendor/CLI11/` (with its LICENSE, version pinned in a comment) rather than
declared in three packaging worlds (vcpkg manifest, Fedora BuildRequires,
Ubuntu AppImage CI) whose archive versions all differ. The GUI keeps its
existing `QCommandLineParser` untouched. `vendor/` (not `third_party/`,
`3rdparty/`) matches the repo's plain-word directory names; one subdirectory
per vendored dependency, each carrying its license.

## Targets, and the extraction ADR 0022 underestimated

`arraw_core` is retired, not repurposed — a survivor named "core" would make
every later conversation ambiguous. The split: **`arraw_engine`** (`core/`,
`develop/`, `pipeline/`, `render/`, `io/`, the Widgets-free shell files, the
shader bake, the Headless Render Context), **`arraw_ui`** (`ui/`,
`MainWindow*`, `ChromeHider`; links Widgets and the engine), and
**`arraw_cli`** (`src/cli/` — dispatch, CLI11 wiring, command
implementations; engine-only). Linux `arraw` links cli+ui; Windows `arraw.exe`
links cli only, `arraw-gui.exe` links ui.

ADR 0022 claimed the only new processing code would be the render context and
argument handling. Untrue: the export render was staged inside the widget —
`ImageViewport::renderToImage` owned the oriented-crop sizing, the export
`FrameParams` recipe, curve-LUT priming, and the buffer/LUT/mask uploads. A
CLI calling `renderOffscreen` directly would have re-derived all of that: a
second copy of the algorithm's staging, the exact drift 0022 forbids. So the
orchestration is **extracted into an engine-side offscreen-render component**
with three callers — `ImageViewport`, the CLI, and the golden tests, which
adopt it (and the Headless Render Context) in the same change, finally
dropping their hidden-widget dance and proving the new substrate on CI before
the CLI exists.

## Headless Render Context backend policy

Mirror `QRhiWidget`'s platform defaults — D3D11 on Windows, Metal on macOS,
OpenGL 3.3 core over a `QOffscreenSurface` elsewhere (correcting 0022's
"GLES2") — so "CLI output ≡ GUI export" is true per machine; across backends
the last bits may differ and the claim is per-platform. Fallbacks: Linux needs
nothing (Mesa hands headless boxes llvmpipe, the goldens' CI path); Windows
retries D3D11 device creation with the WARP software adapter. Backend choice
is exposed only as the `ARRAW_RHI_BACKEND` environment variable, not a flag —
a debugging screwdriver, not a supported workflow.

## Consequences

- Command names (`ui`, `export`, `help`, `version`, …) are reserved words; a
  file literally named `export` needs `arraw ui export` or `./export`.
- Format no longer rides on the output extension (0022's `out.jpg` pun);
  `--format` owns it.
- v1 tests: golden adoption of the headless context, one CLI end-to-end test
  (command function, not subprocess; output within ADR 0005 tolerance of the
  extracted renderer's own product), and GPU-free Catch2 cases pinning the
  dispatch rules, flag validation, and collision pre-flight.
- macOS app-bundle packaging remains future work; nothing here blocks it
  (file opens arrive as `QFileOpenEvent`, not argv).
