#!/usr/bin/env bash
# Release check: the AppImage and the Flatpak on your desktop, with your sound card and plugins.
# The automatic checks already ran them on fresh Arch, Fedora and Ubuntu systems without sound or plugins.
#
# What it does:
#   1. Runs each package's self-test (make, save, open and export a song; the helpers; the AI's Python).
#   2. Lists the plugins each package finds when it scans (your ~/.vst3, ~/.clap, ~/.lv2 and, for the
#      AppImage, /usr/lib too). Nothing is changed in your Spinus settings: each run uses its own.
#   3. Starts each package so you can play the demo song and open a plugin: close the window to go on.
# Paste the whole output into the issue, and say whether you heard sound and saw plugin windows.
#
# Run it from the spinus-daw folder after building the packages (see docs/user/install.md):
#   tools/owner-checks/packages.sh
set -u
cd "$(dirname "$0")/../.." || exit 1
appimage=$(ls build/appimage/Spinus-*-x86_64.AppImage 2>/dev/null | head -1)
bundle=build/flatpak/Spinus.flatpak
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "== AppImage: ${appimage:-not built (packaging/appimage/build.sh)}"
if [ -n "$appimage" ]; then
    "$appimage" --self-test "$work/appimage" 2>&1 | grep -E '^ok|FAILED|works|failed'
    XDG_CONFIG_HOME="$work/config" "$appimage" --scan-plugins 2>&1 | grep -vE 'ALSA|jack|Jack' | tail -40
    echo "Starting the AppImage: play something, open a plugin's window, then close Spinus."
    "$appimage" >/dev/null 2>&1
fi

echo
echo "== Flatpak: $( [ -f "$bundle" ] && echo "$bundle" || echo 'not built (packaging/flatpak/build.sh)')"
if [ -f "$bundle" ]; then
    echo "Installing it for you only (flatpak install --user); remove it later with:"
    echo "  flatpak uninstall --user io.github.tuturu742.spinus"
    flatpak install -y --user --noninteractive "$bundle" || exit 1
    flatpak run --command=sh io.github.tuturu742.spinus -c "cd /tmp && spinus --self-test /tmp/spinus-check" 2>&1 \
        | grep -E '^ok|FAILED|works|failed'
    echo "Starting the Flatpak: play something, open a plugin's window, then close Spinus."
    flatpak run io.github.tuturu742.spinus >/dev/null 2>&1
fi
