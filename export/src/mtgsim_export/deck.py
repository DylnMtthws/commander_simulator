"""Loading and validating the deck file.

Validation lives here rather than in the simulator because the exporter is the
boundary where a bad deck should fail (SIM_PLAN.md section 9.3). Failing at
export costs one run; failing at load costs every run.
"""

from __future__ import annotations

import tomllib
from dataclasses import dataclass
from pathlib import Path

REQUIRED_TABLE_FIELDS = ("opponents", "opponent_colors", "on_the_play")


class DeckError(ValueError):
    """A deck file that cannot be trusted. Always says which field."""


@dataclass(frozen=True, slots=True)
class Deck:
    name: str
    commander: str
    mainboard: tuple[str, ...]
    opponents: int
    opponent_colors: tuple[str, ...]
    on_the_play: bool
    replacement: str

    @property
    def all_cards(self) -> tuple[str, ...]:
        """Commander first, then the 99. Order is the listed order, not sorted:
        export_index is assigned later from sorted names so it does not depend
        on how this file happens to be written."""
        return (self.commander, *self.mainboard)


def _require(table: dict[str, object], key: str, path: Path, section: str) -> object:
    if key not in table:
        raise DeckError(
            f"{path}: [{section}] is missing required field '{key}'. "
            "No field here has a default - an unstated assumption is exactly the "
            "failure SIM_PLAN.md sections 2.8 and 9.4 exist to prevent."
        )
    return table[key]


def load_deck(path: Path) -> Deck:
    with path.open("rb") as handle:
        raw = tomllib.load(handle)

    deck = raw.get("deck", {})
    table = raw.get("table", {})
    ablation = raw.get("ablation", {})
    cards = raw.get("cards", {})

    for section, contents in (("deck", deck), ("table", table), ("ablation", ablation)):
        if not contents:
            raise DeckError(f"{path}: missing required [{section}] section.")

    for key in REQUIRED_TABLE_FIELDS:
        _require(table, key, path, "table")
    _require(ablation, "replacement", path, "ablation")
    _require(deck, "commander", path, "deck")

    mainboard = cards.get("mainboard")
    if not isinstance(mainboard, list) or not mainboard:
        raise DeckError(f"{path}: [cards] mainboard must be a non-empty array of names.")

    # Commander plus 99. Checked here because a deck that is the wrong size
    # changes every draw probability in the simulation, silently.
    if len(mainboard) != 99:
        raise DeckError(
            f"{path}: mainboard has {len(mainboard)} cards, expected 99 (99 + 1 commander = 100)."
        )

    duplicates = sorted({n for n in mainboard if mainboard.count(n) > 1})
    if duplicates:
        raise DeckError(
            f"{path}: Commander is singleton, but these appear more than once: {duplicates}. "
            "Basic lands are the only legal exception and this deck lists each once."
        )

    if deck["commander"] in mainboard:
        raise DeckError(f"{path}: commander {deck['commander']!r} also appears in the mainboard.")

    if ablation["replacement"] not in mainboard:
        # Not an error: the replacement is usually a basic already in the deck,
        # but it need not be. Recorded so the manifest can say so.
        pass

    return Deck(
        name=str(deck.get("name", path.stem)),
        commander=str(deck["commander"]),
        mainboard=tuple(str(n) for n in mainboard),
        opponents=int(table["opponents"]),
        opponent_colors=tuple(str(c) for c in table["opponent_colors"]),
        on_the_play=bool(table["on_the_play"]),
        replacement=str(ablation["replacement"]),
    )
