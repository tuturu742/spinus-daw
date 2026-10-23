"""Mix plan: changes to a mix with a reason each, from the measurements of spinus_ai.mixing.

Rules come first and work without any AI connection: headroom, masking (an EQ cut on the part that
needs the band less), mud and rumble on parts with nothing useful down there, gentle compression of very
uneven parts, and the master's loudness. When a connection is set up, an LLM reviews the rules' plan
with the measurements: it may drop items, change amounts within limits, add a few items of the same kinds
and word the reasons better. Its answer is checked and limited here; nothing it writes reaches the project
except through these items, which Spinus turns into commands the user sees and can untick.

Items:
  {"kind": "volume", "insert", "db"}                      change of the insert's fader, dB
  {"kind": "pan", "insert", "pan"}                        new pan, -1 to 1
  {"kind": "eq", "insert", "bands": [{"type", "freq", "gain", "q"}]}   type: Bell, Hi-pass, Lo-shelf, Hi-shelf
  {"kind": "compressor", "insert", "threshold", "ratio", "attack", "release", "makeup"}   dB, ms
  {"kind": "limiter", "insert", "threshold", "gain"}      on the master: input gain to reach the loudness
Each also has "title" (a few words) and "reason" (one sentence a musician understands).
"""

from __future__ import annotations

import math
from typing import Any

import httpx

from .assistant import NoConnection, Say, complete_with_fallback
from .connections import ConnectionStore
from .gateway import Message, Request, Text, Tool
from .mixing import BANDS
from .routing import Routing
from .usage import UsageLog

TARGETS = {"streaming": -14.0, "club": -9.0}
BAND_CENTRES = {name: math.sqrt(lo * hi) for name, lo, hi in BANDS}
EQ_TYPES = ("Bell", "Hi-pass", "Lo-shelf", "Hi-shelf")
MOST_ITEMS = 24


def _fmt_freq(hz: float) -> str:
    return f"{hz / 1000:.1f} kHz" if hz >= 1000 else f"{round(hz)} Hz"


def _and(words: list[str]) -> str:
    return words[0] if len(words) == 1 else ", ".join(words[:-1]) + " and " + words[-1]


