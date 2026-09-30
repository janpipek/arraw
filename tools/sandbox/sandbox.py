#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# ///
"""Build and run the arraw dev sandbox (ADR 016).

The current checkout, mounted read-write at /workspace in a rootless Podman
container, with CI's toolchain and the coding agents. Agents start with their
permission prompts off unless --safe is given. Changes to scripts and Git
metadata must be reviewed before running host commands against the checkout.
Agent logins and caches persist in one store shared by every checkout;
the rest of the container home is discarded on exit.

Examples:
    tools/sandbox/sandbox.py                     # a shell
    tools/sandbox/sandbox.py claude              # unattended
    tools/sandbox/sandbox.py codex --safe
    tools/sandbox/sandbox.py --safe claude -- --resume   # arguments for the agent
    tools/sandbox/sandbox.py --gpu -- just test
    tools/sandbox/sandbox.py --gui --photos ~/Pictures/raw
    tools/sandbox/sandbox.py build --refresh     # rebuild with the newest agents
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CONTAINERFILE = REPO / "tools" / "sandbox" / "Containerfile"
# What the image is built from: a change to any of these rebuilds it.
IMAGE_INPUTS = [CONTAINERFILE, REPO / "tools" / "ci" / "install-deps.sh", REPO / ".containerignore"]
IMAGE = "localhost/arraw-sandbox"
HASH_LABEL = "arraw.sandbox.inputs"
STALE_AFTER_DAYS = 14

WORKSPACE = "/workspace"
HOME = "/home/dev"
# Store directory -> where it appears in the container. Only these persist.
STORE_MOUNTS = {
    "claude": f"{HOME}/.claude",
    "codex": f"{HOME}/.codex",
    "pi": f"{HOME}/.pi",
    "opencode-config": f"{HOME}/.config/opencode",
    "opencode-data": f"{HOME}/.local/share/opencode",
    "cache": f"{HOME}/.cache",
}

# Each agent, started unattended and with --safe.
AGENTS = {
    "claude": (["claude", "--dangerously-skip-permissions"], ["claude"]),
    "codex": (["codex", "--dangerously-bypass-approvals-and-sandbox"], ["codex"]),
    # pi has no permission prompts to skip.
    "pi": (["pi"], ["pi"]),
    "opencode": (["opencode"], ["opencode"]),
}


def fail(message: str) -> None:
    sys.exit(f"sandbox: {message}")


def podman() -> str:
    """Return the Podman executable, the only engine the sandbox supports."""
    path = shutil.which("podman")
    if not path:
        fail("podman not found; the sandbox needs rootless Podman")
    return path


def inputs_hash() -> str:
    """Hash the files the image is built from, to tell when it is out of date."""
    digest = hashlib.sha256()
    for path in IMAGE_INPUTS:
        digest.update(path.name.encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()[:16]


def image_state(cli: str) -> tuple[str, datetime] | None:
    """Return the built image's input hash and creation time, or None if there is none."""
    result = subprocess.run(
        [cli, "image", "inspect", "--format", f'{{{{index .Labels "{HASH_LABEL}"}}}}|{{{{.Created.Unix}}}}', IMAGE],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    label, created = result.stdout.strip().split("|")
    return label, datetime.fromtimestamp(int(created), timezone.utc)


def build(cli: str, refresh: bool) -> None:
    """Build the image; with refresh, pull the base and fetch the newest agents."""
    cmd = [cli, "build", "-t", IMAGE, "-f", str(CONTAINERFILE), "--label", f"{HASH_LABEL}={inputs_hash()}"]
    if refresh:
        cmd += ["--pull=always", "--no-cache"]
    cmd.append(str(REPO))
    print("+", " ".join(cmd), file=sys.stderr)
    # Keep build logs separate from the command's redirected output.
    if subprocess.run(cmd, stdout=sys.stderr).returncode != 0:
        fail("image build failed")


def ensure_image(cli: str) -> None:
    """Build the image when it is missing or its inputs changed; warn when it is old."""
    state = image_state(cli)
    if state is None:
        print("sandbox: no image yet, building it", file=sys.stderr)
        build(cli, refresh=False)
        return
    label, created = state
    if label != inputs_hash():
        print("sandbox: image inputs changed, rebuilding", file=sys.stderr)
        build(cli, refresh=False)
        return
    age = (datetime.now(timezone.utc) - created).days
    if age > STALE_AFTER_DAYS:
        print(f"sandbox: image is {age} days old; `just sandbox-build --refresh` updates the agents", file=sys.stderr)


def store_root() -> Path:
    """Return the host directory the agents' logins and caches persist in."""
    data = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share")
    return data / "arraw-sandbox"


def overlaps(first: Path, second: Path) -> bool:
    """Check whether either resolved directory contains the other."""
    return first.is_relative_to(second) or second.is_relative_to(first)


def host_path(value: str | Path) -> Path:
    """Resolve a host path and reject Podman's volume-option delimiters."""
    path = Path(value).expanduser().resolve()
    if any(character in str(path) for character in (":", ",", "\n", "\r")):
        fail(f"unsupported character in mount path: {path}")
    return path


def validate_paths(photos_dir: str | None, keep_writes: str | None) -> tuple[Path, Path]:
    """Validate the mount boundary before creating directories or invoking Podman."""
    workspace = host_path(REPO)
    git_dir = workspace / ".git"
    if git_dir.is_symlink() or not git_dir.is_dir() or (git_dir / "commondir").exists():
        fail(f"{workspace} is not a standalone clone; use a clone, not a worktree")
    store = host_path(store_root())
    if overlaps(store, workspace):
        fail("the sandbox store must be outside the checkout")

    # Resolve the actual bind sources too: an existing store entry could be a
    # symlink even when the store root is outside the checkout.
    writable = [workspace, store]
    for name in STORE_MOUNTS:
        source = host_path(store / name)
        if not source.is_relative_to(store) or source == store:
            fail(f"store entry escapes its store directory: {store / name}")

    if keep_writes and not photos_dir:
        fail("--keep-writes needs --photos")
    if keep_writes:
        keep = host_path(keep_writes)
        if any(overlaps(keep, path) for path in writable):
            fail("--keep-writes must be outside the checkout and store")
        for name in ("changes", ".work"):
            source = host_path(keep / name)
            if source != keep / name:
                fail(f"overlay directory must not be a symlink: {keep / name}")
        writable.append(keep)

    if photos_dir:
        photos = host_path(photos_dir)
        if not photos.is_dir():
            fail(f"--photos: not a directory: {photos}")
        if any(overlaps(photos, path) for path in writable):
            fail("--photos must not overlap the checkout, store or --keep-writes directory")
    return workspace, store


def prepare_store(root: Path) -> None:
    """Create the store, readable by the host user alone."""
    root.mkdir(parents=True, exist_ok=True)
    root.chmod(0o700)
    for name in STORE_MOUNTS:
        (root / name).mkdir(exist_ok=True)


def git_identity() -> list[str]:
    """Return env arguments carrying the host's git identity for commits made inside."""
    args = []
    for key, variables in (("user.name", ("GIT_AUTHOR_NAME", "GIT_COMMITTER_NAME")),
                           ("user.email", ("GIT_AUTHOR_EMAIL", "GIT_COMMITTER_EMAIL"))):
        result = subprocess.run(["git", "-C", str(REPO), "config", key], capture_output=True, text=True)
        value = result.stdout.strip()
        if value:
            for variable in variables:
                args += ["-e", f"{variable}={value}"]
    return args


def memory_limit() -> str:
    """Return the memory limit: ARRAW_SANDBOX_MEMORY, else three quarters of the host's RAM."""
    if configured := os.environ.get("ARRAW_SANDBOX_MEMORY"):
        return configured
    with open("/proc/meminfo") as meminfo:
        for line in meminfo:
            if line.startswith("MemTotal:"):
                return f"{int(line.split()[1]) * 3 // 4}k"
    fail("cannot read MemTotal from /proc/meminfo; set ARRAW_SANDBOX_MEMORY")


def selinux_enforcing() -> bool:
    try:
        return Path("/sys/fs/selinux/enforce").read_text().strip() == "1"
    except OSError:
        return False


def gpu_args() -> list[str]:
    """Pass the GPU render nodes through, for Vulkan on real hardware."""
    if not Path("/dev/dri").is_dir():
        fail("--gpu: this host has no /dev/dri")
    if selinux_enforcing():
        state = subprocess.run(["getsebool", "container_use_devices"], capture_output=True, text=True).stdout
        if not state.strip().endswith("on"):
            fail("--gpu: SELinux keeps containers off devices; allow it once with\n"
                 "    sudo setsebool -P container_use_devices on")
    # keep-groups keeps the host user's render/video groups, which own the nodes.
    return ["--device", "/dev/dri", "--group-add", "keep-groups"]


def gui_args() -> list[str]:
    """Pass the host's Wayland socket through, so Qt windows open on the desktop."""
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    display = os.environ.get("WAYLAND_DISPLAY")
    if not runtime or not display:
        fail("--gui needs a Wayland session (XDG_RUNTIME_DIR and WAYLAND_DISPLAY)")
    socket = Path(display) if Path(display).is_absolute() else Path(runtime) / display
    if not socket.exists():
        fail(f"--gui: no Wayland socket at {socket}")
    inside = "/tmp/runtime"
    return [
        "-v", f"{socket}:{inside}/wayland-0",
        "-e", f"XDG_RUNTIME_DIR={inside}",
        "-e", "WAYLAND_DISPLAY=wayland-0",
        "-e", "QT_QPA_PLATFORM=wayland",
    ]


def photos_args(photos_dir: str, keep_writes: str | None) -> list[str]:
    """Mount a photo folder as an overlay: writable inside, the originals never touched.

    Writes (sidecars, deletions) go to a layer of their own, discarded on exit or,
    with keep_writes, kept in keep_writes/changes. Neither the folder nor its
    SELinux labels change.
    """
    photos = host_path(photos_dir)
    options = "O"
    if keep_writes:
        keep = host_path(keep_writes)
        # The kernel wants the work directory beside the kept layer, on the same
        # filesystem; it ends up owned by a namespace UID (`podman unshare rm` it).
        (keep / "changes").mkdir(parents=True, exist_ok=True)
        (keep / ".work").mkdir(exist_ok=True)
        options += f",upperdir={keep / 'changes'},workdir={keep / '.work'}"
    return ["-v", f"{photos}:/photos:{options}"]


def run(cli: str, args: argparse.Namespace) -> int:
    workspace, store = validate_paths(args.photos, args.keep_writes)
    if not args.dry_run:
        ensure_image(cli)
    prepare_store(store)

    cmd = [
        cli, "run", "--rm", "--init",
        "--hostname", "arraw-sandbox",
        # Run as the host user, unprivileged inside too, so files stay theirs.
        "--userns=keep-id",
        "--cap-drop=ALL",
        "--security-opt", "no-new-privileges",
        f"--memory={memory_limit()}",
        f"--pids-limit={os.environ.get('ARRAW_SANDBOX_PIDS', '4096')}",
        # :z labels the mounts for containers but keeps SELinux confining this one.
        # Source, launch scripts and Git metadata are writable; review changes
        # before running host commands against this checkout again.
        "-v", f"{workspace}:{WORKSPACE}:z",
        "-w", WORKSPACE,
    ]
    if sys.stdin.isatty():
        cmd.append("-i")
        if sys.stdout.isatty():
            cmd.append("-t")
    for name, target in STORE_MOUNTS.items():
        cmd += ["-v", f"{store / name}:{target}:z"]
    cmd += git_identity()
    if args.gpu or args.gui:
        cmd += gpu_args()
    if args.gui:
        cmd += gui_args()
    if args.photos:
        cmd += photos_args(args.photos, args.keep_writes)

    cmd.append(IMAGE)
    if args.agent:
        unattended, safe = AGENTS[args.agent]
        cmd += (safe if args.safe else unattended) + args.command
    elif args.command:
        cmd += args.command
    else:
        cmd.append("bash")

    print("+", " ".join(cmd), file=sys.stderr)
    if args.dry_run:
        return 0
    return subprocess.run(cmd).returncode


def main() -> int:
    argv = sys.argv[1:]
    if argv[:1] == ["build"]:
        parser = argparse.ArgumentParser(prog="sandbox.py build", description="Build the sandbox image.")
        parser.add_argument("--refresh", action="store_true",
                            help="pull the base image and fetch the newest agents (no layer cache)")
        build(podman(), parser.parse_args(argv[1:]).refresh)
        return 0

    command = []
    if "--" in argv:
        split = argv.index("--")
        argv, command = argv[:split], argv[split + 1:]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("agent", nargs="?", choices=sorted(AGENTS), help="agent to start (default: a shell)")
    parser.add_argument("--safe", action="store_true", help="start the agent with its permission prompts")
    parser.add_argument("--gpu", action="store_true", help="pass the host GPU through (/dev/dri)")
    parser.add_argument("--gui", action="store_true", help="pass the Wayland socket through; implies --gpu")
    parser.add_argument("--photos", metavar="DIR",
                        help="mount DIR at /photos as an overlay: writable inside, DIR itself never changes")
    parser.add_argument("--keep-writes", metavar="DIR",
                        help="keep what was written to /photos in DIR/changes instead of discarding it")
    parser.add_argument("--dry-run", action="store_true", help="print the podman command, run nothing")
    args = parser.parse_args(argv)
    # After --: the agent's arguments, or without an agent, the command to run.
    args.command = command
    return run(podman(), args)


if __name__ == "__main__":
    sys.exit(main())
