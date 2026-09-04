"""Explicitly invoked batch sweep runner.

A full leave-one-out sweep is ~98 ablations and hundreds of CPU-seconds. The
interactive HTTP service refuses that workload (`RequestHandler._authorize_sweep`)
so a web request can never buy five minutes of compute. This module is the
deliberate path instead: one process, one sweep, one result document on stdout,
then exit.

It is what an ephemeral Fly machine, a CI job, or a developer shell runs. It
holds no port, joins no queue, and nothing keeps it warm. Sizing is a property of
the machine it is launched on, not of the always-on web tier.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from dataclasses import replace
from pathlib import Path
from typing import Any

from .server import RequestFailure, Settings, SimulatorService

DEFAULT_GAMES = 30000
DEFAULT_TIMEOUT_SECONDS = 3600


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="mtgsim-sweep",
        description="Run one full leave-one-out ablation sweep and write the v2 result document.",
    )
    parser.add_argument(
        "--candidate",
        required=True,
        type=Path,
        help="cedh-deck-candidate.v1 document to sweep",
    )
    parser.add_argument("--games", type=int, default=DEFAULT_GAMES)
    parser.add_argument("--turn", type=int, default=3)
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--scenario", default="goldfish_assembly.v1")
    parser.add_argument(
        "--threads",
        type=int,
        default=None,
        help="worker threads; defaults to CS_THREADS or the machine's vCPU count",
    )
    parser.add_argument(
        "--timeout-seconds",
        type=int,
        default=None,
        help=f"subprocess timeout; defaults to SWEEP_TIMEOUT_SECONDS or {DEFAULT_TIMEOUT_SECONDS}",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="write the result document here instead of stdout",
    )
    return parser


def sweep_settings(base: Settings, *, threads: int | None, timeout_seconds: int | None) -> Settings:
    """Batch settings: sweeps allowed, and sized independently of the web tier.

    `SIM_TIMEOUT_SECONDS` is the interactive budget and is far too short for a
    sweep, so the batch path takes its own timeout. `allow_sweep` is forced on
    because running this command *is* the authorization; the flag exists to keep
    the HTTP surface shut, not to gate an operator's own shell.
    """
    if threads is not None and threads < 1:
        raise ValueError("--threads must be at least 1")
    if timeout_seconds is not None and timeout_seconds < 1:
        raise ValueError("--timeout-seconds must be at least 1")
    if timeout_seconds is None:
        timeout_seconds = int(os.environ.get("SWEEP_TIMEOUT_SECONDS", DEFAULT_TIMEOUT_SECONDS))
        if timeout_seconds < 1:
            raise ValueError("SWEEP_TIMEOUT_SECONDS must be at least 1")
    return replace(
        base,
        cs_threads=base.cs_threads if threads is None else threads,
        timeout_seconds=timeout_seconds,
        allow_sweep=True,
        max_concurrent=1,
    )


def main(argv: list[str] | None = None) -> int:
    arguments = build_parser().parse_args(argv)
    if arguments.games < 1:
        print("error: --games must be at least 1", file=sys.stderr)
        return 2
    try:
        settings = sweep_settings(
            Settings.from_env(),
            threads=arguments.threads,
            timeout_seconds=arguments.timeout_seconds,
        )
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    try:
        candidate: Any = json.loads(arguments.candidate.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print(f"error: cannot read candidate {arguments.candidate}: {exc}", file=sys.stderr)
        return 2
    if not isinstance(candidate, dict):
        print("error: candidate must be a JSON object", file=sys.stderr)
        return 2

    print(
        f"sweep: {arguments.games} games, {settings.cs_threads} threads, "
        f"timeout {settings.timeout_seconds}s",
        file=sys.stderr,
    )
    started = time.monotonic()
    try:
        response = SimulatorService(settings).simulate(
            candidate,
            games=arguments.games,
            turn=arguments.turn,
            seed=arguments.seed,
            scenario=arguments.scenario,
            sweep=True,
            ablate=[],
        )
    except RequestFailure as failure:
        print(f"error: {failure.code}: {failure.detail}", file=sys.stderr)
        if failure.stderr:
            print(failure.stderr, file=sys.stderr)
        return 1
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    elapsed = time.monotonic() - started

    if arguments.output is None:
        sys.stdout.buffer.write(response.body)
        sys.stdout.buffer.flush()
    else:
        arguments.output.write_bytes(response.body)
    print(f"sweep: completed in {elapsed:.2f}s", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
