# cEDH simulation integration handoff

This document defines the boundary between a deck-candidate producer and
`commander_simulator` 0.2.0. The simulator is a versioned goldfish assembly
service. It is not a general Magic rules engine, an opponent model, or a source
of card recommendations.

## Ownership

The producer owns candidate construction, user constraints, corpus selection,
card-data provenance, and the deterministic candidate hash. The Python exporter
owns resolving those candidate oracle IDs through the public `mtg_v1` contract
and producing the enriched card snapshot. The C++ service owns mechanics,
authored effect execution, the selected strategy pack and play policy,
statistics, simulator provenance, and coverage warnings. An application may
render the result but must not relabel or reinterpret its metric.

The authoritative contracts are:

- `contracts/cedh-simulation-request.v1.schema.json` — the `POST /simulate` envelope
- `contracts/cedh-deck-candidate.v2.schema.json` — the deck document inside it
- `contracts/cedh-simulation-result.v3.schema.json` — the result
- `contracts/hash-golden-vectors.json` — the two hashes, as executable vectors

Additive or semantic changes require a new schema version; consumers should
reject unknown versions.

**Both directions are pinned, deliberately.** The previous revision published a
schema for the response only and described the request body in prose. Each side
built its own reading of that prose, each test suite checked its own shape
against itself and stayed green, and every real request failed on the wire with
`422 unsupported candidate schema_version`. A contract pinned in one direction
is not pinned.

`v1`/`v2` of these documents are superseded and are **not accepted**. The files
remain in `contracts/` as historical record; nothing reads them. See
"The two hashes" below for what changed and why a clean cutover was correct.

## The two hashes

There are two, they answer different questions, and v1 had one field that tried
to be both.

| | `deck_sha256` | `simulation_input_sha256` |
|---|---|---|
| Answers | "is this the same deck?" | "is this the same measurement?" |
| Covers | commander oracle IDs + library oracle IDs and quantities | the deck hash, the **resolved** pack's id/version/content hash, simulator version, card-data manifest and `cards_sha256`, scenario, seed, games, turn, sweep, ablations |
| Computed by | the producer, then **independently recomputed** by the simulator | the simulator only |
| Appears in | request (`candidate.deck_sha256`) and result (`candidate.deck_sha256`) | result, top level |
| Correct use | deck identity; "does this stored result describe the list in front of the user?" | cache key; "can I reuse these numbers?" |

`deck_sha256` deliberately excludes the strategy pack, the requester, the
simulator and the corpus. The same 99 cards hash identically no matter who
asked or how they will be run. That is the producer's ADR-025 and the algorithm
is adopted from it verbatim.

**What v1 got wrong.** `candidate_hash` hashed the deck list *together with*
`strategy_pack_id` and `strategy_pack_version`, under a name that read like a
deck identity. A consumer computing an honest deck hash could never match it,
and the mismatch surfaced to users as "this is a different deck" when the deck
was identical and only the pack differed. The two meanings were both reasonable
and mutually incompatible, which is why the fix is two names rather than one
agreed definition.

`candidate_hash` is refused **by name** with an explanatory error rather than
ignored: a producer still sending it would otherwise believe a hash had been
verified when nothing had looked at it.

### `deck_sha256` preimage

SHA-256 over the concatenation of, with no separator beyond the newlines shown:

```text
"C:<oracle_id>\n"        for each commander oracle_id, sorted
"<oracle_id>:<quantity>\n"  for each library entry, coalesced, sorted by oracle_id
```

rendered `sha256:<lowercase hex>`. The `sha256:` prefix says which algorithm
produced the digest, so replacing it is a visible contract change.

### `simulation_input_sha256` preimage

SHA-256 over compact UTF-8 JSON (separators `,` and `:`) with lexicographically
sorted object keys of exactly these fields: `ablations` (sorted),
`card_data_manifest_hash`, `cards_sha256`, `deck_sha256`, `games`,
`objective_turn`, `scenario_id`, `scenario_version`, `schema_version`
(`cedh-simulation-input.v1`), `seed`, `simulator_version`,
`strategy_pack_content_sha256`, `strategy_pack_derived`, `strategy_pack_id`,
`strategy_pack_version`, `sweep`. Rendered `sha256:<lowercase hex>`.

