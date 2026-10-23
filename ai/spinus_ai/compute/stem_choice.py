"""Which of a many-instrument separation's stems to keep (the MVSep Mega 53 stems model).

That model gives every stem it knows, most of them silent for a given song, and some overlap: "drums" holds
the kick, snare, hi-hat and toms that also have stems of their own, "vocal" the lead and backing vocals, and
so on. Kept: the stems heard, without the same sound twice. Plain Python, so the AI service's tests can
check it without PyTorch.
"""

from __future__ import annotations

# A stem quieter than this, as a share of the song's energy, is not heard (about -35 dB).
HEARD = 10 ** (-35 / 10)

# A group and the stems inside it.
GROUPS: dict[str, tuple[str, ...]] = {
    "drums": ("kick", "snare", "hh", "toms"),
    "vocal": ("lead-vocal", "back-vocal"),
    "guitar": ("acoustic-guitar", "electric-guitar"),
    "bowed_strings": ("violin", "viola", "cello", "double-bass"),
    "strings": ("bowed_strings", "violin", "viola", "cello", "double-bass", "harp"),
    "brass": ("trumpet", "trombone", "french-horn", "tuba"),
    "woodwind": ("flute", "clarinet", "oboe", "bassoon", "saxophone"),
    "wind": (
        "brass",
        "woodwind",
        *("trumpet", "trombone", "french-horn", "tuba"),
        *("flute", "clarinet", "oboe", "bassoon", "saxophone", "harmonica"),
    ),
    "keys": ("piano", "digital-piano", "organ", "harpsichord"),
    "percussion": ("congas", "tambourine", "triangle", "timpani", "bells", "glockenspiel", "marimba", "wind-chimes"),
}

# Groups kept whole even when their parts are heard: Spinus turns a drums stem into a Drum Machine pattern.
KEEP_WHOLE = ("drums",)


def choose_stems(energy: dict[str, float], mix_energy: float) -> list[str]:
    """The stems to keep, in the model's order, from each stem's energy (sum of squares) and the song's."""
    if mix_energy <= 0:
        return []
    heard = {name for name, e in energy.items() if e / mix_energy >= HEARD}
    dropped: set[str] = set()
    # Biggest groups first, so "wind" gives way to "brass" before "brass" gives way to "trumpet".
    for group, parts in sorted(GROUPS.items(), key=lambda item: -len(item[1])):
        if group not in heard or group in dropped:
            continue
        inside = [p for p in parts if p in heard and p not in dropped]
        if not inside:
            continue
        if group in KEEP_WHOLE:
            dropped.update(inside)
        elif sum(energy[p] for p in inside) >= 0.5 * energy[group]:
            dropped.add(group)  # its parts hold most of it: they say more
        else:
            dropped.update(inside)  # mostly something else: the group, once
    return [name for name in energy if name in heard and name not in dropped]
