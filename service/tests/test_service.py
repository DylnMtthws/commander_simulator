from __future__ import annotations

import http.client
import json
import os
import subprocess
import sys
import threading
import tomllib
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import replace
from pathlib import Path
from typing import Any
from unittest.mock import patch

import pytest
from jsonschema import Draft202012Validator, FormatChecker
from mtgsim_export.hashes import deck_sha256

from mtgsim_service.server import (
    DEFAULT_MAX_ABLATIONS,
    REQUEST_SCHEMA,
    Settings,
    make_server,
)
from mtgsim_service.sweep import DEFAULT_TIMEOUT_SECONDS, sweep_settings
from mtgsim_service.sweep import main as sweep_main

SWEEP_TOKEN = "test-sweep-token"

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(scope="session")
def repro_files(tmp_path_factory: pytest.TempPathFactory) -> tuple[Path, Path]:
    directory = tmp_path_factory.mktemp("repro")
    cards = directory / "cards.json"
    candidate = directory / "candidate.json"
    subprocess.run(
        [
            sys.executable,
            ROOT / "scripts/make_repro_fixture.py",
            "--cards",
            cards,
            "--candidate",
            candidate,
        ],
        check=True,
    )
    return cards, candidate


def settings(cards: Path | None, *, database_url: str | None = None) -> Settings:
    return Settings(
        database_url=database_url,
        cs_bin=Path(os.environ.get("CS_BIN", ROOT / "build/ci/src/cli/cs")),
        cs_threads=2,
        packs_dir=ROOT / "data",
        port=8080,
        max_games=50,
        timeout_seconds=5,
        max_concurrent=1,
        cards_file=cards,
        testing=True,
    )


def sweep_worker_settings(cards: Path | None, *, token: str | None = SWEEP_TOKEN) -> Settings:
    """What deploy/fly.sweep.toml configures: sweeps allowed, token required."""
    return replace(settings(cards), allow_sweep=True, sweep_token=token, timeout_seconds=120)


@contextmanager
def running_server(configuration: Settings) -> Iterator[tuple[Any, int]]:
    server = make_server(configuration, host="127.0.0.1", port=0)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server, int(server.server_address[1])
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def exchange(
    port: int,
    method: str,
    path: str,
    document: object | bytes | None = None,
    *,
    content_type: str = "application/json",
    authorization: str | None = None,
) -> tuple[int, dict[str, str], bytes]:
    if isinstance(document, bytes):
        body = document
    elif document is None:
        body = None
    else:
        body = json.dumps(document).encode()
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    headers = {"Content-Type": content_type} if body is not None else {}
    if authorization is not None:
        headers["Authorization"] = authorization
    connection.request(method, path, body=body, headers=headers)
    response = connection.getresponse()
    response_body = response.read()
    response_headers = {name: value for name, value in response.getheaders()}
    connection.close()
    return response.status, response_headers, response_body


def candidate(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text())


def rehash_candidate(document: dict[str, Any]) -> None:
    """Recompute the deck hash after a test has edited the list.

    Deliberately calls the production helper rather than restating the
    algorithm: a second copy of the preimage in the tests would keep agreeing
    with itself while drifting from the contract.
    """
    document["deck_sha256"] = deck_sha256(
        document["commander_oracle_ids"], document["library"]
    )


def test_healthz_is_fast_and_reports_installed_pack(repro_files: tuple[Path, Path]) -> None:
    cards, _ = repro_files
    with running_server(settings(cards)) as (_, port):
        status, _, body = exchange(port, "GET", "/healthz")
    assert status == 200
    assert json.loads(body) == {
        "status": "ok",
        "cs_version": "0.2.0",
        "strategy_packs": ["kinnan-midrange-goldfish@1.0.0"],
        "sweep_enabled": False,
    }


