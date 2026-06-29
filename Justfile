set windows-powershell := true

qt_flag := if os() == "macos" { "-DCMAKE_PREFIX_PATH=" + `brew --prefix qt` } else { "" }
clang_format := env_var_or_default("CLANG_FORMAT", "clang-format")
clazy := env_var_or_default("CLAZY", "clazy")
run_clang_tidy := env_var_or_default("RUN_CLANG_TIDY", "run-clang-tidy")
clang_tidy := env_var_or_default("CLANG_TIDY", "clang-tidy")
container_backend := env_var_or_default("CONTAINER_BACKEND", "")

# List available tasks
default:
    @just --list

# Configure and build (Debug)
build:
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug {{qt_flag}}
    ninja -C build

# Build and run the application
run: build
    ./build/arraw

# Build and run the test suite
test: build
    ctest --test-dir build --output-on-failure

# Format all source files with clang-format
format:
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print | xargs {{clang_format}} -i

# Regenerate app icon PNGs and resources/arraw.ico from resources/icon.svg
icons:
    uv run tools/render_icons.py

# Check formatting without modifying files
format-check:
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print | xargs {{clang_format}} --dry-run --Werror

# Run clazy static analysis
clazy:
    cmake -B build-clazy -G Ninja -DCMAKE_BUILD_TYPE=Debug -DARRAW_BUILD_TESTS=OFF -DCMAKE_CXX_COMPILER={{clazy}} {{qt_flag}}
    ninja -C build-clazy -t clean arraw
    ninja -C build-clazy arraw

# Run clang-tidy on sources
tidy: build
    {{run_clang_tidy}} -p build -clang-tidy-binary {{clang_tidy}} '{{justfile_directory()}}/src'

# Configure and build (Release)
release:
    cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release {{qt_flag}}
    ninja -C build-release

# Rebuild without reconfiguring (incremental)
rebuild:
    ninja -C build

# Rebuild and run the application
rerun: rebuild
    ./build/arraw

# Build the development sandbox container image (ADR 0044)
[unix]
sandbox-build:
    uv run tools/sandbox.py build

# Run the dev sandbox (bash shell); pass --claude for the agent, plus --gui, --photos DIR
[unix]
sandbox *args:
    uv run tools/sandbox.py run {{args}}

[windows]
sandbox-build:
    @echo "sandbox is Linux-only (rootless Podman + Wayland); run it on a Linux host."; exit 1

[windows]
sandbox *args:
    @echo "sandbox is Linux-only (rootless Podman + Wayland); run it on a Linux host."; exit 1

# Create an appimage for linux
[unix]
appimage:
    #!/usr/bin/env bash
    set -euo pipefail
    backend="{{container_backend}}"
    if [[ -z "$backend" ]]; then
        backend=$(command -v podman 2>/dev/null || command -v docker 2>/dev/null || true)
    fi
    [[ -n "$backend" ]] || { echo "error: install podman or docker (or set CONTAINER_BACKEND)"; exit 1; }
    mkdir -p dist
    "$backend" build -t arraw-appimage-builder \
        -f packaging/linux/Containerfile packaging/linux
    "$backend" run --rm \
        -v "$PWD:/src:ro,z" \
        -v "$PWD/dist:/dist:z" \
        arraw-appimage-builder \
        bash /src/packaging/linux/build-appimage.sh

# Build Fedora RPM and SRPM from committed HEAD
[unix]
rpm:
    bash tools/package_fedora.sh

# Install and smoke-test the RPM in a clean Fedora 44 container
[unix]
rpm-smoke:
    bash tools/smoke_fedora_rpm.sh

# --- Windows stubs: the recipes above are Linux-only; fail fast with a clear message ---

[windows]
appimage:
    @echo "appimage is a Linux-only task; build it on a Linux host or via CI."; exit 1

[windows]
rpm:
    @echo "rpm is a Linux-only task (Fedora packaging); run it on a Linux host."; exit 1

[windows]
rpm-smoke:
    @echo "rpm-smoke is a Linux-only task (Fedora container); run it on a Linux host."; exit 1

# Create a windows-installer (.exe)
[windows]
windows-installer:
    # Inno setup must be present
    uv run tools/package_windows.py --installer

[unix]
windows-installer:
    @echo "windows-installer is a Windows-only task (builds an .exe); run it on Windows."; exit 1

# Remove all build and distribution artifacts
clean:
    uv run tools/clean.py

# Bump the version in CMake, the RPM spec, and the AppStream metainfo (no git)
bump version:
    uv run tools/bump_version.py {{version}}
