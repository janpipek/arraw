# Windows native icon and GUI subsystem (resolving 0013's deferral)

[ADR 0013](0013-app-icon-svg-source-runtime-only.md) shipped the *runtime* window
icon from a single SVG and deliberately deferred the OS-level native icons. The
Windows half of that deferral is now done: `arraw.ico` is **embedded into
`arraw.exe`** as a Win32 resource, so Explorer, the taskbar, and the title bar show
the app icon for the file itself — not just the running window. The `.ico` is
generated from the *same* SVG-derived PNGs (`tools/render_icons.py` packs the 16–256
px PNGs, PNG-compressed, into `resources/arraw.ico`), so 0013's single-source rule
holds — there is no new master asset, only one more derived artifact committed
beside the PNGs. `resources/arraw.rc` references the icon at id `1` (the lowest id,
which the shell uses for the executable's icon); `enable_language(RC)` compiles it,
guarded by `if(WIN32)` so other platforms are untouched.

Separately, `arraw.exe` is now linked for the **Windows GUI subsystem** (the `WIN32`
keyword on `add_executable`). The default console subsystem popped a console window
before the GUI on every double-click — wrong for a GUI app. `Qt6::EntryPoint` (pulled
in transitively via `Qt6::Core`) supplies `WinMain → main`, so `src/main.cpp` is
unchanged.

## Considered Options

- **Adopt `qt_add_executable` / `windeployqt`.** Qt's own helper sets the GUI
  subsystem and can generate an icon `.rc`, and `windeployqt` would deploy Qt +
  plugins. Rejected: the project uses plain `add_executable` with a manual,
  per-config vcpkg deployment (this vcpkg port set ships no `windeployqt`). A
  three-line `.rc` plus `enable_language(RC)` is far less machinery than switching
  to `qt_add_executable` and inheriting its deployment assumptions.
- **Render the exe icon from the SVG at runtime.** Not possible: the executable's
  file icon is a Win32 resource the shell reads *before* the process starts, so it
  must be a static `.ico`, not a runtime `QIcon`. Runtime SVG rendering was already
  rejected for the window icon in 0013 for separate (legibility) reasons.
- **Keep the console subsystem.** The console doubles as a convenient sink for the
  `ARRAW_TRACE` timing output. Rejected — popping a terminal on every launch is the
  wrong default for a GUI app; tracing is redirected to a file when needed.

## Consequences

- 0013's "the icon is **not wired into any installer or bundle**" is now superseded
  **for the Windows `.exe` icon specifically**. The macOS `.icns` (`.app` bundle) and
  Linux `.desktop` + `hicolor` PNGs remain deferred, exactly as 0013 framed them.
- `resources/arraw.ico` is a generated artifact committed like the PNGs — do not
  hand-edit it. Edit `resources/icon.svg` and rerun `uv run tools/render_icons.py`,
  which now writes the PNGs **and** the `.ico`.
- A GUI-subsystem app has no console attached, so the `ARRAW_TRACE` diagnostics
  (`src/Trace.h`) print nowhere when launched from a terminal. Redirect to capture
  them: `arraw.exe 2> trace.txt`. Recorded at the call site and in
  [windows-build.md](../windows-build.md).
- `tools/package_windows.py` needs nothing special — it bundles `arraw.exe`, which
  now carries both the icon and the GUI subsystem.
- This stays branding/packaging, not domain language: `CONTEXT.md` is untouched.
