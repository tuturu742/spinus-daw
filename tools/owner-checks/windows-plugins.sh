#!/usr/bin/env bash
# Windows plugins set up and managed by Spinus.
#
# Runs the end-to-end tests: Spinus's own Wine and yabridge (downloads about 90 MB once into /tmp),
# the Wine and yabridge on this computer if there are any, a Windows plugin added and scanned, and the
# Windows plugins window's whole flow. Uses scratch folders in /tmp; your own Wine setup is not touched.
#
# Usage (from the repository root, after building the release preset):
#   tools/owner-checks/windows-plugins.sh
#
# Then, in Spinus: Options > Windows plugins..., add a real Windows plugin installer you own or a free one
# (for example a free synth's Windows installer), play it, and paste the output below plus what you saw.

set -u
tests=./build/release/tests/spinus_tests
[ -x "$tests" ] || { echo "Build first: cmake --preset release && cmake --build --preset release"; exit 1; }

echo "== System"
command -v wine >/dev/null && wine --version || echo "no wine on PATH"
pacman -Q yabridge wine-staging 2>/dev/null || true
echo "memlock limit: $(ulimit -l) KiB; groups: $(id -nG)"

echo
echo "== Runtimes, a plugin, a scan"
"$tests" "[.windows-e2e]" -s 2>&1 | grep -E "runtime:|test took|All tests|FAILED|test cases" 
