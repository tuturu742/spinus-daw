#!/usr/bin/env bash
# Makes the Windows installer on Linux, without Windows: the clang-cl cross-build, the staged folder with
# its Python, and NSIS run under Wine (the official NSIS 3.11 zip, pinned). Writes
# build/Spinus-<version>-Setup.exe. GitHub Actions (.github/workflows/windows.yml) does the same on Windows.
#
#   packaging/windows/build-on-linux.sh
#
# Needs: clang (clang-cl), lld, llvm (llvm-rc, llvm-mt), cmake, ninja, uv, wine, and Microsoft's SDK from
# tools/windows/get-msvc-sdk.sh (its licence has to be accepted once).
set -euo pipefail
cd "$(dirname "$0")/../.."

nsisUrl="https://downloads.sourceforge.net/project/nsis/NSIS%203/3.11/nsis-3.11.zip"
nsisSha256="c7d27f780ddb6cffb4730138cd1591e841f4b7edb155856901cdf5f214394fa1"
cache="build/windows-stage.cache"

cmake --preset windows-cross
cmake --build --preset windows-cross
python3 -I packaging/windows/stage.py --build build/windows-cross --out build/windows-stage --cache "$cache"

if [[ ! -x "$cache/nsis-3.11/makensis.exe" ]]; then
    curl -sSfL -o "$cache/nsis-3.11.zip" "$nsisUrl"
    echo "$nsisSha256  $cache/nsis-3.11.zip" | sha256sum -c --quiet
    (cd "$cache" && unzip -q -o nsis-3.11.zip)
fi
WINEDEBUG=-all wine "$cache/nsis-3.11/makensis.exe" /V2 "/DSTAGE=$(winepath -w build/windows-stage)" \
    "$(winepath -w packaging/windows/spinus.nsi)"
ls -la build/Spinus-*-Setup.exe
