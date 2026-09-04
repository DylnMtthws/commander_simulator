"""The two hashes that identify a simulation, and what each one is *for*.

There are two, they answer different questions, and conflating them is the
defect this module exists to make impossible.

``deck_sha256`` identifies **a deck list and nothing else**: the sorted
commander oracle IDs and the sorted, coalesced library oracle IDs with their
quantities. It deliberately excludes the strategy pack, the requester, the
simulator, the card corpus, and every run parameter. A list built today and
the same list built next month hash identically, which is what lets a consumer
ask "does this stored result describe the list in front of the user?".

The algorithm is the Deck Lab's, adopted verbatim under its ADR-025, which
requires that a deck hash identify a LIST rather than a list-plus-requester.
Preimage, concatenated with no separator beyond the newlines shown::

    "C:<oracle_id>\\n"        for each commander oracle_id, sorted
    "<oracle_id>:<qty>\\n"    for each library card, sorted by oracle_id

``simulation_input_sha256`` identifies **everything that can change the
numbers**: the deck hash, the resolved strategy pack's identity and content,
the simulator and card-data versions, the scenario, and the run parameters.
Two runs with equal ``simulation_input_sha256`` must produce equal statistics.
Two runs with equal ``deck_sha256`` and different packs must NOT.

Both are returned as ``sha256:<64 lowercase hex>``. The prefix is not
decoration: it says which algorithm produced the digest, so a future change of
algorithm is a visible contract change rather than a silent one.
"""

from __future__ import annotations

import hashlib
import json
from collections.abc import Iterable, Mapping, Sequence
from typing import Any

#: Every field the simulation-input fingerprint covers. Named as a constant so
#: a reviewer can see the whole behaviour-affecting surface in one place, and
#: so the C++ implementation has something to be checked against.
SIMULATION_INPUT_FIELDS: tuple[str, ...] = (
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

SIMULATION_INPUT_SCHEMA_VERSION = "cedh-simulation-input.v1"


def deck_sha256(
    commander_oracle_ids: Iterable[str],
    library: Iterable[Mapping[str, Any]] | Sequence[tuple[str, int]],
) -> str:
    """Hash a deck list, and only a deck list.

    Args:
        commander_oracle_ids: One or two commander oracle IDs, any order.
        library: The 99, either as mappings with ``oracle_id``/``quantity``
            keys or as ``(oracle_id, quantity)`` pairs, in any order. Entries
            must already be coalesced: one row per oracle ID.

    Returns:
        ``sha256:<hex>`` over the ADR-025 preimage described in the module
        docstring.
    """
    entries: list[tuple[str, int]] = []
    for item in library:
        if isinstance(item, Mapping):
            entries.append((str(item["oracle_id"]), int(item["quantity"])))
        else:
            oracle_id, quantity = item
            entries.append((str(oracle_id), int(quantity)))

    digest = hashlib.sha256()
    for oracle_id in sorted(str(value) for value in commander_oracle_ids):
        digest.update(f"C:{oracle_id}\n".encode())
    for oracle_id, quantity in sorted(entries, key=lambda entry: entry[0]):
        digest.update(f"{oracle_id}:{quantity}\n".encode())
    return "sha256:" + digest.hexdigest()


def simulation_input_document(
    *,
    deck_hash: str,
    strategy_pack_id: str,
    strategy_pack_version: str,
    strategy_pack_content_sha256: str,
    strategy_pack_derived: bool,
    simulator_version: str,
    card_data_manifest_hash: str,
    cards_sha256: str,
    scenario_id: str,
    scenario_version: str,
    seed: int,
    games: int,
    objective_turn: int,
    sweep: bool,
    ablations: Iterable[str],
) -> dict[str, Any]:
    """The canonical pre-hash document, exposed so a mismatch is debuggable.

    The strategy pack fields describe the pack the simulator **resolved and
    ran**, not the one the request asked for. A request that asked for a pack
    which is not installed never reaches this function: it is an
    execution-context error, refused before any game is played.
    """
    document = {
        "ablations": sorted(str(name) for name in ablations),
        "card_data_manifest_hash": card_data_manifest_hash,
        "cards_sha256": cards_sha256,
        "deck_sha256": deck_hash,
        "games": int(games),
        "objective_turn": int(objective_turn),
        "scenario_id": scenario_id,
        "scenario_version": scenario_version,
        "schema_version": SIMULATION_INPUT_SCHEMA_VERSION,
        "seed": int(seed),
        "simulator_version": simulator_version,
        "strategy_pack_content_sha256": strategy_pack_content_sha256,
        "strategy_pack_derived": bool(strategy_pack_derived),
        "strategy_pack_id": strategy_pack_id,
        "strategy_pack_version": strategy_pack_version,
        "sweep": bool(sweep),
    }
    assert tuple(sorted(document)) == SIMULATION_INPUT_FIELDS, (
        "simulation-input fields drifted from SIMULATION_INPUT_FIELDS"
    )
    return document


def simulation_input_sha256(**kwargs: Any) -> str:
    """Hash every behaviour-affecting input. See :func:`simulation_input_document`."""
    canonical = json.dumps(
        simulation_input_document(**kwargs), sort_keys=True, separators=(",", ":")
    )
    return "sha256:" + hashlib.sha256(canonical.encode()).hexdigest()
