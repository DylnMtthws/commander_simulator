"""Parse a Scryfall mana cost string into something the simulator can pay.

The `{D}` lesson from the ingestion pipeline (PLAN.md section 11.0) applies
here directly. That parser was written from the set of symbols someone expected
to see, and it raised on `{2}{R}{D}` - a real cost on a real card - because the
symbol list it was built from excluded it.

The defence is not a longer list. It is that this parser recognises a CLOSED
set and raises on anything outside it, naming the symbol. A parser that
silently skips what it does not understand produces a cost that is too cheap,
and a card that costs less than it should looks exactly like a deck that is
faster than it is.

The eleven symbols in this deck, measured rather than assumed:

    {U} {G} {1} {2} {X} {0} {3} {4} {5} {U/P} {13}

Deliberately NOT supported, so that meeting one is an error rather than a
guess: hybrid ({G/U}), colourless ({C}), snow ({S}), land drop ({D}), and
half-mana. Each is well understood and each would take one line to add - the
point is that adding it is a decision someone makes on purpose.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

# A frozenset, NOT the string "WUBRG". `in` on a str is substring containment,
# so `"" in "WUBRG"` is True and the empty symbol {} would parse as a colour pip
# with no colour - a silent malformed cost. The rejection test for {} is what
# surfaced this; a happy-path suite would have shipped it.
COLOURS = frozenset("WUBRG")

_SYMBOL = re.compile(r"\{([^}]*)\}")
_GENERIC = re.compile(r"\A(\d+)\Z")
_PHYREXIAN = re.compile(r"\A([WUBRG])/P\Z")


class ManaCostError(ValueError):
    """An unparseable cost. Always names the offending symbol and the cost."""


@dataclass(frozen=True, slots=True)
class Cost:
    """A cost split into the parts the mana system pays differently.

    `generic` is payable by any source. `pips` must be matched by colour, which
    is the constraint that makes payment a bipartite matching rather than a sum
    (SIM_PLAN.md section 6.4). `variable` is the count of {X} symbols - see
    section 2.2 on why an X spell's minimum castable cost is a cost at which
    the card does nothing.
    """

    generic: int = 0
    pips: tuple[str, ...] = ()
    variable: int = 0
    phyrexian: tuple[str, ...] = field(default=())

    @property
    def mana_value_at_x_zero(self) -> int:
        """What Scryfall's `cmc` should be for this cost.

        Every symbol contributes 1 except generic, which contributes its own
        number, and {X}, which contributes 0. Used to cross-check the parser
        (see `check_against_mana_value`).
        """
        return self.generic + len(self.pips) + len(self.phyrexian)

    def to_json(self) -> dict[str, object]:
        return {
            "generic": self.generic,
            "pips": list(self.pips),
            "variable": self.variable,
            "phyrexian": list(self.phyrexian),
        }


def parse_cost(mana_cost: str | None) -> Cost | None:
    """Parse a Scryfall cost string. None or empty means "not castable".

    An empty cost is meaningful and is not an error: lands have no cost, and so
    does the card-level `mana_cost` of every transform and modal_dfc row, whose
    real costs live on the faces (SIM_PLAN.md section 2.2).
    """
    if mana_cost is None or mana_cost.strip() == "":
        return None

    remainder = _SYMBOL.sub("", mana_cost).strip()
    if remainder:
        raise ManaCostError(
            f"cost {mana_cost!r} has text outside any {{...}} symbol: {remainder!r}. "
            "A '//' combined cost must be split per face before parsing."
        )

    generic = 0
    pips: list[str] = []
    variable = 0
    phyrexian: list[str] = []

    for symbol in _SYMBOL.findall(mana_cost):
        if _GENERIC.match(symbol):
            generic += int(symbol)
        elif symbol in COLOURS:
            pips.append(symbol)
        elif symbol == "X":
            variable += 1
        elif match := _PHYREXIAN.match(symbol):
            phyrexian.append(match.group(1))
        else:
            raise ManaCostError(
                f"unsupported mana symbol {{{symbol}}} in cost {mana_cost!r}. "
                "This parser recognises a closed set on purpose - see the module "
                "docstring. Add the symbol deliberately rather than widening a regex."
            )

    return Cost(
        generic=generic,
        pips=tuple(pips),
        variable=variable,
        phyrexian=tuple(phyrexian),
    )


def check_against_mana_value(cost: Cost, mana_value: int, label: str) -> None:
    """Cross-check a parse against Scryfall's own `cmc`.

    What this catches: bugs in THIS parser - a mis-summed generic, a dropped
    pip, a symbol counted twice. Scryfall computed `cmc` independently of our
    regex, so a disagreement is evidence about our code.

    What it does NOT catch, stated because PLAN.md section 11.0's fifth rule
    applies to our own work too: whether Scryfall is right. Both `mana_cost`
    and `cmc` come from the same printing of the same card, so agreement here
    is not evidence about reality - only that we read the string the same way
    its publisher did.
    """
    expected = cost.mana_value_at_x_zero
    if expected != mana_value:
        raise ManaCostError(
            f"{label}: parsed cost implies mana value {expected} but the database "
            f"says {mana_value}. Parsed {cost!r}."
        )
