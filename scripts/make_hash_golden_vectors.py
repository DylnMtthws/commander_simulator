#!/usr/bin/env python3
"""Regenerate ``contracts/hash-golden-vectors.json``.

The vectors are the executable form of the hash contract. They are checked in
so that a change to either implementation shows up as a diff in a reviewed
file rather than as two implementations quietly agreeing on something new.

Run: ``uv run --project export python scripts/make_hash_golden_vectors.py``
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "export" / "src"))

from mtgsim_export.hashes import deck_sha256, simulation_input_sha256  # noqa: E402

A = "8d11aa49-d4cd-48b1-aa0f-8548fa733416"  # Kinnan, Bonder Prodigy
B = "3f2d1c4a-5b6e-4f70-8192-a3b4c5d6e7f8"  # a different commander
PARTNER = "11111111-2222-4333-8444-555555555555"
X = "0259f063-3e3f-4e07-9066-f748d4819829"
Y = "0895c9b7-ae7d-4bb3-af17-3b75deb50a25"
Z = "08afc0d7-192f-4ab6-b6a0-c4265cf5e225"
W = "fedcba98-7654-4321-8fed-cba987654321"

KINNAN_PACK = {
    "strategy_pack_id": "kinnan-midrange-goldfish",
    "strategy_pack_version": "1.0.0",
    "strategy_pack_content_sha256": "sha256:" + "1" * 64,
    "strategy_pack_derived": False,
}
DERIVED_PACK = {
    "strategy_pack_id": "derived-generic",
    "strategy_pack_version": "1.0.0",
    "strategy_pack_content_sha256": "sha256:" + "2" * 64,
    "strategy_pack_derived": True,
}
RUN = {
    "simulator_version": "0.2.0",
    "card_data_manifest_hash": "sha256:" + "a" * 64,
    "cards_sha256": "b" * 64,
    "scenario_id": "goldfish_assembly",
    "scenario_version": "1.0.0",
    "seed": 12345,
    "games": 20000,
    "objective_turn": 3,
    "sweep": False,
    "ablations": [],
}


def card(oracle_id: str, quantity: int = 1) -> dict[str, object]:
    return {"oracle_id": oracle_id, "quantity": quantity}


CASES: list[dict[str, object]] = [
    {
        "name": "base",
        "note": "The reference deck and execution context every other vector varies from.",
        "commander_oracle_ids": [A],
        "library": [card(X), card(Y), card(Z, 2)],
        "execution": {**KINNAN_PACK, **RUN},
    },
    {
        "name": "reordered_inputs",
        "note": (
            "Same deck, every list submitted in a different order. Both hashes must equal "
            "'base': ordering is a serialisation accident, not a property of a deck."
        ),
        "commander_oracle_ids": [A],
        "library": [card(Z, 2), card(X), card(Y)],
        "execution": {**KINNAN_PACK, **RUN},
    },
    {
        "name": "two_commanders_reordered",
        "note": "A partner pair addressed in either order is one deck.",
        "commander_oracle_ids": [PARTNER, A],
        "library": [card(X), card(Y), card(Z, 2)],
        "execution": {**KINNAN_PACK, **RUN},
    },
    {
        "name": "two_commanders",
        "note": "The same pair, sorted. Must equal two_commanders_reordered.",
        "commander_oracle_ids": [A, PARTNER],
        "library": [card(X), card(Y), card(Z, 2)],
        "execution": {**KINNAN_PACK, **RUN},
    },
    {
        "name": "commander_changed",
        "note": "A different commander is a different deck.",
        "commander_oracle_ids": [B],
        "library": [card(X), card(Y), card(Z, 2)],
        "execution": {**KINNAN_PACK, **RUN},
    },
    {
        "name": "card_changed",
        "note": "Swapping one library card is a different deck.",
        "commander_oracle_ids": [A],
        "library": [card(X), card(Y), card(W, 2)],
        "execution": {**KINNAN_PACK, **RUN},
    },
    {
        "name": "quantity_changed",
        "note": "Changing one quantity is a different deck.",
        "commander_oracle_ids": [A],
        "library": [card(X), card(Y), card(Z, 3)],
        "execution": {**KINNAN_PACK, **RUN},
    },
    {
        "name": "pack_changed",
        "note": (
            "THE LOAD-BEARING VECTOR. Identical deck, different strategy pack. "
            "deck_sha256 must equal 'base' (ADR-025: a deck hash identifies a list, not a "
            "list-plus-pack). simulation_input_sha256 must differ from 'base' (a different "
            "pack produces different numbers)."
        ),
        "commander_oracle_ids": [A],
        "library": [card(X), card(Y), card(Z, 2)],
        "execution": {**DERIVED_PACK, **RUN},
    },
    {
        "name": "seed_changed",
        "note": "Deck unchanged; a run parameter moved. Only the input fingerprint moves.",
        "commander_oracle_ids": [A],
        "library": [card(X), card(Y), card(Z, 2)],
        "execution": {**KINNAN_PACK, **RUN, "seed": 999},
    },
    {
        "name": "ablations_reordered",
        "note": "Ablation order is not meaningful; the fingerprint sorts them.",
        "commander_oracle_ids": [A],
        "library": [card(X), card(Y), card(Z, 2)],
        "execution": {**KINNAN_PACK, **RUN, "ablations": ["Sol Ring", "Basalt Monolith"]},
    },
    {
        "name": "ablations_sorted",
        "note": "Must equal ablations_reordered.",
        "commander_oracle_ids": [A],
        "library": [card(X), card(Y), card(Z, 2)],
        "execution": {**KINNAN_PACK, **RUN, "ablations": ["Basalt Monolith", "Sol Ring"]},
    },
]

ASSERTIONS = [
    {
        "requirement": "ordering does not affect deck_sha256",
        "equal_deck_sha256": ["base", "reordered_inputs"],
    },
    {
        "requirement": "commander order does not affect deck_sha256",
        "equal_deck_sha256": ["two_commanders", "two_commanders_reordered"],
    },
    {
        "requirement": "changing the commander changes deck_sha256",
        "different_deck_sha256": ["base", "commander_changed"],
    },
    {
        "requirement": "changing a card changes deck_sha256",
        "different_deck_sha256": ["base", "card_changed"],
    },
    {
        "requirement": "changing a quantity changes deck_sha256",
        "different_deck_sha256": ["base", "quantity_changed"],
    },
    {
        "requirement": "changing the strategy pack does NOT change deck_sha256",
        "equal_deck_sha256": ["base", "pack_changed"],
    },
    {
        "requirement": "changing the strategy pack DOES change simulation_input_sha256",
        "different_simulation_input_sha256": ["base", "pack_changed"],
    },
    {
        "requirement": "changing the seed changes simulation_input_sha256 only",
        "equal_deck_sha256": ["base", "seed_changed"],
        "different_simulation_input_sha256": ["base", "seed_changed"],
    },
    {
        "requirement": "ablation order does not affect simulation_input_sha256",
        "equal_simulation_input_sha256": ["ablations_sorted", "ablations_reordered"],
    },
]


def main() -> int:
    vectors = []
    for case in CASES:
        commanders = case["commander_oracle_ids"]
        library = case["library"]
        execution = dict(case["execution"])
        deck = deck_sha256(commanders, library)
        vectors.append(
            {
                "name": case["name"],
                "note": case["note"],
                "commander_oracle_ids": commanders,
                "library": library,
                "execution": execution,
                "deck_sha256": deck,
                "simulation_input_sha256": simulation_input_sha256(
                    deck_hash=deck, **execution
                ),
            }
        )

    document = {
        "schema_version": "cedh-hash-golden-vectors.v1",
        "description": (
            "Cross-language golden vectors for the two cEDH simulation hashes. Every "
            "implementation of deck_sha256 and simulation_input_sha256 -- Python in this "
            "repository's exporter, C++ in the simulator, Python in the Deck Lab -- must "
            "reproduce these digests exactly. Libraries here are deliberately tiny: these "
            "vectors pin the hash algorithms, not the 99-card candidate invariant."
        ),
        "deck_sha256_preimage": (
            "sha256 over the concatenation of 'C:<oracle_id>\\n' for each commander "
            "oracle_id in sorted order, then '<oracle_id>:<quantity>\\n' for each library "
            "entry sorted by oracle_id. Rendered 'sha256:<lowercase hex>'. The strategy "
            "pack, requester, simulator and corpus are excluded by design (ADR-025)."
        ),
        "simulation_input_sha256_preimage": (
            "sha256 over compact UTF-8 JSON (separators ',' and ':') with lexicographically "
            "sorted object keys, of the execution object plus deck_sha256 and "
            "schema_version 'cedh-simulation-input.v1'. Rendered 'sha256:<lowercase hex>'."
        ),
        "assertions": ASSERTIONS,
        "vectors": vectors,
    }
    out = Path(__file__).resolve().parents[1] / "contracts" / "hash-golden-vectors.json"
    out.write_text(json.dumps(document, indent=2) + "\n")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
