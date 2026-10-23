#!/bin/sh
# Builds Spinus as an AppImage in an Ubuntu 24.04 container with podman, so it runs on
# other distributions too. Needs podman and network access (Ubuntu packages, the pinned tools, the
# Python packages from ai/uv.lock).
#
# Usage: packaging/appimage/build.sh [output folder]     (default: build/appimage)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=$(mkdir -p "${1:-$root/build/appimage}" && cd "${1:-$root/build/appimage}" && pwd)
mkdir -p "$out/work"

podman build -t spinus-appimage-builder -f "$here/Containerfile" "$here"
podman run --rm --userns=keep-id \
    -v "$root:/src:ro" -v "$out/work:/work" -v "$out:/out" \
    spinus-appimage-builder sh /src/packaging/appimage/inside.sh
