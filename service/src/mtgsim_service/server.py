"""Small stateless HTTP boundary around the versioned ``cs --request`` CLI."""

from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
import threading
import tomllib
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from secrets import compare_digest
from typing import Any
from urllib.parse import urlsplit

import psycopg
from mtgsim_export.candidate import CandidateError, load_candidate
from mtgsim_export.export import ExportError, _assert_consumer_role, export_candidate
from psycopg.rows import dict_row

MAX_REQUEST_BYTES = 10 * 1024 * 1024
REQUEST_SCHEMA = "cedh-simulation-request.v1"
RESULT_SCHEMA = "cedh-simulation-result.v3"

#: ``cs`` prefixes a refused request with its machine code so this service can
#: classify it without matching English prose. The mapping is the whole point
#: of the taxonomy: only ``deck_hash_mismatch`` means "not the deck you think",
#: and an uninstalled pack must never be reported as one.
CS_ERROR_CODES: dict[str, str] = {
    "deck_hash_mismatch": "deck_hash_mismatch",
    "contract_violation": "contract_violation",
    "execution_context_unsupported": "unsupported",
}
CS_ERROR_PATTERN = re.compile(r"^(?:error: )?\[([a-z_]+)\]\s*(.*)$", re.MULTILINE)

# A full leave-one-out sweep is 98 ablations and ~600 CPU-seconds; a single
# interactive run is ~1.4 CPU-seconds. They are different workloads and they get
# different machines. The interactive tier refuses sweeps outright, and caps how
# many named ablations one request may ask for, so the cost of a request is
# bounded server-side rather than by what the client chooses to send.
DEFAULT_MAX_ABLATIONS = 8


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
    allow_sweep: bool = False
    sweep_token: str | None = None
    max_ablations: int = DEFAULT_MAX_ABLATIONS

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
        allow_sweep = os.environ.get("SIM_ALLOW_SWEEP", "0") == "1"
        sweep_token = os.environ.get("SIM_SWEEP_TOKEN") or None
        if allow_sweep and sweep_token is None:
            raise ValueError("SIM_ALLOW_SWEEP=1 requires SIM_SWEEP_TOKEN")
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
            allow_sweep=allow_sweep,
            sweep_token=sweep_token,
            max_ablations=_positive_env("SIM_MAX_ABLATIONS", DEFAULT_MAX_ABLATIONS),
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


