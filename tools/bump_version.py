#!/usr/bin/env python3
"""Bump the project version across the three declarations the release expects to agree.

The version is single-sourced into the binary from CMakeLists.txt (ADR 0014), but the
release workflow's guard also requires the RPM spec to match the pushed vX.Y.Z tag, and
the AppStream metainfo wants a dated <release> entry per version. This rewrites all three
in lockstep so they can't drift:

    1. CMakeLists.txt          project(arraw VERSION X.Y.Z ...)   -> the canonical source
    2. packaging/fedora/arraw.spec                Version: X.Y.Z
    3. packaging/linux/io.github.janpipek.arraw.metainfo.xml      a new <release ... />

It deliberately stops there. The spec %changelog prose, the git tag, and dispatching the
release workflow stay manual.

Examples:
    uv run tools/bump_version.py 0.2.0
    just bump 0.2.0
"""
from __future__ import annotations

import argparse
import datetime
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CMAKE = REPO / "CMakeLists.txt"
SPEC = REPO / "packaging" / "fedora" / "arraw.spec"
METAINFO = REPO / "packaging" / "linux" / "io.github.janpipek.arraw.metainfo.xml"

VERSION_RE = re.compile(r"^\d+\.\d+\.\d+$")


def current_version() -> str:
    """Read VERSION from the project() line of CMakeLists.txt (the canonical source)."""
    match = re.search(r"project\(arraw\s+VERSION\s+(\d+\.\d+\.\d+)", CMAKE.read_text(encoding="utf-8"))
    if not match:
        sys.exit(f"error: could not read current version from {CMAKE}")
    return match.group(1)


def sub_once(path: Path, pattern: str, repl: str) -> None:
    """Apply a single regex substitution to `path`, failing if it matched nothing."""
    text = path.read_text(encoding="utf-8")
    new_text, count = re.subn(pattern, repl, text, count=1, flags=re.MULTILINE)
    if count != 1:
        sys.exit(f"error: pattern not found in {path}: {pattern}")
    path.write_text(new_text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("version", help="new version, X.Y.Z")
    args = parser.parse_args()

    new = args.version
    if not VERSION_RE.match(new):
        sys.exit(f"error: version must be X.Y.Z (got '{new}')")
    old = current_version()
    if old == new:
        sys.exit(f"error: version is already {new}")

    today = datetime.date.today().isoformat()

    sub_once(CMAKE, r"^(project\(arraw VERSION )\d+\.\d+\.\d+", rf"\g<1>{new}")
    sub_once(SPEC, r"^(Version:\s+)\d+\.\d+\.\d+", rf"\g<1>{new}")
    sub_once(METAINFO, r"( *)(<releases>\n)", rf'\g<1>\g<2>\g<1>  <release version="{new}" date="{today}"/>\n')

    print(f"Bumped {old} -> {new} in CMakeLists.txt, arraw.spec, metainfo.xml")
    print()
    print("Still manual:")
    print("  - add a %changelog entry in packaging/fedora/arraw.spec")
    print(f"  - commit, then: git tag v{new} && git push origin v{new}")
    print(f"  - dispatch .github/workflows/release.yml for tag v{new}")


if __name__ == "__main__":
    main()
