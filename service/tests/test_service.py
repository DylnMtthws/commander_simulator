from __future__ import annotations

import hashlib
import http.client
import json
import os
import subprocess
import sys
import threading
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
from typing import Any
from unittest.mock import patch

import pytest
from jsonschema import Draft202012Validator, FormatChecker

from mtgsim_service.server import Settings, make_server

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
) -> tuple[int, dict[str, str], bytes]:
    if isinstance(document, bytes):
        body = document
    elif document is None:
        body = None
    else:
        body = json.dumps(document).encode()
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    headers = {"Content-Type": content_type} if body is not None else {}
    connection.request(method, path, body=body, headers=headers)
    response = connection.getresponse()
    response_body = response.read()
    response_headers = {name: value for name, value in response.getheaders()}
    connection.close()
    return response.status, response_headers, response_body


def candidate(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text())


def rehash_candidate(document: dict[str, Any]) -> None:
    semantic = {
        "commander_oracle_ids": sorted(document["commander_oracle_ids"]),
        "library": sorted(document["library"], key=lambda card: card["oracle_id"]),
        "schema_version": document["schema_version"],
        "strategy_pack_id": document["strategy_pack_id"],
        "strategy_pack_version": document["strategy_pack_version"],
    }
    payload = json.dumps(semantic, sort_keys=True, separators=(",", ":"))
    document["candidate_hash"] = "sha256:" + hashlib.sha256(payload.encode()).hexdigest()


def test_healthz_is_fast_and_reports_installed_pack(repro_files: tuple[Path, Path]) -> None:
    cards, _ = repro_files
    with running_server(settings(cards)) as (_, port):
        status, _, body = exchange(port, "GET", "/healthz")
    assert status == 200
    assert json.loads(body) == {
        "status": "ok",
        "cs_version": "0.2.0",
        "strategy_packs": ["kinnan-midrange-goldfish@1.0.0"],
    }


def test_valid_request_round_trips_and_validates_v2_schema(
    repro_files: tuple[Path, Path],
) -> None:
    cards, candidate_path = repro_files
    request = {"candidate": candidate(candidate_path), "games": 20, "seed": 1}
    with running_server(settings(cards)) as (_, port):
        status, headers, body = exchange(port, "POST", "/simulate", request)

    result = json.loads(body)
    schema = json.loads((ROOT / "contracts/cedh-simulation-result.v2.schema.json").read_text())
    Draft202012Validator(schema, format_checker=FormatChecker()).validate(result)
    assert status == 200
    assert result["simulation"]["games"] == 20
    assert headers["X-Sim-Version"] == "0.2.0"
    assert headers["X-Sim-Result-Schema"] == "cedh-simulation-result.v2"
    assert headers["X-Cards-Sha256"] == result["card_data"]["cards_sha256"]
    assert headers["X-Sim-Threads"] == "2"


def test_multiple_named_ablations_are_forwarded(repro_files: tuple[Path, Path]) -> None:
    cards, candidate_path = repro_files
    request = {
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
        {"candidate": [], "games": 1},
        {"candidate": {}, "games": "20"},
        {"candidate": {}, "sweep": True, "ablate": ["Sol Ring"]},
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
    request = {"candidate": candidate(candidate_path), "games": 51}
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
            port, "POST", "/simulate", {"candidate": unsupported, "games": 1}
        )
    assert status == 422
    assert json.loads(body)["error"] == "unsupported"


def test_export_failure_is_503(repro_files: tuple[Path, Path]) -> None:
    _, candidate_path = repro_files
    configuration = settings(None, database_url="postgresql://unreachable/example")
    with (
        running_server(configuration) as (_, port),
        patch("mtgsim_service.server.export_candidate", side_effect=RuntimeError("offline")),
    ):
        status, _, body = exchange(
            port, "POST", "/simulate", {"candidate": candidate(candidate_path), "games": 1}
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
            port, "POST", "/simulate", {"candidate": candidate(candidate_path), "games": 1}
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
            port, "POST", "/simulate", {"candidate": candidate(candidate_path), "games": 1}
        )
    assert status == 500
    assert json.loads(body)["error"] == "simulator_failed"


def test_success_body_is_returned_byte_for_byte(repro_files: tuple[Path, Path]) -> None:
    cards, candidate_path = repro_files
    exact = (
        b'{\n  "schema_version": "cedh-simulation-result.v2",\n'
        b'  "card_data": {"cards_sha256": "fixture"}\n}\n'
    )
    completed = subprocess.CompletedProcess(["cs"], 0, stdout=exact, stderr=b"")
    with (
        running_server(settings(cards)) as (_, port),
        patch("mtgsim_service.server.subprocess.run", return_value=completed),
    ):
        status, _, body = exchange(
            port, "POST", "/simulate", {"candidate": candidate(candidate_path), "games": 1}
        )
    assert status == 200
    assert body == exact


def test_second_simulation_is_429_busy(repro_files: tuple[Path, Path]) -> None:
    cards, candidate_path = repro_files
    with running_server(settings(cards)) as (server, port):
        assert server.service.slots.acquire(blocking=False)
        try:
            status, headers, body = exchange(
                port, "POST", "/simulate", {"candidate": candidate(candidate_path), "games": 1}
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
