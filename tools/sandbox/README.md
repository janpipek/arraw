# Dev sandbox

A rootless Podman container with CI's toolchain (Ubuntu 26.04, Qt 6.10) and
the coding agents, working directly in this clone. Why it is built this way:
[ADR 016](../../docs/adr/016-a-sandboxed-dev-container.md).

Needs Podman and uv on a Linux host and an ordinary **clone** (worktrees are
refused). Run these commands from the clone:

```sh
just sandbox                   # a shell in /workspace
just sandbox claude            # permission prompts off
just sandbox codex             # likewise pi, opencode
just sandbox codex --safe      # with its prompts
just sandbox -- just test
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

- This clone is at `/workspace`; edits and commits appear on the host directly.
  Builds go to `build/container-debug`.
  `just clean` removes only Debug and Release trees for the current prefix;
  `just clean-all` explicitly removes every build tree. This separates normal
  build operations, but the container can still write to host build trees.
- Git metadata is writable. Commit inside the container; review changes before
  using the clone on the host again, then push from the host.
- Everything outside `/workspace` and the store is discarded on exit.
- `--photos DIR` appears at `/photos`, writable, but as an overlay: sidecars,
  edits and deletions go to a layer of their own. The launcher rejects photo
  directories that overlap the clone, the store, or overlay output, so
  the originals are not also exposed through a writable directory mount.
  Do not provide originals with hardlink or host bind-mount aliases in writable
  paths; path validation cannot establish isolation for such aliases.
  The layer is discarded on exit; with `--keep-writes OUT` it is kept in
  `OUT/changes` (a deletion shows up there as a `0, 0` character device).
  `OUT/.work` belongs to a namespace UID: remove it with
  `podman unshare rm -rf OUT/.work`.
- `just test-lavapipe` pins the software Vulkan driver when a GPU is passed
  through.

## Trust boundary

The container limits direct access to the host while commands run inside it.
It does not make changes in this writable clone safe to execute later on the
host. An agent can modify the Justfile, launcher, build scripts and Git metadata;
a subsequent host launch or Git command can execute those changes.

Review changed scripts and Git metadata before running host commands against
the clone again, including `just sandbox`. Git metadata is not covered by the
normal tracked-file diff, and Git itself can execute configured commands;
inspect suspect metadata with a plain file viewer. This is a development
workflow, not a complete boundary against a malicious agent. There is no
separate checkout or bundle-import step.

## Launcher checks

`just sandbox-check` runs standard-library tests for photo paths, symlinks,
clone validation and command construction; it does not start Podman. These checks do
not replace testing the actual mount behavior on the target host.

## Limits

Memory is capped at three quarters of the host's RAM and processes at 4096;
set `ARRAW_SANDBOX_MEMORY` (e.g. `16g`) or `ARRAW_SANDBOX_PIDS` to change them.
The network is open. Avoid changing mount source paths while a session starts.
