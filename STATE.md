# State

Where the work is. Design decisions and findings live in
[SIM_PLAN.md](SIM_PLAN.md), how to build it in [README.md](README.md). Updated
when an item completes.

## Position

**Phases 0–7, the v2 machine contract, and the stateless HTTP service boundary
are complete, including R3.** The model executes Kinnan's dig rather than
detecting it, which was the last known missing verb on the deck's main line.
`kinnan-midrange-goldfish@1.0.0` is the first and only installed strategy pack;
unsupported packs and commanders are refused.

The current headline, 60,000 games, `data/kinnan.deck.toml` (list A, snapshot
2026-09-01, 99 sha256 `f3919eaf`):

| | |
|---|---|
| P(assembled by turn 3) | **1.41%** |
| P(assembled by turn 6) | **20.86%** |
| P(assembled by turn 12) | **76.82%** |
| censored | 23.18% |

Read §9.5's honesty header before any of those. **A faster number is not a better
deck**, and the model cannot see 31 of the 99 cards.

## Completed

- **Phases 0–5** — exporter, card DB, mana matching, state and turn loop,
  patterns, the authored policy and `--trace`.
- **Phase 6** — `simulate_batch`, Wilson intervals, censored percentiles, the
  honesty header. (§10.6)
- **Phase 7** — effects authored (96 of 100), the parallel driver, the
  leave-one-out sweep with common random numbers (**28× variance reduction,
  measured**), and **R3**: `SELECT` implemented and Kinnan's dig executed.
- **Two consumer-facing outputs** — `--hands` (raw sampled hands, §13.1 option
  B) and `--grid` (the keep/mull feature grid, option A).
- **Two mechanical checks in CI** — `check_core_is_sealed.sh` and
  `check_effects_are_read.sh`. The second caught a field added in the same
  session that added the check.
- **Three versioned JSON schemas** — `cedh-deck-candidate.v1` and simulation
  results v1/v2, with schema fixtures, executable 99-card/hash checks, stable
  JSON-only stdout, provenance/coverage warnings, and explicit rejection of an
  unsupported-pack fixture. The current CLI and HTTP service emit v2.
- **Oracle-ID exporter input** — `mtgsim-export --candidate`, constrained to
  `mtg_v1` and the `mtg_consumer` role.
- **Cloud alignment W1–W9** — `main` was cleaned and pushed; Linux
  joined the macOS CI matrix; baseline simulations use quota-aware threading;
  full sweeps use an ablation/arm work queue; and the zero-envelope HTTP
  service, linux/amd64 image, offline container smoke test, and deployment draft
  are landed on `cloud-alignment`.

## Cloud service hand-off (2026-09-04)

Build the target image without publishing it:

```bash
docker build --platform linux/amd64 --tag mtgsim:local .
```

Runtime configuration:

| Variable | Default | Meaning |
|---|---|---|
| `MTGSIM_DATABASE_URL` | required | Consumer DSN passed unchanged to psycopg; production includes `?sslmode=require` |
| `CS_BIN` | `/app/cs` | Simulator binary |
| `CS_THREADS` | cgroup-aware CPU count | Passed as `--threads` |
| `CS_PACKS_DIR` | `/app/data` | Strategy pack registry passed as `--deck` |
| `SIM_PORT` | `8080` | HTTP listen port |
| `SIM_MAX_GAMES` | `60000` | Requests above this are rejected with 400 |
| `SIM_TIMEOUT_SECONDS` | `300` | Simulator subprocess timeout |
| `SIM_MAX_CONCURRENT` | `1` | Simulations in flight; excess receives 429 |
| `SIM_ALLOW_SWEEP` | `0` | `1` makes this process the batch sweep worker; refuses to start without a token |
| `SIM_SWEEP_TOKEN` | unset | Bearer token a sweep request must present |
| `SIM_MAX_ABLATIONS` | `8` | Most cards one request may name in `ablate` |
| `SIM_CARDS_FILE` | unset | Test/offline card snapshot; cannot coexist with a DSN outside `SIM_TESTING=1` |

Measured Release timings on the native GitHub Actions `ubuntu-latest` x64
runner, seed 1, two threads, and the generated legal Kinnan fixture: a
20,000-game single run took **0.68 s**; the 30,000-game full 98-ablation sweep
took **297.61 s (4m57.61s)**. These values came from manual workflow run
`33906314023`; the benchmark is manual-only so ordinary CI does not pay for a
full sweep.

For comparison, the final linux/amd64 image under amd64 emulation on the arm64
Mac mini took 2.641 s and 1,115.987 s respectively. The emulated sweep before
W5 took 1,154.704 s, so the two-worker queue saved 38.717 s (3.35%) even on that
low-concurrency host.

Reproducibility was checked independently of those timings. A 2,000-game run,
seed 1, and the same generated fixture produced digest `a1d3dda25c8e2d8d` on
both the GitHub Actions macOS arm64 and Ubuntu x64 runners; CI diffed the two
artifacts successfully. W4's pre-change, serial, and threaded reference runs
also all produced `775e9cd226241071`; no expected value was updated.

W5 is landed. A full human sweep report was byte-for-byte identical before and
after; a v2 request sweep was identical after removing only timestamps. The
queue runs the paired-CRN and independently seeded arms as separate serial tasks
and reconstructs results in target order.

## Interactive and batch are separate machines (2026-09-04)

