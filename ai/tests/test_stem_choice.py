"""Which of a many-instrument separation's stems are kept."""

from __future__ import annotations

from spinus_ai.compute.stem_choice import choose_stems
from spinus_ai.compute.weights import STEMS_53, find_model


def song(**heard: float) -> dict[str, float]:
    """Every stem of the model, silent unless given (energies against a song of energy 1)."""
    return {name: heard.get(name.replace("-", "_"), 1e-9) for name in STEMS_53}


def test_silent_stems_are_left_out() -> None:
    kept = choose_stems(song(drums=0.3, bass=0.2, synth=0.1, flute=0.0001), 1.0)
    assert kept == ["bass", "drums", "synth"]  # the model's order; the flute is too quiet to count


def test_drums_stay_whole() -> None:
    kept = choose_stems(song(drums=0.4, kick=0.2, snare=0.1, hh=0.05), 1.0)
    assert kept == ["drums"]


def test_parts_replace_their_group_when_they_hold_most_of_it() -> None:
    kept = choose_stems(song(vocal=0.3, lead_vocal=0.2, back_vocal=0.08), 1.0)
    assert kept == ["back-vocal", "lead-vocal"]
    # A guitar heard mostly as "guitar": kept once, as the group.
    assert choose_stems(song(guitar=0.3, electric_guitar=0.02), 1.0) == ["guitar"]


def test_groups_inside_groups() -> None:
    kept = choose_stems(song(wind=0.2, brass=0.15, trumpet=0.1, trombone=0.04, woodwind=0.01), 1.0)
    assert kept == ["trombone", "trumpet", "woodwind"]


def test_nothing_from_silence() -> None:
    assert choose_stems(song(), 0.0) == []


def test_the_model_is_registered_as_optional_and_many_instruments() -> None:
    model = find_model("mvsep_mega_53")
    assert model is not None
    assert model.architecture == "bs_roformer"
    assert len(model.stems) == 53
    assert model.license == "MIT"
    assert model.file_url(model.files[1]).endswith("/v1.0.21/mvsep_mega_model_bs_roformer_53_stems_v1.ckpt")
    assert [f.path for f in model.files] == ["config.yaml", "model.ckpt"]


def test_every_model_has_a_page_to_check_its_license() -> None:
    from spinus_ai.compute.weights import MODELS

    for model in MODELS:
        assert model.page.startswith("https://")
        assert model.to_json()["page"] == model.page
    mega = find_model("mvsep_mega_53")
    assert mega is not None and mega.page.endswith("/releases/tag/v1.0.21")
