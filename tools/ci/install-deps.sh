#!/bin/sh
# Install what building and testing arraw needs on Ubuntu 26.04: the one list
# shared by CI (.github/workflows/ci.yml) and the dev sandbox image
# (tools/sandbox/Containerfile), so the two cannot drift. Qt 6.10 comes from
# the distribution; 26.04 is an LTS, so the series stays fixed. Run as root.
set -eu

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    ca-certificates git g++ cmake ninja-build pkg-config \
    qt6-base-dev qt6-base-private-dev qt6-shadertools-dev qt6-image-formats-plugins \
    libraw-dev libtiff6 catch2 \
    libgl-dev libxkbcommon-dev \
    libvulkan-dev mesa-vulkan-drivers vulkan-tools