def test_valid_request_round_trips_and_validates_v3_schema(
    repro_files: tuple[Path, Path],
) -> None:
    cards, candidate_path = repro_files
    request = {
        "schema_version": REQUEST_SCHEMA,
        "candidate": candidate(candidate_path),
        "games": 20,
        "seed": 1,
    }
    with running_server(settings(cards)) as (_, port):
        status, headers, body = exchange(port, "POST", "/simulate", request)

    result = json.loads(body)
    schema = json.loads((ROOT / "contracts/cedh-simulation-result.v3.schema.json").read_text())
    Draft202012Validator(schema, format_checker=FormatChecker()).validate(result)
    assert status == 200
    assert result["simulation"]["games"] == 20
    assert headers["X-Sim-Version"] == "0.2.0"
    assert headers["X-Sim-Result-Schema"] == "cedh-simulation-result.v3"
    assert headers["X-Cards-Sha256"] == result["card_data"]["cards_sha256"]
    assert headers["X-Sim-Threads"] == "2"


def test_multiple_named_ablations_are_forwarded(repro_files: tuple[Path, Path]) -> None:
    cards, candidate_path = repro_files
    request = {
        "schema_version": REQUEST_SCHEMA,
        "candidate": candidate(candidate_path),
        "games": 3,
        "ablate": ["Mental Misstep", "Sol Ring"],
    }
    with running_server(settings(cards)) as (_, port):
        status, _, body = exchange(port, "POST", "/simulate", request)
    assert status == 200
    result = json.loads(body)
    assert len(result["ablation_results"]) == 2


@pytest.mark.parametrize(
    "payload",
    [
        {},
        {"schema_version": REQUEST_SCHEMA, "candidate": [], "games": 1},
        {"schema_version": REQUEST_SCHEMA, "candidate": {}, "games": "20"},
        {"schema_version": REQUEST_SCHEMA, "candidate": {}, "sweep": True, "ablate": ["Sol Ring"]},
    ],
)
def test_invalid_requests_are_400(
    repro_files: tuple[Path, Path], payload: dict[str, object]
) -> None:
    cards, _ = repro_files
    with running_server(settings(cards)) as (_, port):
        status, _, body = exchange(port, "POST", "/simulate", payload)
    assert status == 400
    assert json.loads(body)["error"] == "invalid_request"


def test_malformed_json_is_400(repro_files: tuple[Path, Path]) -> None:
    cards, _ = repro_files
    with running_server(settings(cards)) as (_, port):
        status, _, body = exchange(port, "POST", "/simulate", b"{")
    assert status == 400
    assert json.loads(body)["error"] == "invalid_request"


def test_games_over_cap_is_rejected_not_clamped(repro_files: tuple[Path, Path]) -> None:
    cards, candidate_path = repro_files
    request = {
        "schema_version": REQUEST_SCHEMA,
        "candidate": candidate(candidate_path),
        "games": 51,
    }
    with running_server(settings(cards)) as (_, port):
        status, _, body = exchange(port, "POST", "/simulate", request)
    assert status == 400
    assert json.loads(body)["error"] == "invalid_request"


def test_candidate_rejection_is_422(repro_files: tuple[Path, Path]) -> None:
    cards, candidate_path = repro_files
    unsupported = candidate(candidate_path)
    unsupported["strategy_pack_version"] = "9.9.9"
    rehash_candidate(unsupported)
    with running_server(settings(cards)) as (_, port):
        status, _, body = exchange(
            port,
            "POST",
            "/simulate",
            {
                "schema_version": REQUEST_SCHEMA,
                "candidate": unsupported,
                "games": 1,
            },
        )
    assert status == 422
    assert json.loads(body)["error"] == "unsupported"


def test_a_pack_the_service_cannot_run_is_not_reported_as_a_deck_problem(
    repro_files: tuple[Path, Path],
) -> None:
    """An uninstalled pack is an execution-context fault, not a deck mismatch.

    Conflating the two is what made an unsupported pack surface to a user as
    "your deck changed", which is both wrong and unactionable.
    """
    cards, candidate_path = repro_files
    unsupported = candidate(candidate_path)
    unsupported["strategy_pack_version"] = "9.9.9"
    rehash_candidate(unsupported)
    with running_server(settings(cards)) as (_, port):
        status, _, body = exchange(
            port,
            "POST",
            "/simulate",
            {"schema_version": REQUEST_SCHEMA, "candidate": unsupported, "games": 1},
        )
    assert status == 422
    assert json.loads(body)["error"] == "unsupported"
    assert json.loads(body)["error"] != "deck_hash_mismatch"


