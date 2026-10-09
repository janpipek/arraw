#!/usr/bin/env bash

set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

backend=${CONTAINER_BACKEND:-}
if [[ -z "$backend" ]]; then
    backend=$(command -v podman 2>/dev/null || command -v docker 2>/dev/null || true)
fi
if [[ -z "$backend" ]]; then
    echo "error: install podman or docker, or set CONTAINER_BACKEND" >&2
    exit 1
fi

version=$(git show HEAD:CMakeLists.txt \
    | sed -nE 's/^project\(arraw VERSION ([0-9]+\.[0-9]+\.[0-9]+).*$/\1/p')
if [[ ! -f dist/fedora/BINARY_RPM ]]; then
    echo "error: dist/fedora/BINARY_RPM is missing; run 'just rpm' first" >&2
    exit 1
fi
rpm_name=$(<dist/fedora/BINARY_RPM)
if [[ "$rpm_name" != arraw-"$version"*-1*.x86_64.rpm || ! -f "dist/fedora/$rpm_name" ]]; then
    echo "error: recorded RPM '$rpm_name' is missing or has the wrong version" >&2
    exit 1
fi

"$backend" run --rm \
    -v "$repo_root/dist/fedora:/dist:ro,z" \
    registry.fedoraproject.org/fedora:44 \
    bash -euxo pipefail -c '
        dnf install -y --setopt=install_weak_deps=False \
            desktop-file-utils glib2 shared-mime-info "$1"
        arraw-cli --version | grep -qx "arraw-cli $2"
        # The GUI, offscreen: the dynamic linker and Qt find everything.
        QT_QPA_PLATFORM=offscreen arraw-ui --help | grep -q "^Usage:"
        desktop-file-validate \
            /usr/share/applications/io.github.janpipek.arraw.desktop
        grep -q "^Exec=arraw-ui %f$" \
            /usr/share/applications/io.github.janpipek.arraw.desktop
        grep -q "image/x-canon-cr3" \
            /usr/share/applications/io.github.janpipek.arraw.desktop
        grep -q "inode/directory" \
            /usr/share/applications/io.github.janpipek.arraw.desktop
        gio mime image/x-canon-cr3 | grep -q io.github.janpipek.arraw.desktop
        gio mime image/x-sony-arw | grep -q io.github.janpipek.arraw.desktop
        gio mime inode/directory | grep -q io.github.janpipek.arraw.desktop
    ' bash "/dist/$rpm_name" "$version"
