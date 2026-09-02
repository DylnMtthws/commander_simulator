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

from mtgsim_export.deck import DeckError, load_deck
from mtgsim_export.export import ExportError, build_export
from mtgsim_export.mana import ManaCostError

ENV_VAR = "MTGSIM_DATABASE_URL"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Export card data for the simulator.")
    parser.add_argument("--deck", type=Path, default=Path("data/kinnan.deck.toml"))
    parser.add_argument("--out", type=Path, default=Path("data/cards.json"))
    parser.add_argument(
        "--database-url",
        default=os.environ.get(ENV_VAR),
        help=f"defaults to ${ENV_VAR}; connect as mtg_consumer, not the pipeline role",
    )
    args = parser.parse_args(argv)

    if not args.database_url:
        parser.error(f"no database URL: pass --database-url or set {ENV_VAR}")

    try:
        deck = load_deck(args.deck)
        with psycopg.connect(args.database_url, row_factory=dict_row) as conn:
            document = build_export(conn, deck)
    except (DeckError, ExportError, ManaCostError) as exc:
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
