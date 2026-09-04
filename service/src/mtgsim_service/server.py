"""Small stateless HTTP boundary around the versioned ``cs --request`` CLI."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import threading
import tomllib
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

from mtgsim_export.candidate import CandidateError, load_candidate
from mtgsim_export.export import export_candidate

MAX_REQUEST_BYTES = 10 * 1024 * 1024
RESULT_SCHEMA = "cedh-simulation-result.v2"


def _positive_env(name: str, default: int) -> int:
    raw = os.environ.get(name, str(default))
    try:
        value = int(raw)
    except ValueError as exc:
        raise ValueError(f"{name} must be an integer") from exc
    if value < 1:
        raise ValueError(f"{name} must be at least 1")
    return value


def cgroup_cpu_count() -> int:
    """Return the cgroup-v2 quota when present, else the host CPU count."""
    fallback = os.cpu_count() or 1
    try:
        quota_text, period_text = Path("/sys/fs/cgroup/cpu.max").read_text().split()
        if quota_text == "max":
            return fallback
        quota = int(quota_text)
        period = int(period_text)
        if quota <= 0 or period <= 0:
            return fallback
        return max(1, quota // period + int(quota % period != 0))
    except (OSError, ValueError):
        return fallback


@dataclass(frozen=True, slots=True)
class Settings:
    database_url: str | None
    cs_bin: Path
    cs_threads: int
    packs_dir: Path
    port: int
    max_games: int
    timeout_seconds: int
    max_concurrent: int
    cards_file: Path | None
    testing: bool

    @classmethod
    def from_env(cls) -> Settings:
        database_url = os.environ.get("MTGSIM_DATABASE_URL") or None
        cards_value = os.environ.get("SIM_CARDS_FILE")
        cards_file = Path(cards_value) if cards_value else None
        testing = os.environ.get("SIM_TESTING") == "1"
        if database_url is None and cards_file is None:
            raise ValueError("MTGSIM_DATABASE_URL is required unless SIM_CARDS_FILE is set")
        if database_url is not None and cards_file is not None and not testing:
            raise ValueError(
                "MTGSIM_DATABASE_URL and SIM_CARDS_FILE may coexist only with SIM_TESTING=1"
            )
        if cards_file is not None and not cards_file.is_file():
            raise ValueError(f"SIM_CARDS_FILE does not exist: {cards_file}")
        return cls(
            database_url=database_url,
            cs_bin=Path(os.environ.get("CS_BIN", "/app/cs")),
            cs_threads=_positive_env("CS_THREADS", cgroup_cpu_count()),
            packs_dir=Path(os.environ.get("CS_PACKS_DIR", "/app/data")),
            port=_positive_env("SIM_PORT", 8080),
            max_games=_positive_env("SIM_MAX_GAMES", 60000),
            timeout_seconds=_positive_env("SIM_TIMEOUT_SECONDS", 300),
            max_concurrent=_positive_env("SIM_MAX_CONCURRENT", 1),
            cards_file=cards_file,
            testing=testing,
        )


class RequestFailure(Exception):
    def __init__(self, status: int, code: str, detail: str, stderr: str = "") -> None:
        super().__init__(detail)
        self.status = status
        self.code = code
        self.detail = detail
        self.stderr = stderr[-4096:]


@dataclass(frozen=True, slots=True)
class SimulationResponse:
    body: bytes
    schema: str
    cards_sha256: str


def _stderr_tail(value: bytes | str | None) -> str:
    if value is None:
        return ""
    text = value.decode(errors="replace") if isinstance(value, bytes) else value
    return text[-4096:]


def _candidate_rejection(stderr: str) -> bool:
    lowered = stderr.lower()
    return any(
        marker in lowered
        for marker in (
            "candidate:",
            "candidate requests",
            "candidate oracle",
            "strategy pack",
            "commander oracle",
        )
    )


class SimulatorService:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        self.slots = threading.BoundedSemaphore(settings.max_concurrent)
        self.cs_version = self._read_cs_version()
        self.strategy_packs = self._read_strategy_packs()

    def _read_cs_version(self) -> str:
        try:
            completed = subprocess.run(
                [self.settings.cs_bin, "--version"],
                capture_output=True,
                timeout=2,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise RuntimeError(f"cannot execute CS_BIN {self.settings.cs_bin}: {exc}") from exc
        if completed.returncode != 0:
            raise RuntimeError(f"CS_BIN --version failed: {_stderr_tail(completed.stderr).strip()}")
        fields = completed.stdout.decode(errors="replace").strip().split()
        if len(fields) < 2:
            raise RuntimeError("CS_BIN --version did not return '<semver> <build-type>'")
        return fields[0]

    def _read_strategy_packs(self) -> list[str]:
        packs: list[str] = []
        for path in sorted(self.settings.packs_dir.glob("*.deck.toml")):
            try:
                pack = tomllib.loads(path.read_text())["strategy_pack"]
                packs.append(f"{pack['id']}@{pack['version']}")
            except (OSError, KeyError, TypeError, tomllib.TOMLDecodeError) as exc:
                raise RuntimeError(f"cannot read strategy pack {path}: {exc}") from exc
        return packs

    def simulate(
        self,
        candidate_document: dict[str, Any],
        *,
        games: int,
        turn: int,
        seed: int,
        scenario: str,
        sweep: bool,
        ablate: list[str],
    ) -> SimulationResponse:
        with tempfile.TemporaryDirectory(prefix="mtgsim-") as directory:
            temporary = Path(directory)
            candidate_path = temporary / "candidate.json"
            candidate_path.write_text(json.dumps(candidate_document, ensure_ascii=False) + "\n")
            try:
                candidate = load_candidate(candidate_path)
            except (CandidateError, json.JSONDecodeError, OSError) as exc:
                raise RequestFailure(
                    HTTPStatus.UNPROCESSABLE_ENTITY, "unsupported", str(exc)
                ) from exc

            cards_path = self.settings.cards_file
            if cards_path is None:
                assert self.settings.database_url is not None
                try:
                    document = export_candidate(self.settings.database_url, candidate)
                except Exception as exc:
                    raise RequestFailure(
                        HTTPStatus.SERVICE_UNAVAILABLE,
                        "card_data_unavailable",
                        str(exc),
                    ) from exc
                cards_path = temporary / "cards.json"
                cards_path.write_text(json.dumps(document, indent=2) + "\n")

            command = [
                str(self.settings.cs_bin),
                "--request",
                str(candidate_path),
                "--cards",
                str(cards_path),
                "--deck",
                str(self.settings.packs_dir),
                "--effects",
                str(self.settings.packs_dir / "effects.toml"),
                "--games",
                str(games),
                "--turn",
                str(turn),
                "--seed",
                str(seed),
                "--scenario",
                scenario,
                "--threads",
                str(self.settings.cs_threads),
                "--output-json",
                "-",
            ]
            if sweep:
                command.append("--sweep")
            else:
                for name in ablate:
                    command.extend(("--ablate", name))

            try:
                completed = subprocess.run(
                    command,
                    capture_output=True,
                    timeout=self.settings.timeout_seconds,
                    check=False,
                )
            except subprocess.TimeoutExpired as exc:
                stderr = _stderr_tail(exc.stderr)
                raise RequestFailure(
                    HTTPStatus.GATEWAY_TIMEOUT,
                    "timeout",
                    f"cs exceeded {self.settings.timeout_seconds} seconds",
                    stderr,
                ) from exc
            except OSError as exc:
                raise RequestFailure(
                    HTTPStatus.INTERNAL_SERVER_ERROR,
                    "simulator_failed",
                    f"could not execute cs: {exc}",
                ) from exc

            stderr = _stderr_tail(completed.stderr)
            if completed.returncode != 0:
                detail = stderr.strip() or f"cs exited with status {completed.returncode}"
                if detail.startswith("error: "):
                    detail = detail[7:]
                if _candidate_rejection(stderr):
                    raise RequestFailure(
                        HTTPStatus.UNPROCESSABLE_ENTITY, "unsupported", detail, stderr
                    )
                raise RequestFailure(
                    HTTPStatus.INTERNAL_SERVER_ERROR, "simulator_failed", detail, stderr
                )

            try:
                result = json.loads(completed.stdout)
                schema = str(result["schema_version"])
                cards_sha256 = str(result["card_data"]["cards_sha256"])
            except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
                raise RequestFailure(
                    HTTPStatus.INTERNAL_SERVER_ERROR,
                    "simulator_failed",
                    "cs stdout was not a simulation result document",
                    stderr,
                ) from exc
            if schema != RESULT_SCHEMA:
                raise RequestFailure(
                    HTTPStatus.INTERNAL_SERVER_ERROR,
                    "simulator_failed",
                    f"cs returned unexpected result schema {schema!r}",
                    stderr,
                )
            return SimulationResponse(completed.stdout, schema, cards_sha256)


class ServiceHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(
        self,
        address: tuple[str, int],
        handler: type[BaseHTTPRequestHandler],
        service: SimulatorService,
    ) -> None:
        super().__init__(address, handler)
        self.service = service


class RequestHandler(BaseHTTPRequestHandler):
    server: ServiceHTTPServer

    def log_message(self, format: str, *args: object) -> None:
        return

    def _send_bytes(self, status: int, body: bytes, headers: dict[str, str] | None = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def _fail(self, failure: RequestFailure, headers: dict[str, str] | None = None) -> None:
        body = json.dumps(
            {"error": failure.code, "detail": failure.detail, "stderr": failure.stderr},
            separators=(",", ":"),
        ).encode()
        self._send_bytes(failure.status, body, headers)

    def do_GET(self) -> None:
        if urlsplit(self.path).path != "/healthz":
            self._fail(RequestFailure(HTTPStatus.NOT_FOUND, "not_found", "route not found"))
            return
        service = self.server.service
        body = json.dumps(
            {
                "status": "ok",
                "cs_version": service.cs_version,
                "strategy_packs": service.strategy_packs,
            },
            separators=(",", ":"),
        ).encode()
        self._send_bytes(HTTPStatus.OK, body)

    def _read_request(self) -> dict[str, Any]:
        content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        if content_type != "application/json":
            raise RequestFailure(
                HTTPStatus.BAD_REQUEST, "invalid_request", "Content-Type must be application/json"
            )
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError as exc:
            raise RequestFailure(
                HTTPStatus.BAD_REQUEST, "invalid_request", "Content-Length must be an integer"
            ) from exc
        if length < 1 or length > MAX_REQUEST_BYTES:
            raise RequestFailure(
                HTTPStatus.BAD_REQUEST, "invalid_request", "request body size is invalid"
            )
        try:
            document = json.loads(self.rfile.read(length))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise RequestFailure(
                HTTPStatus.BAD_REQUEST, "invalid_request", f"malformed JSON: {exc}"
            ) from exc
        if not isinstance(document, dict):
            raise RequestFailure(
                HTTPStatus.BAD_REQUEST, "invalid_request", "request must be a JSON object"
            )
        return document

    @staticmethod
    def _integer(document: dict[str, Any], name: str, default: int) -> int:
        value = document.get(name, default)
        if isinstance(value, bool) or not isinstance(value, int):
            raise RequestFailure(
                HTTPStatus.BAD_REQUEST, "invalid_request", f"{name} must be an integer"
            )
        return value

    def do_POST(self) -> None:
        if urlsplit(self.path).path != "/simulate":
            self._fail(RequestFailure(HTTPStatus.NOT_FOUND, "not_found", "route not found"))
            return
        try:
            document = self._read_request()
            allowed = {"candidate", "games", "turn", "seed", "scenario", "sweep", "ablate"}
            unknown = sorted(set(document) - allowed)
            if unknown:
                raise RequestFailure(
                    HTTPStatus.BAD_REQUEST,
                    "invalid_request",
                    f"unknown request fields: {unknown}",
                )
            candidate = document.get("candidate")
            if not isinstance(candidate, dict):
                raise RequestFailure(
                    HTTPStatus.BAD_REQUEST,
                    "invalid_request",
                    "candidate is required and must be an object",
                )
            games = self._integer(document, "games", 20000)
            turn = self._integer(document, "turn", 3)
            seed = self._integer(document, "seed", 12345)
            if games < 1:
                raise RequestFailure(
                    HTTPStatus.BAD_REQUEST, "invalid_request", "games must be at least 1"
                )
            if games > self.server.service.settings.max_games:
                raise RequestFailure(
                    HTTPStatus.BAD_REQUEST,
                    "invalid_request",
                    f"games exceeds SIM_MAX_GAMES ({self.server.service.settings.max_games})",
                )
            if turn < 1:
                raise RequestFailure(
                    HTTPStatus.BAD_REQUEST, "invalid_request", "turn must be at least 1"
                )
            if seed < 0 or seed > 2**64 - 1:
                raise RequestFailure(
                    HTTPStatus.BAD_REQUEST,
                    "invalid_request",
                    "seed must be between 0 and 18446744073709551615",
                )
            scenario = document.get("scenario", "goldfish_assembly.v1")
            sweep = document.get("sweep", False)
            ablate = document.get("ablate", [])
            if not isinstance(scenario, str):
                raise RequestFailure(
                    HTTPStatus.BAD_REQUEST, "invalid_request", "scenario must be a string"
                )
            if not isinstance(sweep, bool):
                raise RequestFailure(
                    HTTPStatus.BAD_REQUEST, "invalid_request", "sweep must be a boolean"
                )
            if not isinstance(ablate, list) or not all(
                isinstance(name, str) and name for name in ablate
            ):
                raise RequestFailure(
                    HTTPStatus.BAD_REQUEST,
                    "invalid_request",
                    "ablate must be a list of non-empty card names",
                )
            if sweep and ablate:
                raise RequestFailure(
                    HTTPStatus.BAD_REQUEST,
                    "invalid_request",
                    "sweep and non-empty ablate are mutually exclusive",
                )

            service = self.server.service
            if not service.slots.acquire(blocking=False):
                self._fail(
                    RequestFailure(HTTPStatus.TOO_MANY_REQUESTS, "busy", "simulator is busy"),
                    {"Retry-After": "5"},
                )
                return
            try:
                response = service.simulate(
                    candidate,
                    games=games,
                    turn=turn,
                    seed=seed,
                    scenario=scenario,
                    sweep=sweep,
                    ablate=ablate,
                )
            finally:
                service.slots.release()
            self._send_bytes(
                HTTPStatus.OK,
                response.body,
                {
                    "X-Sim-Version": service.cs_version,
                    "X-Sim-Result-Schema": response.schema,
                    "X-Cards-Sha256": response.cards_sha256,
                    "X-Sim-Threads": str(service.settings.cs_threads),
                },
            )
        except RequestFailure as failure:
            self._fail(failure)
        except Exception as exc:
            self._fail(
                RequestFailure(
                    HTTPStatus.INTERNAL_SERVER_ERROR,
                    "simulator_failed",
                    f"internal service error: {exc}",
                )
            )


def make_server(
    settings: Settings, host: str = "0.0.0.0", port: int | None = None
) -> ServiceHTTPServer:
    return ServiceHTTPServer(
        (host, settings.port if port is None else port), RequestHandler, SimulatorService(settings)
    )


def main() -> int:
    settings = Settings.from_env()
    server = make_server(settings)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
