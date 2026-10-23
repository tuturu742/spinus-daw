#!/usr/bin/env bash
# The loud click: is it in Spinus's own sound, or does it come from PipeWire or the sound card?
#
# What it does:
#   1. Start Spinus as usual (and open the project where you hear the click), then run this script.
#   2. It records only Spinus's sound (not other programs, not the microphone) into ~/spinus-clicks.wav.
#   3. You make the click happen: switch the system's output, open the project again, or use the
#      Automixer's Before and After. Note roughly how many seconds after starting you did each.
#   4. Press Ctrl+C. It lists every loud moment in Spinus's sound, with its time. Paste the output into
#      the issue, with what you did when.
#
# Run from the spinus-daw folder: tools/owner-checks/spinus-clicks.sh
set -u
OUT=$HOME/spinus-clicks.wav

# Spinus's output ports (with ALSA "alsa_playback.spinus:output_FL", with JACK "Spinus...:out..."): the first
# two, by the name of the node that has them.
spinus_ports() {
  pw-link -o 2>/dev/null | grep -i "spinus" | grep -iv "capture\|monitor\|clicks-rec" | head -2
}
if [ -z "$(spinus_ports)" ]; then
  echo "Spinus is not playing through PipeWire right now. Start Spinus, then run this again."
  exit 1
fi
echo "Recording Spinus's sound into $OUT (only Spinus: the recorder is connected to its outputs alone)."
echo "Make the click happen now; press Ctrl+C when done."
pw-record -P '{ node.autoconnect = false node.name = spinus-clicks-rec }' --rate 48000 --channels 2 \
  --format f32 "$OUT" &
rec=$!
# Keep Spinus connected to the recorder: a new output or project can make Spinus open its sound again.
(
  while kill -0 $rec 2>/dev/null; do
    i=0
    for port in $(spinus_ports); do
      if [ $i -eq 0 ]; then target=spinus-clicks-rec:input_FL; else target=spinus-clicks-rec:input_FR; fi
      pw-link "$port" "$target" 2>/dev/null
      i=$((i + 1))
    done
    sleep 0.2
  done
) &
linker=$!
trap 'kill -INT $rec 2>/dev/null' INT
wait $rec
trap - INT
kill $linker 2>/dev/null
sleep 0.5
echo
echo "== Loud moments in Spinus's sound (above -30 dBFS), seconds after the recording started"
/usr/bin/python3 -I - "$OUT" <<'EOF'
import math, struct, sys
path = sys.argv[1]
data = open(path, "rb").read()
at, fmt, samples = 12, None, b""
while at + 8 <= len(data):
    chunk, size = data[at:at + 4], struct.unpack("<I", data[at + 4:at + 8])[0]
    body = data[at + 8:at + 8 + size]
    if chunk == b"fmt ":
        fmt = struct.unpack("<HHIIHH", body[:16])
    elif chunk == b"data":
        samples = body
    at += 8 + size + (size & 1)
if fmt is None:
    sys.exit("The recording could not be read.")
kind, channels, rate, _, _, bits = fmt
count = len(samples) // (bits // 8)
values = struct.unpack("<%d%s" % (count, "f" if bits == 32 else "h"), samples[:count * (bits // 8)])
scale = 1.0 if bits == 32 else 32768.0
frames = count // channels
print("Recorded %.1f s." % (frames / rate))
window = rate // 100  # 10 ms
found = 0
last = -1.0
for start in range(0, frames, window):
    peak = 0.0
    for i in range(start * channels, min(frames, start + window) * channels):
        peak = max(peak, abs(values[i]) / scale)
    if peak > 0.0316:  # -30 dBFS
        t = start / rate
        if t - last > 0.2:
            print("  %7.2f s  %6.1f dBFS%s" % (t, 20 * math.log10(peak), "  (clipping)" if peak >= 0.99 else ""))
            found += 1
        last = t
if found == 0:
    print("  none: Spinus's own sound stayed quiet. A click you heard came from PipeWire or the sound card.")
EOF
echo "(The recording is in $OUT.)"
