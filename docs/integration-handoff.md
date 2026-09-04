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

The two authoritative contracts are:

- `contracts/cedh-deck-candidate.v1.schema.json`
- `contracts/cedh-simulation-result.v1.schema.json`

Their `schema_version` values are `cedh-deck-candidate.v1` and
`cedh-simulation-result.v1`. Additive or semantic changes require a new schema
version; consumers should reject unknown versions.

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
falls back to Kinnan logic or invents a policy.

`candidate_hash` is `sha256:` followed by lowercase hexadecimal. Hash compact
UTF-8 JSON, with object keys sorted lexicographically, containing only:

```json
{
  "commander_oracle_ids": ["sorted IDs"],
  "library": [{"oracle_id": "sorted ID", "quantity": 1}],
  "schema_version": "cedh-deck-candidate.v1",
  "strategy_pack_id": "kinnan-midrange-goldfish",
  "strategy_pack_version": "1.0.0"
}
```

Sort commander IDs. Coalesce the library by oracle ID, sort it by oracle ID,
and serialize with no insignificant whitespace. `candidate_id`, `provenance`,
and `user_constraints` are intentionally not semantic and are excluded from the
hash. Producers should validate against the JSON Schema and perform the
quantity-sum invariant before submitting a candidate. A complete accepted
example is `tests/fixtures/kinnan-candidate.v1.json`.

## Export and invoke

The exporter must connect as database role `mtg_consumer`. It checks
`current_user` and reads only `mtg_v1.card_any_medium` and `mtg_v1.card_face`.
It does not read `mtg_internal`.

```sh
cd export
uv run mtgsim-export \
  --candidate ../tests/fixtures/kinnan-candidate.v1.json \
  --out ../data/cards.json
```

Invoke the service and write the result to a file:

```sh
build/release/src/cli/cs \
  --request tests/fixtures/kinnan-candidate.v1.json \
  --cards data/cards.json \
  --games 20000 \
  --seed 12345 \
  --output-json result.json
```

Omit `--output-json`, or pass `--output-json -`, to receive one JSON document on
stdout. Machine mode emits diagnostics only on stderr, so successful stdout is
directly parseable. A nonzero exit means no valid result was produced.

`--objective-turn N` changes the requested assembly horizon. The only v1
scenario is `--scenario goldfish_assembly.v1`. A future controlled-disruption
model must use a distinct scenario ID/version rather than changing the meaning
of this one.

An optional paired common-random-number ablation can be included:

```sh
build/release/src/cli/cs \
  --request tests/fixtures/kinnan-candidate.v1.json \
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

## Determinism and metadata

For a fixed simulator version, candidate, snapshot, strategy pack, game count,
seed, objective turn, and scenario, stochastic output is deterministic.
`run_id` is derived from those request semantics and
`determinism.result_digest` fingerprints the simulated outcomes. Only
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

`tests/fixtures/unsupported-pack-candidate.v1.json` is deliberately small. It
is schema-valid but must be rejected by the Kinnan service boundary, proving
that another named pack does not accidentally execute Kinnan strategy.