def test_a_deck_hash_that_does_not_describe_the_list_is_a_deck_hash_mismatch(
    repro_files: tuple[Path, Path],
) -> None:
    cards, candidate_path = repro_files
    tampered = candidate(candidate_path)
    tampered["deck_sha256"] = "sha256:" + "0" * 64
    with running_server(settings(cards)) as (_, port):
        status, _, body = exchange(
            port,
            "POST",
            "/simulate",
            {"schema_version": REQUEST_SCHEMA, "candidate": tampered, "games": 1},
        )
    assert status == 422
    assert json.loads(body)["error"] == "deck_hash_mismatch"
    assert "does not describe the submitted list" in json.loads(body)["detail"]


def test_editing_the_list_without_rehashing_is_caught(
    repro_files: tuple[Path, Path],
) -> None:
    """The hash is checked against the list, not merely present and well-formed."""
    cards, candidate_path = repro_files
    edited = candidate(candidate_path)
    edited["library"][0]["oracle_id"] = "00000000-0000-4000-8000-000000000000"
    with running_server(settings(cards)) as (_, port):
        status, _, body = exchange(
            port,
            "POST",
            "/simulate",
            {"schema_version": REQUEST_SCHEMA, "candidate": edited, "games": 1},
        )
    assert status == 422
    assert json.loads(body)["error"] == "deck_hash_mismatch"


def test_the_removed_v1_candidate_hash_is_refused_by_name(
    repro_files: tuple[Path, Path],
) -> None:
    """A v1 producer gets told what changed, not a generic schema error.

    Accepting the field and ignoring it would leave that producer believing a
    hash had been verified when nothing had looked at it.
    """
    cards, candidate_path = repro_files
    legacy = candidate(candidate_path)
    legacy["candidate_hash"] = "sha256:" + "f" * 64
    with running_server(settings(cards)) as (_, port):
        status, _, body = exchange(
            port,
            "POST",
            "/simulate",
            {"schema_version": REQUEST_SCHEMA, "candidate": legacy, "games": 1},
        )
    assert status == 422
    assert json.loads(body)["error"] == "contract_violation"
    assert "candidate_hash" in json.loads(body)["detail"]


def test_an_unversioned_request_envelope_is_refused(
    repro_files: tuple[Path, Path],
) -> None:
    """The regression that made every real build fail with a 422 about the
    candidate: the envelope was prose, so the two sides never agreed on it."""
    cards, candidate_path = repro_files
    with running_server(settings(cards)) as (_, port):
        status, _, body = exchange(
            port, "POST", "/simulate", {"candidate": candidate(candidate_path), "games": 1}
        )
    assert status == 400
    assert json.loads(body)["error"] == "invalid_request"
    assert "schema_version" in json.loads(body)["detail"]


def test_the_result_reports_both_hashes_and_they_mean_different_things(
    repro_files: tuple[Path, Path],
) -> None:
    cards, candidate_path = repro_files
    submitted = candidate(candidate_path)
    with running_server(settings(cards)) as (_, port):
        status, _, body = exchange(
            port,
            "POST",
            "/simulate",
            {"schema_version": REQUEST_SCHEMA, "candidate": submitted, "games": 2, "seed": 7},
        )
    assert status == 200
    result = json.loads(body)
    # The simulator returns its OWN recomputation, and it agrees.
    assert result["candidate"]["deck_sha256"] == submitted["deck_sha256"]
    # The input fingerprint covers the pack and the run, so it differs.
    assert result["simulation_input_sha256"] != result["candidate"]["deck_sha256"]
    assert result["run_id"] == "run-" + result["simulation_input_sha256"][7:31]


