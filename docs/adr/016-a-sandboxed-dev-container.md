# A sandboxed dev container: CI's toolchain, current clone, agents unattended

Coding agents (Claude Code, Codex, pi, opencode) do their best work when they
can build, test and probe the GPU without asking before every command, and
that is only acceptable when something other than the agent bounds what a
command can reach. The VibePod overlay described in `.vibepod/README.md` was
meant to be that boundary, but its Dockerfile never existed. The overlay it
describes also built Qt from source on Debian 12, had no Vulkan, and could not
run git in a worktree. `main` solved the same problem in its own ADR 0044
(Fedora, host parity); this decision starts again from what the rewrite needs,
which is parity with CI.

## Decision

**Our own launcher, not VibePod.** `tools/sandbox/sandbox.py` (run with `uv`,
standard library only) builds the image and starts the container;
`just sandbox` and `just sandbox-build` call it. VibePod's agent images are
Debian-based and its credential model is per agent; neither fits. There is no
`devcontainer.json` yet: nothing here needs an editor inside the container, and
the launcher's work (store, identity, rebuild check) cannot be expressed in one
without a second definition to keep in step.

**Ubuntu 26.04, and CI moves to it too.** 26.04 ships Qt 6.10.2 as ordinary
packages, and as an LTS it stays on the 6.10 series, so the distribution *is*
the Qt pin: no `install-qt-action`, no `aqtinstall`, no source build. One
script, `tools/ci/install-deps.sh`, is the apt list for both CI and the image,
so they cannot drift. Fedora, which matches the development host, was the
alternative; it matches the machine nobody else builds on and drifts from CI
when Fedora rebases Qt. The glibc floor of anything built here is 26.04's,
which matters to packaging, not to this decision.

**clang-format is pinned through uv** (`uvx clang-format@22.1.8` in the
`Justfile`), so the host, the sandbox, CI and Windows format identically.
Different clang-format versions disagree on output, and an agent formatting
with one while the host checks with another produces churn nobody asked for.

**The agents are baked into the image** from their own release channels:
Claude's native binary and Codex's and opencode's release archives into
`/usr/local`, pi through npm on the distribution's Node. Versions are
whatever was newest at build time; `--refresh` rebuilds without the layer
cache to pick up newer ones. Claude's auto-updater is off, since its updates
would land in the discarded home directory and be downloaded again every run.

**Rootless Podman, as the host user.** `--userns=keep-id` runs the container
as the host user's UID, unprivileged inside too, so files written to the clone
stay the user's and a breakout lands as that user, not root, as a rootful
Docker daemon would. On top: `--cap-drop=ALL`, `no-new-privileges`, and memory
(three quarters of RAM) and process limits against a runaway build or agent.

**SELinux stays on.** Mounts use `:z`, which relabels them for container use
and keeps the container confined as `container_t`. `main` used
`label=disable`, to avoid relabelling photo libraries; that removes a whole
layer that would otherwise keep an escaped process out of `~/.ssh` and the
rest of the home directory.

**The current clone at `/workspace`.** `just sandbox` uses the clone containing
the launcher, with no separate checkout or import step. Worktrees are refused:
their Git metadata lives outside the clone mount. The fixed container path
makes the shared ccache useful across clones.

**Writable checkout, explicit trust limit.** The container limits direct host
access during execution, but source files, launch scripts and Git metadata in
the clone remain writable. Protecting only `.git/hooks` and `.git/config` did
not protect their replaceable parent or all of Git's configuration redirections;
those partial mounts are removed. Changes to the launcher, Justfile, build
scripts or Git metadata can execute on the host at the next host command.
Review them before running host commands against the clone again. Git metadata
requires separate inspection because a tracked-file diff does not cover it and
Git itself can execute configured commands.

A separate trusted checkout or installed launcher with isolated Git metadata
would provide a stronger boundary. We choose the simpler single-clone workflow
and accept this delayed host-execution risk; this is not full isolation from a
malicious agent.

**Git is local.** No SSH, no `gh`, no tokens: commits are made inside and pushed
from the host after review. The host's `user.name` and `user.email` are passed in
as the `GIT_AUTHOR_*` and `GIT_COMMITTER_*` variables at every start.

**One store for logins and caches**, `~/.local/share/arraw-sandbox/`, shared
by every clone and readable only by the user. Only named paths persist: the
agents' configuration directories and `~/.cache` (uv, ccache, npm). The rest of
the home directory is thrown away with the container, so nothing can leave a
`.bashrc` behind for the next session. Each agent is logged into once,
interactively, inside the container. Anything an agent can use it can also
read, whether a file in the store or a variable in its environment, so tokens
from the host would be no safer and would add host secrets to manage.

**Agents start unattended by default.** Container restrictions apply while the
agent runs; the writable-checkout trust limit above still applies afterwards.
`--safe` starts agents with their prompts.

**The build tree is `build/container-<preset>`.** The image sets
`ARRAW_BUILD_PREFIX`, which `CMakePresets.json` and the `Justfile` put in
front of the preset name. The host and the container build against different
Qt installs, so a shared tree would be broken by whichever configured last.
`just clean` removes only the current prefix's Debug and Release trees;
`just clean-all` explicitly removes all build trees. This separates ordinary
build operations; host build trees remain accessible through the clone mount.

**Vulkan always, the GPU and the display on request.** Mesa's lavapipe is in
the image, so the headless Vulkan tests run everywhere, CI included.
`QT_QPA_PLATFORM` is not set globally: `arraw-cli` chooses `arraw-headless`
when it is unset, and each test that needs a platform sets its own.
`--gpu` passes `/dev/dri` through (needing the `container_use_devices` SELinux
boolean once), and `--gui` passes the Wayland socket too. They are separate
because the GPU driver and a display connection are separate attack surfaces,
and GPU tests need only the first.

**Photos are mounted as an overlay.** `--photos DIR` appears writable at
`/photos`, because the app will write sidecars next to the files it opens, but
every write lands in a layer of its own (Podman's `:O`): `DIR`, and its SELinux
labels, never change. The layer is discarded on exit, or kept with
`--keep-writes` to inspect what the app wrote. A read-only mount would forbid
sidecars, and a writable one would let an unattended agent delete originals.
Photo directories must not overlap the clone, the credential store, or
overlay output; the launcher checks resolved paths before building an image or
creating mount directories. Mount sources must not have host bind-mount aliases,
and originals must not be hardlinked into writable mounts. Those filesystem
aliases cannot be ruled out by directory containment checks alone.

**Full network.** The agents need their APIs, and builds fetch from PyPI and
GitHub. The repository and the store can therefore be sent anywhere; an
egress allowlist is the fix, and a decision of its own for later.

**The image rebuilds itself when its inputs change.** The launcher labels the
image with a hash of the Containerfile, `install-deps.sh` and
`.containerignore`, rebuilds on a mismatch, and warns once the image is more
than 14 days old.

## Consequences

- `docs/ideas/dev-container-plan.md` and `.vibepod/` are superseded and removed.
- Launch and work in the same clone. Review agent changes before host execution.
- Relabelling with `:z` is permanent on the host side: the clone and the store
  keep their container label afterwards (harmless to the user's own access).
- `--gui` is unverified under SELinux: the Wayland socket belongs to the
  unconfined desktop session, and connecting to it from `container_t` may need
  a further boolean or policy.
- Windows and macOS contributors have no sandbox; the launcher is Linux and
  Podman only, and adding a Docker branch is possible if one is wanted.
