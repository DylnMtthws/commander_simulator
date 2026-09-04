"""Resolution and face normalisation, tested without a database.

These are pure functions on rows, so they need no connection - which is the
point: the logic that decides whether a deck resolved correctly should be
testable without the thing whose bug it exists to catch.
"""

from __future__ import annotations

from typing import Any
from unittest.mock import MagicMock, patch

import pytest

from mtgsim_export.candidate import Candidate
from mtgsim_export.export import ExportError, _face_documents, _resolve, export_candidate
from mtgsim_export.mana import ManaCostError


def card(
    name: str,
    *,
    oracle: str = "",
    faces: int = 0,
    cost: str | None = "{1}{U}",
    mv: int = 2,
    type_line: str = "Instant",
) -> dict[str, Any]:
    return {
        "oracle_id": oracle or name,
        "name": name,
        "mana_cost": cost,
        "mana_value": mv,
        "type_line": type_line,
        "face_count": faces,
    }


def test_resolves_an_exact_name() -> None:
    rows = [card("Sol Ring")]
    assert [r.listed for r in _resolve(["Sol Ring"], rows)] == ["Sol Ring"]


def test_resolves_a_front_face_name() -> None:
    """A decklist writes 'Sink into Stupor'; the database stores the combined
    name. mtg_internal.card_name_index would do this upstream but does not
    exist yet (SIM_PLAN.md section 2.3)."""
    rows = [card("Sink into Stupor // Soporific Springs")]
    resolved = _resolve(["Sink into Stupor"], rows)
    assert resolved[0].row["name"] == "Sink into Stupor // Soporific Springs"


def test_exact_name_beats_a_front_face_match() -> None:
    """A card literally called X must never be shadowed by 'X // Y'."""
    rows = [card("Fire", oracle="exact"), card("Fire // Ice", oracle="split")]
    assert _resolve(["Fire"], rows)[0].row["oracle_id"] == "exact"


def test_rejects_an_unresolved_name_by_name() -> None:
    """This is the section 2.7 defect's shape: the query succeeds and returns
    fewer rows than asked for. Naming the missing card is the whole value."""
    with pytest.raises(ExportError, match=r"Lotus Petal"):
        _resolve(["Sol Ring", "Lotus Petal"], [card("Sol Ring")])


def test_rejects_an_ambiguous_front_face_name() -> None:
    """Two different cards sharing a front face is not resolvable. Picking one
    silently would change the deck."""
    rows = [card("Bruna // A", oracle="a"), card("Bruna // B", oracle="b")]
    with pytest.raises(ExportError, match="ambiguous"):
        _resolve(["Bruna"], rows)


def test_single_faced_card_gets_a_synthetic_face() -> None:
    """Normalisation: the consumer sees one shape, never a branch on
    face_count."""
    faces = _face_documents(
        card("Sol Ring", cost="{1}", mv=1, type_line="Artifact"), [], "Sol Ring"
    )
    assert len(faces) == 1
    assert faces[0]["index"] == 0
    assert faces[0]["cost"] == {"generic": 1, "pips": [], "variable": 0, "phyrexian": []}
    assert faces[0]["is_land"] is False


def test_land_has_no_cost_and_is_flagged() -> None:
    faces = _face_documents(
        card("Forest", cost=None, mv=0, type_line="Basic Land — Forest"), [], "Forest"
    )
    assert faces[0]["cost"] is None
    assert faces[0]["is_land"] is True


def test_multi_faced_card_uses_its_face_rows() -> None:
    row = card("Sink into Stupor // Soporific Springs", faces=2, cost=None, mv=3)
    face_rows = [
        {
            "face_index": 0,
            "name": "Sink into Stupor",
            "mana_cost": "{1}{U}{U}",
            "face_mana_value": 3,
            "type_line": "Instant",
        },
        {
            "face_index": 1,
            "name": "Soporific Springs",
            "mana_cost": None,
            "face_mana_value": None,
            "type_line": "Land",
        },
    ]
    faces = _face_documents(row, face_rows, "Sink into Stupor")
    assert [f["name"] for f in faces] == ["Sink into Stupor", "Soporific Springs"]
    assert faces[0]["cost"]["pips"] == ["U", "U"]
    assert faces[1]["cost"] is None and faces[1]["is_land"] is True


def test_rejects_a_card_whose_faces_are_missing() -> None:
    """face_count says 2 and card_face returns nothing: the contract
    disagreeing with itself. Louder is better than a card with no cost."""
    with pytest.raises(ExportError, match="disagrees with itself"):
        _face_documents(card("X", faces=2, cost=None), [], "X")


def test_face_cost_is_cross_checked_against_mana_value() -> None:
    """Guards against a mis-parse: {1}{U} cannot be mana value 9."""
    with pytest.raises(ManaCostError, match="implies mana value 2 but the database says 9"):
        _face_documents(card("Wrong", cost="{1}{U}", mv=9), [], "Wrong")


def test_candidate_export_passes_a_query_string_dsn_unmodified() -> None:
    dsn = "postgresql://mtg_consumer:secret@db/mtg?sslmode=require"
    candidate = Candidate(
        candidate_id="fixture",
        commander_oracle_ids=("commander",),
        library=(),
        strategy_pack_id="fixture",
        strategy_pack_version="1.0.0",
        deck_sha256="sha256:fixture",
    )
    connection = MagicMock()
    connection.__enter__.return_value = connection
    expected = {"manifest": {}, "cards": []}
    with (
        patch("mtgsim_export.export.psycopg.connect", return_value=connection) as connect,
        patch("mtgsim_export.export.build_candidate_export", return_value=expected),
    ):
        assert export_candidate(dsn, candidate) is expected

    assert connect.call_args.args == (dsn,)
    assert connect.call_args.kwargs.keys() == {"row_factory"}
