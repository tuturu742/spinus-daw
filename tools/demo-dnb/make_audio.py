# The original sounds of the demo song "Sodium Lights" (liquid drum & bass, 174 BPM), synthesised here with
# numpy and scipy: no samples. Writes four WAV files (48 kHz, 16-bit) into the folder given:
#
#   Sodium Break 174bpm.wav   a two-bar breakbeat (kick, snare, ghost notes, hi-hats) for the Beat Slicer
#   Sodium Crackle.wav        eight bars of vinyl crackle and hiss (mono, loops seamlessly)
#   Sodium Impact.wav         a sub boom with a noise crack, for the drops
#   Sodium Reverse Cymbal.wav a two-bar reversed cymbal swell into the drops
#
#   python3 make_audio.py <folder>        (needs numpy and scipy; deterministic: the same files every time)
#
# The break's hits are listed in BREAK below, on a sixteenth-note grid; DnbComposition.h slices the loop at
# the same steps (breakSteps), so keep the two in step.

import sys
import wave

import numpy as np
from scipy.signal import fftconvolve, lfilter

RATE = 48000
BPM = 174.0
STEP = RATE * 60.0 / BPM / 4.0  # a sixteenth note, in samples
rng = np.random.default_rng(174)

# (step, sound, velocity) over two bars of sixteenths (0..31). K kick, S snare, G ghost snare, H closed hat,
# O open hat. An original two-step groove with the swing and ghost notes of the classic breaks.
BREAK = [
    (0, "K", 1.0), (0, "H", 0.8), (2, "H", 0.55), (3, "H", 0.3), (4, "S", 1.0), (4, "H", 0.6),
    (6, "H", 0.55), (7, "G", 0.45), (8, "H", 0.75), (9, "G", 0.3), (10, "K", 0.9), (10, "H", 0.55),
    (12, "S", 1.0), (12, "H", 0.6), (13, "H", 0.3), (14, "O", 0.6), (15, "G", 0.5),
    (16, "K", 1.0), (16, "H", 0.8), (18, "K", 0.65), (18, "H", 0.5), (20, "S", 1.0), (20, "H", 0.6),
    (22, "H", 0.55), (23, "G", 0.4), (25, "G", 0.45), (26, "K", 0.9), (26, "H", 0.55), (27, "H", 0.3),
    (28, "S", 1.0), (28, "H", 0.6), (29, "G", 0.35), (30, "H", 0.6), (31, "G", 0.55),
]


def t_axis(seconds):
    return np.arange(int(seconds * RATE)) / RATE


def one_pole_lp(x, cutoff):
    a = np.exp(-2 * np.pi * cutoff / RATE)
    return lfilter([1 - a], [1, -a], x)


def lp(x, cutoff, order=2):
    for _ in range(order):
        x = one_pole_lp(x, cutoff)
    return x


def hp(x, cutoff, order=2):
    return x - lp(x, cutoff, order)


def bp(x, low, high):
    return lp(hp(x, low), high)


def kick():
    t = t_axis(0.42)
    freq = 52 + 110 * np.exp(-t * 32)
    phase = 2 * np.pi * np.cumsum(freq) / RATE
    body = np.sin(phase) * np.exp(-t * 11.0)
    click = hp(rng.uniform(-1, 1, len(t)), 2500) * np.exp(-t * 260) * 0.6
    return np.tanh(2.2 * (body + click)) / np.tanh(2.2)


def snare(length=0.32, decay=17.0):
    t = t_axis(length)
    tone = (np.sin(2 * np.pi * 186 * t) * 0.8 + np.sin(2 * np.pi * 331 * t) * 0.45) * np.exp(-t * 30)
    noise = bp(rng.uniform(-1, 1, len(t)), 900, 7500) * np.exp(-t * decay) * 2.2
    snap = hp(rng.uniform(-1, 1, len(t)), 4000) * np.exp(-t * 120) * 0.8
    return np.tanh(1.8 * (tone + noise + snap)) * 0.9


def hat(open_=False):
    t = t_axis(0.45 if open_ else 0.07)
    metal = sum(np.sign(np.sin(2 * np.pi * f * t)) for f in (317, 432, 539, 671, 803, 978))
    noise = rng.uniform(-1, 1, len(t))
    x = hp(metal * 0.25 + noise, 6500)
    return x * np.exp(-t * (9 if open_ else 60)) * 1.8


