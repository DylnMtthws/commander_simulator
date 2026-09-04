# commander_simulator

A versioned cEDH goldfish-simulation service boundary whose first and currently
only supported strategy pack is Kinnan, Bonder Prodigy. It is not a general
Magic rules engine. Design and findings live in [SIM_PLAN.md](SIM_PLAN.md),
current status in [STATE.md](STATE.md); this file is how to build it and what it
means.

## What this measures, and what it does not

**It measures one thing: the turn this deck reaches a declared board state,
playing alone.** That is a *goldfish* number.

> **A faster number is not a better deck.** This model has no opponents, no
> stack, no combat and no interaction. It cannot tell you whether a card is
> good; it can tell you how fast a declared line assembles with nobody
> interfering, and those are different questions that a single number is very
> good at blurring.

The output says so first, on every run, before any figure — and the label travels
in the column names too. An ablation column is called
`goldfish_turn_to_assembly_delta`, never `score`, because a sorted table headed
`score` *is* a card-quality ranking whatever a banner said further up.

### "Win" here means "assembled", and that is a real distinction

This deck contains **no card that reads "you win the game."** Every actual kill
in it is opponent-facing — *Emrakul* takes an opponent's turn, *Hullbreaker
Horror* bounces permanents you don't control, *Finale of Devastation* wants a
combat step. So the terminal state is outside the model and is replaced by a
**declared proxy**: a state from which a competent pilot wins.

Patterns are therefore named for states and never outcomes —
`infinite_C_into_thrasios`, not `win_thrasios` — and the loader rejects a pattern
named like an outcome.

### 31 of the 99 cards do nothing here, and the run tells you which

Every card is `modeled` or explicitly `inert` with a required reason and a
category from a closed set. The run prints the inert set **grouped**, because a
count says how much the model cannot see and the categories say *what*:

```
WHAT THE MODEL CANNOT SEE  (31 inert cards, by reason)
  interaction           16   counters or removal with nothing to answer
  no_object_in_model     4   needs combat, the stack, or a meaningful graveyard
  opponent_permanent     4   targets, copies or steals an opponent's permanent
  opponent_trigger       6   fires only when an opponent acts
  timing_only            1   alters timing; no stack and no priority here
```

`interaction` dominating is the model being narrow in the direction it claims to
be. If `no_object_in_model` dominated instead, the fix would be a bigger card
model — a different project.

**The size of that gap has been measured once.** The deck's primer calls
*Consecrated Sphinx* "our greatest form of card advantage"; this model rates it
at **−0.01%**, indistinguishable from a blank card, because a card that triggers
on an opponent drawing draws nothing when there are no opponents. Both are
correct. The distance between them is what the no-opponent assumption costs.

### One inert entry is classified WRONG, on purpose, and it says so

**`Hullbreaker Horror` is filed inert and the classification is a misread.** Only
its *first* mode is restricted to permanents you don't control; the second is an
unrestricted bounce, and the deck's own primer builds an infinite-mana line on
bouncing and replaying *your own* artifacts.

It is left in place because modelling it needs a verb the turn loop does not
have — and it is **printed in the run report**, under the inert table, so nobody
meets a number from this model without meeting its largest known error first:

```
  OF THOSE 31, 1 IS CLASSIFIED WRONG - by the author, and left in place:
    Hullbreaker Horror
```

A wrong category that is documented is auditable. A silently corrected one is
not, and every number in SIM_PLAN.md §16 was measured with it inert.

### Four more cards are unauthored, and the header counts them

*Sylvan Library*, *The One Ring*, *Thrasios*'s activated ability and *Valley
Floodcaller* each need a verb the loop does not have. They are drawn, they dilute
every draw, and they do nothing when cast — which **understates** the deck, and
is reported as such.

### The decklist is a snapshot, and it is stamped

`data/kinnan.deck.toml` carries a `[provenance]` block — source, snapshot date,
and a sha256 of the 99 — because it is a snapshot of a living Moxfield list and
**that list has since changed by 29 cards**. Three versions are known to exist
and SIM_PLAN.md §15A tells them apart. The URL for this snapshot was never
recorded; the run prints that gap rather than inventing one.

---


