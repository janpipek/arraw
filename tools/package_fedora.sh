#!/usr/bin/env bash

set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

version=$(sed -nE 's/^project\(arraw VERSION ([0-9]+\.[0-9]+\.[0-9]+).*$/\1/p' CMakeLists.txt)
spec=packaging/fedora/arraw.spec
spec_version=$(sed -nE 's/^Version:[[:space:]]+([^[:space:]]+).*$/\1/p' "$spec")

if [[ -z "$version" || "$version" != "$spec_version" ]]; then
    echo "error: CMake version '$version' and RPM version '$spec_version' disagree" >&2
    exit 1
fi

if ! git diff --quiet || ! git diff --cached --quiet \
    || [[ -n "$(git ls-files --others --exclude-standard)" ]]; then
    echo "error: RPM sources must come from a clean, committed worktree" >&2
    exit 1
fi

required_packages=(
    appstream
    catch-devel
    cmake
    desktop-file-utils
    gcc-c++
    lcms2-devel
    LibRaw-devel
    ninja-build
    qt6-qtbase-devel
    qt6-qtbase-private-devel
    qt6-qtimageformats
    qt6-qtshadertools-devel
    redhat-rpm-config
    rpm-build
    rpmlint
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
if ! git describe --exact-match --match "v${version}" HEAD >/dev/null 2>&1; then
    release_args=(--define "snapshot_release 0.1.${source_date}git${commit}")
fi

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/arraw-rpm.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT
top_dir="$work_dir/rpmbuild"
mkdir -p "$top_dir"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

source_archive="$top_dir/SOURCES/arraw-${version}.tar.gz"
git archive --format=tar --prefix="arraw-${version}/" HEAD \
    | gzip -n >"$source_archive"
cp "$spec" "$top_dir/SPECS/arraw.spec"

rpmbuild -ba "$top_dir/SPECS/arraw.spec" \
    --define "_topdir $top_dir" \
    "${release_args[@]}"

output_dir="$repo_root/dist/fedora"
mkdir -p "$output_dir"
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
    /usr/bin/arraw
    /usr/share/applications/io.github.janpipek.arraw.desktop
    /usr/share/metainfo/io.github.janpipek.arraw.metainfo.xml
    /usr/share/icons/hicolor/256x256/apps/io.github.janpipek.arraw.png
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

rpmlint "${output_packages[@]}"
(
    cd "$output_dir"
    sha256sum "${output_packages[@]##*/}" >SHA256SUMS
    printf '%s\n' "${binary_rpm##*/}" >BINARY_RPM
)

echo "Fedora packages written to $output_dir:"
printf '  %s\n' "${output_packages[@]##*/}"
