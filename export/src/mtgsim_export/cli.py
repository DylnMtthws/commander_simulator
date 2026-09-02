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
    parser.add_argument("--out", type=Path, default=Path("data/cards.json"))
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
