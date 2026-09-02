# commander_simulator

A Monte Carlo goldfishing simulator for one cEDH Commander deck. Design and
reasoning live in [SIM_PLAN.md](SIM_PLAN.md); this file is how to build it.

**Status:** Phase 0. Build system, layout, test framework and CI. No domain
logic yet, deliberately.

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

## Exporting card data

The simulator never talks to Postgres at run time. A Python exporter reads
`mtg_v1` once and writes `data/cards.json`, so simulations need no database and
a run is pinned to a data snapshot (SIM_PLAN.md §8).

```bash
cd export
cp .env.example .env      # then fill in the mtg_consumer password
uv sync --all-groups
uv run pytest

cd ..
export/.venv/bin/mtgsim-export       # writes data/cards.json
```

`export/.env` is gitignored and holds the only credential in this repo. The
exporter connects as `mtg_consumer`, which has no grant on `mtg_internal` at
all — so a query that reaches past the contract fails immediately instead of
working until a migration moves a column.

`data/cards.json` **is committed**. It is ~75 KB, diffable, and carries a
manifest recording the data snapshot it came from, which is what lets a changed
number be attributed to either the code or the card data.
