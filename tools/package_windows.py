#!/usr/bin/env python3
"""Build arraw in Release and package the runnable app into a distributable ZIP.

Configures a Release build with the vcpkg toolchain, builds the app (tests off),
then collects the executable, its runtime DLLs and the Qt plugin folders
(platforms/, imageformats/) that the build's post-build deploy step produced into a
clean staging folder, and compresses it to dist/arraw-<version>-windows-x64.zip.

Run from a normal shell; the script imports the MSVC developer environment itself
(so rc.exe/mt.exe are on PATH). See docs/windows-build.md for the toolchain details.

Examples:
    python tools/package_windows.py
    python tools/package_windows.py --skip-build   # zip an existing build-release
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

DEFAULT_VCVARS = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
DEFAULT_TOOLCHAIN = "C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake"
DEFAULT_VCPKG_INSTALLED = "C:/dev/vcpkg/installed/x64-windows"

# CRT + OpenMP runtime, bundled app-local so the package needs no system VC++ redist.
RUNTIME_DLLS = ("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "vcomp140.dll")


def copy_runtime_dlls(env: dict[str, str], stage: Path) -> None:
    """Copy the MSVC/OpenMP runtime DLLs from the VS redist tree into `stage`."""
    redist_root = env.get("VCToolsRedistDir")
    if not redist_root:
        sys.exit("VCToolsRedistDir not set; run from the MSVC env (vcvars64.bat) to locate the CRT runtime.")
    redist_x64 = Path(redist_root) / "x64"
    found: dict[str, Path] = {}
    for pattern in ("Microsoft.VC*.CRT/*.dll", "Microsoft.VC*.OpenMP/*.dll"):
        for dll in redist_x64.glob(pattern):
            if dll.name.lower() in RUNTIME_DLLS:
                found[dll.name.lower()] = dll
    missing = [name for name in RUNTIME_DLLS if name not in found]
    if missing:
        sys.exit(f"Runtime DLLs not found under {redist_x64}: {missing}")
    for dll in found.values():
        shutil.copy2(dll, stage)


def msvc_env(vcvars: Path) -> dict[str, str]:
    """Return the environment after sourcing vcvars64.bat (rc.exe/mt.exe, INCLUDE/LIB)."""
    if not vcvars.is_file():
        sys.exit(f"vcvars64.bat not found at: {vcvars} (pass --vcvars)")
    # Run vcvars in cmd (banner to nul), then dump the resulting environment with
    # `set`. shell=True so cmd interprets the redirection and && chaining.
    result = subprocess.run(
        f'"{vcvars}" >nul 2>&1 && set',
        capture_output=True,
        text=True,
        errors="replace",
        shell=True,
    )
    env = {}
    for line in result.stdout.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            env[key] = value
    if "INCLUDE" not in env:
        sys.exit("Failed to import the MSVC environment from vcvars64.bat")
    return env


def project_version() -> str:
    """Read VERSION from the project() line of CMakeLists.txt."""
    text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(arraw\s+VERSION\s+(\d+\.\d+\.\d+)", text)
    return match.group(1) if match else "0.0.0"


def run(cmd: list[str], env: dict[str, str] | None = None) -> None:
    print("==>", " ".join(cmd))
    subprocess.run(cmd, env=env, check=True)


def stage_app(build: Path, stage: Path, env: dict[str, str]) -> None:
    """Stage the runnable app: exe + build runtime DLLs + Qt plugin dirs + CRT runtime."""
    exe = build / "arraw.exe"
    if not exe.is_file():
        sys.exit(f"arraw.exe not found in {build} - build first (omit --skip-build).")
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    shutil.copy2(exe, stage)
    for dll in build.glob("*.dll"):
        shutil.copy2(dll, stage)
    for plugin_dir in ("platforms", "imageformats"):
        src = build / plugin_dir
        if src.is_dir():
            shutil.copytree(src, stage / plugin_dir, ignore=shutil.ignore_patterns("*.pdb"))
    copy_runtime_dlls(env, stage)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default="build-release", help="Release build directory")
    parser.add_argument("--out-dir", default="dist", help="output directory for the ZIP")
    parser.add_argument("--vcvars", default=DEFAULT_VCVARS, help="path to vcvars64.bat")
    parser.add_argument("--toolchain", default=DEFAULT_TOOLCHAIN, help="vcpkg CMake toolchain file")
    parser.add_argument("--vcpkg-installed", default=DEFAULT_VCPKG_INSTALLED, help="vcpkg installed tree")
    parser.add_argument("--skip-build", action="store_true", help="package an existing build instead of rebuilding")
    args = parser.parse_args()

    build = REPO / args.build_dir
    stage_name = f"arraw-{project_version()}-windows-x64"

    env = msvc_env(Path(args.vcvars))
    if not args.skip_build:
        run(
            [
                "cmake", "-S", str(REPO), "-B", str(build), "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Release",
                f"-DCMAKE_TOOLCHAIN_FILE={args.toolchain}",
                "-DVCPKG_TARGET_TRIPLET=x64-windows",
                f"-DARRAW_VCPKG_INSTALLED={args.vcpkg_installed}",
                "-DARRAW_BUILD_TESTS=OFF",
            ],
            env=env,
        )
        run(["cmake", "--build", str(build)], env=env)

    stage = build / "_package" / stage_name
    stage_app(build, stage, env)

    # Compress to dist/<stage_name>.zip, keeping the top-level folder inside the archive.
    out = REPO / args.out_dir
    out.mkdir(parents=True, exist_ok=True)
    zip_base = out / stage_name
    archive = shutil.make_archive(str(zip_base), "zip", root_dir=stage.parent, base_dir=stage_name)

    size_mb = round(Path(archive).stat().st_size / (1024 * 1024), 1)
    file_count = sum(1 for _ in stage.rglob("*") if _.is_file())
    print(f"==> Created {archive} ({size_mb} MB, {file_count} files)")


if __name__ == "__main__":
    main()
