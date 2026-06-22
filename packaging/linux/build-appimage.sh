#!/usr/bin/env bash
# Single source of truth for building the arraw Linux AppImage. Both build paths
# call this exact script so the pipeline never has to be maintained twice:
#   - packaging/linux/Containerfile, via `just appimage` (local repro)
#   - .github/workflows/release.yml  (the shipped release)
#
# Each caller only provides an ubuntu:24.04 build environment with Qt 6.8 and the
# apt packages from apt-build-deps.txt; this script does the rest, step-for-step:
#   validate metainfo → cmake Release build → stage AppDir → stage lensfun DB →
#   linuxdeploy bundle → copy out.
#
# Layout is overridable via env so each caller can map its own paths; the defaults
# match the container's mounts (repo read-only at /src, output at /dist):
#   ARRAW_SRC, ARRAW_BUILD_DIR, ARRAW_APPDIR, ARRAW_DIST_DIR
#   QT / QT_ROOT_DIR — the Qt 6.8 prefix (QT wins; QT_ROOT_DIR is install-qt-action's)
set -euo pipefail

src="${ARRAW_SRC:-/src}"
build_dir="${ARRAW_BUILD_DIR:-/build}"
appdir="${ARRAW_APPDIR:-/appdir}"
dist_dir="${ARRAW_DIST_DIR:-/dist}"
qt="${QT:-${QT_ROOT_DIR:?set QT or QT_ROOT_DIR to the Qt 6.8 prefix}}"

ver=$(grep -Po 'project\(arraw VERSION \K[0-9]+\.[0-9]+\.[0-9]+' "$src/CMakeLists.txt")
appimage="arraw-${ver}-x86_64.AppImage"

echo "--- appstreamcli: validate metainfo ---"
appstreamcli validate --no-net \
    "$src/packaging/linux/io.github.janpipek.arraw.metainfo.xml"

echo "--- cmake: configure + build (Release) ---"
cmake -B "$build_dir" -S "$src" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DARRAW_BUILD_TESTS=OFF \
    -DARRAW_WITH_LENSFUN=ON \
    -DCMAKE_PREFIX_PATH="$qt"
ninja -C "$build_dir" arraw

echo "--- cmake: stage AppDir ---"
DESTDIR="$appdir" cmake --install "$build_dir" --prefix /usr

echo "--- lensfun: stage lens database into the AppDir ---"
# linuxdeploy carries liblensfun (+ glib) but not the lens DB data dir, so stage it
# explicitly; arraw loads it relative to the executable (usr/bin → ../share) (#53).
db_src=/usr/share/lensfun/version_1
db_dst="$appdir/usr/share/lensfun/db"
if [[ ! -d "$db_src" ]] || ! compgen -G "$db_src/*.xml" >/dev/null; then
    echo "error: lensfun database not found at $db_src" >&2
    exit 1
fi
mkdir -p "$db_dst"
cp "$db_src"/*.xml "$db_dst/"
echo "staged $(ls "$db_dst"/*.xml | wc -l) lensfun DB files"

echo "--- linuxdeploy: download ---"
base=https://github.com/linuxdeploy
wget -q "$base/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
wget -q "$base/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
chmod +x linuxdeploy*.AppImage

echo "--- linuxdeploy: build AppImage ---"
APPIMAGE_EXTRACT_AND_RUN=1 \
NO_STRIP=1 \
EXTRA_PLATFORM_PLUGINS=libqoffscreen.so \
QMAKE="$qt/bin/qmake" \
OUTPUT="$appimage" \
./linuxdeploy-x86_64.AppImage --appdir "$appdir" --plugin qt --output appimage

if [[ -d "$dist_dir" ]]; then
    cp "$appimage" "$dist_dir/"
    ls -lh "$dist_dir/$appimage"
else
    ls -lh "$appimage"
fi
