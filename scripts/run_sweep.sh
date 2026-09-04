#!/usr/bin/env bash
# Run one full leave-one-out ablation sweep. This is the only supported way to
# start one: the interactive service (deploy/fly.toml) refuses sweep requests, so
# nobody buys ~600 CPU-seconds by sending a web request.
#
#   scripts/run_sweep.sh --candidate cand.json --cards cards.json      # local
#   scripts/run_sweep.sh --backend fly --candidate cand.json           # ephemeral Fly machine
#
# The local backend runs the built image against an offline card snapshot and
# needs no database. The fly backend starts a one-off machine on the sim-sweep
# app, runs the sweep, prints the result, and destroys the machine; it does not
# deploy, create or scale anything that outlives the run. Sizing comes from
# --vm-size, defaulting to the same size as deploy/fly.sweep.toml.
set -euo pipefail

backend=local
image=mtgsim:local
app=sim-sweep
vm_size=performance-4x
candidate=
cards=
output=
games=30000
threads=

usage() {
  sed -n '2,13p' "$0" >&2
  exit "${1:-2}"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --backend) backend=$2; shift 2 ;;
    --image) image=$2; shift 2 ;;
    --app) app=$2; shift 2 ;;
    --vm-size) vm_size=$2; shift 2 ;;
    --candidate) candidate=$2; shift 2 ;;
    --cards) cards=$2; shift 2 ;;
    --output) output=$2; shift 2 ;;
    --games) games=$2; shift 2 ;;
    --threads) threads=$2; shift 2 ;;
    -h|--help) usage 0 ;;
    *) printf 'unknown argument: %s\n' "$1" >&2; usage ;;
  esac
done

[ -n "${candidate}" ] || { printf 'error: --candidate is required\n' >&2; usage; }
[ -f "${candidate}" ] || { printf 'error: no such candidate: %s\n' "${candidate}" >&2; exit 2; }

sweep_args=(--candidate /work/candidate.json --games "${games}")
if [ -n "${threads}" ]; then
  sweep_args+=(--threads "${threads}")
fi

case "${backend}" in
  local)
    [ -n "${cards}" ] || { printf 'error: --cards is required for the local backend\n' >&2; exit 2; }
    [ -f "${cards}" ] || { printf 'error: no such cards file: %s\n' "${cards}" >&2; exit 2; }
    run=(docker run --rm
      --env SIM_TESTING=1
      --env SIM_CARDS_FILE=/work/cards.json
      --mount "type=bind,source=$(cd "$(dirname "${cards}")" && pwd)/$(basename "${cards}"),target=/work/cards.json,readonly"
      --mount "type=bind,source=$(cd "$(dirname "${candidate}")" && pwd)/$(basename "${candidate}"),target=/work/candidate.json,readonly"
      --entrypoint mtgsim-sweep "${image}" "${sweep_args[@]}")
    ;;
  fly)
    # `fly machine run --rm` is the platform's batch primitive: it creates the
    # machine, runs the command, and destroys it. Nothing is left provisioned.
    if [ "${image}" = mtgsim:local ]; then
      image="registry.fly.io/${app}:latest"
    fi
    run=(fly machine run "${image}" --app "${app}" --rm --vm-size "${vm_size}"
      --entrypoint mtgsim-sweep "${sweep_args[@]}")
    printf 'fly backend: the candidate path must resolve inside the machine.\n' >&2
    ;;
  *)
    printf 'error: --backend must be local or fly\n' >&2
    exit 2
    ;;
esac

printf 'running: %s\n' "${run[*]}" >&2
if [ -n "${output}" ]; then
  "${run[@]}" > "${output}"
  printf 'wrote %s\n' "${output}" >&2
else
  "${run[@]}"
fi
