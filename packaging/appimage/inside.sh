#!/bin/sh
# Builds the AppImage inside the builder container (see build.sh): /src is the source tree (read-only),
# /work keeps the build between runs, /out gets Spinus-<version>-x86_64.AppImage.
set -eu

cmake -S /src -B /work/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
    -DSPINUS_BUILD_TESTS=OFF -DSPINUS_BUILD_SURGE=OFF \
    -DSPINUS_BUILD_BUNDLED_PLUGINS=OFF -DSPINUS_BUILD_OWN_PLUGINS=OFF
cmake --build /work/build

appdir=/work/AppDir
rm -rf "$appdir"
DESTDIR="$appdir" cmake --install /work/build
version=$("$appdir/usr/lib/spinus/spinus" --version | awk '{print $NF}')

# The AI service's own Python with the packages it needs (pinned by ai/uv.lock), and uv for installing
# the analysis engine later.
ai="$appdir/usr/lib/spinus/ai"
export UV_CACHE_DIR=/work/uv-cache UV_PYTHON_INSTALL_DIR=/work/python
uv python install 3.12
python=$(uv python find 3.12)
mkdir -p "$ai/python" "$ai/bin"
cp -a "$(dirname "$(dirname "$python")")"/. "$ai/python/"
(cd /src/ai && uv export --frozen --no-dev --extra listening --extra notes --no-emit-project --format requirements-txt) > /work/requirements.txt
uv pip install --python "$ai/python/bin/python3" --break-system-packages --require-hashes -r /work/requirements.txt
cp /usr/local/bin/uv "$ai/bin/uv"

# The libraries Spinus needs that a desktop system may lack; glibc, ALSA, X and GL stay the system's
# (linuxdeploy's exclude list). JACK is found at run time, from the system.
install -Dm755 /src/packaging/appimage/AppRun "$appdir/AppRun"
cd /work
/opt/linuxdeploy/AppRun --appdir "$appdir" \
    --executable "$appdir/usr/lib/spinus/spinus" \
    --executable "$appdir/usr/lib/spinus/spinus-plugin-scanner" \
    --executable "$appdir/usr/lib/spinus/spinus-plugin-host" \
    --desktop-file "$appdir/usr/share/applications/spinus.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/spinus.svg"
# Spinus's programs live in usr/lib/spinus (the AI service sits beside them); the libraries are in usr/lib.
for program in spinus spinus-plugin-scanner spinus-plugin-host; do
    patchelf --set-rpath '$ORIGIN/..' "$appdir/usr/lib/spinus/$program"
done
mkdir -p "$appdir/usr/share/doc/spinus"
cp /src/LICENSE /src/THIRD_PARTY.md "$appdir/usr/share/doc/spinus/"

ARCH=x86_64 /opt/appimagetool/AppRun --no-appstream "$appdir" "/out/Spinus-$version-x86_64.AppImage"
echo "Built /out/Spinus-$version-x86_64.AppImage"