The 16-vCPU always-on machine in the deployment draft is gone. It was the
largest line item in the hosting estimate, and the only workload that ever
wanted 16 cores — the 98-ablation sweep — no longer runs on the interactive
service at all.

| | Config | Size | Provisioning |
|---|---|---|---|
| `sim-worker`, interactive | `deploy/fly.toml` | `shared-cpu-2x`, 1 GB, `CS_THREADS=2` | one warm machine |
| `sim-sweep`, batch | `deploy/fly.sweep.toml` | `performance-4x`, threads follow the vCPUs | `min_machines_running = 0`; started per sweep, stopped after |

Two vCPUs is sized from the 0.68 s / 20,000-game interactive measurement above
and from resident memory in the tens of MB. Four is sized from the sweep's ~600
CPU-seconds: it is the smallest size that finishes with margin, since 2 vCPUs
projects to ~300 s, exactly `SIM_TIMEOUT_SECONDS`. **Neither number is a
concurrency measurement.** 0.68 s is one run on an idle machine; the load test
that would decide between 2 and 4 vCPUs for the web tier is specified in
`docs/hosting-cost-model.md` and has not been run.

Sweeps are gated server-side, not by what the client sends:

- `sweep: true` on the interactive service returns **403 `sweep_not_allowed`**
  before `cs` is started. It needs `SIM_ALLOW_SWEEP=1` *and* a valid
  `Authorization: Bearer` token, or **401 `unauthorized`**.
- `ablate` is capped at `SIM_MAX_ABLATIONS` (8), so nobody buys a sweep's worth
  of compute by naming 98 cards one at a time. The Deck Lab hardcoding an empty
  ablation list was never a boundary and is not treated as one.
- Startup fails if `SIM_ALLOW_SWEEP=1` with no token; the batch tier fails
  closed.
- `mtgsim-sweep` (`scripts/run_sweep.sh`) runs the same sweep as a one-off
  command, for an ephemeral machine, a CI job or a shell. No queue, database or
  new vendor was added.

Cost is now two line items — baseline always-on, and per-sweep usage — worked
through with named, unfilled price variables in `docs/hosting-cost-model.md`.
Per-sweep cost is roughly invariant to worker size, because ~600 CPU-seconds is
a property of the work; size buys wall time, not money.

## In flight

No implementation phase is currently in flight. The service boundary does not
imply that a second strategy or an opponent model already exists, and the
historical Kinnan measurements and human-readable CLI remain unchanged in
meaning.

## Where the chart stands

`cs --grid 3000 --games 300`, four features taken from the deck's published
primer, ~8 seconds on 8 cores.

- **It works and it separates**: a 97-point spread across cells against a
  0.8-point interval per hand.
- **All four of the primer's stated keep heuristics now confirm.** The fourth
  contradicted the model until R3; building the missing verb closed it (§17.4).
- **10% of cell pairs are ordered differently by turn 3 and turn 12.** That is
  printed on the chart, computed each run, in those words — "which hand is better
  has no answer here without naming the turn".
- **It is not a keep/mull recommendation.** A mulligan decision compares a hand
  against the *expectation over mulliganing*, which is the recursion §13.1
  describes and which is **not built**. The chart is one half of that.

## What the hands need

The primer's 20 worked hands are held out and **unscored** (§18). Two things
have to happen before they are worth anything, in this order:

1. **Correction against the images.** The primer presents each hand as a picture
   of seven cards; what exists in text is the prose around it, which names the
   cards the *line* uses and not the cards the *hand* holds. Thirteen
   reconstructions are recorded in §18 for someone to check against the images.
   This needs a person reading, not more machinery.
2. **Scoring against the version each was written for.** Even perfectly
   transcribed, only 2 of 20 are playable from list A — and both are keeps, so
   the set has no negative class and a rule saying "keep" scores 2 for 2. The
   blockers are cards A never had and cards only version C had (§15A). **The
   validation set and the modelled list must be the same version.**

Until both are done there is no agreement count, and producing one anyway would
be the least defensible number in the document.

## Known wrong, deliberately

- **`Hullbreaker Horror` is filed `inert` and the classification is wrong.**
  Only its *first* mode says "you don't control"; the second is an unrestricted
  bounce and the primer builds an infinite-mana line on it. Left in place because
  authoring it needs a bounce-and-replay verb the loop lacks; **printed in the
  run report** under the inert table so no reader meets a number without it
  (§17.3).
- **Four cards unauthored** — *Sylvan Library*, *The One Ring*, *Thrasios*'s
  activated ability, *Valley Floodcaller*. Each needs a verb the loop does not
  have. Thrasios still appears in the sweep because he is a *pattern term*.
- **`infinite_C_outlet_in_hand` fires zero times, provably** — its entry cost is
  Thrasios's own cast cost, so "in hand" and "castable" cannot both hold under a
  policy that casts what it can afford (§5.3's fifth cause).

## On resuming

- **`source_url` in `[provenance]` is empty and required to be present.** The
  Moxfield URL was never recorded for this snapshot; the run report prints the
  gap every run. Fill it when it is to hand — do not reconstruct one.
- **The deck file is list A and stays list A.** List B (exported 2026-09-02)
  differs by 29 cards. Re-authoring would discard every measurement in §16 and
  produce a document describing neither list.
- **Not in v1, deliberately**: the mulligan recursion, opposition profiles, a
  supported second strategy pack, controlled disruption, pairwise ablation,
  Thrasios's scry, or a general Magic rules engine.
