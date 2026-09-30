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
run: configure
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

# Remove the build trees
clean:
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

# Open the dev sandbox: a shell, or an agent (see tools/sandbox/README.md)
[unix]
sandbox *args:
    uv run tools/sandbox/sandbox.py {{args}}

# Build the sandbox image; --refresh fetches the newest base and agents
[unix]
sandbox-build *args:
    uv run tools/sandbox/sandbox.py build {{args}}
