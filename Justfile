qt_flag := if os() == "macos" { "-DCMAKE_PREFIX_PATH=" + `brew --prefix qt` } else { "" }
clang_format := env_var_or_default("CLANG_FORMAT", "clang-format")
clazy := env_var_or_default("CLAZY", "clazy")

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
    cmake -B build-clazy -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER={{clazy}} {{qt_flag}}
    ninja -C build-clazy

release:
    cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release {{qt_flag}}
    ninja -C build-release

rebuild:
    ninja -C build

rerun: rebuild
    ./build/arraw
