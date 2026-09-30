# Dev sandbox

A rootless Podman container with CI's toolchain (Ubuntu 26.04, Qt 6.10) and
the coding agents, for running an agent unattended against one clone. Why it
is built this way: [ADR 016](../../docs/adr/016-a-sandboxed-dev-container.md).

Needs Podman and uv on a Linux host, and a **clone** (a worktree is refused).

```sh
just sandbox                   # a shell in /workspace
just sandbox claude            # Claude Code, permission prompts off
just sandbox codex             # likewise codex, pi, opencode
just sandbox codex --safe      # with its prompts
just sandbox -- just test      # one command, then exit
just sandbox --gpu -- just gpu-info
just sandbox --gui --photos ~/Pictures/raw
just sandbox --photos ~/Pictures/raw --keep-writes ~/sandbox-writes
just sandbox-build --refresh   # rebuild with the newest base and agents
```

`--dry-run` prints the `podman run` command without running it.

## First run

The image builds on first use, and again whenever `Containerfile`,
`tools/ci/install-deps.sh` or `.containerignore` change. Log into each agent
once inside (`/login` in Claude, `codex login --device-auth`, and so on); the
session is kept in `~/.local/share/arraw-sandbox/`, shared by every clone.
Delete a subdirectory there to log out of that agent in the sandbox.

`--gpu` and `--gui` need SELinux to allow devices in containers, once:

```sh
sudo setsebool -P container_use_devices on
```

## Inside

- The clone is at `/workspace`; builds go to `build/container-debug`, so the
  host's `build/debug` is untouched.
- Commit freely; push from the host. `.git/hooks` and `.git/config` are
  read-only, so anything that writes git config fails.
- Everything outside `/workspace` and the store is discarded on exit.
- `--photos DIR` appears at `/photos`, writable, but as an overlay: sidecars,
  edits and deletions go to a layer of their own and `DIR` never changes.
  The layer is discarded on exit; with `--keep-writes OUT` it is kept in
  `OUT/changes` (a deletion shows up there as a `0, 0` character device).
  `OUT/.work` belongs to a namespace UID: remove it with
  `podman unshare rm -rf OUT/.work`.
- `just test-lavapipe` pins the software Vulkan driver when a GPU is passed
  through.

## Limits

Memory is capped at three quarters of the host's RAM and processes at 4096;
set `ARRAW_SANDBOX_MEMORY` (e.g. `16g`) or `ARRAW_SANDBOX_PIDS` to change them.
The network is open.
