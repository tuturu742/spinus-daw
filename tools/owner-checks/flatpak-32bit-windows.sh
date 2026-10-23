#!/usr/bin/env bash
# 32-bit Windows programs in the Flatpak.
#
# Checks that the installed Spinus Flatpak can start 32-bit Windows programs (many plugin installers are
# 32-bit) with Spinus's own Wine. Uses a scratch Windows folder in /tmp; your own Windows folder is not
# touched. Needs Spinus's tested Wine downloaded once (Options > Windows plugins > Set up), and Flatpak's
# 32-bit support:
#   flatpak install flathub org.freedesktop.Platform.Compat.i386//25.08
#
# Usage: tools/owner-checks/flatpak-32bit-windows.sh
# Paste the whole output into the issue or pull request that asked for it.

set -u
app=io.github.tuturu742.spinus
wine="$HOME/.local/share/Spinus DAW/windows/runtime/wine"
[ -x "$wine/bin/wine64" ] || { echo "Spinus's Wine is not downloaded yet: Options > Windows plugins > Set up"; exit 1; }

echo "== Flatpak"
flatpak info "$app" | grep -E "Version|Runtime|Installed" || { echo "Spinus Flatpak not installed"; exit 1; }
flatpak list --runtime --columns=application,branch,installation | grep -i "Compat.i386" || echo "no Compat.i386 installed"

scratch=$(mktemp -d /tmp/spinus-32bit-check.XXXXXX)
trap 'rm -rf "$scratch"' EXIT

echo
echo "== 32-bit loader in the sandbox"
flatpak run --command=sh "$app" -c 'ls -lL /lib/ld-linux.so.2 2>&1; ls /app/lib/i386-linux-gnu | wc -l'

echo
echo "== A 64-bit and a 32-bit Windows program (scratch Windows folder)"
flatpak run --command=sh --env=WINEPREFIX="$scratch/prefix" --env=WINEDEBUG=-all --env=DISPLAY= \
    --env=WINEDLLOVERRIDES="mscoree,mshtml=" --env=WINELOADER="$wine/bin/wine64" "$app" -c "
        \"$wine/bin/wine64\" wineboot -i >/dev/null 2>&1
        echo 64-bit: \$(\"$wine/bin/wine64\" C:\\\\windows\\\\system32\\\\cmd.exe /c echo hello64 2>&1)
        echo 32-bit: \$(\"$wine/bin/wine64\" C:\\\\windows\\\\syswow64\\\\cmd.exe /c echo hello32 2>&1)
        \"$wine/bin/wineserver\" -k"

echo
echo "Expected: '64-bit: hello64' and '32-bit: hello32'."
echo "Then in the Flatpak Spinus: Options > Windows plugins > Add a plugin or installer..., choose a 32-bit"
echo "installer (for example AME_3_5_0_Installer.exe) and say whether its window appeared."
