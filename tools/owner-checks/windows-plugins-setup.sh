#!/usr/bin/env bash
# Windows plugins through yabridge (VST2, VST3 and CLAP), set up by hand in a scratch Wine prefix.
#
# Uses its own Wine prefix in a scratch folder and sets up yabridge by hand there, so your ~/.wine,
# ~/.vst3, ~/.clap and yabridgectl settings are not touched. Needs: wine (or wine-staging), yabridge.
#
# Usage (from the repository root, after building the release preset):
#   tools/owner-checks/windows-plugins-setup.sh [scratch-folder]     # default ~/spinus-windows-plugins-scratch
#
# Paste the output into the issue or pull request that asked for it. Delete the scratch folder afterwards if you like.

set -eu

scratch=${1:-$HOME/spinus-windows-plugins-scratch}
probe=./build/release/tests/checks/spinus_plugin_hosting_checks
zip_url=https://github.com/michaelwillis/dragonfly-reverb/releases/download/3.2.10/dragonfly-reverb-3.2.10-win64.zip
zip_sha=ca5f35f9dcd9a33f06490d3c92f04061a1a798cb81257d5ca5851bdda0824c66

for tool in wine wineboot; do command -v $tool >/dev/null || { echo "Missing $tool: sudo pacman -S wine-staging"; exit 1; }; done
[ -f /usr/lib/libyabridge-chainloader-vst2.so ] || { echo "Missing yabridge: sudo pacman -S yabridge"; exit 1; }
[ -x "$probe" ] || { echo "Build first: cmake --preset release && cmake --build --preset release"; exit 1; }

echo "== System"
wine --version
pacman -Q yabridge 2>/dev/null || true
echo "memlock limit: $(ulimit -l) KiB (yabridge wants 'unlimited': add yourself to the realtime group)"

mkdir -p "$scratch"
export WINEPREFIX="$scratch/prefix"

if [ ! -d "$WINEPREFIX/drive_c" ]; then
    echo "Creating the Wine prefix (takes a few minutes the first time)..."
    WINEDEBUG=-all wineboot -i >/dev/null 2>&1
fi

zip="$scratch/dragonfly-reverb-3.2.10-win64.zip"
[ -f "$zip" ] || curl -sL -o "$zip" "$zip_url"
echo "$zip_sha  $zip" | sha256sum -c --quiet
rm -rf "$scratch/dfwin" && unzip -q -o "$zip" -d "$scratch/dfwin"
src="$scratch/dfwin/dragonfly-reverb-3.2.10"

# yabridge picks the Wine prefix whose drive_c contains the plugin.
plugins="$WINEPREFIX/drive_c/Plugins"
rm -rf "$plugins" && mkdir -p "$plugins/vst2" "$plugins/vst3" "$plugins/clap"

for name in DragonflyHallReverb DragonflyRoomReverb; do
    # VST2: the chainloader .so sits next to the Windows .dll with the same name.
    cp "$src/$name-vst.dll" "$plugins/vst2/"
    cp /usr/lib/libyabridge-chainloader-vst2.so "$plugins/vst2/$name-vst.so"
    # VST3: one bundle holding the Windows module and the Linux chainloader.
    cp -r "$src/$name.vst3" "$plugins/vst3/"
    mkdir -p "$plugins/vst3/$name.vst3/Contents/x86_64-linux"
    cp /usr/lib/libyabridge-chainloader-vst3.so "$plugins/vst3/$name.vst3/Contents/x86_64-linux/$name.so"
    # CLAP: the Windows plugin renamed to .clap-win next to the chainloader .clap.
    cp "$src/$name.clap" "$plugins/clap/$name.clap-win"
    cp /usr/lib/libyabridge-chainloader-clap.so "$plugins/clap/$name.clap"
done

echo
echo "== Results"
SPINUS_CHECK_WINDOWS_PLUGINS="$plugins" "$probe" --order decl "[windows]" 2>&1 | grep -E "^Hosting \||passed|failed|FAILED"
