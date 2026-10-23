#!/usr/bin/env python3
"""Original sounds for the demo song "Rainy Window" (lo-fi hip hop, 82 BPM, D minor), all synthesised here
with numpy: no recordings, no samples from anywhere else.

    python3 make_audio.py <folder>

writes the song's audio clips to <folder> (three takes of a hummed line, an "ooh" vocal that Spinus Tune
corrects, and an electric-piano swell) and the instruments' sounds to <folder>/samples (the electric
piano the Sampler plays, the vinyl crackle and rain loops, and the Drum Machine's dusty kit). The
generator (lofi_song) loads the samples into the Sampler and the Drum Machine, which keep them in their
own saved state, and puts the clips in the project's audio/ folder.

The melodies here are the ones lofi_song.cpp writes for the guide channel that Spinus Tune follows: keep
them the same.
"""

import pathlib
import sys
import wave

import numpy as np

RATE = 44100
BPM = 82.0
BEAT = 60.0 / BPM
BAR = 4 * BEAT


def midi_hz(m):
    return 440.0 * 2.0 ** ((np.asarray(m, dtype=float) - 69.0) / 12.0)


def write(path, x, peak=0.89, rng=None):
    """16-bit WAV, peak-normalised; x is mono (n,) or (channels, n)."""
    x = np.atleast_2d(np.asarray(x, dtype=float))
    top = np.max(np.abs(x)) or 1.0
    x = x * (peak / top)
    rng = rng or np.random.default_rng(1)
    dither = (rng.random(x.shape) - rng.random(x.shape)) / 32768.0
    pcm = np.clip(np.round((x + dither) * 32767.0), -32768, 32767).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(x.shape[0])
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.T.tobytes())


#==============================================================================
# Filters in the frequency domain: zero-phase, and circular (so loops stay seamless) unless padded.

def spectral(x, gain, pad=True):
    n = len(x)
    size = 1 << int(np.ceil(np.log2(2 * n))) if pad else n
    spectrum = np.fft.rfft(x, size)
    f = np.fft.rfftfreq(size, 1.0 / RATE)
    return np.fft.irfft(spectrum * gain(f), size)[:n]


def lp(fc, order=2):
    return lambda f: 1.0 / np.sqrt(1.0 + (f / fc) ** (2 * order))


def hp(fc, order=2):
    return lambda f: 1.0 / np.sqrt(1.0 + (fc / np.maximum(f, 1e-3)) ** (2 * order))


def bp(lo, hi, order=2):
    return lambda f: lp(hi, order)(f) * hp(lo, order)(f)


def one_pole(x, seconds, rate):
    """A smoother for control curves (at `rate` points per second)."""
    a = np.exp(-1.0 / (seconds * rate))
    y = np.empty_like(x)
    acc = x[0]
    for i, v in enumerate(x):
        acc = a * acc + (1 - a) * v
        y[i] = acc
    return y


#==============================================================================
# The electric piano (a Rhodes-like tine piano): a 1:1 FM pair whose brightness falls as the note
# rings, a quick 1:14 "bark" on the attack, and the pickup's slight asymmetry.

def ep_note(m, seconds, velocity=0.8, detune_cents=0.0):
    n = int(seconds * RATE)
    t = np.arange(n) / RATE
    f = float(midi_hz(m)) * 2.0 ** (detune_cents / 1200.0)
    scale = 2.0 ** (-(m - 60) / 24.0)  # lower notes ring longer
    env = (0.5 * np.exp(-t / (0.7 * scale)) + 0.5 * np.exp(-t / (3.2 * scale))) * (1 - np.exp(-t / 0.0015))
    index = 1.9 * velocity * np.exp(-t / 0.18) + 0.7 * np.exp(-t / 1.8)
    w = 2 * np.pi * f * t
    body = np.sin(w + index * np.sin(w))
    bark = np.sin(w + 1.1 * velocity * np.exp(-t / 0.025) * np.sin(14.0 * w)) * np.exp(-t / 0.35)
    y = env * (body + 0.3 * bark)
    y = y + 0.25 * y * y  # the pickup
    y = spectral(y, hp(30, 1))
    fade = int(0.25 * RATE)
    y[-fade:] *= np.linspace(1, 0, fade) ** 2
    return y


def chord_swell(rng):
    """Two bars of a strummed Dm9 on the electric piano (the song's reversed and slowed clips)."""
    n = int(2 * BAR * RATE)
    y = np.zeros(n)
    for i, m in enumerate([50, 57, 60, 64, 65, 69]):
        start = int((0.035 * i) * RATE)
        note = ep_note(m, 2 * BAR - 0.035 * i, 0.75, rng.uniform(-6, 6))
        y[start:start + len(note)] += note[: n - start]
    y *= np.minimum(1.0, (n - np.arange(n)) / (0.3 * RATE))
    return y


