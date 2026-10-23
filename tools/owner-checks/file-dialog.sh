#!/usr/bin/env bash
# Check: Spinus's file dialogs are your desktop's own (KDE's or GNOME's), through the desktop portal.
# The automatic tests use a stand-in for the portal; this uses the real one on your desktop.
#
# What it does, for the Spinus built in this folder and then for the installed Flatpak (if any):
#   1. Opens a small "Spinus file dialog check" window and, on it, an Open dialog: pick one or two files.
#   2. A Save dialog: type a name (for example "Dialog test") in your home folder or a subfolder, click Save.
#      Spinus makes the project's folder there, as saving a project does, checks it can write in it, and
#      removes it again.
#   3. A folder dialog: pick any folder.
# Cancel any of them to skip it. Each prints which dialog it was and the path it gave.
# Paste the whole output into the issue, and say whether the dialogs looked like your desktop's
# own and stayed in front of the small Spinus window.
#
# Run it from the spinus-daw folder after building (cmake --build --preset release):
#   tools/owner-checks/file-dialog.sh
set -u
cd "$(dirname "$0")/../.." || exit 1

echo "== Desktop: ${XDG_CURRENT_DESKTOP:-unknown}, session: ${XDG_SESSION_TYPE:-unknown}"
for p in xdg-desktop-portal xdg-desktop-portal-kde xdg-desktop-portal-gnome xdg-desktop-portal-gtk; do
    pacman -Q "$p" 2>/dev/null || true
done

run_checks() {
    for mode in open save folder; do
        echo "-- $mode"
        "$@" --choose-file "$mode" 2>&1 | grep -E '^(Spinus|Dialog|Why|Chosen|  |Project folder)'
    done
}

echo
spinus=build/release/spinus
echo "== This build: $spinus"
if [ -x "$spinus" ]; then
    run_checks "$spinus"
else
    echo "Not built yet: run  cmake --build --preset release"
fi

echo
echo "== The installed Flatpak"
if flatpak info io.github.tuturu742.spinus >/dev/null 2>&1; then
    if flatpak run --command=sh io.github.tuturu742.spinus -c 'grep -q -a -- --choose-file /app/bin/spinus' 2>/dev/null; then
        run_checks flatpak run io.github.tuturu742.spinus
    else
        echo "The installed Flatpak is older than this check (no --choose-file): install one built from this"
        echo "change (packaging/flatpak/build.sh, then flatpak install --user build/flatpak/Spinus.flatpak) and run"
        echo "this again."
    fi
else
    echo "Not installed."
fi
