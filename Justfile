set windows-powershell := true

clang_format := env_var_or_default("CLANG_FORMAT", "clang-format")

# List available tasks
default:
    @just --list

# Configure the Debug build tree (build/debug); cheap to re-run
configure:
    cmake --preset debug

# Configure and build everything (Debug)
build: configure
    cmake --build --preset debug

# Build and run the GUI application
run: configure
    cmake --build --preset debug --target arraw-ui
    ./build/debug/arraw-ui

# Build and run the CLI
cli *args: configure
    cmake --build --preset debug --target arraw-cli
    ./build/debug/arraw-cli {{args}}

# Build and run the test suite
test *args: configure
    cmake --build --preset debug --target arraw-tests
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

# Configure and build Release (build/release)
release:
    cmake --preset release
    cmake --build --preset release

# Remove the build trees
clean:
    cmake -E rm -rf build
