qt_prefix := `brew --prefix qt`

build:
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_PREFIX_PATH={{qt_prefix}}
    ninja -C build

run: build
    ./build/arraw

release:
    cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH={{qt_prefix}}
    ninja -C build-release

rebuild:
    ninja -C build

rerun: rebuild
    ./build/arraw
