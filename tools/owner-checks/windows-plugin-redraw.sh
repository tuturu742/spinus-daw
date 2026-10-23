#!/usr/bin/env bash
# A Windows plugin's window doesn't update (e.g. Ample Bass P Lite): collects what decides it, and says
# what to try. Changes nothing; paste the whole output into the pull request or issue.
#
# What to try in Spinus (Options > Windows plugins):
#   1. Turn on "Draw plugin windows without the graphics card", close the plugin's window, open it again,
#      and click its settings or preset buttons. Does the window follow now?
#   2. If it still does not: is it the same with the plugin's other format? (Right-click the plugin in
#      the Browser to choose VST3 or VST2.)
# Then run this script once more and paste both outputs.
#
# Usage: tools/owner-checks/windows-plugin-redraw.sh

set -u
folder="${XDG_DATA_HOME:-$HOME/.local/share}/Spinus DAW/windows"
[ -d "$folder" ] || folder="$HOME/.local/share/Spinus DAW/windows"

echo "== Session"
echo "XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-?} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-} DISPLAY=${DISPLAY:-}"
if command -v glxinfo >/dev/null; then
    glxinfo -B 2>/dev/null | grep -E "OpenGL renderer|OpenGL core profile version|direct rendering" || true
else
    echo "(glxinfo not installed: sudo pacman -S mesa-utils)"
fi

echo "== Spinus's Windows plugins settings ($folder/settings.json)"
cat "$folder/settings.json" 2>/dev/null || echo "(none)"
echo
echo "== Wine of the Windows folder"
cat "$folder/prefix/.spinus-wine" 2>/dev/null || echo "(not recorded)"
echo
echo "== Drawing without the graphics card (Wine's Direct3D renderer in the Windows folder)"
if grep -q -A3 'Software\\\\Wine\\\\Direct3D' "$folder/prefix/user.reg" 2>/dev/null; then
    grep -A3 'Software\\\\Wine\\\\Direct3D' "$folder/prefix/user.reg"
else
    echo "(not set: drawing with the graphics card)"
fi
echo "== Plugin format choices"
cat "$HOME/.config/Spinus DAW/plugin-formats.json" 2>/dev/null || echo "(none)"
echo
echo "== Wine and yabridge on this computer"
command -v wine >/dev/null && wine --version
pacman -Q wine-staging wine yabridge 2>/dev/null || true