def rules(
    mix: dict[str, Any], master: dict[str, Any] | None, target: str, reference: dict[str, Any] | None = None
) -> list[dict[str, Any]]:
    """The rules' plan. `mix` is spinus_ai.mixing.analyze_mix's result for the inserts; `master` the same
    measurements for the whole mix (or None); `target` "streaming", "club" or "none"; `reference` the
    measurements of a song whose tonal balance to match (or None)."""
    inserts: dict[str, dict[str, Any]] = mix.get("inserts", {})
    items: list[dict[str, Any]] = []
    sounding = {name: m for name, m in inserts.items() if m["loudness"] > -70}

    # Headroom: a part peaking near 0 dB leaves the master nothing to work with.
    for name, m in sounding.items():
        if m["truePeak"] > -3.0:
            change = round(-6.0 - m["truePeak"], 1)
            items.append({"kind": "volume", "insert": name, "db": change, "title": f"Lower {name}",
                          "reason": f"{name} peaks at {m['truePeak']:.1f} dB, leaving no headroom; "
                                    f"{-change:.1f} dB lower gives the mix room."})  # fmt: skip

    # Masking: cut the band on the part that needs it less (where it is a smaller share of its sound); one
    # EQ per part, with up to three cuts.
    cuts: dict[str, list[tuple[str, str, float]]] = {}
    for clash in mix.get("masking", [])[:8]:
        a, b, band = clash["a"], clash["b"], clash["band"]
        if a not in sounding or b not in sounding:
            continue
        share_a, share_b = sounding[a]["bands"][band], sounding[b]["bands"][band]
        keep, give = (a, b) if share_a >= share_b else (b, a)
        if any(c[0] == band for c in cuts.get(give, [])) or len(cuts.get(give, [])) >= 3:
            continue
        cuts.setdefault(give, []).append((band, keep, clash["amount"]))
    for give, found in cuts.items():
        found.sort(key=lambda c: BAND_CENTRES[c[0]])
        freqs = [_fmt_freq(BAND_CENTRES[band]) for band, _, _ in found]
        keepers: dict[str, list[str]] = {}
        for band, keep, _ in found:
            keepers.setdefault(keep, []).append(band)
        where = _and([f"{keep} in the {_and(bands)}" for keep, bands in keepers.items()])
        needs = "needs" if len(keepers) == 1 else "need"
        items.append({"kind": "eq", "insert": give, "title": f"Cut {_and(freqs)} on {give}",
                      "bands": [{"type": "Bell", "freq": round(BAND_CENTRES[band]),
                                 "gain": -4.0 if amount >= 0.7 else -3.0, "q": 1.0} for band, _, amount in found],
                      "reason": f"{give} masks {where}, which {needs} the room more."})  # fmt: skip

    # Rumble: parts with almost nothing in the sub and bass bands still add low-end mud and noise.
    for name, m in sounding.items():
        if m["bands"]["sub"] < -30 and m["bands"]["bass"] < -24:
            items.append({"kind": "eq", "insert": name, "title": f"High-pass {name}",
                          "bands": [{"type": "Hi-pass", "freq": 120, "gain": 0.0, "q": 0.7}],
                          "reason": f"{name} has nothing useful below 120 Hz; a high-pass clears rumble "
                                    "from the low end."})  # fmt: skip

    # Very uneven parts: gentle compression.
    for name, m in sounding.items():
        if m["crest"] > 20.0:
            items.append({"kind": "compressor", "insert": name, "title": f"Compress {name} gently",
                          "threshold": round(m["loudness"] + 6.0, 1), "ratio": 3.0, "attack": 15.0,
                          "release": 150.0, "makeup": 2.0,
                          "reason": f"{name}'s peaks are {m['crest']:.0f} dB above its average; gentle "
                                    "compression evens it out so it sits in the mix."})  # fmt: skip

    # A reference's tonal balance: gentle master EQ where the mix's balance differs by more than 3 dB (shelves
    # at the ends, bells between), at most three bands and 4 dB each; before the limiter.
    if master is not None and reference is not None and master["loudness"] > -70:
        differences = []
        for name, _, _ in BANDS:
            if name in master["bands"] and name in reference.get("bands", {}):
                d = float(reference["bands"][name]) - float(master["bands"][name])
                if abs(d) > 3.0:
                    differences.append((abs(d), name, d))
        chosen = sorted(differences, reverse=True)[:3]
        low = [d for _, name, d in chosen if name in ("sub", "bass")]
        bands = []
        if low:  # the sub and the bass share one low shelf
            shelf = round(max(-4.0, min(4.0, 0.6 * sum(low) / len(low))), 1)
            bands.append({"type": "Lo-shelf", "freq": 100, "gain": shelf, "q": 0.7})
        for _, name, d in chosen:
            gain = round(max(-4.0, min(4.0, d * 0.6)), 1)
            if name == "air":
                bands.append({"type": "Hi-shelf", "freq": 8000, "gain": gain, "q": 0.7})
            elif name not in ("sub", "bass"):
                bands.append({"type": "Bell", "freq": round(BAND_CENTRES[name]), "gain": gain, "q": 0.8})
        if bands:
            names = [n for _, n, _ in chosen]
            more = [n for _, n, d in chosen if d > 0]
            less = [n for _, n, d in chosen if d < 0]
            told = " and ".join(
                p for p in (f"more {_and(more)}" if more else "", f"less {_and(less)}" if less else "") if p
            )
            items.append({"kind": "eq", "insert": "Master", "title": "Match the reference's tone",
                          "bands": bands,
                          "reason": f"Next to the reference, the mix needs {told}: a gentle EQ on the master "
                                    f"moves the {_and(names)} towards it."})  # fmt: skip

    # The master's loudness.
    if master is not None and target in TARGETS and master["loudness"] > -70:
        wanted = TARGETS[target]
        gain = round(wanted - master["loudness"], 1)
        if abs(gain) >= 0.5:
            items.append({"kind": "limiter", "insert": "Master", "threshold": -1.0, "gain": max(-12.0, min(12.0, gain)),
                          "title": f"Master to {wanted:.0f} LUFS",
                          "reason": f"The mix measures {master['loudness']:.1f} LUFS; a limiter with "
                                    f"{gain:+.1f} dB in, peaks held at -1 dB, brings it to about {wanted:.0f} LUFS "
                                    f"for {target}."})  # fmt: skip
    return items[:MOST_ITEMS]


def _number(value: Any, low: float, high: float) -> float | None:
    try:
        v = float(value)
    except (TypeError, ValueError):
        return None
    if math.isnan(v):
        return None
    return max(low, min(high, v))


def check(item: dict[str, Any], names: set[str]) -> dict[str, Any] | None:
    """An item as Spinus will use it, with every number inside its limits; None if unusable."""
    kind = item.get("kind")
    insert = str(item.get("insert", ""))
    if insert not in names:
        return None
    clean: dict[str, Any] = {"kind": kind, "insert": insert, "title": str(item.get("title", ""))[:80],
                             "reason": str(item.get("reason", ""))[:300]}  # fmt: skip
    if not clean["title"] or not clean["reason"]:
        return None
    if kind == "volume":
        db = _number(item.get("db"), -12.0, 6.0)
        return None if db is None else {**clean, "db": round(db, 1)}
    if kind == "pan":
        pan = _number(item.get("pan"), -1.0, 1.0)
        return None if pan is None else {**clean, "pan": round(pan, 2)}
    if kind == "eq":
        bands = []
        for band in list(item.get("bands", []))[:4]:
            kind_ = str(band.get("type", ""))
            freq = _number(band.get("freq"), 20.0, 20000.0)
            gain = _number(band.get("gain", 0.0), -9.0, 6.0)
            q = _number(band.get("q", 1.0), 0.3, 8.0)
            if kind_ in EQ_TYPES and freq is not None and gain is not None and q is not None:
                bands.append({"type": kind_, "freq": round(freq), "gain": round(gain, 1), "q": round(q, 2)})
        return {**clean, "bands": bands} if bands else None
    if kind == "compressor":
        values = {
            "threshold": _number(item.get("threshold"), -60.0, 0.0),
            "ratio": _number(item.get("ratio"), 1.0, 10.0),
            "attack": _number(item.get("attack", 15.0), 0.5, 200.0),
            "release": _number(item.get("release", 150.0), 10.0, 2000.0),
            "makeup": _number(item.get("makeup", 0.0), 0.0, 12.0),
        }
        if any(v is None for v in values.values()):
            return None
        return {**clean, **{k: round(float(v or 0.0), 1) for k, v in values.items()}}
    if kind == "limiter":
        threshold = _number(item.get("threshold", -1.0), -6.0, 0.0)
        gain = _number(item.get("gain"), -12.0, 12.0)
        if threshold is None or gain is None or insert != "Master":
            return None
        return {**clean, "threshold": round(threshold, 1), "gain": round(gain, 1)}
    return None


