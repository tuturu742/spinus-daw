#!/bin/sh
# Runs a Spinus AppImage on fresh Arch, Fedora and Ubuntu systems (podman containers), with only what a
# desktop has anyway (ALSA, X, fonts, GL): `spinus --self-test` (make, save, open and export a song; the
# helpers; the AI service's Python), then a picture of the main window on a virtual display.
#
# Usage: packaging/appimage/check.sh <Spinus-...AppImage> [arch|fedora|ubuntu ...]
set -eu
appimage=$(readlink -f "$1")
shift
systems=${*:-arch fedora ubuntu}
results=$(dirname "$appimage")/check
mkdir -p "$results"

for system in $systems; do
    case $system in
        arch)
            image=docker.io/library/archlinux:latest
            setup="pacman -Syu --noconfirm --needed alsa-lib libx11 libxext libxrender libxinerama libxrandr \
                   libxcursor libxcomposite fontconfig ttf-dejavu mesa xorg-server-xvfb xorg-xauth >/dev/null" ;;
        fedora)
            image=registry.fedoraproject.org/fedora:latest
            setup="dnf install -y -q alsa-lib libX11 libXext libXrender libXinerama libXrandr libXcursor \
                   libXcomposite fontconfig dejavu-sans-fonts mesa-libGL xorg-x11-server-Xvfb xorg-x11-xauth \
                   >/dev/null" ;;
        ubuntu)
            image=docker.io/library/ubuntu:24.04
            setup="apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
                   libasound2t64 libx11-6 libxext6 libxrender1 libxinerama1 libxrandr2 libxcursor1 libxcomposite1 \
                   fontconfig fonts-dejavu-core libgl1 xvfb xauth >/dev/null" ;;
        *) echo "unknown system: $system"; exit 1 ;;
    esac
    echo "== $system ($image)"
    podman run --rm -v "$appimage:/Spinus.AppImage:ro" -v "$results:/results" "$image" sh -c "
        set -e
        $setup
        cd /tmp && /Spinus.AppImage --appimage-extract >/dev/null
        ./squashfs-root/AppRun --self-test /tmp/check
        xvfb-run -a -s '-screen 0 1400x900x24' ./squashfs-root/AppRun --snapshot /results/$system.png
        echo 'window: ok'
    " && echo "== $system: passed" || { echo "== $system: FAILED"; failed=1; }
done
exit ${failed:-0}
