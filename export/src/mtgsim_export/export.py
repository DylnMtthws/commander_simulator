"""Query mtg_v1, resolve the deck, and build the export document.

Three decisions worth knowing before reading the code:

1. It queries `mtg_v1.card_any_medium`, never `mtg_v1.card`. `card` silently
   drops 5 of this deck's 99 cards and 254 Reserved List cards corpus-wide,
   because `games` describes the representative printing rather than the card
   (SIM_PLAN.md section 2.7). This is the single most important line in the file.

2. Faces are NORMALISED. Every card gets a `faces` array, including single-faced
   ones, which get a synthetic face 0 built from the card row. The exporter
   absorbs the layout mess so the simulator has exactly one code path instead of
   a branch on `face_count` at every use site.

3. Counts are asserted at every boundary, and the diff is named (RULE C1). A
   contract bug that returns fewer rows is worse than one that errors, so
   "99 names in, 99 rows out" is checked rather than assumed.
"""

from __future__ import annotations

import hashlib
import json
from collections.abc import Sequence
from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Any

import psycopg
from psycopg.rows import dict_row

from mtgsim_export.deck import Deck
from mtgsim_export.mana import check_against_mana_value, parse_cost

SOURCE_VIEW = "mtg_v1.card_any_medium"
EXPORTER_VERSION = "0.1.0"

_CARD_QUERY = f"""
SELECT oracle_id, name, layout, mana_cost, mana_value, type_line, castable_cmcs,
       all_types, color_identity, has_land_face, face_count, content_updated_at
FROM {SOURCE_VIEW}
WHERE name = ANY(%(names)s) OR split_part(name, ' // ', 1) = ANY(%(names)s)
"""

_FACE_QUERY = """
SELECT oracle_id, face_index, name, mana_cost, face_mana_value, type_line
FROM mtg_v1.card_face
WHERE oracle_id = ANY(%(ids)s)
ORDER BY oracle_id, face_index
"""


class ExportError(RuntimeError):
    """An export that must not produce a file. Always names what is missing."""


@dataclass(frozen=True, slots=True)
class Resolution:
    listed: str
    row: dict[str, Any]


def _resolve(names: Sequence[str], rows: list[dict[str, Any]]) -> list[Resolution]:
    """Match each listed name to exactly one card row.

    Exact name wins over a front-face match, so a card literally called "X"
    is never shadowed by "X // Y". Anything ambiguous is an error rather than a
    guess: picking one silently would change the deck.
    """
    by_exact: dict[str, list[dict[str, Any]]] = {}
    by_front: dict[str, list[dict[str, Any]]] = {}
    for row in rows:
        by_exact.setdefault(row["name"], []).append(row)
        by_front.setdefault(row["name"].split(" // ")[0], []).append(row)

    resolved: list[Resolution] = []
    unresolved: list[str] = []
    ambiguous: list[str] = []

    for listed in names:
        candidates = by_exact.get(listed) or by_front.get(listed) or []
        if not candidates:
            unresolved.append(listed)
        elif len({c["oracle_id"] for c in candidates}) > 1:
            ambiguous.append(f"{listed} -> {sorted(c['name'] for c in candidates)}")
        else:
            resolved.append(Resolution(listed=listed, row=candidates[0]))

    if unresolved:
        raise ExportError(
            f"{len(unresolved)} card(s) did not resolve in {SOURCE_VIEW}: {unresolved}. "
            "Check spelling against the database; if a name looks right, this view "
            "may have changed."
        )
    if ambiguous:
        raise ExportError(f"{len(ambiguous)} ambiguous name(s): {ambiguous}")

    # RULE C1: the count assertion, not a truthiness check. It would have caught
    # the section 2.7 defect in one second without anyone having heard of the
    # Reserved List.
    if len(resolved) != len(names):
        raise ExportError(
            f"resolved {len(resolved)} cards from {len(names)} names. "
            "Counts must match exactly."
        )
    return resolved


