qt_flag := if os() == "macos" { "-DCMAKE_PREFIX_PATH=" + `brew --prefix qt` } else { "" }

build:
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug {{qt_flag}}
    ninja -C build

run: build
    ./build/arraw

release:
    cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release {{qt_flag}}
    ninja -C build-release

rebuild:
    ninja -C build

rerun: rebuild
    ./build/arraw
