"""What a learned instrument's keys do (Learn this instrument), as Spinus tells the AI service.

Spinus sends a learned instrument's keys as InstrumentProfiles::describeForAgents writes them:
{modes: [{name, enter?, goodFor?, voices?, plays: [[lo, hi]], playsInWords, switches: [{key, note, what}],
special: [...]}]}. The music tools and the pattern generator both keep notes on keys that play with these.
"""

from __future__ import annotations

import re
from collections.abc import Callable
from typing import Any


def fit_key(pitch: int, plays: set[int]) -> int:
    """A note moved by octaves onto a key that plays, as few as it takes (up before down when as far), or the
    nearest key that plays when no octave of it does (InstrumentProfiles' PlayableKeys::fit)."""
    if not plays or pitch in plays:
        return pitch
    for octaves in range(1, 11):
        for candidate in (pitch + 12 * octaves, pitch - 12 * octaves):
            if candidate in plays:
                return candidate
    return min(plays, key=lambda k: (abs(k - pitch), -k))


def switch_word(what: str) -> str:
    """A switch key's name as one word, to write it as a note's pitch: "palm mute" -> "palm-mute"."""
    return re.sub(r"\s+", "-", what.strip().lower())


def first_mode(keys: dict[str, Any] | None) -> dict[str, Any]:
    """The mode the instrument opens in, or nothing when its keys aren't known."""
    modes = (keys or {}).get("modes") or []
    return modes[0] if modes and isinstance(modes[0], dict) else {}


def plays(keys: dict[str, Any] | None) -> set[int]:
    """The keys the instrument plays as notes as it opens (its first mode's); empty when not known."""
    out: set[int] = set()
    for run in first_mode(keys).get("plays") or []:
        if isinstance(run, list) and len(run) == 2:
            out.update(range(int(run[0]), int(run[1]) + 1))
    return out


def switch_keys(keys: dict[str, Any] | None) -> dict[str, int]:
    """The first mode's switch and special keys by name ("palm-mute": 12), to write one on purpose."""
    mode = first_mode(keys)
    out: dict[str, int] = {}
    for k in [*(mode.get("switches") or []), *(mode.get("special") or [])]:
        if isinstance(k, dict) and k.get("what"):
            out.setdefault(switch_word(str(k["what"])), int(k.get("key", 0)))
    return out


def in_words(keys: dict[str, Any], note_name: Callable[[int], str]) -> list[str]:
    """Each mode in a line: 'Normal (one note at a time): plays E2 to G6; switches: palm-mute (C1)', with notes
    named by `note_name` (the music tools name C5 = 60, as FL does; the pattern generator C4 = 60)."""
    out = []
    for mode in keys.get("modes") or []:
        if not isinstance(mode, dict):
            continue
        head = str(mode.get("name") or "Normal")
        voices = {"chords": "chords", "one": "one note at a time"}.get(str(mode.get("voices")))
        how = [x for x in (voices, str(mode.get("goodFor") or "")) if x]
        if mode.get("enter"):
            how.append(f"entered with {mode['enter']}")
        line = head + (f" ({'; '.join(how)})" if how else "")
        runs = [r for r in mode.get("plays") or [] if isinstance(r, list) and len(r) == 2]
        said = ", ".join(f"{note_name(int(a))} to {note_name(int(b))}" for a, b in runs)
        line += f": plays {said or 'keys not known'}"
        for kind in ("switches", "special"):
            named = [
                f"{switch_word(str(k.get('what') or kind))} ({note_name(int(k.get('key', 0)))})"
                for k in mode.get(kind) or []
                if isinstance(k, dict)
            ]
            if named:
                line += f"; {kind}: {', '.join(named)}"
        out.append(line)
    return out