The strategy-pack fields describe the pack the simulator **resolved and ran**,
not the one the request asked for. A request naming an uninstalled pack is
refused before any game runs, so the two cannot differ in a returned result.

`build_flavour` is excluded: debug and release are required to produce
identical statistics, and including it would fragment the cache key for no
behavioural reason.

### Golden vectors

`contracts/hash-golden-vectors.json` pins both algorithms. The C++ simulator
(`tests/unit/test_hash_contract.cpp`), the Python exporter
(`export/tests/test_hash_contract.py`) and the Deck Lab all read that one file.
It proves, executably, that ordering does not affect `deck_sha256`; that
changing a commander, a card or a quantity does; that changing the strategy
pack does **not**; and that changing the strategy pack **does** change
`simulation_input_sha256`. Regenerate with
`uv run --project export python scripts/make_hash_golden_vectors.py` and
re-vendor the file to consumers together with the schemas.

## Error taxonomy

Three failures that must never be conflated. `cs` prefixes its stderr with the
machine code (`error: [<code>] <detail>`) so the HTTP service classifies
without matching English prose.

| `cs` code | HTTP | Meaning | Consumer should say |
|---|---|---|---|
| `deck_hash_mismatch` | 422 | the submitted `deck_sha256` does not describe the submitted list | the deck is not what you think it is |
| `contract_violation` | 422 | malformed, mis-versioned, or structurally invalid document | we and the simulator disagree about the contract |
| `execution_context_unsupported` | 422 `unsupported` | pack not installed, pack/commander unsupported, card snapshot mismatch | this deck cannot be simulated here |

Only the first says anything about the deck. An uninstalled strategy pack or a
stale card export is a fault in *this* service's execution context and says
nothing whatsoever about the user's list; reporting it as a deck mismatch is
both wrong and unactionable.

## Candidate production

A candidate has one or two commander oracle IDs and library quantities totaling
exactly 99. Library oracle IDs are unique entries: coalesce repeated cards by
increasing `quantity`. The current Kinnan strategy pack accepts exactly one
commander:

```text
8d11aa49-d4cd-48b1-aa0f-8548fa733416
```

It must name strategy pack `kinnan-midrange-goldfish` version `1.0.0`.
Unsupported pack IDs, versions, and commanders are errors. The simulator never
falls back to Kinnan logic or invents a policy. `derived-generic@1.0.0` is the
reserved id for explicit derived execution.

The pack is a **request for an execution context**, not part of the deck's
identity: it travels beside `deck_sha256` and is excluded from it.

Compute `deck_sha256` as described under "The two hashes" above. `candidate_id`,
`provenance`, `strategy_pack_id`, `strategy_pack_version` and `user_constraints`
are all excluded from it. Producers should validate against the JSON Schema and
check the quantity-sum invariant before submitting. The simulator recomputes
`deck_sha256` from the submitted list and refuses a mismatch — it never trusts
the submitted value. A complete accepted example is
`tests/fixtures/kinnan-candidate.v2.json`; `tests/fixtures/kinnan-derived-candidate.v2.json`
is the same 99 cards requesting a different pack, and carries the **same**
`deck_sha256`.

## Export and invoke

The exporter must connect as database role `mtg_consumer`. It checks
`current_user` and reads only `mtg_v1.card_any_medium` and `mtg_v1.card_face`.
It does not read `mtg_internal`.

```sh
cd export
uv run mtgsim-export \
  --candidate ../tests/fixtures/kinnan-candidate.v2.json \
  --out ../data/cards.json
```

Invoke the service and write the result to a file:

```sh
build/release/src/cli/cs \
  --request tests/fixtures/kinnan-candidate.v2.json \
  --cards data/cards.json \
  --games 20000 \
  --seed 12345 \
  --output-json result.json
```

Omit `--output-json`, or pass `--output-json -`, to receive one JSON document on
stdout. Machine mode emits diagnostics only on stderr, so successful stdout is
directly parseable. A nonzero exit means no valid result was produced.

`--turn N` changes the requested assembly horizon. The only v1
scenario is `--scenario goldfish_assembly.v1`. A future controlled-disruption
model must use a distinct scenario ID/version rather than changing the meaning
of this one.

