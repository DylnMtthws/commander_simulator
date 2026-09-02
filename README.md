# commander_simulator

A Monte Carlo goldfishing simulator for one cEDH Commander deck. Design and
reasoning live in [SIM_PLAN.md](SIM_PLAN.md); this file is how to build it.

**Status:** Phases 0-7 landed except the ablation sweep. The card model is
authored (94 of 100 cards; the four that remain each need an effect kind the
loop does not have), the mana system is real, the policy is authored and
traceable, and the run report carries Wilson intervals and censored percentiles.

What it says so far is in [SIM_PLAN.md](SIM_PLAN.md) §16, and the short version
is that eight clones are worth a third of a point and one tutor is worth seven
and a half.

```bash
./build/release/src/cli/cs --games 20000        # the report
./build/release/src/cli/cs --trace 12           # one game, turn by turn
./build/release/src/cli/cs --sweep --games 30000   # 98 ablations, ~45s on 8 cores
./build/release/src/cli/cs --ablate "Sol Ring"     # just one
```

```bash
./build/release/src/cli/cs --hands 60 --games 4000   # sampled opening hands, raw
./build/release/src/cli/cs --grid 3000 --games 300   # the keep/mull feature grid
```

`--turn N` sets the objective (default 3, see SIM_PLAN.md §4.1). The sweep
reports effect sizes with paired intervals and a measured null taken from the
deck's own inert cards — never a significance verdict. `--hands` is deliberately
a list rather than a chart (§13.1).

## Build

Needs CMake, Ninja and Apple Clang (Xcode command line tools).

```bash
brew install cmake ninja

cmake --preset asan          # configure
cmake --build --preset asan  # build
ctest --preset asan          # test
./build/asan/src/cli/cs      # run
```

## Presets

| Preset | What it is | When |
|---|---|---|
| `debug` | `-O0 -g`, assertions on | Editing, stepping in a debugger |
| `asan` | `-O1 -g`, ASan + UBSan, assertions on | **Default for running tests** |
| `release` | `-O3 -DNDEBUG` | Benchmarks and real runs only |
| `ci` | `asan` plus `-Werror` | CI |

Run tests under `asan`. It catches use-after-free, buffer overruns and signed
overflow *at the moment they happen* rather than as corrupted output three
functions later, which is worth far more than the ~2x slowdown. `asan` uses
`Debug`, not `RelWithDebInfo`, specifically so `NDEBUG` stays undefined and
`assert()` survives — running the suite with assertions compiled out would
weaken every test in it.

Use `release` only for timing. A benchmark taken under `asan` is meaningless.

## Layout

```
src/core/     the simulation. Links ONLY the standard library.
src/cli/      command-line front end. All I/O lives on this side.
tests/unit/   Catch2 tests
scripts/      check_core_is_sealed.sh
```

`src/core` doing no I/O and linking nothing is the constraint the whole design
rests on: it is what makes Python bindings additive rather than a rewrite, and
what lets the core be called concurrently. CMake enforces the coarse half — a
core file reaching for `io/` will fail to link, because `cs_core` does not link
that target. `scripts/check_core_is_sealed.sh` catches what still compiles:
`<iostream>`, `std::unordered_map`, `std::random_device`, `rand()`. See
SIM_PLAN.md §6.5, §7.3 and §12.5 for why each is banned.

Includes are written `#include "core/version.hpp"` — the include root is `src/`,
so every include names the layer it comes from.

## Getting this to run

**Card data is generated, not committed.** `data/cards.json` is absent from a
fresh clone by design — it is built from a Postgres database that lives outside
this repository, so the file cannot be checked in without also checking in a
snapshot of someone else's data and pretending it is source.

If `cs` says it cannot open `data/cards.json`, nothing is broken. Run the
exporter.

### What you need

- **Postgres with the `mtg_v1` contract schema.** Built by a separate project
  (`ingestion_pipeline_mtg`), which ingests Scryfall into Postgres and exposes a
  stable read-only view layer. This repo does not include it, depend on its
  code, or read its config.
- Credentials for the **`mtg_consumer`** role. That role has `USAGE` on `mtg_v1`
  and no grant whatsoever on `mtg_internal`, so a query reaching past the
  contract fails immediately rather than working until a migration moves a
  column underneath it.

### Generating it

```bash
cd export
cp .env.example .env      # then fill in the mtg_consumer password
uv sync --all-groups
uv run pytest

cd ..
export/.venv/bin/mtgsim-export       # writes data/cards.json
./build/asan/src/cli/cs              # reads it
```

`export/.env` is gitignored and holds the only credential in this repo.

### The manifest, and why a stale file is detectable

Every generated file carries a manifest, and the loader surfaces it on every
run rather than hiding it behind a flag:

```
deck: kinnan.deck.toml   cards: 100   modelled: 65   inert: 31   patterns: 4
data: manifest 0ca3ccc3 (2026-09-01)
```

with the full provenance printed on a bare `cs` run:

```
data/cards.json
  100 cards from mtg_v1.card_any_medium
  data as of 2026-09-01T21:20:19.430220+00:00
  cards sha256 85dbcda196a6
  corpus 34566 rows
```

| Field | What it is for |
|---|---|
| `source_view` | Which view the data came from. `mtg_v1.card` silently drops 254 Reserved List cards, so this is not a detail (SIM_PLAN.md §2.7) |
| `max_content_updated_at` | The newest card in the export. **This is the staleness signal** — an upstream nightly moves it |
| `cards_sha256` | Hash of the card array. Two runs agreeing here used identical card data |
| `corpus_row_count` | Size of the source corpus, which moves when upstream ingests |
| `card_count` | Cross-checked against the array length at load; a mismatch is refused |

Without these, a number that changes six months from now is unattributable —
it could be the code or the card data, and you cannot tell which. With them,
`cards_sha256` answers it in one comparison.

### Testing without a database

CI has no Postgres, and neither do the C++ tests. They use
`tests/fixtures/cards.fixture.json`, a committed 8-card file **selected by
shape rather than by taking the first N**: a normal spell, a basic land, a
nonbasic Reserved List land, a modal DFC, a transform Battle, an X spell, a
Phyrexian cost, and the commander. Each card records what it is there to
exercise in the fixture's own `fixture_rationale`.

**Keep it small. Small is what makes it useful.** The fixture has now caught
three defects the 100-card deck could not, and none was a scale problem:

| Found | Why the real deck could not |
|---|---|
| `export_index` must be dense | Only a *subset* of an export has holes in it |
| `below()` hangs on power-of-two bounds | 8 cards reach a library of 2 on the opening hand; 99 need ~97 draws |
| The S1 signature was blind to draw order | With 100 cards the stub's counters vary; with 8 they do not |

It is not a scaled-down corpus, it is a **differently-shaped input**, and the
shape is what finds things. Growing it toward realism would make it resemble
`data/cards.json` and stop finding anything that file does not. So add a card
only to cover a *shape* that is absent, never to make the set more
representative — if a bug needs a hundred cards to reproduce, it belongs in a
test against the real export.

Regenerate it after any change to the export format:

```bash
uv run --python 3.13 --no-project python scripts/make_fixture.py
```

It is derived from a real export, so it cannot drift into describing a shape
the exporter never produces, and it is reindexed `0..7` so it is a valid card
database in its own right rather than a fragment of a hundred-card one.

## Why the simulator never talks to Postgres

The simulation core does no I/O at all, and the exporter runs once per data
refresh rather than once per simulation. That keeps `libpq` out of the C++
build entirely, makes a run reproducible against a pinned snapshot, and means
simulations run on a machine with no database (SIM_PLAN.md §8).
