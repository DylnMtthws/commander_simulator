"""The Python half of the cross-language hash contract.

``contracts/hash-golden-vectors.json`` is shared with the C++ simulator
(``tests/unit/test_hash_contract.cpp``) and with the Deck Lab. All three read
the same file. A test that only checked an implementation against itself would
pass forever while the implementations diverged -- which is precisely how a
deck hash and a deck-plus-pack hash came to share the name ``candidate_hash``.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

from mtgsim_export.hashes import (
    SIMULATION_INPUT_FIELDS,
    deck_sha256,
    simulation_input_document,
    simulation_input_sha256,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
GOLDEN = json.loads((REPO_ROOT / "contracts" / "hash-golden-vectors.json").read_text())
VECTORS: dict[str, dict[str, Any]] = {v["name"]: v for v in GOLDEN["vectors"]}


def deck_hash_of(name: str) -> str:
    vector = VECTORS[name]
    return deck_sha256(vector["commander_oracle_ids"], vector["library"])


def input_hash_of(name: str) -> str:
    vector = VECTORS[name]
    return simulation_input_sha256(deck_hash=deck_hash_of(name), **vector["execution"])


@pytest.mark.parametrize("name", sorted(VECTORS))
def test_published_digests_are_reproduced_exactly(name: str) -> None:
    assert deck_hash_of(name) == VECTORS[name]["deck_sha256"]
    assert input_hash_of(name) == VECTORS[name]["simulation_input_sha256"]


def test_ordering_does_not_affect_deck_sha256() -> None:
    assert deck_hash_of("base") == deck_hash_of("reordered_inputs")
    assert deck_hash_of("two_commanders") == deck_hash_of("two_commanders_reordered")


def test_changing_the_commander_changes_deck_sha256() -> None:
    assert deck_hash_of("base") != deck_hash_of("commander_changed")


def test_changing_a_card_changes_deck_sha256() -> None:
    assert deck_hash_of("base") != deck_hash_of("card_changed")


def test_changing_a_quantity_changes_deck_sha256() -> None:
    assert deck_hash_of("base") != deck_hash_of("quantity_changed")


def test_changing_the_strategy_pack_does_not_change_deck_sha256() -> None:
    """ADR-025: a deck hash identifies a LIST, not a list plus its pack."""
    assert deck_hash_of("base") == deck_hash_of("pack_changed")


def test_changing_the_strategy_pack_does_change_simulation_input_sha256() -> None:
    """The other half. Same deck, different pack, different measurement."""
    assert input_hash_of("base") != input_hash_of("pack_changed")


def test_run_parameters_move_only_the_input_fingerprint() -> None:
    assert deck_hash_of("base") == deck_hash_of("seed_changed")
    assert input_hash_of("base") != input_hash_of("seed_changed")


def test_ablation_order_does_not_affect_the_input_fingerprint() -> None:
    assert input_hash_of("ablations_sorted") == input_hash_of("ablations_reordered")


@pytest.mark.parametrize("rule", GOLDEN["assertions"], ids=lambda r: r["requirement"])
def test_the_golden_files_own_assertions_hold(rule: dict[str, Any]) -> None:
    """Walks the file's ``assertions`` block so a requirement added there is
    executed even before somebody writes a named test for it."""
    checks = {
        "equal_deck_sha256": (deck_hash_of, True),
        "different_deck_sha256": (deck_hash_of, False),
        "equal_simulation_input_sha256": (input_hash_of, True),
        "different_simulation_input_sha256": (input_hash_of, False),
    }
    applied = 0
    for key, (hash_of, want_equal) in checks.items():
        if key not in rule:
            continue
        left, right = rule[key]
        assert (hash_of(left) == hash_of(right)) is want_equal, rule["requirement"]
        applied += 1
    assert applied, f"assertion {rule['requirement']!r} checks nothing"


def test_every_behaviour_affecting_input_is_in_the_fingerprint() -> None:
    """The fingerprint's field list is the audit surface, so pin it.

    Adding a behaviour-affecting simulator input without adding it here would
    let two genuinely different runs share one identity -- a cache that
    returns the wrong numbers rather than a test that fails.
    """
    assert SIMULATION_INPUT_FIELDS == (
        "ablations",
        "card_data_manifest_hash",
        "cards_sha256",
        "deck_sha256",
        "games",
        "objective_turn",
        "scenario_id",
        "scenario_version",
        "schema_version",
        "seed",
        "simulator_version",
        "strategy_pack_content_sha256",
        "strategy_pack_derived",
        "strategy_pack_id",
        "strategy_pack_version",
        "sweep",
    )
    document = simulation_input_document(
        deck_hash="sha256:" + "0" * 64, **VECTORS["base"]["execution"]
    )
    assert tuple(sorted(document)) == SIMULATION_INPUT_FIELDS


def test_the_deck_hash_never_reads_the_submitted_claim() -> None:
    """A recomputation that consulted the submitted hash would verify nothing."""
    source = (
        REPO_ROOT / "export" / "src" / "mtgsim_export" / "candidate.py"
    ).read_text()
    body = source.split("def candidate_deck_sha256")[1].split("\ndef ")[0]
    assert "candidate.deck_sha256" not in body
