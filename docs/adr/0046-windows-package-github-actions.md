# Windows package builds in GitHub Actions

The Windows package is currently verified locally with `tools/package_windows.py`.
That is enough to prove the packaging script works on one developer machine, but
it is not enough for release confidence: the Windows artifact should be built in a
clean, repeatable environment and retained as a CI artifact.

We will add a dedicated GitHub Actions workflow for the Windows package. The
workflow should start manual-only, prove the package build and lensfun DB bundling
path, then graduate into PR/release automation once run time and caching behaviour
are understood.

## Runner: pin Windows Server 2022

Use the hosted `windows-2022` runner, not `windows-latest`.

The project documentation and package script target Visual Studio 2022. Pinning
the image avoids surprise churn when GitHub advances `windows-latest`. The hosted
image already contains the pieces we need: Visual Studio 2022 Enterprise, CMake,
Ninja, Python, vcpkg, GitHub CLI, and Inno Setup.

The packaging script should be called with the runner's Visual Studio path:

```powershell
python tools/package_windows.py `
  --vcvars "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" `
  --toolchain "C:\vcpkg\scripts\buildsystems\vcpkg.cmake" `
  --vcpkg-installed "C:\vcpkg\installed\x64-windows"
```

## Caching: vcpkg binary cache first

The expensive dependency is Qt, but Qt is installed through vcpkg, so the right
unit of caching is vcpkg's binary package cache rather than a hand-rolled Qt
cache.

Use GitHub Actions' vcpkg binary-cache backend:

```yaml
env:
  VCPKG_ROOT: C:\vcpkg
  VCPKG_BINARY_SOURCES: clear;x-gha,readwrite
```

The job needs permission to write the Actions cache:

```yaml
permissions:
  contents: read
  actions: write
```

Also cache `C:\vcpkg\downloads` with `actions/cache` so source archives do not
need to be re-downloaded when a binary cache miss occurs.

Do **not** initially cache `C:\vcpkg\installed`. It is large, coupled to the
runner image and triplet state, and easier to make stale incorrectly. If the
binary cache is still too slow after real workflow runs, we can consider a more
aggressive installed-tree cache keyed by runner image, triplet, vcpkg baseline,
and dependency list.

## Dependencies: install explicitly

The workflow should install the same Windows dependency set documented for
developers:

```powershell
C:\vcpkg\vcpkg.exe install `
  qtbase qttools qtshadertools `
  "libraw[openmp]" lcms lensfun exiv2 `
  --triplet x64-windows
```

This keeps the CI environment aligned with the local Windows build guide. The
`lensfun` port is required: the package build configures with
`ARRAW_WITH_LENSFUN=ON`, and should fail rather than silently ship a package with
lens correction compiled out.

## Verification before packaging

Build and run the focused lensfun tests before creating the package:

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DARRAW_VCPKG_INSTALLED=C:\vcpkg\installed\x64-windows `
  -DARRAW_WITH_LENSFUN=ON

cmake --build build --target arraw_tests

$env:QT_QPA_PLATFORM = "offscreen"
.\build\tests\arraw_tests.exe "[lensfun]"
```

This proves lensfun is compiled in and that the bundled database discovery path
continues to work on Windows.

## Package and sanity-check the artifact

Run the package script after the focused tests pass. Then inspect the staged tree
before uploading anything:

- `arraw.exe` exists;
- `lensfun.dll` exists next to the executable;
- `lensfun\db\*.xml` exists and contains at least one XML file;
- `arraw.exe --version` exits successfully.

The version command should be treated as an exit-code smoke test only. The Windows
binary is built as a GUI-subsystem executable, so CI must not depend on console
output from that command.

Upload the portable ZIP from `dist/` as the initial artifact. The Inno Setup
installer can be added after the ZIP path is stable.

## Rollout

1. Add a manual-only workflow with `workflow_dispatch`.
2. Install dependencies, build focused lensfun tests, package the ZIP, and upload
   the ZIP artifact.
3. Enable vcpkg binary caching and the downloads cache.
4. Add PR triggers for packaging-relevant paths once run time is acceptable.
5. Add installer output with `tools/package_windows.py --installer` if we want CI
   to produce the setup executable.
6. Fold the job into the release workflow after the standalone workflow has
   proven stable.

Suggested PR path filters once automatic PR coverage is enabled:

```yaml
paths:
  - ".github/workflows/windows-package.yml"
  - "CMakeLists.txt"
  - "tools/package_windows.py"
  - "tools/installer/**"
  - "src/pipeline/LensfunSource.*"
  - "tests/test_LensfunSource.cpp"
  - "docs/windows-build.md"
```

## Rejected and deferred

- **Rejected for the first version: caching `C:\vcpkg\installed`.** It may be
  useful later, but vcpkg's binary cache is the cleaner first-line cache.
- **Rejected: `windows-latest`.** Use `windows-2022` until there is a reason to
  move.
- **Deferred: installer artifact.** Start with the portable ZIP; add the setup
  executable after the main package path is stable.
- **Deferred: release integration.** Keep this as a dedicated workflow until it
  has real timing data and reliable cache behaviour.
