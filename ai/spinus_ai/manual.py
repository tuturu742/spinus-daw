"""Keyword search over the Spinus manual (docs/user) for the assistant: BM25 over sections,
built when the service starts. No model download."""

from __future__ import annotations

import hashlib
import json
import math
import re
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any

WORD = re.compile(r"[a-z0-9]+")
STOP = frozenset(
    [
        "the",
        "a",
        "an",
        "and",
        "or",
        "to",
        "of",
        "in",
        "on",
        "for",
        "is",
        "it",
        "be",
        "as",
        "by",
        "with",
        "this",
        "that",
        "you",
        "your",
        "are",
        "at",
        "from",
    ]
)


def words(text: str) -> list[str]:
    return [w for w in WORD.findall(text.lower()) if w not in STOP]


@dataclass
class Section:
    page: str  # e.g. "mixer.md"
    title: str  # the page title and the section heading
    text: str


class Manual:
    def __init__(self, folder: Path | None) -> None:
        self.sections: list[Section] = []
        if folder is not None and folder.is_dir():
            for page in sorted(folder.glob("*.md")):
                self.sections.extend(self.split(page))
        self.terms = [Counter(words(s.title + " " + s.text)) for s in self.sections]
        self.lengths = [sum(t.values()) for t in self.terms]
        self.average = sum(self.lengths) / len(self.lengths) if self.lengths else 0.0
        self.frequency: Counter[str] = Counter()
        for t in self.terms:
            self.frequency.update(t.keys())
        self.model: Any = None  # an EmbeddingModel, when the user chose one
        self.vectors: Any = None

    def fingerprint(self) -> str:
        text = "\n".join(s.page + s.title + s.text for s in self.sections)
        return hashlib.sha256(text.encode()).hexdigest()[:16]

    def use_model(self, model: Any, cache: Path | None) -> None:
        """Embeds the sections with the model (once per model and manual version; cached)."""
        import numpy as np

        self.model, self.vectors = model, None
        if model is None or not self.sections:
            return
        file = cache / f"manual-{model.spec.id}-{self.fingerprint()}.json" if cache is not None else None
        if file is not None and file.exists():
            self.vectors = np.array(json.loads(file.read_text(encoding="utf-8")), dtype=np.float32)
            return
        self.vectors = model.embed([f"{s.title}\n{s.text}" for s in self.sections], document=True)
        if file is not None:
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(json.dumps(self.vectors.round(5).tolist()), encoding="utf-8")

    @staticmethod
    def split(page: Path) -> list[Section]:
        lines = page.read_text(encoding="utf-8").splitlines()
        title = next((line[2:].strip() for line in lines if line.startswith("# ")), page.stem)
        sections: list[Section] = []
        heading, body = title, list[str]()
        for line in lines:
            if line.startswith("## "):
                if body:
                    sections.append(Section(page.name, heading, "\n".join(body).strip()))
                heading, body = f"{title} > {line[3:].strip()}", []
            elif not line.startswith("# ") and not line.startswith("!["):
                body.append(line)
        if body:
            sections.append(Section(page.name, heading, "\n".join(body).strip()))
        return [s for s in sections if s.text]

    def search(self, query: str, limit: int = 4) -> list[Section]:
        """Keywords (BM25); with a model, combined with meaning by reciprocal rank fusion."""
        by_words = self.keyword_ranking(query)
        if self.model is None or self.vectors is None:
            return [self.sections[i] for i in by_words[:limit]]
        scores = self.vectors @ self.model.embed([query], document=False)[0]
        by_meaning = [int(i) for i in scores.argsort()[::-1][:20]]
        fused: dict[int, float] = {}
        for ranking in (by_words, by_meaning):
            for rank, i in enumerate(ranking):
                fused[i] = fused.get(i, 0.0) + 1.0 / (60 + rank)
        return [self.sections[i] for i, _ in sorted(fused.items(), key=lambda x: -x[1])[:limit]]

    def keyword_ranking(self, query: str) -> list[int]:
        q = words(query)
        if not q or not self.sections:
            return []
        n = len(self.sections)
        scores: list[tuple[float, int]] = []
        for i, terms in enumerate(self.terms):
            score = 0.0
            for w in q:
                f = terms.get(w, 0)
                if f == 0:
                    continue
                idf = math.log(1 + (n - self.frequency[w] + 0.5) / (self.frequency[w] + 0.5))
                score += idf * f * 2.2 / (f + 1.2 * (0.25 + 0.75 * self.lengths[i] / (self.average or 1)))
            if score > 0:
                scores.append((score, i))
        scores.sort(reverse=True)
        return [i for _, i in scores[:20]]