SYSTEM = """You are a mixing engineer reviewing an automatic mix plan in Spinus, a digital audio workstation. \
You get each insert's measurements (loudness in LUFS, true peak in dBTP, crest factor, the level of each \
frequency band relative to its loudest band, stereo width), which inserts mask each other where, and the \
plan the rules made. Review it like a careful engineer: keep what helps, drop what would hurt (never cut \
what defines a part's sound: a kick's low end, a vocal's presence), adjust amounts, add at most four \
items of the same kinds where the measurements clearly call for them, and write each reason as one \
sentence a musician understands, naming the parts and the frequency. Prefer small moves (EQ within \
±4 dB). Do not touch the master's loudness item except to word it. Answer only by calling write_mix_plan \
once with the complete plan."""

ITEM_SCHEMA = {
    "type": "object",
    "properties": {
        "kind": {"type": "string", "enum": ["volume", "pan", "eq", "compressor", "limiter"]},
        "insert": {"type": "string", "description": "The insert's name, exactly as given"},
        "title": {"type": "string"},
        "reason": {"type": "string"},
        "db": {"type": "number", "description": "volume: change in dB"},
        "pan": {"type": "number", "description": "pan: -1 (left) to 1 (right)"},
        "bands": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "type": {"type": "string", "enum": list(EQ_TYPES)},
                    "freq": {"type": "number"},
                    "gain": {"type": "number"},
                    "q": {"type": "number"},
                },
                "required": ["type", "freq"],
            },
        },
        "threshold": {"type": "number"},
        "ratio": {"type": "number"},
        "attack": {"type": "number", "description": "ms"},
        "release": {"type": "number", "description": "ms"},
        "makeup": {"type": "number"},
        "gain": {"type": "number", "description": "limiter: input gain in dB"},
    },
    "required": ["kind", "insert", "title", "reason"],
}
WRITE_MIX_PLAN = Tool(
    "write_mix_plan",
    "Write the reviewed mix plan: every item to keep, change or add.",
    {"type": "object", "properties": {"items": {"type": "array", "items": ITEM_SCHEMA}}, "required": ["items"]},
)


def describe(mix: dict[str, Any], master: dict[str, Any] | None, target: str, plan: list[dict[str, Any]]) -> str:
    import json

    return json.dumps({"inserts": mix.get("inserts", {}), "masking": mix.get("masking", []), "master": master,
                       "loudnessTarget": target, "plan": plan}, indent=1)  # fmt: skip


async def review(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routing: Routing,
    usage_log: UsageLog,
    mix: dict[str, Any],
    master: dict[str, Any] | None,
    target: str,
    plan: list[dict[str, Any]],
    say: Say,
) -> tuple[list[dict[str, Any]], str]:
    """The plan as an LLM reviewed it, checked; and the model's name. NoConnection without one."""
    routes = routing.resolve("mixing")
    if not routes:
        raise NoConnection("No AI connection is set up.")

    async def no_text(_: str) -> None:
        pass

    await say({"kind": "status", "text": "The AI is reviewing the plan..."})
    request = Request(routes[0].model, SYSTEM, [Message("user", [Text(describe(mix, master, target, plan))])],
                      [WRITE_MIX_PLAN], 4000)  # fmt: skip
    completion, index = await complete_with_fallback(client, connections, routes, 0, request, no_text, say)
    usage_log.add(routes[index].connection.id, "mixing", completion.usage)
    names = set(mix.get("inserts", {})) | {"Master"}
    reviewed: list[dict[str, Any]] = []
    for use in completion.tool_uses:
        if use.name == "write_mix_plan":
            for item in list(use.input.get("items", []))[:MOST_ITEMS]:
                if isinstance(item, dict) and (checked := check(item, names)) is not None:
                    reviewed.append(checked)
    # The loudness target is the user's choice: keep the rules' item whatever the review did with it.
    loudness = [i for i in plan if i["kind"] == "limiter"]
    reviewed = [i for i in reviewed if i["kind"] != "limiter"] + loudness
    return reviewed, completion.model