def test_the_same_deck_under_two_seeds_keeps_one_deck_hash(
    repro_files: tuple[Path, Path],
) -> None:
    cards, candidate_path = repro_files
    results = []
    with running_server(settings(cards)) as (_, port):
        for seed in (1, 2):
            status, _, body = exchange(
                port,
                "POST",
                "/simulate",
                {
                    "schema_version": REQUEST_SCHEMA,
                    "candidate": candidate(candidate_path),
                    "games": 2,
                    "seed": seed,
                },
            )
            assert status == 200
            results.append(json.loads(body))
    assert results[0]["candidate"]["deck_sha256"] == results[1]["candidate"]["deck_sha256"]
    assert results[0]["simulation_input_sha256"] != results[1]["simulation_input_sha256"]


def test_export_failure_is_503(repro_files: tuple[Path, Path]) -> None:
    _, candidate_path = repro_files
    configuration = settings(None, database_url="postgresql://unreachable/example")
    with (
        running_server(configuration) as (_, port),
        patch("mtgsim_service.server.export_candidate", side_effect=RuntimeError("offline")),
    ):
        status, _, body = exchange(
            port,
            "POST",
            "/simulate",
            {
                "schema_version": REQUEST_SCHEMA,
                "candidate": candidate(candidate_path),
                "games": 1,
            },
        )
    assert status == 503
    assert json.loads(body)["error"] == "card_data_unavailable"


def test_simulator_timeout_is_504(repro_files: tuple[Path, Path]) -> None:
    cards, candidate_path = repro_files
    with (
        running_server(settings(cards)) as (_, port),
        patch(
            "mtgsim_service.server.subprocess.run",
            side_effect=subprocess.TimeoutExpired(["cs"], 5, stderr=b"timed out"),
        ),
    ):
        status, _, body = exchange(
            port,
            "POST",
            "/simulate",
            {
                "schema_version": REQUEST_SCHEMA,
                "candidate": candidate(candidate_path),
                "games": 1,
            },
        )
    error = json.loads(body)
    assert status == 504
    assert error["error"] == "timeout"
    assert error["stderr"] == "timed out"


def test_other_nonzero_exit_is_500(repro_files: tuple[Path, Path]) -> None:
    cards, candidate_path = repro_files
    failed = subprocess.CompletedProcess(["cs"], 7, stdout=b"", stderr=b"engine failure")
    with (
        running_server(settings(cards)) as (_, port),
        patch("mtgsim_service.server.subprocess.run", return_value=failed),
    ):
        status, _, body = exchange(
            port,
            "POST",
            "/simulate",
            {
                "schema_version": REQUEST_SCHEMA,
                "candidate": candidate(candidate_path),
                "games": 1,
            },
        )
    assert status == 500
    assert json.loads(body)["error"] == "simulator_failed"


def test_success_body_is_returned_byte_for_byte(repro_files: tuple[Path, Path]) -> None:
    cards, candidate_path = repro_files
    exact = (
        b'{\n  "schema_version": "cedh-simulation-result.v3",\n'
        b'  "card_data": {"cards_sha256": "fixture"}\n}\n'
    )
    completed = subprocess.CompletedProcess(["cs"], 0, stdout=exact, stderr=b"")
    with (
        running_server(settings(cards)) as (_, port),
        patch("mtgsim_service.server.subprocess.run", return_value=completed),
    ):
        status, _, body = exchange(
            port,
            "POST",
            "/simulate",
            {
                "schema_version": REQUEST_SCHEMA,
                "candidate": candidate(candidate_path),
                "games": 1,
            },
        )
    assert status == 200
    assert body == exact


