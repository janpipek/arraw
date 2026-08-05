# A containerised, autonomous dev sandbox for AI coding agents

We want to run an AI coding agent (Claude Code) **autonomously** against this
repo — letting it build, test, edit and iterate without approving every action —
without granting it that freedom on the host. The container is the safety
boundary: inside it the agent runs with permissions skipped; outside, the blast
radius is one bind-mounted working tree and a throwaway container.

The same image doubles as a **reproducible, host-clean build environment** —
Qt6/LibRaw/lcms2/lensfun and the full lint/analysis toolchain pinned in one
place — so a contributor (human or agent) gets a known-good toolchain without
installing it on their machine, and several sandboxes can run in parallel on
disposable containers.

This is a *development* environment, distinct in purpose from the existing
`packaging/linux/Containerfile`, which deliberately targets **ubuntu:24.04 + Qt
6.8 via aqtinstall** to fix the AppImage's glibc floor (ADR 0014). That image
optimises for *release parity*; this one optimises for *host parity* and
interactive iteration. They are kept separate on purpose.

## Runtime: rootless Podman, not Docker

The host runs **rootless Podman** (Docker is not installed). We target it
directly rather than installing the Docker daemon, and it is also the better fit:
rootless Podman is itself a stronger isolation boundary (no root daemon), and it
maps cleanly onto the access decisions below. The image is defined by a
Dockerfile-compatible `Containerfile`, so nothing here is Podman-locked. The
backend is chosen the same way the `appimage` recipe already does it — honour
`CONTAINER_BACKEND`, else prefer `podman`, else `docker`.

## Base image: Fedora 44, host parity

The base is **Fedora 44**, matching the development host. Dependencies install
from the same `dnf` line documented in the README's build guide, so the agent
builds against the same Qt6/Mesa/LibRaw versions the developer runs natively —
the point of a parity sandbox. The full toolchain is baked in (build deps + Catch2/ctest via
CMake's network fetch, plus `clang-format`, `clazy`, `clang-tidy`, `uv`, `just`,
and node/npm + Claude Code), so every Justfile recipe works inside. GUI runtime
bits (`qtwayland`, Mesa drivers) are included but only matter in GUI mode below.

## Filesystem: one bind mount, identical absolute path, shared build

The working tree is bind-mounted **read-write at the identical absolute path it
occupies on the host**, and the agent is free to manipulate everything in it —
including `build/`, which is **shared** with the host rather than redirected to a
container-only directory.

The identical-path choice is load-bearing: CMake bakes absolute paths into its
cache and the Ninja build graph, so mounting the tree at the same path keeps a
single `build/` valid whether the last `cmake` ran on the host or in the
container — no forced reconfigure when switching between them. The cost is that
host and container must not configure `build/` with *incompatible* toolchains at
the same time; we accept this because both are Fedora 44 with the same compiler,
and a stray reconfigure is cheap. Under enforcing SELinux the container runs with
`--security-opt label=disable` rather than relabelling the mounts (`:Z`):
relabelling recursively rewrites the SELinux context of every bind-mounted file,
which is both invasive to host directories and prohibitively slow for a large
read-only photo library. Disabling the container's SELinux label leaves the
user-namespace and filesystem scoping — the boundary we actually rely on —
intact.

## Identity: map to the host user (`keep-id`)

The agent runs as a **non-root user mapped to the host UID** (`--userns=keep-id`)
rather than as container root or a subuid user. This is chosen for the bind
mount: files the agent writes are owned by the developer on the host (editable
without `chown`), and we avoid the root-only tooling warnings (npm, Claude Code)
that running as container root would trigger. A subuid user was rejected because
it leaves repo files owned by an unusable high UID on the host.

## Access posture: ephemeral auth, skipped permissions, full network

- **Ephemeral authentication.** The agent's config is *not* persisted; each fresh
  container logs in anew. This matches the throwaway/parallel model and keeps the
  sandbox's credentials separate from the host's own login — at the cost of an
  interactive login per run, which we accept.
- **Permissions skipped.** Inside the sandbox the agent launches with permission
  prompts disabled. The container boundary is what makes this safe, and frictionless
  autonomy is the whole reason for the boundary.
- **Full network.** Builds fetch dependencies (Catch2, FetchContent), and an
  autonomous agent needs the API; an egress allow-list is brittle against build
  hosts changing, and offline is incompatible with running the agent at all. The
  filesystem boundary, not the network, is the isolation we rely on.

## GUI is opt-in; headless is the default

The default mode is **headless**: build and `ctest` only (the GPU golden-render
tests stay skipped exactly as they do on a headless host today). This is the safe
default — no display socket or device nodes are exposed.

A GUI mode is available behind an explicit flag for when the agent (or developer)
needs to *see* the viewport render. It uses **Wayland-native** passthrough — the
host is a Wayland session, so we mount the Wayland socket and run Qt with
`QT_QPA_PLATFORM=wayland`, avoiding the looser `xhost` X11 boundary — and exposes
the GPU render nodes (`/dev/dri`), where Mesa selects the integrated Intel GPU
used for the display. The discrete NVIDIA GPU is intentionally **not** wired up
(it would need `nvidia-container-toolkit` and buys nothing for this 2D
viewport). The RAW editor needs real files to render, so the GUI flag also takes
an optional **read-only** photos directory to mount.

## Invocation and cross-IDE use

The long `podman run` line (keep-id, `:Z` relabel, and the conditional
Wayland/`/dev/dri` block) lives in a stdlib **`tools/sandbox.py`** (argparse,
styled like `tools/package_windows.py`), fronted by thin `just sandbox` /
`just sandbox-build` recipes — matching the AGENTS.md rule that non-trivial
recipe logic belongs in `tools/*.py`.

The same image is also described by a **`.devcontainer/devcontainer.json`** so
the Dev Containers standard works across editors (VS Code, JetBrains/PyCharm,
Zed). One image definition (`.devcontainer/Containerfile`) backs both the CLI
launcher and the devcontainer, so they can't drift.

## Rejected and deferred

- **Rejected — Docker:** not installed, and rootless Podman is the stronger,
  host-native boundary.
- **Rejected — separate/volume build dir:** breaks the identical-path CMake-cache
  sharing that lets host and container reuse one `build/`.
- **Rejected — persisted agent auth:** at odds with the ephemeral/throwaway model.
- **Rejected — X11 passthrough:** a Wayland host makes Wayland-native cleaner and
  tighter than sharing the X11 socket.
- **Deferred — NVIDIA passthrough** and a tightened **network egress** policy:
  add only if a concrete need appears.
