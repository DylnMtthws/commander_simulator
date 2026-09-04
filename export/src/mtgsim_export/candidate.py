"""The producer-facing deck-candidate contract.

This module deliberately uses only the standard library. The exporter needs to
accept a candidate before it has connected to Postgres, and schema-validation
libraries remain a development/test dependency rather than production weight.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class CandidateError(ValueError):
    """A candidate that cannot safely identify a 99-card library."""


@dataclass(frozen=True, slots=True)
class CandidateCard:
    oracle_id: str
    quantity: int


@dataclass(frozen=True, slots=True)
class Candidate:
    candidate_id: str
    commander_oracle_ids: tuple[str, ...]
    library: tuple[CandidateCard, ...]
    strategy_pack_id: str
    strategy_pack_version: str
    candidate_hash: str

    @property
    def all_oracle_ids(self) -> tuple[str, ...]:
        expanded = list(self.commander_oracle_ids)
        for card in self.library:
            expanded.extend([card.oracle_id] * card.quantity)
        return tuple(expanded)


def semantic_document(candidate: Candidate) -> dict[str, Any]:
    """Fields that define simulator semantics; provenance/UI notes are excluded."""
    return {
        "commander_oracle_ids": sorted(candidate.commander_oracle_ids),
        "library": [
            {"oracle_id": card.oracle_id, "quantity": card.quantity}
            for card in sorted(candidate.library, key=lambda card: card.oracle_id)
        ],
        "schema_version": "cedh-deck-candidate.v1",
        "strategy_pack_id": candidate.strategy_pack_id,
        "strategy_pack_version": candidate.strategy_pack_version,
    }


def candidate_hash(candidate: Candidate) -> str:
    canonical = json.dumps(semantic_document(candidate), sort_keys=True, separators=(",", ":"))
    return "sha256:" + hashlib.sha256(canonical.encode()).hexdigest()


def load_candidate(path: Path) -> Candidate:
    raw = json.loads(path.read_text())
    if not isinstance(raw, dict):
        raise CandidateError("candidate must be a JSON object")
    if raw.get("schema_version") != "cedh-deck-candidate.v1":
        raise CandidateError("unsupported candidate schema_version")
    try:
        commanders = tuple(str(value) for value in raw["commander_oracle_ids"])
        library = tuple(
            CandidateCard(str(item["oracle_id"]), int(item["quantity"]))
            for item in raw["library"]
        )
        candidate = Candidate(
            candidate_id=str(raw["candidate_id"]),
            commander_oracle_ids=commanders,
            library=library,
            strategy_pack_id=str(raw["strategy_pack_id"]),
            strategy_pack_version=str(raw["strategy_pack_version"]),
            candidate_hash=str(raw["candidate_hash"]),
        )
    except (KeyError, TypeError, ValueError) as exc:
        raise CandidateError(f"malformed candidate: {exc}") from exc
    if len(commanders) not in (1, 2) or len(set(commanders)) != len(commanders):
        raise CandidateError("commander_oracle_ids must contain one or two unique IDs")
    if not library or any(card.quantity < 1 for card in library):
        raise CandidateError("library entries need positive quantities")
    if len({card.oracle_id for card in library}) != len(library):
        raise CandidateError("duplicate library oracle_id; coalesce it with quantity")
    total = sum(card.quantity for card in library)
    if total != 99:
        raise CandidateError(f"library quantities sum to {total}, expected exactly 99")
    expected = candidate_hash(candidate)
    if candidate.candidate_hash != expected:
        raise CandidateError(f"candidate_hash mismatch; expected {expected}")
    provenance = raw.get("provenance")
    if not isinstance(provenance, dict):
        raise CandidateError("provenance must be an object")
    producer = provenance.get("producer")
    if not isinstance(producer, dict) or not all(
        isinstance(producer.get(key), str) and producer[key] for key in ("name", "version")
    ):
        raise CandidateError("provenance.producer requires non-empty name and version")
    for field in ("card_data", "corpus"):
        source = provenance.get(field)
        if not isinstance(source, dict) or not all(
            isinstance(source.get(key), str) and source[key]
            for key in ("source", "snapshot_at", "hash")
        ):
            raise CandidateError(
                f"provenance.{field} requires non-empty source, snapshot_at, and hash"
            )
    return candidate
