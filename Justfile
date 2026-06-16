qt_flag := if os() == "macos" { "-DCMAKE_PREFIX_PATH=" + `brew --prefix qt` } else { "" }
clang_format := env_var_or_default("CLANG_FORMAT", "clang-format")
clazy := env_var_or_default("CLAZY", "clazy")
run_clang_tidy := env_var_or_default("RUN_CLANG_TIDY", "run-clang-tidy")
clang_tidy := env_var_or_default("CLANG_TIDY", "clang-tidy")

build:
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug {{qt_flag}}
    ninja -C build

run: build
    ./build/arraw

test: build
    ctest --test-dir build --output-on-failure

format:
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print | xargs {{clang_format}} -i

format-check:
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print | xargs {{clang_format}} --dry-run --Werror

clazy:
    cmake -B build-clazy -G Ninja -DCMAKE_BUILD_TYPE=Debug -DARRAW_BUILD_TESTS=OFF -DCMAKE_CXX_COMPILER={{clazy}} {{qt_flag}}
    ninja -C build-clazy -t clean arraw
    ninja -C build-clazy arraw

tidy: build
    {{run_clang_tidy}} -p build -clang-tidy-binary {{clang_tidy}} '{{justfile_directory()}}/src'

release:
    cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release {{qt_flag}}
    ninja -C build-release

rebuild:
    ninja -C build

rerun: rebuild
    ./build/arraw