#==============================================================================
# Loops for the Sampler: seamless, so a held note can loop them for as long as it lasts.

def periodic_noise(n, rng, gain):
    return spectral(rng.standard_normal(n), gain, pad=False)


def impulses(n, rate_per_second, amplitude, rng):
    x = np.zeros(n)
    count = rng.poisson(rate_per_second * n / RATE)
    at = rng.integers(0, n, count)
    np.add.at(x, at, amplitude(count) * rng.choice([-1.0, 1.0], count))
    return x


def vinyl_crackle(rng, seconds=4.0):
    n = int(seconds * RATE)
    shared_ticks = impulses(n, 45, lambda c: np.minimum(0.7, 0.05 * rng.pareto(2.5, c)), rng)
    shared_pops = impulses(n, 0.9, lambda c: rng.uniform(0.25, 0.7, c), rng)
    channels = []
    for _ in range(2):
        own = impulses(n, 10, lambda c: np.minimum(0.5, 0.04 * rng.pareto(2.5, c)), rng)
        ticks = spectral(shared_ticks * rng.uniform(0.7, 1.0) + own, bp(1500, 9000, 2), pad=False)
        pops = spectral(shared_pops, bp(150, 2500, 1), pad=False) * 0.9
        hiss = periodic_noise(n, rng, bp(800, 7000, 1)) * 0.02
        channels.append(ticks + pops + hiss)
    return np.array(channels)


def rain(rng, seconds=6.0):
    n = int(seconds * RATE)
    t = np.arange(n) / RATE
    channels = []
    for ch in range(2):
        pink = periodic_noise(n, rng, lambda f: bp(250, 6000, 1)(f) / np.sqrt(np.maximum(f, 20.0) / 250.0))
        swell = 1.0 + 0.15 * np.sin(2 * np.pi * 2 * t / seconds + ch) + 0.1 * np.sin(2 * np.pi * 5 * t / seconds)
        drops = spectral(impulses(n, 160, lambda c: rng.uniform(0.05, 0.3, c), rng), bp(2000, 7000, 2), pad=False)
        y = pink * swell * 0.5 + drops * 0.6
        # Drops on the window: short, high plinks that rise a little in pitch.
        for _ in range(rng.poisson(1.6 * seconds)):
            at = rng.integers(0, n)
            length = int(0.04 * RATE)
            tt = np.arange(length) / RATE
            f = rng.uniform(1700, 3400)
            plink = np.sin(2 * np.pi * f * (tt + 4.0 * tt * tt)) * np.exp(-tt / 0.008) * rng.uniform(0.08, 0.22)
            idx = (at + np.arange(length)) % n
            y[idx] += plink
        channels.append(y)
    return np.array(channels)


#==============================================================================
# The dusty drum kit (for the Drum Machine's pads): soft, dark, a little crunchy.

def crunch(y, bits=12):
    q = 2.0 ** (bits - 1)
    return np.round(y / np.max(np.abs(y)) * q) / q


def kick():
    t = np.arange(int(0.55 * RATE)) / RATE
    f = 47 + 95 * np.exp(-t / 0.03)
    body = np.sin(2 * np.pi * np.cumsum(f) / RATE) * np.exp(-t / 0.26) * (1 - np.exp(-t / 0.0008))
    click = spectral(np.random.default_rng(3).standard_normal(len(t)) * np.exp(-t / 0.004), lp(3500, 2)) * 0.25
    y = np.tanh(2.0 * (body + click)) / np.tanh(2.0)
    return crunch(spectral(y, lp(5000, 2)))


def snare(rng):
    t = np.arange(int(0.6 * RATE)) / RATE
    tone = 0.55 * np.sin(2 * np.pi * 182 * t) * np.exp(-t / 0.05) + 0.3 * np.sin(2 * np.pi * 324 * t) * np.exp(-t / 0.035)
    noise = spectral(rng.standard_normal(len(t)), bp(1100, 7500, 2)) * np.exp(-t / 0.13) * 0.9
    dry = (tone + noise) * (1 - np.exp(-t / 0.0007))
    room = spectral(rng.standard_normal(len(t)) * np.exp(-t / 0.09), bp(400, 5000, 1))
    size = 1 << int(np.ceil(np.log2(2 * len(t))))
    wet = np.fft.irfft(np.fft.rfft(dry, size) * np.fft.rfft(room, size), size)[: len(t)] * 0.012
    return crunch(spectral(dry + wet, lp(8500, 2)))


