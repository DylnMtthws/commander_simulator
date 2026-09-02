#!/usr/bin/env python3
"""Fill in `authored_from` hashes in data/effects.toml from the current export.

Line-based rather than a TOML round-trip, because effects.toml is a
hand-authored document and its comments carry most of its value - a parse and
re-emit would silently delete every one of them.

Run this ONLY when the text was genuinely re-read. Stamping to silence a drift
warning without reading the card defeats the entire mechanism: the hash then
records "someone ran a script", not "a human read this text".
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CARDS = ROOT / "data" / "cards.json"
EFFECTS = ROOT / "data" / "effects.toml"

HEADER = re.compile(r'^\[cards\."(?P<name>.+)"\]\s*$')
AUTHORED = re.compile(r'^(?P<lead>authored_from\s*=\s*)"(?P<value>[^"]*)"\s*$')


def main() -> int:
    only_check = "--check" in sys.argv
    hashes = {c["listed_name"]: c["oracle_sha256"] for c in json.loads(CARDS.read_text())["cards"]}

    lines = EFFECTS.read_text().splitlines()
    current: str | None = None
    stamped = 0
    drifted: list[str] = []

    for i, line in enumerate(lines):
        header = HEADER.match(line)
        if header:
            current = header["name"]
            continue
        match = AUTHORED.match(line)
        if not match or current is None:
            continue
        want = hashes.get(current)
        if want is None:
            print(f"effects.toml names a card not in the export: {current}")
            return 1
        if match["value"] == want:
            continue
        if match["value"]:
            drifted.append(current)
        lines[i] = f'{match["lead"]}"sha256:{want}"' if False else f'{match["lead"]}"{want}"'
        stamped += 1

    if only_check:
        print(f"{stamped} entries would change; drifted: {drifted or 'none'}")
        return 1 if stamped else 0

    EFFECTS.write_text("\n".join(lines) + "\n")
    print(f"stamped {stamped} authored_from hashes")
    if drifted:
        print(f"  NOTE: {len(drifted)} had a DIFFERENT hash, meaning the text changed:")
        for name in drifted:
            print(f"    {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