def _face_documents(
    row: dict[str, Any], faces: list[dict[str, Any]], label: str
) -> list[dict[str, Any]]:
    """Normalise a card to a list of faces, one code path for the consumer."""
    if row["face_count"] == 0:
        source = [
            {
                "face_index": 0,
                "name": row["name"],
                "mana_cost": row["mana_cost"],
                "face_mana_value": row["mana_value"],
                "type_line": row["type_line"],
            }
        ]
    else:
        if not faces:
            raise ExportError(
                f"{label}: face_count is {row['face_count']} but mtg_v1.card_face "
                "returned no rows. The contract disagrees with itself."
            )
        source = faces

    documents: list[dict[str, Any]] = []
    for face in source:
        cost = parse_cost(face["mana_cost"])
        face_mv = face["face_mana_value"]
        if cost is not None and face_mv is not None:
            check_against_mana_value(cost, int(face_mv), f"{label} face {face['face_index']}")
        type_line = face["type_line"] or ""
        documents.append(
            {
                "index": int(face["face_index"]),
                "name": face["name"],
                "type_line": type_line,
                "is_land": "Land" in type_line,
                "mana_value": int(face_mv) if face_mv is not None else None,
                "cost": cost.to_json() if cost is not None else None,
            }
        )
    return documents


def build_export(conn: psycopg.Connection[dict[str, Any]], deck: Deck) -> dict[str, Any]:
    names = list(deck.all_cards)

    with conn.cursor(row_factory=dict_row) as cur:
        rows = cur.execute(_CARD_QUERY, {"names": names}).fetchall()
        resolved = _resolve(names, rows)

        ids = [r.row["oracle_id"] for r in resolved]
        face_rows = cur.execute(_FACE_QUERY, {"ids": ids}).fetchall()

        corpus = cur.execute(f"SELECT count(*) AS n FROM {SOURCE_VIEW}").fetchone()

    faces_by_id: dict[Any, list[dict[str, Any]]] = {}
    for face in face_rows:
        faces_by_id.setdefault(face["oracle_id"], []).append(face)

    # export_index is the stable tiebreak key the simulator uses to make every
    # ordering total (SIM_PLAN.md section 6.5). Assigned from the sorted listed
    # name so it never depends on the query plan or on deck-file order.
    ordered = sorted(resolved, key=lambda r: r.listed)

    cards: list[dict[str, Any]] = []
    for index, item in enumerate(ordered):
        row = item.row
        cards.append(
            {
                "export_index": index,
                "listed_name": item.listed,
                "name": row["name"],
                "oracle_id": str(row["oracle_id"]),
                "layout": row["layout"],
                "mana_value": int(row["mana_value"]),
                "castable_cmcs": list(row["castable_cmcs"]),
                "all_types": list(row["all_types"]),
                "color_identity": list(row["color_identity"]),
                "has_land_face": bool(row["has_land_face"]),
                "is_commander": item.listed == deck.commander,
                "faces": _face_documents(row, faces_by_id.get(row["oracle_id"], []), item.listed),
            }
        )

    if len(cards) != 100:
        raise ExportError(f"built {len(cards)} cards, expected 100 (commander + 99).")

    updated = max(r.row["content_updated_at"] for r in resolved)
    document: dict[str, Any] = {
        "manifest": {
            "exporter_version": EXPORTER_VERSION,
            "generated_at": datetime.now(UTC).isoformat(),
            "source_view": SOURCE_VIEW,
            "deck_name": deck.name,
            "commander": deck.commander,
            "card_count": len(cards),
            "max_content_updated_at": updated.isoformat(),
            "corpus_row_count": int(corpus["n"]) if corpus else None,
            "table": {
                "opponents": deck.opponents,
                "opponent_colors": list(deck.opponent_colors),
                "on_the_play": deck.on_the_play,
            },
            "ablation_replacement": deck.replacement,
            # What the numbers built from this file mean (SIM_PLAN.md 8.3).
            # A stored field, not a comment, so anything rendering results
            # carries it without knowing it exists.
            "metric": "goldfish_turns_to_assembly",
            "measures": "turns until a declared pattern is assembled, unopposed",
            "does_not_measure": "deck strength, win rate, or card quality",
        },
        "cards": cards,
    }
    payload = json.dumps(document["cards"], sort_keys=True, separators=(",", ":"))
    document["manifest"]["cards_sha256"] = hashlib.sha256(payload.encode()).hexdigest()
    return document
