#!/usr/bin/env bash
# Runs inside the arraw-appimage-builder container (packaging/linux/Containerfile).
# Mirrors .github/workflows/release.yml step-for-step:
#   validate → cmake Release build → stage AppDir → linuxdeploy bundle → copy out.
#
# Expected mounts:
#   /src   — repo root (read-only)
#   /dist  — output directory on the host (writable)
set -euo pipefail

ver=$(grep -Po 'project\(arraw VERSION \K[0-9]+\.[0-9]+\.[0-9]+' /src/CMakeLists.txt)
appimage="arraw-${ver}-x86_64.AppImage"

echo "--- appstreamcli: validate metainfo ---"
appstreamcli validate --no-net \
    /src/packaging/linux/io.github.janpipek.arraw.metainfo.xml

echo "--- cmake: configure + build (Release) ---"
cmake -B /build -S /src -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DARRAW_BUILD_TESTS=OFF \
    -DCMAKE_PREFIX_PATH="$QT"
ninja -C /build arraw

echo "--- cmake: stage AppDir ---"
DESTDIR=/appdir cmake --install /build --prefix /usr

echo "--- linuxdeploy: download ---"
base=https://github.com/linuxdeploy
wget -q "$base/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
wget -q "$base/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
chmod +x linuxdeploy*.AppImage

echo "--- linuxdeploy: build AppImage ---"
APPIMAGE_EXTRACT_AND_RUN=1 \
NO_STRIP=1 \
EXTRA_PLATFORM_PLUGINS=libqoffscreen.so \
QMAKE="$QT/bin/qmake" \
OUTPUT="$appimage" \
./linuxdeploy-x86_64.AppImage --appdir /appdir --plugin qt --output appimage

cp "$appimage" /dist/
ls -lh "/dist/$appimage"
