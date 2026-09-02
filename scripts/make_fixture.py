#!/usr/bin/env python3
"""Build tests/fixtures/cards.fixture.json from a real export.

Derived from data/cards.json rather than hand-written, so the fixture cannot
drift into describing a shape the exporter never produces. Regenerate after any
change to the export format:

    uv run --python 3.13 --no-project python scripts/make_fixture.py

The selection is by SHAPE, not by the first N cards. A fixture set chosen for
convenience tests whatever the alphabet happened to put first; this one names
what each card is here to exercise, and the rationale ships inside the file so
a reader of the fixture knows why each row is present.
"""

from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "data" / "cards.json"
TARGET = ROOT / "tests" / "fixtures" / "cards.fixture.json"

# listed_name -> what breaks if the loader mishandles it.
SELECTION = {
    "Kinnan, Bonder Prodigy": "the commander: is_commander true, two coloured pips {G}{U}",
    "Sol Ring": "the ordinary case: one face, generic-only cost, castable",
    "Forest": "a basic land: no cost at all, is_land true - cost null is not cost zero",
    "Tropical Island": "a nonbasic land, and a Reserved List card mtg_v1.card silently drops",
    "Sink into Stupor": "modal_dfc: two faces, front-face name resolution, land back face",
    "Invasion of Ikoria": "transform + Battle + X spell, and empty card-level mana_cost",
    "Finale of Devastation": "an X spell: variable=1 with mana_value 2, castable at a cost that does nothing",
    "Mental Misstep": "the only Phyrexian cost in the deck, {U/P}",
}


def main() -> int:
    if not SOURCE.is_file():
        print(f"{SOURCE} not found - run the exporter first")
        return 1

    document = json.loads(SOURCE.read_text())
    by_name = {c["listed_name"]: c for c in document["cards"]}

    missing = sorted(set(SELECTION) - set(by_name))
    if missing:
        print(f"fixture selection names cards not in the export: {missing}")
        return 1

    # Reindexed 0..N-1 rather than carrying indices out of the 100-card export.
    # A fixture should be a VALID card database in its own right, not a fragment
    # of one: export_index is a dense slot number, and section 11's zone bitsets
    # index by it. A fixture with holes would either force the loader to accept
    # a gap it should reject, or fail its own validation. The original index is
    # kept for traceability.
    cards = []
    for slot, name in enumerate(SELECTION):
        card = dict(by_name[name])
        card["source_export_index"] = card["export_index"]
        card["export_index"] = slot
        cards.append(card)

    manifest = dict(document["manifest"])
    manifest["card_count"] = len(cards)
    manifest["is_fixture"] = True
    manifest["fixture_rationale"] = SELECTION
    manifest["derived_from_cards_sha256"] = document["manifest"]["cards_sha256"]
    # Recomputed, not copied: the fixture is a different set of cards, so
    # carrying the full export's hash would make a stale fixture undetectable.
    payload = json.dumps(cards, sort_keys=True, separators=(",", ":"))
    import hashlib

    manifest["cards_sha256"] = hashlib.sha256(payload.encode()).hexdigest()

    TARGET.parent.mkdir(parents=True, exist_ok=True)
    TARGET.write_text(json.dumps({"manifest": manifest, "cards": cards}, indent=2) + "\n")
    print(f"wrote {TARGET.relative_to(ROOT)}: {len(cards)} cards")
    for name, why in SELECTION.items():
        print(f"  {name:<24} {why}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
