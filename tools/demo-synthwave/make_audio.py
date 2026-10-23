# Original sound effects for the "Neon Coast" demo song, synthesised here with numpy (no samples):
# ocean waves (a seamless 4-bar loop), a reverse cymbal swell (one bar) and a gated impact.
#   python3 make_audio.py <folder>      (needs numpy)
import sys
import wave

import numpy as np

rate = 48000
bpm = 104.0
bar = 4 * 60.0 / bpm
out = sys.argv[1]
rng = np.random.default_rng(1986)


def write(name, left, right, peak_db=-1.0):
    stereo = np.stack([left, right], axis=1)
    stereo *= 10 ** (peak_db / 20) / max(np.max(np.abs(stereo)), 1e-9)
    data = (np.clip(stereo, -1, 1) * 32767).astype("<i2")
    with wave.open(f"{out}/{name}", "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(data.tobytes())


def shaped_noise(n, shape):
    """Noise of n samples whose spectrum follows shape(frequencies): periodic, so it loops without a seam."""
    spectrum = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1 / rate)
    return np.fft.irfft(spectrum * shape(np.maximum(f, 1.0)), n)


# Ocean waves: two waves per four bars. Each wave is a dark rumble that rises, breaks into a brighter wash and
# fades; left and right are different noise, the right a little later, so the beach is wide.
n = int(round(rate * 4 * bar))
t = np.arange(n) / rate
period = 2 * bar
phase = (t % period) / period


def wave_envelope(p):
    rise = np.clip(p / 0.35, 0, 1) ** 2
    fall = np.exp(-np.clip(p - 0.35, 0, None) * 4.5)
    return rise * fall + 0.08


channels = []
for delay in (0.0, 0.06):
    p = ((t - delay * period) % period) / period
    env = wave_envelope(p)
    dark = shaped_noise(n, lambda f: 1 / np.sqrt(f) * (f < 900) * (f > 40))
    bright = shaped_noise(n, lambda f: (f > 1500) * (f < 9000) / np.sqrt(f / 1500))
    dark /= np.std(dark)
    bright /= np.std(bright)
    crest = np.clip((p - 0.3) / 0.1, 0, 1) * np.exp(-np.clip(p - 0.4, 0, None) * 6)  # the wave breaking
    channels.append(dark * env + bright * (0.35 * crest + 0.04))
write("Ocean Waves.wav", channels[0], channels[1], peak_db=-3.0)

# Reverse cymbal: a metallic shimmer (inharmonic partials over bright noise) that grows over one bar and
# stops dead on the next downbeat.
n = int(round(rate * bar))
t = np.arange(n) / rate
x = t / t[-1]
env = x**3.5
partials = [3150, 4270, 5110, 6630, 7380, 8920, 10450, 12100]
channels = []
for side in (0, 1):
    metal = sum(np.sin(2 * np.pi * f * (1 + 0.003 * side) * t + rng.uniform(0, 6.28)) / (1 + i * 0.3)
                for i, f in enumerate(partials))
    hiss = shaped_noise(n, lambda f: (f > 4000) * 1.0)
    hiss /= np.std(hiss)
    channels.append(env * (0.25 * metal + hiss))
write("Reverse Cymbal.wav", channels[0], channels[1], peak_db=-2.0)

# Gated impact: a falling sine boom, a noise crack and a big bright tail that a gate cuts off after 0.45 s,
# like the gated drums of the 1980s.
n = int(rate * 1.6)
t = np.arange(n) / rate
f = 32 + 90 * np.exp(-t * 12)
boom = np.sin(2 * np.pi * np.cumsum(f) / rate) * np.exp(-t * 3)
gate = np.clip((0.45 - t) / 0.03, 0, 1)
channels = []
for side in (0, 1):
    tail = shaped_noise(n, lambda f: (f > 300) * (f < 11000) / np.sqrt(f / 300))
    tail /= np.std(tail)
    crack = tail * np.exp(-t * 40)
    channels.append(boom * 1.1 + 0.5 * crack + 0.3 * tail * np.exp(-t * 2) * gate)
write("Gated Impact.wav", channels[0], channels[1], peak_db=-1.0)