**Status:** Phases 0-7 and the v1 service boundary are landed. The card model is
authored (96 of 100 cards; the four that remain each need an effect kind the
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

## Machine-readable service boundary

The authoritative wire contracts, both directions:

- `contracts/cedh-simulation-request.v1.schema.json` — the `POST /simulate` envelope
- `contracts/cedh-deck-candidate.v2.schema.json` — the deck document inside it
- `contracts/cedh-simulation-result.v3.schema.json` — the result
- `contracts/hash-golden-vectors.json` — both hashes, as executable vectors

Superseded versions stay in `contracts/` as historical record and are not
accepted. See `docs/integration-handoff.md` for the cutover.

The normal subprocess form writes JSON only to stdout; diagnostics are stderr:

```bash
./build/release/src/cli/cs \
  --request candidate.json \
  --cards data/cards.json \
  --games 20000 \
  --seed 12345 \
  --output-json - > result.json
```

Use `--output-json result.json` to let `cs` write the file. Optional
`--turn 3` selects the reported objective, `--scenario goldfish_assembly.v1`
names the only v1 scenario, and `--ablate "Sol Ring"` or `--sweep` adds paired
common-random-number ablation results with uncertainty.

Candidate libraries are keyed only by `oracle_id`; quantities must sum to
exactly 99.

Two hashes, answering two questions. `deck_sha256` identifies **the deck list
and nothing else** — sorted commander oracle IDs then the coalesced library,
excluding the strategy pack, the requester and every run parameter. The
simulator recomputes it from the submitted list and refuses a mismatch; it
never trusts the submitted value. `simulation_input_sha256`, on the result,
covers **everything that can change the numbers**: the deck hash, the resolved
pack's identity and content hash, the simulator and card-data versions, the
scenario, seed, games, turn, sweep and ablations. Compare `deck_sha256` for
deck identity; key caches on `simulation_input_sha256`.

These replace v2's `candidate_hash`, which mixed the strategy pack into a field
named like a deck identity — so an identical deck run under a different pack
reported as a different deck. `candidate_hash` is now refused by name rather
than ignored. Producer/card-data/corpus provenance travels beside the hashes,
while `user_constraints` is opaque and is never interpreted by the simulator.

The loaded strategy pack must match both the requested pack version and the
commander oracle IDs. Anything else is rejected. There is no generic fallback
to Kinnan behavior. The current pack is:

```text
kinnan-midrange-goldfish@1.0.0
commander oracle_id 8d11aa49-d4cd-48b1-aa0f-8548fa733416
scenario goldfish_assembly.v1
```

Every result calls the metric `goldfish_turns_to_assembly`, carries card-data
and simulator provenance, reports coverage and censored-aware statistics, and
states explicitly that assembly probability is not win rate. See
[`docs/integration-handoff.md`](docs/integration-handoff.md) for the producer and
UI contract.

## Running as a service

Build the production-shaped Linux image locally without publishing it:

```bash
docker build --platform linux/amd64 --tag mtgsim:local .
```

Production resolves each candidate through the read-only Postgres exporter.
The DSN may include `?sslmode=require` and is passed to psycopg unchanged:

```bash
docker run --rm --platform linux/amd64 -p 8080:8080 \
  -e 'MTGSIM_DATABASE_URL=postgresql://mtg_consumer:password@db/mtg?sslmode=require' \
  mtgsim:local
```

The service reads:

| Variable | Default | Meaning |
|---|---|---|
| `MTGSIM_DATABASE_URL` | required | Consumer DSN; already read by the exporter |
| `CS_BIN` | `/app/cs` | Path to the binary |
| `CS_THREADS` | cgroup-aware CPU count | Passed as `--threads` |
| `CS_PACKS_DIR` | `/app/data` | Strategy pack registry passed as `--deck` |
| `SIM_PORT` | `8080` | Plain HTTP listen port |
| `SIM_MAX_GAMES` | `60000` | Per-request game limit; larger requests are rejected |
| `SIM_TIMEOUT_SECONDS` | `300` | Simulator subprocess timeout |
| `SIM_MAX_CONCURRENT` | `1` | Maximum simulations in flight |
| `SIM_ALLOW_SWEEP` | `0` | `1` makes this process the batch sweep worker |
| `SIM_SWEEP_TOKEN` | unset | Bearer token a sweep request must present; required when sweeps are on |
| `SIM_MAX_ABLATIONS` | `8` | Most cards one request may name in `ablate` |
| `SIM_CARDS_FILE` | unset | Test/offline mode: skip the export and use this file |

On startup, the HTTP service checks that `MTGSIM_DATABASE_URL` connects as
`mtg_consumer`, using the exporter's existing role assertion. A wrong role or
failed connection closes the server and exits with a configuration error.
`/healthz` remains process liveness and responds while that bounded check runs;
each export still checks its own connection. `SIM_CARDS_FILE` skips the startup
database check for offline operation. No database connection is made at import.

Submit the candidate document inside the HTTP request. The successful response
body is exactly the JSON bytes written by `cs --output-json -`:

```bash
curl http://127.0.0.1:8080/healthz

jq -n --slurpfile candidate candidate.json \
  '{candidate: $candidate[0], games: 20000, turn: 3, seed: 12345,
    scenario: "goldfish_assembly.v1", sweep: false, ablate: []}' \
  | curl --fail --header 'Content-Type: application/json' \
      --data-binary @- http://127.0.0.1:8080/simulate > result.json
```

For an offline smoke test, generate a disposable legal 100-card snapshot from
the small shape fixture, then mount it read-only. This mode never contacts a
database:

```bash
mkdir -p build/service-fixture
uv run --python 3.13 --no-project python scripts/make_repro_fixture.py \
  --cards build/service-fixture/cards.json \
  --candidate build/service-fixture/candidate.json

docker run --rm --platform linux/amd64 -p 8080:8080 \
  -e SIM_TESTING=1 -e SIM_CARDS_FILE=/tmp/cards.json \
  --mount "type=bind,source=$(pwd)/build/service-fixture/cards.json,target=/tmp/cards.json,readonly" \
  mtgsim:local
```

Outside `SIM_TESTING=1`, startup refuses a configuration containing both a DSN
and `SIM_CARDS_FILE`. The complete frozen HTTP contract, including response
headers and error bodies, is in
[`docs/integration-handoff.md`](docs/integration-handoff.md).

### Sweeps are a different workload and a different machine

A single interactive run is 20,000 games in **0.68 s** on two threads. A full
leave-one-out sweep is 98 ablations, **297.61 s** on the same two threads and
about 600 CPU-seconds. Serving both from one always-on machine means paying for
the sweep's cores around the clock and letting a five-minute job sit in front of
a sub-second request, so the two are deployed separately:

| | Config | Size | Runs |
|---|---|---|---|
| Interactive | [`deploy/fly.toml`](deploy/fly.toml) | 2 vCPU, 1 GB | warm |
| Sweep | [`deploy/fly.sweep.toml`](deploy/fly.sweep.toml) | 4 vCPU, configurable | only when asked |

`POST /simulate` with `sweep: true` is **refused with 403 by the interactive
service** before `cs` is started. It succeeds only on a deployment with
`SIM_ALLOW_SWEEP=1` and a matching `Authorization: Bearer $SIM_SWEEP_TOKEN`
header. The client sending `"ablate": []` is not what keeps sweeps out of the
request path; the server is.

Without HTTP at all, the same code path runs as a one-off command — the image's
second entry point, and what an ephemeral batch machine executes:

```bash
scripts/run_sweep.sh \
  --candidate build/service-fixture/candidate.json \
  --cards build/service-fixture/cards.json \
  --games 30000 --output sweep.json          # local, no database

scripts/run_sweep.sh --backend fly --candidate /app/candidate.json \
  --vm-size performance-8x                   # one ephemeral Fly machine, destroyed after
```

Sizing, the baseline-versus-per-sweep cost split, and the concurrency test still
outstanding for the web tier are in
[`docs/hosting-cost-model.md`](docs/hosting-cost-model.md).

```bash
./build/release/src/cli/cs --hands 60 --games 4000   # sampled opening hands, raw
./build/release/src/cli/cs --grid 3000 --games 300   # the keep/mull feature grid
```

`--turn N` sets the objective (default 3, see SIM_PLAN.md §4.1). The sweep
reports effect sizes with paired intervals and a measured null taken from the
deck's own inert cards — never a significance verdict. `--hands` is deliberately
a list rather than a chart (§13.1).

## Build

Needs CMake, Ninja and a C++20 compiler. Apple Clang and GCC 12 are both tested.

```bash
# macOS
brew install cmake ninja

cmake --preset asan          # configure
cmake --build --preset asan  # build
ctest --preset asan          # test
./build/asan/src/cli/cs      # run
```

On Debian or Ubuntu, install `cmake ninja-build g++ git`; the same presets and
commands apply. CI runs the sealed-core checks and the full `ci` preset on both
`macos-15` and `ubuntu-latest`.

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
service/      stateless HTTP wrapper around the versioned CLI.
export/       read-only Postgres-to-card-snapshot package.
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

To export the snapshot for a service candidate instead of the legacy
name-based deck TOML:

```bash
export/.venv/bin/mtgsim-export --candidate candidate.json --out data/cards.json
```

That path resolves only `oracle_id` values and queries only the `mtg_v1`
contract while connected as `mtg_consumer`; it refuses a more privileged
database role. It never reads `mtg_internal`.

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
