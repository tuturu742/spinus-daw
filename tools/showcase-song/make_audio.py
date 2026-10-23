# Original sound effects for the showcase song (synthesised here, no samples): an impact and a reverse swell.
import math, random, struct, wave, sys
rate = 48000
out = sys.argv[1]
def write(name, left, right):
    peak = max(max(abs(x) for x in left), max(abs(x) for x in right)) or 1.0
    g = 0.89 / peak
    with wave.open(f"{out}/{name}", "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<hh", int(l * g * 32767), int(r * g * 32767)) for l, r in zip(left, right)))
random.seed(7)
# Impact: a falling sine boom, a filtered noise crack, and a darker noise tail.
n = int(rate * 3.0)
L, R = [], []
phase = 0.0; lp1 = lp2 = 0.0; tl = tr = 0.0
for i in range(n):
    t = i / rate
    f = 35 + 70 * math.exp(-t * 9)
    phase += 2 * math.pi * f / rate
    boom = math.sin(phase) * math.exp(-t * 2.2)
    crack = random.uniform(-1, 1) * math.exp(-t * 30)
    lp1 += 0.25 * (crack - lp1)
    nl, nr = random.uniform(-1, 1), random.uniform(-1, 1)
    tl += 0.04 * (nl - tl); tr += 0.04 * (nr - tr)
    tail = math.exp(-t * 1.6) * 0.9
    L.append(boom * 0.9 + lp1 * 0.6 + tl * tail * 3)
    R.append(boom * 0.9 + lp1 * 0.6 + tr * tail * 3)
write("Showcase Impact.wav", L, R)
# Reverse swell: a bright noise wash that grows over two bars at 124 BPM, then stops dead.
n = int(rate * 2 * 4 * 60 / 124)
L, R = [], []
hl = hr = 0.0; pl = pr = 0.0
for i in range(n):
    x = i / n
    env = x ** 3
    nl, nr = random.uniform(-1, 1), random.uniform(-1, 1)
    hl += 0.3 * (nl - hl); hr += 0.3 * (nr - hr)
    bl, br = nl - hl, nr - hr  # high-passed
    tone = math.sin(2 * math.pi * 880 * i / rate) * 0.15 + math.sin(2 * math.pi * 1320 * i / rate) * 0.1
    L.append(env * (bl + tone)); R.append(env * (br + tone))
write("Showcase Reverse Swell.wav", L, R)
