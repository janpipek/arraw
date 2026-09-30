set windows-powershell := true

# The one clang-format everyone formats with: host, sandbox, CI and Windows
clang_format := env_var_or_default("CLANG_FORMAT", "uvx clang-format@22.1.8")

# Build trees live in build/<prefix><preset>; the sandbox sets the prefix, so
# its tree never collides with the host's (see CMakePresets.json)
build_dir := "build/" + env_var_or_default("ARRAW_BUILD_PREFIX", "")

# List available tasks
default:
    @just --list

# Configure the Debug build tree (build/[prefix]debug); cheap to re-run
configure:
    cmake --preset debug

# Configure and build everything (Debug)
build: configure
    cmake --build --preset debug

# Build and run the GUI application
gui: configure
    cmake --build --preset debug --target arraw-ui
    ./{{build_dir}}debug/arraw-ui

# Build and run the CLI
cli *args: configure
    cmake --build --preset debug --target arraw-cli
    ./{{build_dir}}debug/arraw-cli {{args}}

# Build and run the test suite
test *args: configure
    cmake --build --preset debug --target arraw-test-binaries
    ctest --preset debug {{args}}

# Build the Python extension in its own tree (build/[prefix]py-debug) and run pytest against it.
# Needs the uv-managed .venv; `--no-install-project` uninstalls an editable arraw that a plain `uv sync` installed.
[unix]
py-test *args:
    uv sync --no-install-project
    cmake --preset py-debug -DPython_EXECUTABLE="$(pwd)/.venv/bin/python"
    cmake --build --preset py-debug --target _arraw
    PYTHONPATH="$(pwd)/{{build_dir}}py-debug/python" uv run --no-sync pytest tests/python {{args}}

# Regenerate the committed test fixtures (see tests/fixtures/README.md)
fixtures:
    uv run tests/fixtures/make_fixtures.py
    uv run tests/fixtures/make_raw_fixtures.py

# Format C++ source/header files
[unix]
format:
    find src include tests \( -name '*.cpp' -o -name '*.h' \) -print | xargs {{clang_format}} -i

[windows]
format:
    & {{clang_format}} -i @(Get-ChildItem src,include,tests -Recurse -File -Include *.cpp,*.h | ForEach-Object FullName)

# Check C++ formatting without modifying files
[unix]
format-check:
    find src include tests \( -name '*.cpp' -o -name '*.h' \) -print | xargs {{clang_format}} --dry-run --Werror

[windows]
format-check:
    & {{clang_format}} --dry-run --Werror @(Get-ChildItem src,include,tests -Recurse -File -Include *.cpp,*.h | ForEach-Object FullName)

# Configure and build Release (build/[prefix]release)
release:
    cmake --preset release
    cmake --build --preset release

# Remove Debug and Release build trees for the current environment
clean:
    cmake -E rm -rf "{{build_dir}}debug" "{{build_dir}}release"

# Remove every environment's build trees
clean-all:
    cmake -E rm -rf build

# Show which Vulkan devices this session has, and whether arraw can use one
gpu-info: configure
    vulkaninfo --summary
    cmake --build --preset debug --target arraw-cli
    ./{{build_dir}}debug/arraw-cli gpu-test --allow-software

# Run the test suite on lavapipe alone, independent of the host's GPU
[unix]
test-lavapipe *args:
    VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json just test {{args}}

# Open the dev sandbox in this checkout: a shell, or an agent
[unix]
sandbox *args:
    uv run tools/sandbox/sandbox.py {{args}}

# Build the sandbox image; --refresh fetches the newest base and agents
[unix]
sandbox-build *args:
    uv run tools/sandbox/sandbox.py build {{args}}

# Check sandbox mount boundaries without starting a container
[unix]
sandbox-check:
    uv run --script tools/sandbox/test_sandbox.py