def test_second_simulation_is_429_busy(repro_files: tuple[Path, Path]) -> None:
    cards, candidate_path = repro_files
    with running_server(settings(cards)) as (server, port):
        assert server.service.slots.acquire(blocking=False)
        try:
            status, headers, body = exchange(
                port,
                "POST",
                "/simulate",
                {
                    "schema_version": REQUEST_SCHEMA,
                    "candidate": candidate(candidate_path),
                    "games": 1,
                },
            )
        finally:
            server.service.slots.release()
    assert status == 429
    assert headers["Retry-After"] == "5"
    assert json.loads(body)["error"] == "busy"


def test_offline_file_and_dsn_require_explicit_testing_mode(
    repro_files: tuple[Path, Path], monkeypatch: pytest.MonkeyPatch
) -> None:
    cards, _ = repro_files
    monkeypatch.setenv("SIM_CARDS_FILE", str(cards))
    monkeypatch.setenv("MTGSIM_DATABASE_URL", "postgresql://db/example")
    monkeypatch.delenv("SIM_TESTING", raising=False)
    with pytest.raises(ValueError, match="coexist only"):
        Settings.from_env()


# --- Workload separation -------------------------------------------------
#
# The web UI happening to send `ablate: []` is not a limit. These tests hold the
# server-side boundary: the interactive tier refuses sweeps outright, caps how
# much ablation work one request may name, and the batch tier demands a token.


def test_sweep_is_refused_by_the_interactive_service(repro_files: tuple[Path, Path]) -> None:
    cards, candidate_path = repro_files
    request = {
        "schema_version": REQUEST_SCHEMA,
        "candidate": candidate(candidate_path),
        "games": 1,
        "sweep": True,
    }
    with running_server(settings(cards)) as (_, port):
        status, _, body = exchange(port, "POST", "/simulate", request)
    assert status == 403
    assert json.loads(body)["error"] == "sweep_not_allowed"


def test_sweep_refusal_precedes_any_simulator_work(repro_files: tuple[Path, Path]) -> None:
    """The 403 must cost nothing; `cs` is never started."""
    cards, candidate_path = repro_files
    request = {
        "schema_version": REQUEST_SCHEMA,
        "candidate": candidate(candidate_path),
        "games": 1,
        "sweep": True,
    }
    with (
        running_server(settings(cards)) as (_, port),
        patch("mtgsim_service.server.subprocess.run") as run,
    ):
        status, _, _ = exchange(port, "POST", "/simulate", request)
    assert status == 403
    assert run.call_count == 0


def test_ablation_list_over_the_cap_is_rejected(repro_files: tuple[Path, Path]) -> None:
    cards, candidate_path = repro_files
    names = [f"Card {index}" for index in range(DEFAULT_MAX_ABLATIONS + 1)]
    request = {
        "schema_version": REQUEST_SCHEMA,
        "candidate": candidate(candidate_path),
        "games": 1,
        "ablate": names,
    }
    with running_server(settings(cards)) as (_, port):
        status, _, body = exchange(port, "POST", "/simulate", request)
    assert status == 400
    assert "SIM_MAX_ABLATIONS" in json.loads(body)["detail"]


def test_sweep_worker_requires_a_bearer_token(repro_files: tuple[Path, Path]) -> None:
    cards, candidate_path = repro_files
    request = {
        "schema_version": REQUEST_SCHEMA,
        "candidate": candidate(candidate_path),
        "games": 1,
        "sweep": True,
    }
    with running_server(sweep_worker_settings(cards)) as (_, port):
        missing, _, missing_body = exchange(port, "POST", "/simulate", request)
        wrong, _, wrong_body = exchange(
            port, "POST", "/simulate", request, authorization="Bearer wrong-token"
        )
        malformed, _, _ = exchange(port, "POST", "/simulate", request, authorization=SWEEP_TOKEN)
    assert missing == wrong == malformed == 401
    assert json.loads(missing_body)["error"] == "unauthorized"
    assert json.loads(wrong_body)["error"] == "unauthorized"


