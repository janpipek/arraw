#!/usr/bin/env bash

set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

# Everything comes from committed HEAD, as the source archive does, so untracked
# files (docs/reviews/) and uncommitted edits cannot make the build disagree.
spec=packaging/fedora/arraw.spec
version=$(git show HEAD:CMakeLists.txt \
    | sed -nE 's/^project\(arraw VERSION ([0-9]+\.[0-9]+\.[0-9]+).*$/\1/p')
spec_version=$(git show "HEAD:$spec" \
    | sed -nE 's/^%global[[:space:]]+base_version[[:space:]]+([^[:space:]]+).*$/\1/p')

if [[ -z "$version" || "$version" != "$spec_version" ]]; then
    echo "error: CMake version '$version' and RPM version '$spec_version' disagree" >&2
    exit 1
fi

if ! git diff --quiet HEAD -- . ; then
    echo "warning: tracked files differ from HEAD; the package is built from HEAD" >&2
fi

required_packages=(
    appstream
    catch-devel
    cmake
    desktop-file-utils
    gcc-c++
    exiv2-devel
    LibRaw-devel
    ninja-build
    qt6-qtbase-devel
    qt6-qtbase-private-devel
    qt6-qtimageformats
    qt6-qtshadertools-devel
    redhat-rpm-config
    rpm-build
    rpmlint
    vulkan-headers
)
missing_packages=()
for package in "${required_packages[@]}"; do
    rpm -q "$package" >/dev/null 2>&1 || missing_packages+=("$package")
done
if ((${#missing_packages[@]})); then
    echo "error: missing Fedora packaging dependencies" >&2
    printf '  %s\n' "${missing_packages[@]}" >&2
    printf 'install them with:\n  sudo dnf install' >&2
    printf ' %q' "${missing_packages[@]}" >&2
    printf '\n' >&2
    exit 1
fi

commit=$(git rev-parse --short=12 HEAD)
source_date=$(git show -s --format=%cd --date=format:%Y%m%d HEAD)
release_args=()
if ! git tag --points-at HEAD | grep -qx "v${version}"; then
    # Caret version: sorts above the release it follows, by date, then by SHA.
    release_args=(--define "snapshot ${source_date}git${commit}")
fi

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/arraw-rpm.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT
top_dir="$work_dir/rpmbuild"
mkdir -p "$top_dir"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS,TMP}

source_archive="$top_dir/SOURCES/arraw-${version}.tar.gz"
git archive --format=tar --prefix="arraw-${version}/" HEAD \
    | gzip -n >"$source_archive"
git show "HEAD:$spec" >"$top_dir/SPECS/arraw.spec"

rpmbuild -ba "$top_dir/SPECS/arraw.spec" \
    --define "_topdir $top_dir" \
    --define "_tmppath $top_dir/TMP" \
    "${release_args[@]}"

output_dir="$repo_root/dist/fedora"
mkdir -p "$output_dir"
rm -f "$output_dir"/*.rpm
mapfile -t built_packages < <(find "$top_dir/RPMS" "$top_dir/SRPMS" -type f -name '*.rpm' -print | sort)
if ((${#built_packages[@]} == 0)); then
    echo "error: rpmbuild produced no packages" >&2
    exit 1
fi
for package in "${built_packages[@]}"; do
    install -m 0644 "$package" "$output_dir/"
done

output_packages=()
for package in "${built_packages[@]}"; do
    output_packages+=("$output_dir/$(basename "$package")")
done
binary_rpm=
for package in "${output_packages[@]}"; do
    base=$(basename "$package")
    if [[ "$base" != *.src.rpm && "$base" != *-debuginfo-* && "$base" != *-debugsource-* ]]; then
        binary_rpm=$package
        break
    fi
done
if [[ -z "$binary_rpm" ]]; then
    echo "error: could not identify the installable RPM" >&2
    exit 1
fi

required_payload=(
    /usr/bin/arraw-ui
    /usr/bin/arraw-cli
    /usr/share/applications/io.github.janpipek.arraw.desktop
    /usr/share/metainfo/io.github.janpipek.arraw.metainfo.xml
    /usr/share/icons/hicolor/256x256/apps/io.github.janpipek.arraw.png
    /usr/share/icons/hicolor/scalable/apps/io.github.janpipek.arraw.svg
)
payload=$(rpm -qlp "$binary_rpm")
for path in "${required_payload[@]}"; do
    if ! grep -Fxq "$path" <<<"$payload"; then
        echo "error: RPM payload is missing $path" >&2
        exit 1
    fi
done

if ! rpm -qp --requires "$binary_rpm" | grep -Eq 'Qt_6\.[0-9]+_PRIVATE_API'; then
    echo "error: RPM lacks a generated Qt private-ABI dependency" >&2
    exit 1
fi

git show HEAD:packaging/fedora/arraw.rpmlintrc >"$work_dir/arraw.rpmlintrc"
rpmlint --rpmlintrc "$work_dir/arraw.rpmlintrc" "${output_packages[@]}"
(
    cd "$output_dir"
    sha256sum "${output_packages[@]##*/}" >SHA256SUMS
    printf '%s\n' "${binary_rpm##*/}" >BINARY_RPM
)

echo "Fedora packages written to $output_dir:"
printf '  %s\n' "${output_packages[@]##*/}"
