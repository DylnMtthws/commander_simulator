"""Startup role validation stays offline in tests and does not block liveness."""

from __future__ import annotations

import json
import threading
import time
from dataclasses import replace
from unittest.mock import MagicMock, patch

import psycopg
import pytest
from test_service import exchange, settings

from mtgsim_service import server


def connection_for(role: str) -> MagicMock:
    connection = MagicMock()
    connection.__enter__.return_value = connection
    cursor = connection.cursor.return_value.__enter__.return_value
    cursor.execute.return_value.fetchone.return_value = {"role": role}
    return connection


def test_startup_checks_the_connected_role_once() -> None:
    connection = connection_for("mtg_consumer")
    with patch.object(server.psycopg, "connect", return_value=connection) as connect:
        server.assert_startup_database_role(settings(None, database_url="test-dsn"))
    connect.assert_called_once()
    assert connect.call_args.args == ("test-dsn",)
    assert connect.call_args.kwargs["connect_timeout"] == 5
    cursor = connection.cursor.return_value.__enter__.return_value
    cursor.execute.assert_called_once_with("SELECT current_user AS role")
    connection.__exit__.assert_called_once()


def test_pipeline_role_is_fatal_even_in_testing_mode() -> None:
    with (
        patch.object(server.psycopg, "connect", return_value=connection_for("mtg_pipeline")),
        pytest.raises(ValueError, match=r"MTGSIM_DATABASE_URL.*mtg_pipeline.*mtg_consumer"),
    ):
        server.assert_startup_database_role(settings(None, database_url="test-dsn"))


def test_connection_failure_does_not_log_the_dsn() -> None:
    with (
        patch.object(server.psycopg, "connect", side_effect=psycopg.OperationalError("secret")),
        pytest.raises(ValueError, match="MTGSIM_DATABASE_URL") as error,
    ):
        server.assert_startup_database_role(settings(None, database_url="secret"))
    assert "secret" not in str(error.value)


@pytest.mark.parametrize("testing", [False, True])
def test_offline_snapshot_skips_database_check(tmp_path, testing: bool) -> None:
    configuration = replace(settings(tmp_path / "cards.json"), testing=testing)
    with patch.object(server.psycopg, "connect") as connect:
        server.assert_startup_database_role(configuration)
    connect.assert_not_called()


def test_health_is_fast_while_boot_checks_role_and_failure_closes_server() -> None:
    configuration = settings(None, database_url="test-dsn")
    http_server = server.make_server(configuration, host="127.0.0.1", port=0)
    checking = threading.Event()
    release = threading.Event()
    errors: list[Exception] = []

    def slow_connect(*args, **kwargs):
        checking.set()
        assert release.wait(5)
        return connection_for("mtg_pipeline")

    def run() -> None:
        try:
            server.main()
        except Exception as exc:
            errors.append(exc)

    with (
        patch.object(server.Settings, "from_env", return_value=configuration),
        patch.object(server, "make_server", return_value=http_server),
        patch.object(server.psycopg, "connect", side_effect=slow_connect),
    ):
        started = time.monotonic()
        thread = threading.Thread(target=run, daemon=True)
        thread.start()
        try:
            assert checking.wait(1)
            status, _, body = exchange(int(http_server.server_address[1]), "GET", "/healthz")
            assert status == 200
            assert json.loads(body)["status"] == "ok"
            assert time.monotonic() - started < 2
        finally:
            release.set()
            thread.join(timeout=5)
    assert not thread.is_alive()
    assert len(errors) == 1
    assert isinstance(errors[0], ValueError)
    assert "mtg_pipeline" in str(errors[0])
    assert http_server.socket.fileno() == -1
