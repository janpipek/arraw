#!/usr/bin/env python3
"""Remove all build and distribution artifacts.

Deletes the CMake build trees (build/, build-release/, build-clazy/) and the
packaging output (dist/) — everything produced by `just build`, `just release`,
`just clazy`, `just appimage`, `just rpm`, and `just windows-installer`. Missing
directories are skipped, so it is safe to run on a clean checkout, and it works
the same on Linux, macOS, and Windows (PowerShell has no `rm -rf` equivalent).

Run from a normal shell:
    python tools/clean.py
"""
from __future__ import annotations

import shutil
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Everything in .gitignore that we generate; kept in sync with the build recipes.
ARTIFACTS = ("build", "build-release", "build-clazy", "dist")


def main() -> None:
    for name in ARTIFACTS:
        target = REPO / name
        if target.is_dir():
            shutil.rmtree(target)
            print(f"removed {name}/")
        else:
            print(f"skipped {name}/ (absent)")


if __name__ == "__main__":
    main()