def test_sweep_worker_without_a_token_configured_refuses_rather_than_admits(
    repro_files: tuple[Path, Path],
) -> None:
    cards, candidate_path = repro_files
    request = {
        "schema_version": REQUEST_SCHEMA,
        "candidate": candidate(candidate_path),
        "games": 1,
        "sweep": True,
    }
    with running_server(sweep_worker_settings(cards, token=None)) as (_, port):
        status, _, body = exchange(port, "POST", "/simulate", request, authorization="Bearer ")
    assert status == 403
    assert json.loads(body)["error"] == "sweep_not_allowed"


def test_authorized_sweep_runs_the_full_leave_one_out_set(
    repro_files: tuple[Path, Path],
) -> None:
    cards, candidate_path = repro_files
    request = {
        "schema_version": REQUEST_SCHEMA,
        "candidate": candidate(candidate_path),
        "games": 2,
        "seed": 1,
        "sweep": True,
    }
    with running_server(sweep_worker_settings(cards)) as (_, port):
        status, _, body = exchange(
            port, "POST", "/simulate", request, authorization=f"Bearer {SWEEP_TOKEN}"
        )
    assert status == 200
    assert len(json.loads(body)["ablation_results"]) > DEFAULT_MAX_ABLATIONS


def test_healthz_distinguishes_the_two_roles(repro_files: tuple[Path, Path]) -> None:
    cards, _ = repro_files
    with running_server(sweep_worker_settings(cards)) as (_, port):
        status, _, body = exchange(port, "GET", "/healthz")
    assert status == 200
    assert json.loads(body)["sweep_enabled"] is True


# --- Configuration defaults ----------------------------------------------


@pytest.fixture
def clean_env(repro_files: tuple[Path, Path], monkeypatch: pytest.MonkeyPatch) -> Path:
    cards, _ = repro_files
    for name in (
        "MTGSIM_DATABASE_URL",
        "SIM_ALLOW_SWEEP",
        "SIM_SWEEP_TOKEN",
        "SIM_MAX_ABLATIONS",
        "SIM_TESTING",
        "SWEEP_TIMEOUT_SECONDS",
    ):
        monkeypatch.delenv(name, raising=False)
    monkeypatch.setenv("SIM_CARDS_FILE", str(cards))
    return cards


def test_web_service_defaults_are_the_interactive_shape(clean_env: Path) -> None:
    configuration = Settings.from_env()
    assert configuration.allow_sweep is False
    assert configuration.sweep_token is None
    assert configuration.max_ablations == DEFAULT_MAX_ABLATIONS
    assert configuration.max_games == 60000
    assert configuration.timeout_seconds == 300
    assert configuration.max_concurrent == 1


