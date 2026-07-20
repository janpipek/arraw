#!/usr/bin/env python3
"""Bundle the lensfun database next to a local Windows dev build.

Windows has no system-wide lensfun database path (see
LensfunSource.cpp::bundledLensfunDbDir), so a plain `just run`/`just build`
links liblensfun but never finds a database to match lenses against - every
image shows "No lens profile" regardless of its EXIF. vcpkg's lensfun port
doesn't install the database either (only headers/lib); it exists, already
extracted, under the vcpkg buildtrees. This copies it to <build-dir>/lensfun/db,
the same "beside the exe" layout bundledLensfunDbDir() already looks for, and
the one tools/package_windows.py produces for release packages.

Run from a normal shell:
    python tools/dev_lensfun_db.py
    python tools/dev_lensfun_db.py --build-dir build-release
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_windows import DEFAULT_TOOLCHAIN, DEFAULT_VCPKG_INSTALLED, copy_lensfun_db  # noqa: E402

REPO = Path(__file__).resolve().parent.parent


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default="build", help="build directory the exe lives in")
    parser.add_argument("--toolchain", default=DEFAULT_TOOLCHAIN, help="vcpkg CMake toolchain file")
    parser.add_argument("--vcpkg-installed", default=DEFAULT_VCPKG_INSTALLED, help="vcpkg installed tree")
    parser.add_argument("--lensfun-db-dir", help="directory containing lensfun database XML files")
    args = parser.parse_args()

    stage = REPO / args.build_dir
    if not stage.is_dir():
        sys.exit(f"{stage} does not exist - run `just configure` first.")
    copy_lensfun_db(
        stage,
        Path(args.vcpkg_installed),
        Path(args.toolchain),
        Path(args.lensfun_db_dir) if args.lensfun_db_dir else None,
    )


if __name__ == "__main__":
    main()
