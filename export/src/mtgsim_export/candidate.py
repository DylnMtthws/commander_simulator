"""The producer-facing deck-candidate contract, ``cedh-deck-candidate.v2``.

This module deliberately uses only the standard library. The exporter needs to
accept a candidate before it has connected to Postgres, and schema-validation
libraries remain a development/test dependency rather than production weight.

**v2 replaced v1's ``candidate_hash``.** v1 hashed the deck list *and* the
requested strategy pack under a name that read like "this deck". A consumer
that computed a deck hash the honest way could never match it, and the
mismatch surfaced as "this is a different deck" when the deck was in fact
identical and only the pack differed. v2 carries :func:`~mtgsim_export.hashes.
deck_sha256` — the list, and only the list — and the pack-inclusive
fingerprint moved to ``simulation_input_sha256`` on the *result*, where it can
be computed against the pack the simulator actually resolved.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from mtgsim_export.hashes import deck_sha256

SCHEMA_VERSION = "cedh-deck-candidate.v2"


class CandidateError(ValueError):
    """A candidate that cannot safely identify a 99-card library.

    ``code`` separates the two failures that must never be confused:

    ``contract_violation``
        The document is malformed, mis-versioned, or breaks a structural
        invariant. Nothing is known about the deck.
    ``deck_hash_mismatch``
        The document is well-formed, but the ``deck_sha256`` the producer
        submitted does not describe the list it submitted alongside it. This
        is the only condition that means "different deck".

    A pack that is not installed is neither: that is an execution-context
    error raised by the simulator, not by candidate parsing.
    """

    def __init__(self, detail: str, code: str = "contract_violation") -> None:
        super().__init__(detail)
        self.code = code
        self.detail = detail


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
    deck_sha256: str

    @property
    def all_oracle_ids(self) -> tuple[str, ...]:
        expanded = list(self.commander_oracle_ids)
        for card in self.library:
            expanded.extend([card.oracle_id] * card.quantity)
        return tuple(expanded)


def candidate_deck_sha256(candidate: Candidate) -> str:
    """Recompute a candidate's deck hash from its own list.

    Independent of whatever the producer submitted: this is the value the
    submission is checked *against*, so it must never read ``candidate.
    deck_sha256``.
    """
    return deck_sha256(
        candidate.commander_oracle_ids,
        [(card.oracle_id, card.quantity) for card in candidate.library],
    )


def load_candidate(path: Path) -> Candidate:
    """Parse and fully validate a ``cedh-deck-candidate.v2`` document.

    Raises:
        CandidateError: With ``code == "deck_hash_mismatch"`` when the deck
            hash does not describe the submitted list, and
            ``code == "contract_violation"`` for every structural failure.
    """
    raw = json.loads(path.read_text())
    if not isinstance(raw, dict):
        raise CandidateError("candidate must be a JSON object")
    declared = raw.get("schema_version")
    if declared != SCHEMA_VERSION:
        raise CandidateError(
            f"unsupported candidate schema_version {declared!r}; supported: {SCHEMA_VERSION}"
        )
    if "candidate_hash" in raw:
        raise CandidateError(
            "candidate carries the removed v1 field 'candidate_hash', which mixed the "
            "strategy pack into a deck identity. Send deck_sha256 (the deck list only); "
            "the pack-inclusive fingerprint is the result's simulation_input_sha256."
        )
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
            deck_sha256=str(raw["deck_sha256"]),
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

    expected = candidate_deck_sha256(candidate)
    if candidate.deck_sha256 != expected:
        raise CandidateError(
            f"deck_sha256 does not describe the submitted list; recomputed {expected}, "
            f"received {candidate.deck_sha256}",
            code="deck_hash_mismatch",
        )

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


def semantic_document(candidate: Candidate) -> dict[str, Any]:
    """The deck-identifying fields, for diagnostics.

    Kept as a debugging aid only. It is **not** a hash preimage: the deck hash
    is line-oriented (see :mod:`mtgsim_export.hashes`) and includes no pack.
    """
    return {
        "commander_oracle_ids": sorted(candidate.commander_oracle_ids),
        "library": [
            {"oracle_id": card.oracle_id, "quantity": card.quantity}
            for card in sorted(candidate.library, key=lambda card: card.oracle_id)
        ],
        "schema_version": SCHEMA_VERSION,
    }
