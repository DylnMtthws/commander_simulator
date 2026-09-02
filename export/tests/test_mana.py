"""Tests for the cost parser.

Weighted towards what the parser REJECTS. A check is worth exactly the set of
inputs it rejects (ingestion PLAN.md section 11.0), and for a parser the
dangerous failure is not raising on a bad cost - it is silently producing a
cheap one.
"""

from __future__ import annotations

import pytest

from mtgsim_export.mana import Cost, ManaCostError, check_against_mana_value, parse_cost


@pytest.mark.parametrize(
    ("text", "expected"),
    [
        ("{2}", Cost(generic=2)),
        ("{0}", Cost(generic=0)),
        ("{13}", Cost(generic=13)),
        ("{G}", Cost(pips=("G",))),
        ("{1}{U}", Cost(generic=1, pips=("U",))),
        ("{U}{U}", Cost(pips=("U", "U"))),
        ("{5}{G}{U}", Cost(generic=5, pips=("G", "U"))),
        # Every X spell in the deck. Note generic stays 0: {X} is a variable,
        # not a cost of zero, and conflating them is how Finale of Devastation
        # gets cast on turn two to find nothing (SIM_PLAN.md section 2.2).
        ("{X}{G}{G}", Cost(pips=("G", "G"), variable=1)),
        ("{X}{U}", Cost(pips=("U",), variable=1)),
        ("{X}{G}{G}{G}", Cost(pips=("G", "G", "G"), variable=1)),
        ("{U/P}", Cost(phyrexian=("U",))),
    ],
)
def test_parses_every_cost_shape_in_the_deck(text: str, expected: Cost) -> None:
    assert parse_cost(text) == expected


@pytest.mark.parametrize("text", [None, "", "   "])
def test_absent_cost_is_none_not_zero(text: str | None) -> None:
    """Empty is meaningful: a land has no cost, and so does the card-level
    mana_cost of every transform and modal_dfc row. Returning Cost(generic=0)
    would make an uncastable card look like a free spell."""
    assert parse_cost(text) is None


@pytest.mark.parametrize(
    "text",
    [
        "{G/U}",  # hybrid
        "{2/W}",  # monocoloured hybrid
        "{C}",  # colourless
        "{S}",  # snow
        "{D}",  # land drop - the symbol that broke the upstream parser
        "{HW}",  # half mana
        "{Q}",  # untap
        "{}",  # empty symbol
    ],
)
def test_rejects_unsupported_symbols_by_name(text: str) -> None:
    """The whole design of this parser. An unknown symbol must raise, not be
    skipped - skipping yields a cost that is too cheap, which reads downstream
    as a deck that is faster than it is."""
    with pytest.raises(ManaCostError, match="unsupported mana symbol"):
        parse_cost(text)


def test_rejects_a_combined_two_face_cost() -> None:
    """Adventure and split cards store "A // B" in one string. Parsing that as
    a single cost would sum both halves into one impossible spell, so it must
    be split per face before it gets here."""
    with pytest.raises(ManaCostError, match="outside any"):
        parse_cost("{1}{U}{U} // {1}{U}")


def test_mana_value_cross_check_accepts_agreement() -> None:
    cost = parse_cost("{1}{U}")
    assert cost is not None
    check_against_mana_value(cost, 2, "Test Card")


def test_mana_value_cross_check_rejects_disagreement() -> None:
    """Catches bugs in this parser by comparing against Scryfall's independently
    computed cmc. It says nothing about whether Scryfall is right - both fields
    come off the same printing."""
    cost = parse_cost("{1}{U}")
    assert cost is not None
    with pytest.raises(ManaCostError, match="implies mana value 2 but the database says 7"):
        check_against_mana_value(cost, 7, "Test Card")


def test_x_contributes_zero_to_mana_value() -> None:
    """Finale of Devastation is {X}{G}{G} with a database mana_value of 2."""
    cost = parse_cost("{X}{G}{G}")
    assert cost is not None
    assert cost.mana_value_at_x_zero == 2
    check_against_mana_value(cost, 2, "Finale of Devastation")


def test_phyrexian_counts_toward_mana_value() -> None:
    """Mental Misstep is {U/P} with mana_value 1, not 0."""
    cost = parse_cost("{U/P}")
    assert cost is not None
    assert cost.mana_value_at_x_zero == 1
