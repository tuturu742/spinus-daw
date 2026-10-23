"""Shared test fixtures."""

from __future__ import annotations

from pathlib import Path

import pytest

from spinus_ai import drums


@pytest.fixture(scope="session")
def drum_model(request: pytest.FixtureRequest) -> drums.Recogniser:
    """The real drum recogniser, trained once (about 25 s) and kept in pytest's cache between runs."""
    return drums.recogniser(Path(request.config.cache.mkdir("spinus-drum-recogniser")))
