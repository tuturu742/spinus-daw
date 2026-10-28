"""Key names as musicians usually spell them: Bb minor and Eb major rather than A# minor and D# major. Every
key name Spinus writes comes from key_name; every one it reads goes through tonic_of, which takes either
spelling (older analyses in the library say "A# minor")."""

from __future__ import annotations

SHARPS = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
# The usual name of each tonic, per mode (F# major and C# minor, but Bb minor and Eb major).
MAJOR = ("C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B")
MINOR = ("C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B")
FLATS = {"Db": 1, "Eb": 3, "Gb": 6, "Ab": 8, "Bb": 10, "Cb": 11, "Fb": 4}


def key_name(tonic: int, scale: str) -> str:
    """'Bb minor' for (10, 'minor')."""
    return f"{(MINOR if scale == 'minor' else MAJOR)[tonic % 12]} {scale}"


def tonic_of(note: str) -> int | None:
    """0-11 for a note name in either spelling ('A#', 'Bb'), None otherwise."""
    if note in SHARPS:
        return SHARPS.index(note)
    return FLATS.get(note)


def spelled(name: str) -> str:
    """A key name ('A# minor') in its usual spelling ('Bb minor'); anything else as it is."""
    parts = name.split()
    if len(parts) != 2 or parts[1] not in ("major", "minor") or (tonic := tonic_of(parts[0])) is None:
        return name
    return key_name(tonic, parts[1])
