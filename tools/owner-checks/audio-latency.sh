#!/usr/bin/env bash
# Audio latency and dropout (xrun) check on your machine.
#
# Plays SILENCE through each audio path Spinus can use, at several buffer sizes, with and without a
# simulated DSP load, and prints one summary line per run. Nothing is audible.
#
# Usage (from the repository root, after building the release preset):
#   tools/owner-checks/audio-latency.sh            # about 6 minutes
#   SECONDS_PER_RUN=60 tools/owner-checks/audio-latency.sh
#
# Paste the whole output into the issue or pull request that asked for it.

set -u

probe=./build/release/tests/checks/spinus_latency_probe
seconds=${SECONDS_PER_RUN:-20}

if [ ! -x "$probe" ]; then
    echo "Build first: cmake --preset release && cmake --build --preset release"
    exit 1
fi

# PipeWire's JACK library: the pipewire-jack package (pw-jack in /usr/bin), or any pw-jack on PATH.
pwjack=$(command -v pw-jack || true)

# PipeWire's per-node error (xrun) counters, from pw-top: one "node count" line per node.
pipewire_errors() {
    command -v pw-top >/dev/null || return 0
    timeout 3 pw-top -b -n 2 2>/dev/null |
        awk '$1 ~ /^[RIS]$/ && $2 ~ /^[0-9]+$/ { last[$NF] = $9 } END { for (k in last) print k, last[k] }' | sort
}

# Nodes whose error counter grew between two snapshots, as "node+N" (or "none").
pipewire_error_growth() {
    local grown
    grown=$(join <(echo "$1") <(echo "$2") | awk '$3 > $2 { printf "%s+%d ", $1, $3 - $2 }')
    echo "${grown:-none}"
}

echo "== System"
uname -r
pipewire --version 2>/dev/null | tail -1
pacman -Q pipewire pipewire-jack jack2 2>/dev/null
echo "pw-jack: ${pwjack:-not found}"
echo "realtime priority limit: $(ulimit -r)"
echo "load average: $(cut -d' ' -f1-3 /proc/loadavg)"

if command -v pw-dump >/dev/null && command -v wpctl >/dev/null; then
    sink=$(wpctl inspect @DEFAULT_AUDIO_SINK@ 2>/dev/null | awk -F'"' '/node.name/ {print $2; exit}')
    echo "default output: ${sink:-unknown}"
    echo "graph driver of default output (if not itself, a different device clocks your audio):"
    wpctl inspect @DEFAULT_AUDIO_SINK@ 2>/dev/null | grep -E "node.driver-id|object.id" | sed 's/^/  /'
fi

run() {
    local label=$1; shift
    local before after out
    before=$(pipewire_errors)
    out=$("$@" 2>/dev/null | grep "Latency | summary" | sed 's/^Latency | summary | //')
    after=$(pipewire_errors)
    printf '%-14s %s pipewire_node_errors=%s\n' "$label" "${out:-FAILED}" "$(pipewire_error_growth "$before" "$after")"
}

echo
echo "== Runs (${seconds} s each)"

for load in 0 0.6; do
    for buffer in 64 128 256 512; do
        if [ -n "$pwjack" ]; then
            run "pipewire-jack" "$pwjack" -p "$buffer" "$probe" --type JACK --buffer "$buffer" --load "$load" --seconds "$seconds"
        fi
        run "alsa-pipewire" "$probe" --type ALSA --device "PipeWire Sound Server" --buffer "$buffer" --load "$load" --seconds "$seconds"
    done
done

echo
echo "== Done. latency_ms = Spinus buffer + device latency reported by the driver;"
echo "   xruns = dropouts reported by the driver; late = audio callbacks more than 1.5 blocks apart."
