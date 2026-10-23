#!/usr/bin/env bash
# Downloads Microsoft's C runtime and Windows SDK (headers and libraries) for cross-building Spinus for
# Windows on Linux (cmake/toolchains/windows-msvc-xwin.cmake), with xwin (Apache-2.0 or MIT;
# `cargo install xwin --locked`). They are Microsoft's, under the Visual Studio Build Tools licence, which
# the person running this accepts: read it first at https://go.microsoft.com/fwlink/?LinkId=2086102.
# They are only used to build: never commit or ship them.
#
#   tools/windows/get-msvc-sdk.sh --accept-license [folder]     (default ~/.local/share/spinus-xwin)
set -euo pipefail

if [[ "${1:-}" != "--accept-license" ]]; then
    echo "This downloads Microsoft's Windows SDK and C runtime (about 700 MB) under Microsoft's licence:"
    echo "  https://go.microsoft.com/fwlink/?LinkId=2086102"
    echo "Read it, then run this again with --accept-license to accept it and download."
    exit 1
fi
out="${2:-$HOME/.local/share/spinus-xwin}"
xwin="$(command -v xwin || echo "$HOME/.cargo/bin/xwin")"
if [[ ! -x "$xwin" ]]; then
    echo "xwin is missing: install it with  cargo install xwin --locked" >&2
    exit 1
fi
cache="${XDG_CACHE_HOME:-$HOME/.cache}/spinus-xwin"
"$xwin" --accept-license --cache-dir "$cache" --arch x86_64 splat --output "$out"

# Windows ignores the case of file names, Linux does not: JUCE and Tracktion ask for "D2d1.lib" and the
# SDK has "d2d1.lib". Link each library the sources ask for under the spelling they use.
repo="$(cd "$(dirname "$0")/../.." && pwd)"
grep -rhoE 'pragma comment *\( *lib *, *"[^"]+"' "$repo/external/JUCE/modules" "$repo/external/tracktion_engine/modules" \
        "$repo/src" "$repo/app" 2>/dev/null | sed -E 's/.*"([^"]+)"/\1/' | sort -u |
while read -r wanted; do
    [[ "$wanted" == *.* ]] || wanted="$wanted.lib"
    for dir in "$out/sdk/lib/um/x86_64" "$out/sdk/lib/ucrt/x86_64" "$out/crt/lib/x86_64"; do
        [[ -e "$dir/$wanted" ]] && break
        found="$(find "$dir" -maxdepth 1 -iname "$wanted" -print -quit)"
        if [[ -n "$found" ]]; then
            ln -s "$(basename "$found")" "$dir/$wanted"
            break
        fi
    done
done
echo "Windows SDK ready in $out"
