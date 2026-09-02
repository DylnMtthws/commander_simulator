"""Deck-file validation. Weighted towards what it rejects."""

from __future__ import annotations

from pathlib import Path

import pytest

from mtgsim_export.deck import DeckError, load_deck

REPO_ROOT = Path(__file__).resolve().parents[2]

VALID = """
[deck]
commander = "Kinnan, Bonder Prodigy"
[table]
opponents = 3
opponent_colors = ["W","U","B","R","G"]
on_the_play = true
[ablation]
replacement = "Forest"
[cards]
mainboard = [{names}]
"""


def _write(tmp_path: Path, body: str) -> Path:
    path = tmp_path / "d.toml"
    path.write_text(body)
    return path


def _names(n: int, *, extra: list[str] | None = None) -> str:
    cards = [f'"Card {i}"' for i in range(n)]
    cards.extend(f'"{e}"' for e in extra or [])
    return ", ".join(cards)


def test_loads_the_real_deck() -> None:
    deck = load_deck(REPO_ROOT / "data" / "kinnan.deck.toml")
    assert deck.commander == "Kinnan, Bonder Prodigy"
    assert len(deck.mainboard) == 99
    assert len(deck.all_cards) == 100
    assert deck.opponents == 3
    assert deck.on_the_play is True
    assert deck.replacement == "Forest"


def test_accepts_a_minimal_valid_deck(tmp_path: Path) -> None:
    deck = load_deck(_write(tmp_path, VALID.format(names=_names(99))))
    assert len(deck.all_cards) == 100


@pytest.mark.parametrize("field", ["opponents", "opponent_colors", "on_the_play"])
def test_rejects_a_missing_table_field(tmp_path: Path, field: str) -> None:
    """No field has a default. An unstated assumption is the failure the
    [table] block exists to prevent (SIM_PLAN.md section 2.8)."""
    body = VALID.format(names=_names(99))
    body = "\n".join(line for line in body.splitlines() if not line.startswith(f"{field} "))
    with pytest.raises(DeckError, match=f"missing required field '{field}'"):
        load_deck(_write(tmp_path, body))


def test_rejects_a_missing_ablation_replacement(tmp_path: Path) -> None:
    body = VALID.format(names=_names(99)).replace('replacement = "Forest"', "")
    with pytest.raises(DeckError, match="\\[ablation\\]"):
        load_deck(_write(tmp_path, body))


@pytest.mark.parametrize("count", [98, 100])
def test_rejects_a_deck_that_is_not_99(tmp_path: Path, count: int) -> None:
    """A deck of the wrong size changes every draw probability, silently."""
    with pytest.raises(DeckError, match=f"mainboard has {count} cards"):
        load_deck(_write(tmp_path, VALID.format(names=_names(count))))


def test_rejects_a_duplicated_card(tmp_path: Path) -> None:
    body = VALID.format(names=_names(98, extra=["Card 0"]))
    with pytest.raises(DeckError, match="appear more than once"):
        load_deck(_write(tmp_path, body))


def test_rejects_a_commander_also_in_the_mainboard(tmp_path: Path) -> None:
    body = VALID.format(names=_names(98, extra=["Kinnan, Bonder Prodigy"]))
    with pytest.raises(DeckError, match="also appears in the mainboard"):
        load_deck(_write(tmp_path, body))