An optional paired common-random-number ablation can be included:

```sh
build/release/src/cli/cs \
  --request tests/fixtures/kinnan-candidate.v2.json \
  --cards data/cards.json \
  --games 20000 \
  --seed 12345 \
  --ablate "Sol Ring"
```

`--sweep` produces the existing full leave-one-out set. Each JSON ablation
reports its interval and `paired_common_random_numbers` method.

The service verifies that the candidate's commander and 99 library oracle IDs
and quantities exactly match the enriched snapshot. Export the same candidate
that is sent to the simulator. The result's `card_data` block identifies that
snapshot and corpus boundary.

## HTTP service contract

Listen on `0.0.0.0:8080`. Plain HTTP; TLS is the platform's job.

`GET /healthz` → `200 {"status":"ok","cs_version":"<semver>","strategy_packs":["kinnan-midrange-goldfish@1.0.0", ...],"sweep_enabled":false}`

`sweep_enabled` tells a caller which of the two deployments it reached: `false`
is the interactive service, `true` is the batch sweep worker. It was added when
the workloads were split; consumers that ignore it are unaffected.

`POST /simulate`, `Content-Type: application/json`:

```json
{
  "schema_version": "cedh-simulation-request.v1",
  "candidate": { ...a cedh-deck-candidate.v2 document, verbatim... },
  "games": 20000,
  "turn": 3,
  "seed": 12345,
  "scenario": "goldfish_assembly.v1",
  "sweep": false,
  "ablate": []
}
```

- `schema_version` and `candidate` are required. All other fields optional with the defaults shown. An unrecognised or absent `schema_version` is `400 invalid_request`. `ablate` is a list of card names; `sweep: true` and non-empty `ablate` are mutually exclusive (400 if both).
- `ablate` may name at most `SIM_MAX_ABLATIONS` cards (env, default 8); longer lists are rejected with 400. A client sending an empty list is not a limit — this is.
- **`sweep: true` is batch work and the interactive service refuses it.** It needs a deployment with `SIM_ALLOW_SWEEP=1` (`deploy/fly.sweep.toml`) *and* an `Authorization: Bearer <SIM_SWEEP_TOKEN>` header. On the interactive service the answer is 403 regardless of the header, and `cs` is never started. See [hosting-cost-model.md](hosting-cost-model.md) for why.
- `games` is clamped to `SIM_MAX_GAMES` (env, default 60000). A request above the cap is **rejected with 400**, not silently clamped; the result must never claim more games than were run.
- The service resolves card data itself: it runs the exporter for the candidate's oracle ids against `MTGSIM_DATABASE_URL`, then runs `cs --request <candidate> --cards <exported> --games N --turn T --seed S --scenario X [--sweep | --ablate ...] --threads $CS_THREADS --output-json -`.
- **Response 200 body is byte-for-byte the document `cs --output-json -` wrote.** No envelope. The Deck Lab validates it against the result schema it vendors from `contracts/`.
- Response headers: `X-Sim-Version: <cs semver>`, `X-Sim-Result-Schema: <schema id, e.g. cedh-simulation-result.v3>`, `X-Cards-Sha256: <cards_sha256 from the export manifest>`, `X-Sim-Threads: <n>`.
- Errors, JSON body `{"error": "<machine code>", "detail": "<human text>", "stderr": "<tail of cs stderr, max 4 KB>"}`:
  - `400 invalid_request`: malformed JSON, missing candidate, bad field types, games over cap, sweep+ablate, ablate over `SIM_MAX_ABLATIONS`.
  - `401 unauthorized`: `sweep: true` on a sweep worker without a valid bearer token.
  - `403 sweep_not_allowed`: `sweep: true` on a service that does not run sweeps.
  - `422 unsupported`: the execution context cannot run this candidate (pack not installed, pack/version/commander unsupported, card-snapshot mismatch). Says nothing about the deck.
  - `422 deck_hash_mismatch`: the submitted `deck_sha256` does not describe the submitted list. The **only** code that means "different deck".
  - `422 contract_violation`: malformed or mis-versioned candidate, including one still carrying v1's `candidate_hash`.
  - `503 card_data_unavailable`: the export failed (database unreachable, role check failed).
  - `504 timeout`: `cs` exceeded `SIM_TIMEOUT_SECONDS` (env, default 300).
  - `500 simulator_failed`: any other nonzero exit.
