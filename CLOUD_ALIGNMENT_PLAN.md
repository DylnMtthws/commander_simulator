# Cloud alignment plan: commander_simulator

Purpose: make this repository deployable as a stateless, on-demand simulation service on Linux x86-64, without changing what the simulator computes. This plan is self-contained. Do not read or modify the sibling repositories (`ingestion_pipeline_mtg`, `bristly_billy_beane`); integrate only through the contracts pinned in section 2.

Scope boundary: **code, tests, images and documentation only.** Do not create cloud accounts, deploy anything, or run anything against a remote database. Deployment happens in a later phase using the artifacts this plan produces.

---

## 1. Context the agent needs

Facts established on 2026-09-04 by direct measurement and code survey:

- The code is portable C++20 with no Apple dependencies. A Release build with GCC 12 on Debian bookworm (arm64) compiled cleanly with no source changes and ran `cs --games 20000` in 0.98 s. Only CI config and prose assume macOS.
- The RNG is fully specified (xoshiro256++ / splitmix64, `src/core/rng.hpp:24-49`), so the run digest must be identical across macOS and Linux. Treat any digest difference as a bug.
- A full sweep (`--sweep --games 30000`, 98 ablations) costs about **400 CPU-seconds** (7m06s CPU on 2 Linux cores; README's 45 s on 8 M1 cores agrees). Memory is tens of MB. The service wants cores, not RAM.
- Three FetchContent dependencies, all pinned: Catch2 v3.7.1 (`tests/CMakeLists.txt:22-28`), nlohmann/json v3.11.3 (`src/io/CMakeLists.txt:27-33`), toml++ v3.4.0 (`src/io/CMakeLists.txt:39-45`). Configure needs GitHub access, or `FIND_PACKAGE_ARGS` picks up system packages.
- CI (`.github/workflows/ci.yml:24`) runs on `macos-15` only; the comment at lines 3-6 calls Linux "a configuration nobody uses." That policy is what this plan reverses.
- No Dockerfile, no HTTP server, no Python bindings exist. Bindings are explicitly out of v1 (`SIM_PLAN.md:2737`). This plan does not add bindings; it wraps the CLI.
- The working tree is 9 commits ahead of `origin/main`, with `contracts/cedh-simulation-result.v2.schema.json`, `data/kinnan.deck.toml`, `export/tests/test_contracts.py` and `src/cli/main.cpp` modified. Commit or stash, then push, before starting.
- `build/` (985 MB) and `export/.venv` (120 MB) are regenerable and gitignored. Never copy them into an image.

Threading facts that determine service latency:

| Path | Location | Threaded today? |
|---|---|---|
| `--sweep` / `--ablate` | `src/cli/main.cpp:560-596` via `drive()` at `:517` | Yes, across games; ablations run serially at `:794-797` |
| `--hands`, `--grid` | `main.cpp:1009`, `:1222` | Yes |
| plain `--games` report | `main.cpp:617-618` | **No** |
| `--request` service baseline | `main.cpp:690-691` | **No**; only its optional ablations are threaded (`:711-713`) |
| thread count | `main.cpp:1374` `hardware_concurrency()`, `--threads` at `:1404` | Ignores container CPU quotas |

---

## 2. Contracts this repo must honor (pinned, do not change unilaterally)

### 2.1 Postgres (unchanged)
The exporter connects as `mtg_consumer`, reads only `mtg_v1.card_any_medium` and `mtg_v1.card_face`, never `mtg_internal`. The DSN arrives in `MTGSIM_DATABASE_URL` and **will carry `?sslmode=require`** in production. psycopg passes this through; add one test that a DSN with a query string is accepted unmodified.

### 2.2 Simulation service HTTP contract (new; the Deck Lab is being built against exactly this)

Listen on `0.0.0.0:8080`. Plain HTTP; TLS is the platform's job.

`GET /healthz` → `200 {"status":"ok","cs_version":"<semver>","strategy_packs":["kinnan-midrange-goldfish@1.0.0", ...]}`

`POST /simulate`, `Content-Type: application/json`:

```json
{
  "candidate": { ...a cedh-deck-candidate.v1 document, verbatim... },
  "games": 20000,
  "turn": 3,
  "seed": 12345,
  "scenario": "goldfish_assembly.v1",
  "sweep": false,
  "ablate": []
}
```

- `candidate` is required. All other fields optional with the defaults shown. `ablate` is a list of card names; `sweep: true` and non-empty `ablate` are mutually exclusive (400 if both).
- `games` is clamped to `SIM_MAX_GAMES` (env, default 60000). A request above the cap is **rejected with 400**, not silently clamped; the result must never claim more games than were run.
- The service resolves card data itself: it runs the exporter for the candidate's oracle ids against `MTGSIM_DATABASE_URL`, then runs `cs --request <candidate> --cards <exported> --games N --turn T --seed S --scenario X [--sweep | --ablate ...] --threads $CS_THREADS --output-json -`.
- **Response 200 body is byte-for-byte the document `cs --output-json -` wrote.** No envelope. The Deck Lab validates it against the result schema it vendors from `contracts/`.
- Response headers: `X-Sim-Version: <cs semver>`, `X-Sim-Result-Schema: <schema id, e.g. cedh-simulation-result.v2>`, `X-Cards-Sha256: <cards_sha256 from the export manifest>`, `X-Sim-Threads: <n>`.
- Errors, JSON body `{"error": "<machine code>", "detail": "<human text>", "stderr": "<tail of cs stderr, max 4 KB>"}`:
  - `400 invalid_request`: malformed JSON, missing candidate, bad field types, games over cap, sweep+ablate.
  - `422 unsupported`: `cs` rejected the candidate (pack/version/commander mismatch, wrong quantity sum). Pass the `cs` exit reason through in `detail`.
  - `503 card_data_unavailable`: the export failed (database unreachable, role check failed).
  - `504 timeout`: `cs` exceeded `SIM_TIMEOUT_SECONDS` (env, default 300).
  - `500 simulator_failed`: any other nonzero exit.
- Concurrency: at most `SIM_MAX_CONCURRENT` (env, default 1) simulations in flight; extra requests receive `429 busy` with `Retry-After: 5`. One machine, all cores, one job at a time is the intended shape.
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
| `SIM_CARDS_FILE` | unset | **test/offline mode**: skip the export and use this file; the service must refuse to start with both this and a DSN set outside `SIM_TESTING=1` |

### 2.3 Platform assumptions
- Target image platform: **linux/amd64** (the Mac mini test was arm64; the cloud is x86-64). CI on `ubuntu-latest` is amd64, which covers it. Build multi-arch if cheap, amd64 is what ships.
- One image contains both the binary and the service; the exporter's Python 3.13 environment is part of it.
- The platform starts the machine on the first inbound request and stops it when idle. Startup to first `/healthz` 200 must be under 2 s; do not do work at import time.

---

## 3. Work items, in order

### W1. Push and clean the tree
Commit or discard the 9 local commits' uncommitted changes, push `main`. Everything below is a PR against a clean `main`.

### W2. Linux CI (`.github/workflows/ci.yml`)
- Add a matrix: `macos-15` and `ubuntu-latest`. On Ubuntu: `sudo apt-get install -y ninja-build`, GCC default. Keep the `ci` preset (asan + `-Werror`).
- Expect GCC to emit warnings Apple Clang did not under `-Wconversion -Wsign-conversion -Wold-style-cast`; fix them at the source, do not weaken the flag set. Budget: under ten sites.
- Rewrite the comment at `ci.yml:3-6`; the target is now Linux first, macOS for local development.
- Add a job step that records `cs --games 2000 --seed 1` digest on both runners and diffs them. The digests must match; this is the cross-platform reproducibility guard.
- Keep the FetchContent cache keyed on `tests/CMakeLists.txt` and `src/io/CMakeLists.txt`.

### W3. `--version` and machine-readable provenance
- Add `cs --version` printing the semver from `src/core/version.cpp` and the build type. The service embeds it in `X-Sim-Version`.
- Confirm the JSON result already carries simulator version, `cards_sha256`, and the schema id. If the schema id is not in the document, add it as a top-level field in the v2 schema and result builder in `src/io/service.cpp:298`.

### W4. Thread the single-core paths
- Route the plain report (`main.cpp:617-618`) and the `--request` baseline (`main.cpp:690-691`) through `drive()` (`main.cpp:517`). This is the primary path the service exercises; leaving it single-threaded wastes every core the machine is paid for.
- Add an existing-digest test: for a fixed seed, the threaded and serial paths must produce identical merged statistics and identical digest. `tests/unit/test_seeding.cpp` already asserts S1 for sweeps; extend the same pattern.
- Make the default thread count cgroup-aware: read `/sys/fs/cgroup/cpu.max` (cgroup v2) when present, else `hardware_concurrency()`. Keep `--threads` as the override.

### W5. Optional but high-value: parallelize `--sweep` across ablations
- `main.cpp:794-797` iterates ablation targets serially, each internally forking per game with two join barriers. Replace with a work queue over (ablation, arm) tasks, each task running its games serially on one thread. 196 independent tasks on 16 threads has no barriers and scales linearly.
- Preserve output order and the paired common-random-number structure. The digest and every reported number must be unchanged; verify against a pre-change run with the same seed.
- Skip this if W2-W4 consume the budget; note it in `STATE.md` as the next compute item.

### W6. Service package
Location: `service/` at the repo root, a small `uv` project (Python 3.13) depending on the `export` package by relative path and on `fastapi` + `uvicorn` (or the standard library `http.server` with `ThreadingHTTPServer` if you prefer zero dependencies; either is acceptable, but the contract in 2.2 is fixed).

- Entry point `mtgsim-serve`. Implements exactly section 2.2.
- The export step reuses the existing exporter's candidate mode (`mtgsim-export --candidate`) as a library call, not a subprocess, so role assertion and `mtg_v1`-only behavior are inherited unchanged.
- Cache exported card files under `/tmp` keyed by (sorted oracle id set, `max_content_updated_at` from the manifest); a cache hit still re-checks the manifest's timestamp against the database at most once per 10 minutes. Correctness first: a stale cache must never serve cards newer data would change. If in doubt, skip the cache.
- `cs` runs with `subprocess.run(..., timeout=SIM_TIMEOUT_SECONDS)`, stdout captured as bytes and returned unmodified, stderr captured and tailed into error bodies.
- Tests (`service/tests/`) run with `SIM_TESTING=1 SIM_CARDS_FILE=tests/fixtures/cards.fixture.json` and a built `cs` from `CS_BIN`, covering: healthz, a valid request round-trip whose body parses and validates against `contracts/cedh-simulation-result.v2.schema.json`, each error class in 2.2, the games cap, the sweep+ablate conflict, and the 429 path with `SIM_MAX_CONCURRENT=1`.
- Add `service` to the CI matrix: build `cs` (release), then run the service tests.

### W7. Dockerfile (repo root) and `.dockerignore`
Multi-stage:

1. `builder`: `debian:bookworm-slim` + `cmake ninja-build g++ git ca-certificates`; `cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF`; build; strip the binary. Configure needs network for FetchContent; that is acceptable at build time.
2. `python`: `python:3.13-slim-bookworm` + `uv`; `uv sync --frozen --no-dev --no-editable` for `export/` and `service/`.
3. `runtime`: `python:3.13-slim-bookworm`, non-root uid 1001, `COPY` the binary to `/app/cs`, `data/` to `/app/data` (excluding `cards.json`, which is generated), the two venvs, `EXPOSE 8080`, `ENTRYPOINT ["mtgsim-serve"]`.

`.dockerignore` must exclude `build/`, `export/.venv`, `service/.venv`, `.git`, `*.md` except what the README says the loader reads.

Add a CI job that builds the image for `linux/amd64` with `push: false`, then runs the container with `SIM_TESTING=1 SIM_CARDS_FILE=...` and curls `/healthz` and one `/simulate`. Image publishing is **not** part of this plan.

### W8. `fly.toml` draft (do not deploy)
Commit a `deploy/fly.toml` documenting the intended shape so the deployment phase does not guess:

```toml
app = "sim-worker"
primary_region = "ord"
[build]
  dockerfile = "Dockerfile"
[env]
  SIM_MAX_CONCURRENT = "1"
  SIM_TIMEOUT_SECONDS = "300"
[http_service]
  internal_port = 8080
  auto_stop_machines = "stop"
  auto_start_machines = true
  min_machines_running = 0
[[vm]]
  size = "performance-16x"
```

Note in the file header that `MTGSIM_DATABASE_URL` is a platform secret, never in the file, and that the app is reached only over the private network (no public IP).

### W9. Documentation
- `README.md`: a "Running as a service" section (image build, env table from 2.2, curl example, offline test mode). Replace the "runs on an Apple Silicon Mac mini and nowhere else" framing wherever it appears.
- `docs/integration-handoff.md`: add the HTTP contract from 2.2 verbatim beside the existing subprocess contract. The subprocess form stays valid.
- `STATE.md`: record what landed, the measured Linux digest parity, and W5's status.

---

## 4. Do not

- Do not add network, JSON, filesystem or threading code to `src/core/`. `scripts/check_core_is_sealed.sh` must keep passing; threading stays in `cli/` as `main.cpp:512-515` explains.
- Do not change any number the simulator reports, any schema field, or the `--request` CLI. Existing subprocess consumers must keep working.
- Do not add Python bindings. The service wraps the CLI.
- Do not commit `data/cards.json`, `export/.env`, or any DSN.
- Do not remove the macOS CI leg; developers still build locally on Apple Silicon.
- Do not deploy, push images, or create accounts.

---

## 5. Definition of done

- [ ] `main` pushed; CI green on both `macos-15` and `ubuntu-latest`, digests equal across platforms.
- [x] `cs --version` exists; result documents carry version, cards hash and schema id.
- [x] `--request` baseline and plain report use all threads; digest unchanged versus serial.
- [ ] `docker build --platform linux/amd64 .` succeeds; container answers `/healthz` in under 2 s and `/simulate` in offline mode.
- [x] Service tests cover every status code in section 2.2.
- [x] `deploy/fly.toml` committed; `README.md`, `docs/integration-handoff.md`, `STATE.md` updated.
- [ ] Hand-off note in `STATE.md` listing: image build command, the env table, the measured sweep and single-run wall times on the CI runner, and W5 status.
