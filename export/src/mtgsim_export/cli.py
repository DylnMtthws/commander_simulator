"""Command-line entry point for the exporter."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any

import psycopg
from psycopg.rows import dict_row

from mtgsim_export.candidate import CandidateError, load_candidate
from mtgsim_export.deck import DeckError, load_deck
from mtgsim_export.effects import check_effects
from mtgsim_export.export import ExportError, build_export, deck_from_candidate
from mtgsim_export.mana import ManaCostError

ENV_VAR = "MTGSIM_DATABASE_URL"
DEFAULT_ENV_FILE = Path(__file__).resolve().parents[2] / ".env"


def load_env_file(path: Path) -> dict[str, str]:
    """Read KEY=VALUE lines from a .env file.

    Hand-rolled rather than a python-dotenv dependency: this needs to handle
    KEY=VALUE and comments, and nothing else. A dependency here would be more
    supply chain than the twenty lines are worth.

    Values already in the real environment WIN. An explicit
    `MTGSIM_DATABASE_URL=... mtgsim-export` must not be silently overridden by
    a stale file, which is the usual way a dotenv loader surprises someone.
    """
    if not path.is_file():
        return {}
    found: dict[str, str] = {}
    for line in path.read_text().splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or "=" not in stripped:
            continue
        key, _, value = stripped.partition("=")
        found[key.strip()] = value.strip().strip("\"'")
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Export card data for the simulator.")
    parser.add_argument("--deck", type=Path, default=Path("data/kinnan.deck.toml"))
    parser.add_argument(
        "--candidate",
        type=Path,
        help="cedh-deck-candidate.v1 JSON; resolves the deck by oracle_id",
    )
    parser.add_argument("--out", type=Path, default=Path("data/cards.json"))
    parser.add_argument("--effects", type=Path, default=Path("data/effects.toml"))
    parser.add_argument(
        "--database-url",
        default=os.environ.get(ENV_VAR),
        help=f"defaults to ${ENV_VAR}, then export/.env; "
        "connect as mtg_consumer, not the pipeline role",
    )
    args = parser.parse_args(argv)

    if not args.database_url:
        args.database_url = load_env_file(DEFAULT_ENV_FILE).get(ENV_VAR)

    if not args.database_url:
        parser.error(
            f"no database URL: pass --database-url, set {ENV_VAR}, "
            f"or put it in {DEFAULT_ENV_FILE} (see .env.example)"
        )

    try:
        with psycopg.connect(args.database_url, row_factory=dict_row) as conn:
            candidate = load_candidate(args.candidate) if args.candidate else None
            deck = deck_from_candidate(conn, candidate) if candidate else load_deck(args.deck)
            document = build_export(conn, deck)
            if candidate:
                document["manifest"].pop("table", None)
                document["manifest"].pop("ablation_replacement", None)
                document["manifest"]["candidate_id"] = candidate.candidate_id
                document["manifest"]["candidate_hash"] = candidate.candidate_hash
                document["manifest"]["strategy_pack_id"] = candidate.strategy_pack_id
                document["manifest"]["strategy_pack_version"] = candidate.strategy_pack_version
    except (CandidateError, DeckError, ExportError, ManaCostError) as exc:
        # These are the loud failures the design asks for. Print the reason and
        # write nothing: a partial cards.json is worse than none, because the
        # simulator would happily run on it.
        print(f"export failed: {exc}", file=sys.stderr)
        return 1

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(document, indent=2, sort_keys=False) + "\n")

    manifest: dict[str, Any] = document["manifest"]
    print(f"wrote {args.out}: {manifest['card_count']} cards from {manifest['source_view']}")
    print(f"  data as of {manifest['max_content_updated_at']}")
    print(f"  cards sha256 {manifest['cards_sha256'][:12]}")

    report = check_effects(args.effects, document["cards"])
    print(
        f"\neffects: {report.authored}/{manifest['card_count']} authored "
        f"({report.modeled} modeled, {report.inert} inert, "
        f"{len(report.unauthored)} unauthored)"
    )
    for category, count in sorted(report.inert_by_category.items(), key=lambda kv: -kv[1]):
        print(f"    inert/{category:<22} {count}")
    for name in report.unknown:
        print(f"  effects.toml has an entry for '{name}', which is not in this deck")

    # Not fatal, deliberately. A drift warning means a human should RE-READ the
    # card, and failing the export would tempt someone to re-stamp the hash to
    # make it go away - which records "a script ran", not "a person read this".
    if report.drifted:
        print(f"\n  WARNING: {len(report.drifted)} card(s) changed text since authoring.")
        print("  Re-read each and re-run scripts/stamp_effects.py ONLY after reading:")
        for drift in report.drifted:
            print(f"    {drift.name}")

            # Head AND tail: a truncated hash can be identical at the front
            # while differing at the back, which made the first version of this
            # warning print two apparently equal values.
            def brief(value: str) -> str:
                return f"{value[:10]}...{value[-10:]}" if len(value) > 24 else value

            print(f"      authored against {brief(drift.authored_from)}")
            print(f"      text is now      {brief(drift.actual)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
