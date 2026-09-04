# Hosting cost model

Two line items, priced separately, because they are two workloads:

| | What it is | When it costs money |
|---|---|---|
| **Baseline** | `sim-worker`, the always-on interactive service (`deploy/fly.toml`) | continuously |
| **Per sweep** | `sim-sweep`, the ephemeral batch worker (`deploy/fly.sweep.toml`) | only while a sweep runs |

The earlier draft had one line item: a single always-on `performance-16x`
machine serving both. That machine was the largest entry in the hosting
estimate, and nothing measured in this repository justifies 16 dedicated cores
for a 0.68-second request.

## Measured inputs

All from this repository. Native GitHub Actions `ubuntu-latest` x64, Release
build, seed 1, the generated legal Kinnan fixture, manual workflow run
`33906314023` (recorded in [STATE.md](../STATE.md)).

| Quantity | Value | Note |
|---|---|---|
| Interactive run, 20,000 games, 2 threads | **0.68 s** wall | ~1.4 CPU-seconds |
| Full sweep, 30,000 games, 98 ablations, 2 threads | **297.61 s** wall | ~600 CPU-seconds |
| Resident memory | tens of MB | the workload wants cores, not RAM |
| Sweep parallelism | ~196 independent tasks | 2 per ablation target, `measure_ablations` in `src/cli/main.cpp` |

The 0.68 s figure is **one run on an otherwise idle machine**. It is evidence
that a single interactive request is cheap. It is not a measurement of
concurrent capacity, and nothing here has measured that yet — see
[Open measurement](#open-measurement-the-load-test).

## Price inputs

Prices are **not** asserted here. Each symbol below is a variable to be filled
from the vendor's current pricing page at the time of deployment. The "anchor"
column records the only figures already present in the project's own planning
documents, so a reader can tell a stale estimate from a fresh one.

| Symbol | Meaning | Value | Anchor already in the project |
|---|---|---|---|
| `P_web` | Fly `shared-cpu-2x` + 1 GB, running 24/7, per month | **unfilled — read the Fly pricing page** | `CLOUD_MIGRATION_PLAN.md` priced `shared-cpu-1x` + 1 GB at $6–$11/mo |
| `P_cpu_s` | Fly `performance` vCPU, per vCPU-second | **unfilled** | `CLOUD_MIGRATION_PLAN.md` priced one `performance-16x` sweep at ~$0.006 |
| `P_rootfs` | Stopped machine, rootfs storage only, per month | **unfilled** | `CLOUD_MIGRATION_PLAN.md`: "a stopped machine costs only rootfs storage" |
| `N_sweeps` | Sweeps run per month | **unfilled — a product decision** | `CLOUD_MIGRATION_PLAN.md` modelled 200/mo |

## Baseline (always-on)

```
baseline_monthly = P_web  +  P_rootfs        # sim-worker running  +  sim-sweep stopped
```

`sim-sweep` contributes only `P_rootfs` to the baseline: `min_machines_running
= 0` with `auto_start_machines`, so no batch compute is provisioned between
sweeps. That is the point of splitting the apps.

`sim-worker` runs one machine warm (`min_machines_running = 1`) so the first
request of the day does not pay a ~2 s cold start. Setting it to `0` reduces the
baseline to `2 × P_rootfs` and is a one-line, reversible change; nothing else in
the deployment depends on the value.

## Per sweep (usage-based)

```
cost_per_sweep   ≈ 600 CPU-seconds × P_cpu_s  +  machine start/stop overhead
sweep_monthly    =  N_sweeps × cost_per_sweep
```

The important property: **~600 CPU-seconds is a property of the work, not of the
machine.** The sweep is a work queue over ~196 independent tasks, so doubling
vCPUs roughly halves wall time while leaving CPU-seconds spent about the same.
Per-sweep cost is therefore close to invariant under machine size — size buys
*latency*, not *money*:

| `sim-sweep` size | Projected wall time | CPU-seconds billed | Cost relative to 16x |
|---|---|---|---|
| `performance-2x` | ~300 s | ~600 | ~1× |
| **`performance-4x` (default)** | **~150 s** | **~600** | **~1×** |
| `performance-8x` | ~75 s | ~600 | ~1× |
| `performance-16x` | ~37 s | ~600 | ~1× |

Only the 2-thread row is measured; the rest are projections from it and from the
queue's task count. The consequence is that the migration plan's ~$0.006 per
sweep and ~$1.20 per month at 200 sweeps carry over to `performance-4x` within
rounding. Dropping from 16 to 4 vCPUs saves nothing per sweep. **What the split
saves is the 16-vCPU machine's idle time**, which under the old draft was billed
continuously whether or not anyone swept.

`performance-4x` is the default because it is the smallest size that completes
with real margin: at 2 vCPUs the projected 300 s equals `SIM_TIMEOUT_SECONDS`
exactly. No completion-time requirement exists anywhere in this repository, so
nothing argues for buying more. Raise `size` in `deploy/fly.sweep.toml` if one
appears; `CS_THREADS` is deliberately unset there so the thread count follows
the machine.

## Open measurement: the load test

The web tier is sized at 2 vCPUs on single-run evidence plus a small-memory
measurement. Before treating that as settled, run a concurrency test and size
from its result:

```bash
docker run --rm -p 8080:8080 -e SIM_TESTING=1 -e SIM_CARDS_FILE=/tmp/cards.json \
  --mount "type=bind,source=$(pwd)/build/service-fixture/cards.json,target=/tmp/cards.json,readonly" \
  --cpus 2 --memory 1g mtgsim:local
```

Then drive `/simulate` at 20,000 games with 1, 2, 4 and 8 concurrent clients and
record p50/p95/p99 end-to-end latency, the 429 rate, and peak RSS.

- `SIM_MAX_CONCURRENT = 1` means the machine runs one simulation at a time and
  answers the rest with `429 busy`. The number to watch is therefore the **429
  rate at expected peak concurrency**, not CPU saturation.
- **Stay at 2 vCPUs** if p95 stays inside the latency objective and 429s are
  rare at expected peak.
- **Move to 4 vCPUs** (`shared-cpu-4x`, or `performance-2x` if the variance
  rather than the mean is the problem) if p95 degrades or 429s are common. Raise
  `SIM_MAX_CONCURRENT` in the same change, and re-measure: two concurrent
  simulations each want `CS_THREADS` cores.
- Peak RSS above ~500 MB would be the one result that argues for more memory;
  the measured tens of MB says it will not.