def rim(rng):
    t = np.arange(int(0.25 * RATE)) / RATE
    click = spectral(rng.standard_normal(len(t)) * np.exp(-t / 0.002), bp(1500, 6000, 2))
    tone = 0.7 * np.sin(2 * np.pi * 820 * t) * np.exp(-t / 0.016) + 0.35 * np.sin(2 * np.pi * 1630 * t) * np.exp(-t / 0.009)
    return crunch(click * 0.8 + tone)


def hat(rng, decay, seconds):
    t = np.arange(int(seconds * RATE)) / RATE
    metal = sum(np.sign(np.sin(2 * np.pi * f * t + rng.uniform(0, 6.28))) for f in (205.3, 304.4, 369.6, 522.7, 540.0, 800.0))
    noise = rng.standard_normal(len(t))
    y = spectral(metal * 0.35 + noise * 0.6, bp(6500, 11000, 2)) * np.exp(-t / decay) * (1 - np.exp(-t / 0.0005))
    return crunch(y)


def shaker(rng):
    t = np.arange(int(0.22 * RATE)) / RATE
    env = np.where(t < 0.014, t / 0.014, np.exp(-(t - 0.014) / 0.045))
    return crunch(spectral(rng.standard_normal(len(t)), bp(4000, 10000, 2)) * env)


#==============================================================================
# Voices: a sung "ooh" and a closed-mouth hum, from harmonics shaped by the vowel's formants, with
# vibrato, slow pitch drift, scoops into notes, breath, and the singer's mistakes.

VOWELS = {
    # (formant Hz, bandwidth Hz, level)
    "ooh": [(330, 90, 1.0), (820, 110, 0.32), (2450, 160, 0.06), (3300, 220, 0.03)],
    "hum": [(260, 70, 1.0), (1050, 220, 0.05), (2300, 300, 0.015)],
}


def voice(notes, seconds, vowel, rng, breath=0.004):
    """notes: (start beat, length in beats, MIDI pitch, cents off, scoop in cents, timing error in beats)."""
    control = 1000  # control points per second
    nc = int(seconds * control) + 1
    pitch = np.full(nc, np.nan)
    gate = np.zeros(nc)
    scoop = np.zeros(nc)
    vib_amount = np.zeros(nc)
    tc = np.arange(nc) / control
    for start, length, m, cents, scoop_cents, late in notes:
        s = (start + late) * BEAT
        e = s + length * BEAT
        on = (tc >= s) & (tc < e)
        pitch[on] = m + cents / 100.0
        gate[on] = 1.0
        since = tc[on] - s
        scoop[on] = -scoop_cents / 100.0 * np.exp(-since / 0.07)
        vib_amount[on] = np.clip((since - 0.25) / 0.4, 0, 1) * min(1.0, length / 1.5)
    # Between notes the pitch holds (and glides into the next note through the smoothing).
    filled = pitch.copy()
    last = np.nan
    for i in range(nc):
        if np.isnan(filled[i]):
            filled[i] = last
        else:
            last = filled[i]
    first = np.flatnonzero(~np.isnan(filled))[0]
    filled[:first] = filled[first]
    drift = one_pole(rng.standard_normal(nc), 0.35, control)
    drift = drift / (np.std(drift) or 1.0) * 0.12  # semitones
    vibrato = 0.28 * vib_amount * np.sin(2 * np.pi * 5.3 * tc + rng.uniform(0, 6.28))
    semitones = one_pole(filled, 0.03, control) + scoop + drift + vibrato
    amp = one_pole(one_pole(gate, 0.05, control), 0.04, control)

    n = int(seconds * RATE)
    t = np.arange(n) / RATE
    f0 = np.interp(t, tc, midi_hz(semitones))
    a = np.interp(t, tc, amp)
    phase = 2 * np.pi * np.cumsum(f0) / RATE
    y = np.zeros(n)
    formants = VOWELS[vowel]
    for k in range(1, 80):
        fk = k * f0
        if fk.min() > 9000:
            break
        level = sum(g / (1.0 + ((fk - fc) / (bw / 2)) ** 2) for fc, bw, g in formants) + 0.002
        level = level * k ** -0.6 * (fk < 9000)
        y += level * np.sin(k * phase)
    noise = spectral(rng.standard_normal(n), bp(900, 6000, 1)) * breath / 0.05
    attack = np.interp(t, tc, np.clip(np.gradient(amp) * control * 0.02, 0, 1))
    y = y * a + noise * (0.3 * a + attack)
    return spectral(y, bp(70, 9000, 1))


