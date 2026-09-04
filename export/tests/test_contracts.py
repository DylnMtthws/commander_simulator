"""Public JSON contracts, their fixtures, and non-schema semantic invariants."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from jsonschema import Draft202012Validator, FormatChecker

from mtgsim_export.candidate import CandidateError, candidate_hash, load_candidate

REPO_ROOT = Path(__file__).resolve().parents[2]
CONTRACTS = REPO_ROOT / "contracts"
FIXTURES = REPO_ROOT / "tests" / "fixtures"


def _load(path: Path) -> dict[str, object]:
    return json.loads(path.read_text())


@pytest.mark.parametrize(
    ("schema_name", "fixture_name"),
    [
        ("cedh-deck-candidate.v1.schema.json", "kinnan-candidate.v1.json"),
        ("cedh-deck-candidate.v1.schema.json", "unsupported-pack-candidate.v1.json"),
        ("cedh-simulation-result.v1.schema.json", "simulation-result.v1.json"),
        ("cedh-simulation-result.v2.schema.json", "simulation-result.v2.json"),
    ],
)
def test_contract_fixture_validates(schema_name: str, fixture_name: str) -> None:
    schema = _load(CONTRACTS / schema_name)
    Draft202012Validator.check_schema(schema)
    Draft202012Validator(schema, format_checker=FormatChecker()).validate(
        _load(FIXTURES / fixture_name)
    )


def test_candidate_schema_refuses_an_extra_semantic_field() -> None:
    schema = _load(CONTRACTS / "cedh-deck-candidate.v1.schema.json")
    candidate = _load(FIXTURES / "kinnan-candidate.v1.json")
    candidate["commander_name"] = "Kinnan, Bonder Prodigy"
    errors = list(Draft202012Validator(schema).iter_errors(candidate))
    assert any("Additional properties" in error.message for error in errors)


def test_result_schema_cannot_call_assembly_probability_win_rate() -> None:
    schema = _load(CONTRACTS / "cedh-simulation-result.v2.schema.json")
    result = _load(FIXTURES / "simulation-result.v2.json")
    result["metric"]["id"] = "win_rate"  # type: ignore[index]
    errors = list(Draft202012Validator(schema).iter_errors(result))
    assert errors


def test_candidate_hash_and_exact_99_are_executable_invariants(tmp_path: Path) -> None:
    path = FIXTURES / "kinnan-candidate.v1.json"
    candidate = load_candidate(path)
    assert candidate.candidate_hash == candidate_hash(candidate)
    assert sum(card.quantity for card in candidate.library) == 99

    malformed = _load(path)
    malformed["library"][0]["quantity"] = 2  # type: ignore[index]
    bad_path = tmp_path / "bad.json"
    bad_path.write_text(json.dumps(malformed))
    with pytest.raises(CandidateError, match="expected exactly 99"):
        load_candidate(bad_path)


def test_candidate_loader_requires_provenance_shape(tmp_path: Path) -> None:
    malformed = _load(FIXTURES / "kinnan-candidate.v1.json")
    malformed["provenance"] = {}
    bad_path = tmp_path / "missing-provenance.json"
    bad_path.write_text(json.dumps(malformed))
    with pytest.raises(CandidateError, match=r"provenance\.producer"):
        load_candidate(bad_path)


def test_exporter_source_never_reaches_into_mtg_internal() -> None:
    source = (REPO_ROOT / "export" / "src" / "mtgsim_export" / "export.py").read_text()
    assert "mtg_internal" not in source
    assert "mtg_v1.card_any_medium" in source
    assert "mtg_v1.card_face" in source
