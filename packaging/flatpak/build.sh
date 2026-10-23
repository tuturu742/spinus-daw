#!/bin/sh
# Builds the Flatpak with flatpak-builder in Flathub's own builder image (podman), into a
# local repository and a single-file bundle: build/flatpak/Spinus.flatpak. Install it with
#   flatpak install --user build/flatpak/Spinus.flatpak
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out="$root/build/flatpak"
mkdir -p "$out"

podman run --rm --privileged -v "$root:/src:ro" -v "$out:/out" \
    ghcr.io/flathub-infra/flatpak-github-actions:freedesktop-25.08 sh -c '
        set -e
        rm -rf /out/manifest && cp -r /src/packaging/flatpak /out/manifest
        sed -i "s|path: ../..|path: /src|" /out/manifest/io.github.tuturu742.spinus.yml
        # No cache: flatpak-builder does not notice changes in a local source folder.
        flatpak-builder --disable-rofiles-fuse --disable-cache --force-clean --state-dir=/out/state --repo=/out/repo \
            /out/app /out/manifest/io.github.tuturu742.spinus.yml
        flatpak build-bundle /out/repo /out/Spinus.flatpak io.github.tuturu742.spinus
        echo "Built /out/Spinus.flatpak"
    '