def room(x, seconds=0.35, mix=0.12):
    """A small, dark room: convolution with a short decaying noise burst."""
    t = t_axis(seconds)
    ir = lp(rng.uniform(-1, 1, len(t)), 5000) * np.exp(-t * 14)
    ir /= np.sqrt(np.sum(ir**2))
    wet = fftconvolve(x, ir)[: len(x)]
    return x + wet * mix


def write(path, channels, peak_db=-1.0):
    data = np.stack(channels, axis=1) if len(channels) > 1 else channels[0][:, None]
    data = data / (np.max(np.abs(data)) or 1.0) * 10 ** (peak_db / 20)
    pcm = np.round(data * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(data.shape[1])
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())


def make_break(folder):
    length = int(round(32 * STEP))
    mix = np.zeros(length + RATE)
    sounds = {"K": kick, "S": snare, "G": lambda: snare(0.16, 30.0), "H": hat, "O": lambda: hat(True)}
    for step, kind, velocity in BREAK:
        start = int(round(step * STEP))
        # A little human timing: late ghost notes and hats, the backbeat right on time.
        if kind in "GH":
            start += int(rng.integers(0, 120))
        sound = sounds[kind]() * velocity
        if kind == "O":  # the next closed hat chokes it
            sound = sound[: int(2 * STEP)] * np.linspace(1, 0.6, int(2 * STEP))
        mix[start : start + len(sound)] += sound
    mix = room(mix)
    mix = lp(hp(mix, 35, 1), 13000, 1)
    mix = np.tanh(1.4 * mix / np.max(np.abs(mix))) / np.tanh(1.4)
    mix = mix[:length]
    mix[-64:] *= np.linspace(1, 0, 64)
    # A touch of width: the room slightly different on each side.
    side = hp(np.roll(mix, 23), 3000) * 0.08
    write(f"{folder}/Sodium Break 174bpm.wav", [mix + side, mix - side])


def make_crackle(folder):
    n = int(round(8 * 16 * STEP))
    hiss = lp(hp(rng.normal(0, 1, n), 800, 1), 9000, 1) * 0.04
    pops = np.zeros(n)
    for at in rng.integers(0, n - 200, 1400):
        size = rng.uniform(0.1, 1.0) ** 3
        click = hp(rng.uniform(-1, 1, 60), 2000, 1) * np.exp(-np.arange(60) / 9) * size
        pops[at : at + 60] += click
    rumble = lp(rng.normal(0, 1, n), 60) * 0.6
    x = hiss + pops + rumble
    fade = 2400  # loops seamlessly: the end crossfades into the start
    x[:fade] = x[:fade] * np.linspace(0, 1, fade) + x[-fade:] * np.linspace(1, 0, fade)
    x = x[: n]
    write(f"{folder}/Sodium Crackle.wav", [x], peak_db=-6.0)


def make_impact(folder):
    t = t_axis(3.5)
    freq = 38 + 90 * np.exp(-t * 7)
    boom = np.sin(2 * np.pi * np.cumsum(freq) / RATE) * np.exp(-t * 1.6)
    crack = lp(rng.uniform(-1, 1, len(t)), 6000) * np.exp(-t * 22)
    left = boom + crack * 0.5 + lp(rng.uniform(-1, 1, len(t)), 2500) * np.exp(-t * 1.8) * 0.35
    right = boom + crack * 0.5 + lp(rng.uniform(-1, 1, len(t)), 2500) * np.exp(-t * 1.8) * 0.35
    write(f"{folder}/Sodium Impact.wav", [np.tanh(left * 1.5), np.tanh(right * 1.5)])


def make_reverse_cymbal(folder):
    n = int(round(32 * STEP))
    t = np.arange(n) / RATE
    channels = []
    for _ in range(2):
        metal = sum(np.sin(2 * np.pi * f * t + rng.uniform(0, 6)) for f in (3120, 4410, 5370, 6880, 8200))
        x = hp(rng.uniform(-1, 1, n) + metal * 0.08, 4000)
        decay = np.exp(-t[::-1] * 1.9)  # a cymbal's decay, backwards
        x = x * decay
        x[-240:] *= np.linspace(1, 0, 240)
        channels.append(x)
    write(f"{folder}/Sodium Reverse Cymbal.wav", channels, peak_db=-3.0)


if __name__ == "__main__":
    out = sys.argv[1]
    make_break(out)
    make_crackle(out)
    make_impact(out)
    make_reverse_cymbal(out)
