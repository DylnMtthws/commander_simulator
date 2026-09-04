"""Build a deterministic, disposable card snapshot for CLI/service tests.

The committed eight-card fixture tests data shapes, not full simulation. This
generator uses those real shapes plus the 99 names in the authored deck to make
a complete temporary snapshot without committing upstream card data.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import uuid
from pathlib import Path
from typing import Any

import sys
import tomllib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "export" / "src"))

from mtgsim_export.hashes import deck_sha256  # noqa: E402
NAMESPACE = uuid.UUID("86b6bd22-e564-44d0-a374-c52a021d6a84")


def _compact_sha256(value: object) -> str:
    payload = json.dumps(value, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(payload.encode()).hexdigest()


def build_documents() -> tuple[dict[str, Any], dict[str, Any]]:
    deck = tomllib.loads((ROOT / "data/kinnan.deck.toml").read_text())
    source = json.loads((ROOT / "tests/fixtures/cards.fixture.json").read_text())
    by_name = {card["listed_name"]: card for card in source["cards"]}
    forest = by_name["Forest"]
    commander_name = deck["deck"]["commander"]
    names = [commander_name, *deck["cards"]["mainboard"]]

    cards: list[dict[str, Any]] = []
    for index, name in enumerate(sorted(names)):
        template = by_name.get(name, forest)
        card = copy.deepcopy(template)
        card["export_index"] = index
        card["listed_name"] = name
        card["name"] = name
        card["is_commander"] = name == commander_name
        if name not in by_name:
            card["oracle_id"] = str(uuid.uuid5(NAMESPACE, name))
            card["faces"][0]["name"] = name
        cards.append(card)

    manifest = {
        "exporter_version": "test-fixture",
        "generated_at": "2026-09-04T00:00:00+00:00",
        "source_view": "test.repro_fixture",
        "deck_name": "Kinnan cEDH reproducibility fixture",
        "commander": commander_name,
        "card_count": len(cards),
        "max_content_updated_at": "2026-09-04T00:00:00+00:00",
        "corpus_row_count": len(cards),
        "metric": "goldfish_turns_to_assembly",
        "measures": "turns until a declared pattern is assembled, unopposed",
        "does_not_measure": "deck strength, win rate, or card quality",
        "is_fixture": True,
    }
    manifest["cards_sha256"] = _compact_sha256(cards)
    snapshot = {"manifest": manifest, "cards": cards}

    commander = next(card for card in cards if card["is_commander"])
    library = [
        {"oracle_id": card["oracle_id"], "quantity": 1}
        for card in cards
        if not card["is_commander"]
    ]
    candidate = {
        "schema_version": "cedh-deck-candidate.v2",
        "candidate_id": "ci-repro-fixture",
        "commander_oracle_ids": [commander["oracle_id"]],
        "library": library,
        "strategy_pack_id": "kinnan-midrange-goldfish",
        "strategy_pack_version": "1.0.0",
        "provenance": {
            "producer": {"name": "make_repro_fixture.py", "version": "1"},
            "card_data": {
                "source": "test.repro_fixture",
                "snapshot_at": manifest["max_content_updated_at"],
                "hash": f"sha256:{manifest['cards_sha256']}",
            },
            "corpus": {
                "source": "test.repro_fixture",
                "snapshot_at": manifest["max_content_updated_at"],
                "hash": f"sha256:{manifest['cards_sha256']}",
            },
        },
        # The deck list only. The strategy pack above is deliberately absent
        # from this hash; see contracts/hash-golden-vectors.json.
        "deck_sha256": deck_sha256([commander["oracle_id"]], library),
        "user_constraints": {},
    }
    return snapshot, candidate


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cards", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    args = parser.parse_args()
    snapshot, candidate = build_documents()
    for path, document in ((args.cards, snapshot), (args.candidate, candidate)):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(document, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
