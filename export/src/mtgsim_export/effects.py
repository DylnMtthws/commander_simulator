"""Checking hand-authored effects against the card text they were written from.

The seam where the hand-authored layer meets live data. The nightly ingest moves
card text; an effect authored against text that has since changed is wrong in a
way nothing else in the system can notice, because the effect file is perfectly
valid TOML describing a card that no longer says that.
"""

from __future__ import annotations

import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True, slots=True)
class Drift:
    name: str
    authored_from: str
    actual: str


@dataclass(frozen=True, slots=True)
class EffectsReport:
    authored: int
    modeled: int
    inert: int
    inert_by_category: dict[str, int]
    unauthored: list[str]
    drifted: list[Drift]
    unknown: list[str]


def check_effects(path: Path, cards: list[dict[str, Any]]) -> EffectsReport:
    if not path.is_file():
        return EffectsReport(0, 0, 0, {}, [c["listed_name"] for c in cards], [], [])

    with path.open("rb") as handle:
        entries = tomllib.load(handle).get("cards", {})

    by_name = {c["listed_name"]: c for c in cards}
    modeled = inert = 0
    by_category: dict[str, int] = {}
    drifted: list[Drift] = []
    unknown: list[str] = []

    for name, entry in entries.items():
        card = by_name.get(name)
        if card is None:
            # An effect for a card not in the deck. Not fatal - effects.toml is
            # a shared card-level file and will outgrow any one deck - but worth
            # naming, because it is equally likely to be a typo.
            unknown.append(name)
            continue
        status = entry.get("status")
        if status == "modeled":
            modeled += 1
        elif status == "inert":
            inert += 1
            by_category[entry.get("reason_category", "(none)")] = (
                by_category.get(entry.get("reason_category", "(none)"), 0) + 1
            )
        authored_from = entry.get("authored_from", "")
        if authored_from and authored_from != card["oracle_sha256"]:
            drifted.append(Drift(name, authored_from, card["oracle_sha256"]))

    unauthored = sorted(n for n in by_name if n not in entries)
    return EffectsReport(
        authored=modeled + inert,
        modeled=modeled,
        inert=inert,
        inert_by_category=by_category,
        unauthored=unauthored,
        drifted=drifted,
        unknown=unknown,
    )