def _cs_error_code(stderr: str) -> tuple[str, str] | None:
    """Extract ``cs``'s machine code and its detail, if it emitted one.

    Returns ``None`` for a failure ``cs`` did not classify, which is a real
    simulator fault rather than a rejected request. The previous version of
    this function guessed from substrings like "strategy pack", so a candidate
    refused for one reason could be reported to the user as another.
    """
    for match in CS_ERROR_PATTERN.finditer(stderr):
        code = match.group(1)
        if code in CS_ERROR_CODES:
            return CS_ERROR_CODES[code], match.group(2).strip()
    return None


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
            except CandidateError as exc:
                # The service validates the deck hash itself before spending a
                # subprocess on it. cs re-validates independently; neither
                # trusts the other, and both recompute rather than compare
                # against what the producer claimed.
                raise RequestFailure(
                    HTTPStatus.UNPROCESSABLE_ENTITY, exc.code, exc.detail
                ) from exc
            except (json.JSONDecodeError, OSError) as exc:
                raise RequestFailure(
                    HTTPStatus.UNPROCESSABLE_ENTITY, "contract_violation", str(exc)
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
                classified = _cs_error_code(stderr)
                if classified is not None:
                    code, detail = classified
                    raise RequestFailure(
                        HTTPStatus.UNPROCESSABLE_ENTITY,
                        code,
                        detail or f"cs refused the request ({code})",
                        stderr,
                    )
                detail = stderr.strip() or f"cs exited with status {completed.returncode}"
                if detail.startswith("error: "):
                    detail = detail[7:]
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
                "sweep_enabled": service.settings.allow_sweep,
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

    def _authorize_sweep(self, settings: Settings) -> None:
        """Gate the batch workload. The client's request body is not the boundary."""
        if not settings.allow_sweep:
            raise RequestFailure(
                HTTPStatus.FORBIDDEN,
                "sweep_not_allowed",
                "sweeps are not enabled on this service; submit them to the sweep worker",
            )
        expected = settings.sweep_token
        if expected is None:
            raise RequestFailure(
                HTTPStatus.FORBIDDEN,
                "sweep_not_allowed",
                "sweep worker is misconfigured: SIM_SWEEP_TOKEN is unset",
            )
        scheme, _, presented = self.headers.get("Authorization", "").partition(" ")
        if scheme.lower() != "bearer" or not compare_digest(presented.strip(), expected):
            raise RequestFailure(
                HTTPStatus.UNAUTHORIZED,
                "unauthorized",
                "sweep requires a valid bearer token",
            )

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
        service = self.server.service
        settings = service.settings
        try:
            document = self._read_request()
            allowed = {
                "schema_version", "candidate", "games", "turn", "seed",
                "scenario", "sweep", "ablate",
            }
            unknown = sorted(set(document) - allowed)
            if unknown:
                raise RequestFailure(
                    HTTPStatus.BAD_REQUEST,
                    "invalid_request",
                    f"unknown request fields: {unknown}",
                )
            # The envelope is versioned in its own right. It used to be
            # described only in prose, so each side built its own reading of it
            # and the mismatch surfaced as a 422 about the candidate.
            declared = document.get("schema_version")
            if declared != REQUEST_SCHEMA:
                raise RequestFailure(
                    HTTPStatus.BAD_REQUEST,
                    "invalid_request",
                    f"unsupported request schema_version {declared!r}; "
                    f"supported: {REQUEST_SCHEMA}",
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
            if games > settings.max_games:
                raise RequestFailure(
                    HTTPStatus.BAD_REQUEST,
                    "invalid_request",
                    f"games exceeds SIM_MAX_GAMES ({settings.max_games})",
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
            if sweep:
                self._authorize_sweep(settings)
            if len(ablate) > settings.max_ablations:
                raise RequestFailure(
                    HTTPStatus.BAD_REQUEST,
                    "invalid_request",
                    f"ablate has {len(ablate)} entries; SIM_MAX_ABLATIONS is "
                    f"{settings.max_ablations}",
                )

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
                    "X-Sim-Threads": str(settings.cs_threads),
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
    # Serve process liveness while checking the database. Network latency must
    # not delay /healthz; exports still assert their own role on every connection.
    serving = threading.Thread(target=server.serve_forever, daemon=True)
    serving.start()
    try:
        assert_startup_database_role(settings)
        serving.join()
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()
        server.server_close()
        serving.join()
    return 0


def assert_startup_database_role(settings: Settings) -> None:
    """Refuse pipeline credentials at boot; offline snapshots need no database."""
    if settings.cards_file is not None:
        return
    if not settings.database_url:
        raise ValueError("MTGSIM_DATABASE_URL must be set for database-backed service startup")
    try:
        with psycopg.connect(
            settings.database_url,
            row_factory=dict_row,
            connect_timeout=5,
            options="-c statement_timeout=1000 -c default_transaction_read_only=on",
        ) as conn:
            _assert_consumer_role(conn)
    except ExportError as exc:
        raise ValueError(
            f"MTGSIM_DATABASE_URL startup role check failed: {exc} "
            "Set MTGSIM_DATABASE_URL to the mtg_consumer DSN, not the pipeline's "
            "MTG_DATABASE_URL."
        ) from None
    except psycopg.Error:
        # Connection diagnostics can contain credentials; name the configuration
        # to repair without logging the DSN or the driver's connection error.
        raise ValueError(
            "MTGSIM_DATABASE_URL startup role check could not connect or query. "
            "Set it to a reachable mtg_consumer DSN."
        ) from None


if __name__ == "__main__":
    raise SystemExit(main())