- Concurrency: at most `SIM_MAX_CONCURRENT` (env, default 1) simulations in flight; extra requests receive `429 busy` with `Retry-After: 5`. One machine, all cores, one job at a time is the intended shape.
- Two deployments run this same image. `sim-worker` is small, warm and interactive; `sim-sweep` is larger, scaled to zero, and started only when a sweep is asked for. A sweep can also be run without HTTP at all, via `mtgsim-sweep` (`scripts/run_sweep.sh`), which is the same code path.
- The service is **stateless**. It may be killed between requests at any time. It must not write anything it needs later except an optional cache under `/tmp`.

Environment variables the service reads, all with defaults except the first:

| Variable | Default | Meaning |
|---|---|---|
| `MTGSIM_DATABASE_URL` | required | consumer DSN; already read by the exporter |
| `CS_BIN` | `/app/cs` | path to the binary |
| `CS_THREADS` | cgroup-aware CPU count | passed as `--threads` |
| `CS_PACKS_DIR` | `/app/data` | strategy pack registry passed as `--deck` |
| `SIM_PORT` | `8080` | |
| `SIM_MAX_GAMES` | `60000` | |
| `SIM_TIMEOUT_SECONDS` | `300` | |
| `SIM_MAX_CONCURRENT` | `1` | |
| `SIM_ALLOW_SWEEP` | `0` | `1` turns this process into the batch sweep worker; requires `SIM_SWEEP_TOKEN` or startup fails |
| `SIM_SWEEP_TOKEN` | unset | bearer token a sweep request must present; a platform secret, never in a config file |
| `SIM_MAX_ABLATIONS` | `8` | most cards one request may name in `ablate` |
| `SIM_CARDS_FILE` | unset | **test/offline mode**: skip the export and use this file; the service must refuse to start with both this and a DSN set outside `SIM_TESTING=1` |

## Determinism and metadata

For a fixed simulator version, candidate, snapshot, strategy pack, game count,
seed, objective turn, and scenario, stochastic output is deterministic.
`simulation_input_sha256` is exactly that set of inputs, `run_id` is its first
24 hex characters, and `determinism.result_digest` fingerprints the simulated
outcomes. Only
`timestamps.started_at` and `timestamps.completed_at` are deliberately
nondeterministic metadata. Compare two results after removing the `timestamps`
object.

Threaded ablations retain the existing per-game seed derivation and paired
common random numbers. The result calls the measured quantity
`goldfish_turns_to_assembly`; it never calls assembly probability a win rate.

## Required presentation warnings

Any generator or UI displaying a result must display all returned `warnings`,
`unsupported_assumptions`, coverage counts, inert reason counts, and strategy
pack blind spots. At minimum, users must see that:

- Assembly probability is not win rate, deck strength, or card quality.
- The v1 scenario has no opponents, stack, priority, responses, live
  interaction, combat, or opponent-facing terminal kill.
- A declared assembly pattern ends the game as a proxy; it does not demonstrate
  a rules-complete win.
- Only Kinnan and the authored `kinnan-midrange-goldfish@1.0.0` policy are
  supported. Unauthored cards dilute draws but execute no text; inert cards are
  grouped by their explicit reason.
- The current pack does not model summoning sickness or the legend rule and
  knowingly classifies Hullbreaker Horror as inert. Sylvan Library, The One
  Ring, Thrasios activation, and Valley Floodcaller remain unauthored.
- The model is on the play, keeps its random opening seven without mulligans,
  and uses declared three-opponent/WUBRG context only for card text. Those
  declarations are not opponent behavior.
- The turn cap creates right-censored observations. Percentiles and their
  intervals must preserve their returned observed/censored status.
- Ablations replace one card with the pack's declared baseline and therefore
  measure that controlled substitution, not universal card quality.
- Card-data provenance and candidate/corpus hashes are part of the result and
  must accompany cached or compared measurements.

`tests/fixtures/unsupported-pack-candidate.v2.json` is deliberately small. It
is schema-valid but must be rejected by the Kinnan service boundary, proving
that another named pack does not accidentally execute Kinnan strategy.
