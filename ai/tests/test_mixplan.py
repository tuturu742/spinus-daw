"""Mix plan: the rules, the limits on every item, and the AI's review."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import httpx
from test_service import anthropic_reply, make_service, no_notify, run_job

from spinus_ai.mixplan import check, rules


def measured(loudness: float, peak: float, **bands: float) -> dict[str, Any]:
    levels = {"sub": -40.0, "bass": -30.0, "low mids": -10.0, "mids": 0.0, "high mids": -6.0, "presence": -10.0,
              "air": -20.0}  # fmt: skip
    levels.update({k.replace("_", " "): v for k, v in bands.items()})
    return {"loudness": loudness, "truePeak": peak, "crest": peak - loudness, "bands": levels, "width": 0.2}


MIX = {
    "inserts": {
        "Bass": measured(-14.0, -1.0, sub=-6.0, bass=0.0, mids=-12.0),  # peaks too high
        "Pad": measured(-20.0, -10.0, bass=-8.0),  # sits on the bass a little
        "Vocal": measured(-30.0, -6.0),  # very uneven: crest 24 dB; nothing low
        "Silent": measured(-80.0, -120.0),
    },
    "masking": [{"a": "Bass", "b": "Pad", "band": "bass", "amount": 0.8}],
}
MASTER = {"loudness": -18.0, "truePeak": -2.0, "bands": {}, "width": 0.3, "crest": 16.0}


def test_the_rules_find_headroom_masking_rumble_dynamics_and_loudness() -> None:
    items = rules(MIX, MASTER, "streaming")
    by_kind = {(i["kind"], i["insert"]): i for i in items}
    assert by_kind[("volume", "Bass")]["db"] == -5.0  # -1 dBTP to -6
    cut = by_kind[("eq", "Pad")]
    assert cut["bands"][0]["type"] == "Bell" and cut["bands"][0]["gain"] == -4.0
    assert "masks Bass" in cut["reason"] and cut["title"].startswith("Cut ")
    assert ("eq", "Bass") not in by_kind  # the bass keeps its low end
    assert any(i["kind"] == "eq" and i["insert"] == "Vocal" and i["bands"][0]["type"] == "Hi-pass" for i in items)
    assert by_kind[("compressor", "Vocal")]["ratio"] == 3.0
    assert by_kind[("limiter", "Master")]["gain"] == 4.0  # -18 to -14 LUFS
    assert not any(i["insert"] == "Silent" for i in items)
    assert not any(i["kind"] == "limiter" for i in rules(MIX, MASTER, "none"))


def test_every_item_is_checked_and_kept_inside_its_limits() -> None:
    names = {"Bass", "Master"}
    lowered = check({"kind": "volume", "insert": "Bass", "db": -40, "title": "t", "reason": "r"}, names)
    assert lowered is not None and lowered["db"] == -12.0
    assert check({"kind": "volume", "insert": "Nobody", "db": -1, "title": "t", "reason": "r"}, names) is None
    assert check({"kind": "volume", "insert": "Bass", "db": "loud", "title": "t", "reason": "r"}, names) is None
    assert check({"kind": "eq", "insert": "Bass", "bands": [{"type": "Wobble", "freq": 100}], "title": "t",
                  "reason": "r"}, names) is None  # fmt: skip
    eq = check({"kind": "eq", "insert": "Bass", "bands": [{"type": "Bell", "freq": 99999, "gain": -30}],
                "title": "t", "reason": "r"}, names)  # fmt: skip
    assert eq is not None and eq["bands"][0] == {"type": "Bell", "freq": 20000, "gain": -9.0, "q": 1.0}
    assert check({"kind": "limiter", "insert": "Bass", "gain": 3, "title": "t", "reason": "r"}, names) is None
    assert check({"kind": "delete", "insert": "Bass", "title": "t", "reason": "r"}, names) is None
    assert check({"kind": "volume", "insert": "Bass", "db": -1, "title": "", "reason": "r"}, names) is None


async def test_the_ai_reviews_the_plan_and_its_answer_is_checked(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    answer = {"items": [
        {"kind": "volume", "insert": "Bass", "db": -4.0, "title": "Lower the bass", "reason": "Peaks near 0 dB."},
        {"kind": "eq", "insert": "Pad", "title": "Cut 125 Hz on Pad", "reason": "Makes room for the bass.",
         "bands": [{"type": "Bell", "freq": 125, "gain": -3.0, "q": 1.2}]},
        {"kind": "volume", "insert": "Ghost", "db": 3, "title": "x", "reason": "y"},  # no such insert: dropped
        {"kind": "limiter", "insert": "Master", "gain": 12, "title": "Louder", "reason": "Louder!"},  # not its call
    ]}  # fmt: skip

    def handler(request: httpx.Request) -> httpx.Response:
        sent.append(json.loads(request.content))
        return httpx.Response(200, content=anthropic_reply("", ("write_mix_plan", answer)),
                              headers={"content-type": "text/event-stream"})  # fmt: skip

    service = make_service(tmp_path, handler)
    params = {"mix": MIX, "master": MASTER, "target": "streaming"}
    # No connection yet: the rules' plan, saying so.
    result = await run_job(service, "ai.v1.mix.plan", params)
    assert result["reviewedBy"] == "" and "no AI connection" in result["note"]
    assert any(i["kind"] == "compressor" for i in result["items"])

    await service.handle("ai.v1.connections.save", {"name": "Claude", "provider": "anthropic", "key": "sk-test"},
                         no_notify)  # fmt: skip
    result = await run_job(service, "ai.v1.mix.plan", params)
    assert result["reviewedBy"]
    assert [i["title"] for i in result["items"]] == ["Lower the bass", "Cut 125 Hz on Pad", "Master to -14 LUFS"]
    assert result["items"][2]["gain"] == 4.0  # the loudness target is the user's, not the AI's
    assert sent[0]["tools"][0]["name"] == "write_mix_plan"
    assert "masks Bass" in sent[0]["messages"][0]["content"][0]["text"]  # it saw the rules' plan

    result = await run_job(service, "ai.v1.mix.plan", {**params, "review": False})
    assert result["reviewedBy"] == "" and len(sent) == 1


def test_a_reference_asks_for_gentle_master_eq_before_the_limiter() -> None:
    master = {**MASTER, "bands": {"sub": -20.0, "bass": -10.0, "low mids": -4.0, "mids": 0.0, "high mids": -6.0,
                                  "presence": -12.0, "air": -24.0}}  # fmt: skip
    reference = {"bands": {"sub": -12.0, "bass": -2.0, "low mids": -5.0, "mids": 0.0, "high mids": -5.0,
                           "presence": -9.0, "air": -16.0}}  # fmt: skip
    items = rules({"inserts": {}, "masking": []}, master, "streaming", reference)
    kinds = [(i["kind"], i["insert"]) for i in items]
    assert kinds == [("eq", "Master"), ("limiter", "Master")]  # the EQ before the limiter
    eq = items[0]
    assert [b["type"] for b in eq["bands"]] == ["Lo-shelf", "Hi-shelf"]  # the sub and bass share a shelf
    assert all(abs(b["gain"]) <= 4.0 for b in eq["bands"]) and eq["bands"][0]["gain"] > 0
    assert "more" in eq["reason"] and check(eq, {"Master"}) is not None
    assert not any(i["kind"] == "eq" for i in rules({"inserts": {}, "masking": []}, master, "none", master))
