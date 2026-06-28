# Building arraw on Windows

This guide covers the full Windows development setup for arraw using **vcpkg** for
dependencies and **MSVC 2022** as the compiler. It documents the exact toolchain,
the non-obvious environment requirements, and fixes for every error you are likely
to hit. It reflects a working setup verified on Windows 11 (June 2026).

> The Linux/macOS instructions in [AGENTS.md](../AGENTS.md) are simpler because their
> package managers put compilers, SDK tools, and Qt plugins on a single path. On
> Windows the toolchain is split across three providers (vcpkg, MSVC, scoop), so a
> few extra steps are required.

---

## 1. Prerequisites

Install these once. Paths below are the ones this guide assumes; adjust if yours differ.

| Tool | What for | Location used here |
|---|---|---|
| **Visual Studio 2022** (Community is fine) | MSVC C++ compiler (`cl.exe`), linker, and the Windows SDK (`rc.exe`, `mt.exe`) | `C:\Program Files\Microsoft Visual Studio\2022\Community` |
| **vcpkg** | C++ dependencies (Qt, libraw, lcms2, …) | `C:\dev\vcpkg` |
| **CMake** and **Ninja** | Build system + generator | via [scoop](https://scoop.sh): `C:\Users\<you>\scoop\shims\` |
| **Git** | Source control | `C:\Program Files\Git` |

When installing Visual Studio, make sure the **"Desktop development with C++"**
workload is selected — that is what provides MSVC *and* the Windows SDK. The SDK is
the part that supplies `rc.exe`/`mt.exe`, which the build needs (see §4).

Install CMake/Ninja via scoop (or use the copies bundled with VS — either works):

```powershell
scoop install cmake ninja
```

---

## 2. Install the dependencies with vcpkg

If vcpkg is not yet bootstrapped:

```powershell
git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat
```

Install the ports arraw needs, for the `x64-windows` triplet (this is a long first
build — Qt is large):

```powershell
C:\dev\vcpkg\vcpkg.exe install qtbase qttools qtshadertools "libraw[openmp]" lcms exiv2 --triplet x64-windows
```

`qtshadertools` is required: it provides `qsb`, which compiles the GLSL shaders in
`shaders/` at build time. `qttools` provides the rest of the Qt tooling.

`libraw[openmp]` builds libraw with OpenMP so the demosaic (`dcraw_process`) runs
multithreaded — roughly 3× faster on a multi-core CPU (≈3s → ≈0.9s on the reference
machine). It adds a dependency on the OpenMP runtime `vcomp140.dll`, which the
packaging bundles app-local alongside the CRT DLLs (see §7 / ADR 0016) — no separate
redistributable is required on the target machine. To profile, set the `ARRAW_TRACE`
environment variable before launching `arraw.exe`; expensive operations (RAW load
stages, colour transforms, LUT builds) print a `[trace] <label> N ms` line on stderr
(see `src/Trace.h`).

Verify they landed:

```powershell
C:\dev\vcpkg\vcpkg.exe list
```

You should see `qtbase`, `qttools`, `qtshadertools`, `libraw`, `lcms`, and `exiv2`
among the output.

---

## 3. Point CMake at the vcpkg toolchain

CMake needs the vcpkg toolchain file so `find_package` can locate Qt et al. The
repo keeps this in a **local, untracked** `CMakePresets.json` (the path is
machine-specific, so it is intentionally not committed):

```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "default",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build",
      "cacheVariables": {
        "CMAKE_TOOLCHAIN_FILE": "C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake",
        "VCPKG_TARGET_TRIPLET": "x64-windows"
      }
    }
  ]
}
```

Adjust the toolchain path if your vcpkg lives elsewhere. With this in place you
configure with `cmake --preset default`. (If you prefer not to use a preset, pass
`-DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake` directly.)

---

## 4. The critical step: use the MSVC developer environment

This is the part that trips people up. Having `cl.exe` on your `PATH` is **not
enough**. The build also needs the Windows SDK tools `rc.exe` (resource compiler)
and `mt.exe` (manifest tool), plus the correct `INCLUDE`/`LIB` paths. These are only
set up by the MSVC "developer environment". Without it, CMake's compiler check fails
with messages like:

```
The C++ compiler ... is not able to compile a simple test program.
RC Pass 1: command "rc /fo ... " failed
  no such file or directory
--mt=CMAKE_MT-NOTFOUND
```

You have two ways to get that environment:

### Option A — Developer PowerShell for VS 2022 (simplest)

Open **"Developer PowerShell for VS 2022"** from the Start menu. It launches with the
full MSVC + SDK environment already applied. Run all `cmake`/`ctest` commands there.

### Option B — Import the environment into your current shell

Useful for scripts/automation. Import `vcvars64.bat` into the current PowerShell
session (environment changes do not persist between separate shell invocations, so
keep the build commands in the same session/script):

```powershell
$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>&1 && set" |
    ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] } }
```

After this, `rc`, `mt`, `cl`, and `link` all resolve. You can sanity-check with
`Get-Command rc, mt, cl, link`.

---

## 5. Configure, build, run, test

From a shell that has the developer environment (§4):

```powershell
# Configure (reads CMakePresets.json)
cmake --preset default

# Build everything (app + tests)
cmake --build build

# Run the app
.\build\arraw.exe

# Run the test suite
ctest --test-dir build --output-on-failure
```

A clean run builds `build\arraw.exe` and `build\tests\arraw_tests.exe`, and `ctest`
reports all tests passing. The two golden-image rendering tests are **skipped** under
the headless offscreen platform used for tests — that is expected, not a failure (see
§7).

If you change the vcpkg toolchain path or the configure step gets into a bad state,
delete the `build\` directory and reconfigure from scratch.

---

## 6. Two Windows-specific fixes baked into the build

These are already in `CMakeLists.txt` / `tests/CMakeLists.txt`; you do not need to do
anything, but it helps to know why they exist because the symptoms are confusing.

### 6.1 libraw debug/release DLL mismatch

vcpkg names libraw's **debug** import library differently from the release one
(`rawd.lib` → `rawd.dll` vs `raw.lib` → `raw.dll`), and the debug DLL only lives in
`installed\x64-windows\debug\bin`. A naïve `find_library(... NAMES raw)` picks the
*release* import lib even in a Debug build, so the produced exe imports `raw.dll`,
which is never deployed next to a debug binary. The result is an immediate startup
crash:

```
Exit code 0xc0000135   (STATUS_DLL_NOT_FOUND)
```

The fix in `CMakeLists.txt` selects the import library per build configuration
(`rawd` in Debug, `raw` in Release) so the right DLL gets deployed.

### 6.2 Qt plugin (and codec) deployment

vcpkg's auto-deploy step (`applocal`) copies the Qt **DLLs** next to the binary but
**not** the Qt **plugins** for this Qt 6.11 layout. Two distinct symptoms follow:

- **No platform plugin** → Qt aborts on startup:
  `This application failed to start because no Qt platform plugin could be initialized.`
- **No imageformats plugin** → `QImage` silently fails to decode JPEG/GIF/ICO, so the
  app shows **"Failed to load"** for every JPG thumbnail. (PNG and BMP are built into
  Qt Gui, so they keep working — which is why the gap is easy to miss.)

`CMakeLists.txt` defines `arraw_deploy_qt_plugins(<target>)`, a Windows-only
(`if(WIN32)`) post-build step applied to both `arraw` and `arraw_tests`. It copies:

- `qwindowsd.dll`, `qoffscreend.dll` → `platforms\` next to the exe;
- `qjpegd.dll`, `qgifd.dll`, `qicod.dll` → `imageformats\` next to the exe;
- `jpeg62.dll` (libjpeg-turbo) → **next to the exe itself**. The JPEG plugin links
  this codec, and applocal misses it because the exe never imports it directly —
  only the plugin does. Windows resolves a plugin's imports from the *executable's*
  directory, so the codec must sit beside the exe, not inside `imageformats\`.

The test suite additionally runs with `QT_QPA_PLATFORM=offscreen` (set via
`catch_discover_tests`) so it does not need a visible desktop session.
`test_StandardImageLoader.cpp` decodes a real JPEG and acts as a regression guard for
this whole deployment chain.

> This vcpkg Qt build ships only the jpeg/gif/ico imageformats plugins — there is no
> tiff/webp plugin, so those extensions in `StandardImageLoader::canLoad` will not
> decode until the corresponding vcpkg features are installed and deployed.

The codec DLL is fetched from the vcpkg tree pointed at by the `ARRAW_VCPKG_INSTALLED`
cache variable (default `C:/dev/vcpkg/installed/x64-windows`). Override it at configure
time if your vcpkg lives elsewhere:

```powershell
cmake --preset default -DARRAW_VCPKG_INSTALLED=D:/vcpkg/installed/x64-windows
```

---

## 7. Packaging a release ZIP

`tools/package_windows.py` builds a Release configuration (tests off) and bundles the
runnable app into `dist/arraw-<version>-windows-x64.zip`. It imports the MSVC
developer environment itself, so it can be run from a plain shell:

```powershell
python tools/package_windows.py
```

Useful flags: `--skip-build` (zip an existing `build-release/` without rebuilding),
`--vcvars`, `--toolchain`, `--vcpkg-installed`, `--build-dir`, `--out-dir`. The archive
contains `arraw.exe`, the Release Qt/libraw/lcms runtime DLLs, and the `platforms\` /
`imageformats\` plugin folders plus `jpeg62.dll` — i.e. everything the deploy step
(§6.2) places next to the binary, in its Release variant.

To also build the installer, install Inno Setup (`scoop install inno-setup`, so
`ISCC` is on PATH) and run `python tools/package_windows.py --installer`. It writes
`dist/arraw-<version>-windows-x64-setup.exe` (per-user, no admin; see ADR 0016).

> **VC++ runtime:** the packaging bundles the CRT and OpenMP runtime DLLs
> (`vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140.dll`, `vcomp140.dll`) app-local,
> sourced from the VS redist tree (`VC\Redist\MSVC\<ver>\x64\Microsoft.VC*.CRT\` and
> `…Microsoft.VC*.OpenMP\`). Both the ZIP and the installer are therefore
> self-contained — target machines need no separately installed "Microsoft Visual C++
> 2015–2022 Redistributable". (Decision of record: ADR 0016.)

---

## 8. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `vcpkg.exe not found` | vcpkg installed somewhere other than `C:\dev\vcpkg` | Point the toolchain path in `CMakePresets.json` at your actual vcpkg root. |
| CMake: *"compiler is not able to compile a simple test program"*, `rc ... no such file`, `CMAKE_MT-NOTFOUND` | MSVC developer environment not applied — SDK tools missing | Build from "Developer PowerShell for VS 2022", or import `vcvars64.bat` (§4). Then delete `build\` and reconfigure. |
| App/test exits immediately with `0xc0000135` | A required DLL is missing next to the exe (e.g. `raw.dll`) | Rebuild after a clean configure; the per-config libraw fix (§6.1) deploys the correct DLL. Confirm with `dumpbin /dependents build\arraw.exe`. |
| *"no Qt platform plugin could be initialized"* | Qt plugins not deployed | Ensure `platforms\qwindowsd.dll` exists next to the exe; it is copied by the post-build step (§6.2). A fresh `cmake --build build` recreates it. |
| **"Failed to load" for every JPG** (PNG works) | imageformats plugin (`qjpegd.dll`) and/or its codec (`jpeg62.dll`) not deployed | Both are copied by the post-build step (§6.2). Confirm `imageformats\qjpegd.dll` and `jpeg62.dll` sit next to the exe; check `ARRAW_VCPKG_INSTALLED` points at your vcpkg tree. |
| `LNK1168: cannot open arraw.exe for writing` | A running `arraw.exe` is holding the file | Close the running app (or `Stop-Process -Name arraw`) and rebuild. |
| `find_package(Qt6 ...)` fails | Toolchain file not passed, or ports not installed | Use `cmake --preset default`; verify `vcpkg list` shows the Qt ports for `x64-windows`. |
| Shaders not found / `qsb` missing at configure | `qtshadertools` not installed | `vcpkg install qtshadertools --triplet x64-windows`. |
| Golden-image tests show as *Skipped* | They run headless under the offscreen platform | Expected. To run them you need a platform/RHI device capable of the rendering — out of scope for the standard headless test run. |

---

## 9. Quick reference

```powershell
# One-shot: developer env + configure + build + test, in a single session
$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>&1 && set" |
    ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] } }
cmake --preset default
cmake --build build
ctest --test-dir build --output-on-failure
```
