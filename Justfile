qt_flag := if os() == "macos" { "-DCMAKE_PREFIX_PATH=" + `brew --prefix qt` } else { "" }
clang_format := env_var_or_default("CLANG_FORMAT", "clang-format")
clazy := env_var_or_default("CLAZY", "clazy")
run_clang_tidy := env_var_or_default("RUN_CLANG_TIDY", "run-clang-tidy")
clang_tidy := env_var_or_default("CLANG_TIDY", "clang-tidy")

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