# The melodies, in beats from the clip's start (lofi_song.cpp has the same ones).
OOH_MELODY = [  # over Bbmaj9 | Am7 | Dm9 | Gm9 C13
    (0.0, 1.75, 69), (2.0, 1.5, 65), (3.5, 0.5, 67),
    (4.0, 1.5, 64), (5.5, 2.25, 67),
    (8.0, 2.5, 69), (10.5, 0.5, 67), (11.0, 1.0, 72),
    (12.0, 1.75, 70), (14.0, 1.75, 69),
]
HUM_MELODY = [  # over Gm9 | C13 | Fmaj9 | Bbmaj7 A7(b13), an octave lower (a man humming)
    (0.5, 1.0, 57), (1.5, 0.5, 58), (2.0, 1.5, 57), (3.5, 0.5, 53),
    (4.0, 1.0, 55), (5.0, 0.5, 57), (5.5, 2.0, 52),
    (8.5, 1.0, 60), (9.5, 0.5, 57), (10.0, 1.5, 55), (11.5, 0.5, 52),
    (12.0, 1.5, 53), (13.5, 0.5, 50), (14.0, 1.75, 49),
]


def sung(melody, rng, spread_cents, scoop_cents, flaws):
    """The melody as one try sings it: each note a little off; `flaws` {note index: (cents, late)}."""
    out = []
    for i, (s, l, m) in enumerate(melody):
        cents, late = flaws.get(i, (rng.uniform(-spread_cents, spread_cents), rng.uniform(-0.02, 0.03)))
        out.append((s, l, m, cents, rng.uniform(0.5, 1.0) * scoop_cents, late))
    return out


def take(rng, flaws, crack_at=None, cough_at=None):
    seconds = 4 * BAR + 0.5 * BEAT
    y = voice(sung(HUM_MELODY, rng, 12, 60, flaws), seconds, "hum", rng)
    n = len(y)
    t = np.arange(n) / RATE
    if crack_at is not None:  # the voice breaks: a burst of rough noise and a jump up
        s = crack_at * BEAT
        burst = (t > s) & (t < s + 0.18)
        y[burst] += spectral(rng.standard_normal(n), bp(300, 3000, 1))[burst] * 0.25
    if cough_at is not None:  # a breath too close to the microphone
        s = cough_at * BEAT
        env = np.where(t > s, np.exp(-(t - s) / 0.12), 0.0) * (t < s + 0.6)
        y += spectral(rng.standard_normal(n), bp(200, 4000, 1)) * env * 0.35
    return y


def main():
    out = pathlib.Path(sys.argv[1])
    samples = out / "samples"
    samples.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(82)

    # The Sampler's electric piano: C4, C5 and C6 (Spinus's note names: C5 is middle C).
    for m, name in ((48, "C4"), (60, "C5"), (72, "C6")):
        write(samples / f"Rainy Keys_{name}.wav", ep_note(m, 4.5, 0.8), 0.8)
    write(samples / "Vinyl Crackle.wav", vinyl_crackle(rng), 0.8)
    write(samples / "Rain on the Window.wav", rain(rng), 0.8)
    write(samples / "Dusty Kick.wav", kick(), 0.89)
    write(samples / "Dusty Snare.wav", snare(rng), 0.89)
    write(samples / "Dusty Rim.wav", rim(rng), 0.8)
    write(samples / "Dusty Hat.wav", hat(rng, 0.03, 0.2), 0.7)
    write(samples / "Dusty Open Hat.wav", hat(rng, 0.28, 0.9), 0.7)
    write(samples / "Dusty Shaker.wav", shaker(rng), 0.7)

    # The audio clips. Three takes of the hummed line, each with its own mistakes.
    write(out / "Hum Take 1.wav", take(rng, {8: (-55, 0.06), 9: (-60, 0.05), 10: (-45, 0.04)}), 0.7)
    write(out / "Hum Take 2.wav", take(rng, {0: (48, 0.12), 1: (40, 0.08)}, crack_at=14.3), 0.7)
    write(out / "Hum Take 3.wav", take(rng, {}, cough_at=5.4), 0.7)
    # The "ooh" vocal: sung flat and sharp, with scoops into its notes, for Spinus Tune to correct.
    errors = [35, -40, 25, -35, 45, -30, 30, -45, 40, -25]
    ooh = voice([(s, l, m, errors[i], 110, rng.uniform(-0.02, 0.03)) for i, (s, l, m) in enumerate(OOH_MELODY)],
                4 * BAR + 0.5 * BEAT, "ooh", rng, breath=0.008)
    write(out / "Ooh Vocal.wav", ooh, 0.7)
    write(out / "Keys Swell.wav", chord_swell(rng), 0.8)


if __name__ == "__main__":
    main()