def test_enabling_sweeps_without_a_token_refuses_to_start(
    clean_env: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("SIM_ALLOW_SWEEP", "1")
    with pytest.raises(ValueError, match="SIM_SWEEP_TOKEN"):
        Settings.from_env()


def test_sweep_worker_environment_enables_sweeps(
    clean_env: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("SIM_ALLOW_SWEEP", "1")
    monkeypatch.setenv("SIM_SWEEP_TOKEN", SWEEP_TOKEN)
    monkeypatch.setenv("SIM_MAX_ABLATIONS", "98")
    configuration = Settings.from_env()
    assert configuration.allow_sweep is True
    assert configuration.sweep_token == SWEEP_TOKEN
    assert configuration.max_ablations == 98


# --- The explicit batch entry point --------------------------------------


def test_sweep_settings_allow_sweeps_and_size_independently(repro_files: tuple[Path, Path]) -> None:
    cards, _ = repro_files
    base = settings(cards)
    batch = sweep_settings(base, threads=8, timeout_seconds=900)
    assert batch.allow_sweep is True
    assert batch.cs_threads == 8
    assert batch.timeout_seconds == 900
    assert base.cs_threads == 2 and base.allow_sweep is False


def test_sweep_settings_default_to_a_batch_scale_timeout(
    repro_files: tuple[Path, Path], monkeypatch: pytest.MonkeyPatch
) -> None:
    cards, _ = repro_files
    monkeypatch.delenv("SWEEP_TIMEOUT_SECONDS", raising=False)
    inherited = sweep_settings(settings(cards), threads=None, timeout_seconds=None)
    assert inherited.timeout_seconds == DEFAULT_TIMEOUT_SECONDS
    assert inherited.cs_threads == 2
    monkeypatch.setenv("SWEEP_TIMEOUT_SECONDS", "1200")
    overridden = sweep_settings(settings(cards), threads=None, timeout_seconds=None)
    assert overridden.timeout_seconds == 1200


@pytest.mark.parametrize("threads,timeout", [(0, None), (None, 0)])
def test_sweep_settings_reject_nonsense_sizes(
    repro_files: tuple[Path, Path], threads: int | None, timeout: int | None
) -> None:
    cards, _ = repro_files
    with pytest.raises(ValueError):
        sweep_settings(settings(cards), threads=threads, timeout_seconds=timeout)


def test_sweep_cli_runs_a_sweep_the_web_tier_would_refuse(
    clean_env: Path, tmp_path: Path, repro_files: tuple[Path, Path], monkeypatch: pytest.MonkeyPatch
) -> None:
    _, candidate_path = repro_files
    monkeypatch.setenv("CS_BIN", str(settings(None).cs_bin))
    monkeypatch.setenv("CS_PACKS_DIR", str(ROOT / "data"))
    output = tmp_path / "sweep.json"
    code = sweep_main(
        [
            "--candidate",
            str(candidate_path),
            "--games",
            "2",
            "--seed",
            "1",
            "--threads",
            "2",
            "--output",
            str(output),
        ]
    )
    assert code == 0
    result = json.loads(output.read_text())
    assert result["schema_version"] == "cedh-simulation-result.v3"
    assert len(result["ablation_results"]) > DEFAULT_MAX_ABLATIONS


def test_sweep_cli_reports_a_bad_candidate_without_traceback(
    clean_env: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("CS_BIN", str(settings(None).cs_bin))
    monkeypatch.setenv("CS_PACKS_DIR", str(ROOT / "data"))
    missing = tmp_path / "absent.json"
    assert sweep_main(["--candidate", str(missing)]) == 2


# --- Deployment configuration --------------------------------------------
#
# The split only holds if the two fly configs stay different in the ways that
# matter. These read the committed files rather than describing them.


def deploy_config(name: str) -> dict[str, Any]:
    return tomllib.loads((ROOT / "deploy" / name).read_text())


def test_interactive_deployment_is_small_and_refuses_sweeps() -> None:
    config = deploy_config("fly.toml")
    assert config["app"] == "sim-worker"
    assert config["env"]["SIM_ALLOW_SWEEP"] == "0"
    assert int(config["env"]["CS_THREADS"]) == 2
    assert int(config["env"]["SIM_MAX_ABLATIONS"]) == DEFAULT_MAX_ABLATIONS
    size = config["vm"][0]["size"]
    assert size == "shared-cpu-2x", f"web tier is sized from interactive load, not {size}"


def test_sweep_deployment_is_separate_and_never_idles() -> None:
    interactive = deploy_config("fly.toml")
    batch = deploy_config("fly.sweep.toml")
    assert batch["app"] != interactive["app"]
    assert batch["env"]["SIM_ALLOW_SWEEP"] == "1"
    # Batch compute must not be provisioned between sweeps.
    assert batch["http_service"]["min_machines_running"] == 0
    assert batch["http_service"]["auto_start_machines"] is True
    # Threads follow the machine, so `size` is the only knob that has to move.
    assert "CS_THREADS" not in batch["env"]
    # 2 vCPUs would project to ~300s, exactly the interactive timeout.
    assert int(batch["env"]["SIM_TIMEOUT_SECONDS"]) > 300


@pytest.mark.parametrize("name", ["fly.toml", "fly.sweep.toml"])
def test_deploy_configs_carry_no_secrets(name: str) -> None:
    env = deploy_config(name).get("env", {})
    assert "MTGSIM_DATABASE_URL" not in env
    assert "SIM_SWEEP_TOKEN" not in env
