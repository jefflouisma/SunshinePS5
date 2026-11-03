"""Manifest fixtures exercise validation only; none are release evidence."""

import copy
import hashlib
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/check-release-evidence.py"
SPEC = importlib.util.spec_from_file_location("release_evidence", SCRIPT)
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


@pytest.fixture
def manifest(tmp_path):
    entries = []
    for identifier, categories in CHECKER.REQUIRED.items():
        refs = []
        for category in categories:
            source = "ci" if category == "ci" else "physical-console"
            payload = {
                "schema_version": 1,
                "subject": identifier,
                "verdict": "Pass",
                "category": category,
                "source": source,
                "details": "Unit fixture only; never physical acceptance evidence.",
            }
            raw = json.dumps(payload).encode()
            path = f"{identifier}-{category}.json"
            (tmp_path / path).write_bytes(raw)
            refs.append({"path": path, "sha256": hashlib.sha256(raw).hexdigest()})
        entries.append(
            {"id": identifier, "status": "Pass", "reason": "fixture", "evidence": refs}
        )
    return {"schema_version": 1, "overall": "Pass", "entries": entries}


def evaluate(manifest, tmp_path):
    return CHECKER.evaluate(manifest, tmp_path)


def test_complete_fixture_and_fixed_requirements(manifest, tmp_path):
    assert evaluate(manifest, tmp_path)["overall"] == "Pass"
    assert {k for k in CHECKER.REQUIRED if k.startswith("SC-")} == {
        f"SC-{n:03}" for n in range(1, 15)
    }
    assert {k for k in CHECKER.REQUIRED if k.startswith("US")} == {
        f"US{n}" for n in range(1, 6)
    }
    assert {c for cats in CHECKER.REQUIRED.values() for c in cats} == {
        "ci",
        "acceptance",
        "hardware",
    }


def test_incomplete_precedes_failure_without_erasing_it(manifest, tmp_path):
    failed = manifest["entries"][0]
    failed.update(status="Fail", reason="known failure")
    for ref in failed["evidence"]:
        path = tmp_path / ref["path"]
        payload = json.loads(path.read_text())
        payload["verdict"] = "Fail"
        raw = json.dumps(payload).encode()
        path.write_bytes(raw)
        ref["sha256"] = hashlib.sha256(raw).hexdigest()
    manifest["entries"][1].update(status="Incomplete", evidence=[])
    manifest["overall"] = "Incomplete"
    result = evaluate(manifest, tmp_path)
    assert result["overall"] == "Incomplete"
    assert "SC-001" in result["failures"]
    assert "SC-002" in result["incomplete"]


def test_valid_failure_is_nonzero_not_incomplete(manifest, tmp_path):
    entry = manifest["entries"][0]
    entry["status"] = "Fail"
    for ref in entry["evidence"]:
        path = tmp_path / ref["path"]
        payload = json.loads(path.read_text())
        payload["verdict"] = "Fail"
        raw = json.dumps(payload).encode()
        path.write_bytes(raw)
        ref["sha256"] = hashlib.sha256(raw).hexdigest()
    manifest["overall"] = "Fail"
    assert evaluate(manifest, tmp_path)["overall"] == "Fail"


def test_unverified_failure_is_incomplete_and_not_reported_as_demonstrated(
    manifest, tmp_path
):
    manifest["entries"][0].update(
        status="Fail", reason="unsupported failure assertion", evidence=[]
    )
    manifest["overall"] = "Incomplete"
    result = evaluate(manifest, tmp_path)
    assert result["overall"] == "Incomplete"
    assert "SC-001" in result["incomplete"]
    assert "SC-001" not in result["failures"]


@pytest.mark.parametrize(
    "mutation", ["missing", "unknown", "duplicate", "status", "extra", "overall"]
)
def test_malformed_or_incomplete_sets_fail_closed(manifest, tmp_path, mutation):
    if mutation == "missing":
        manifest["entries"].pop()
    elif mutation == "unknown":
        manifest["entries"][0]["id"] = "SC-999"
    elif mutation == "duplicate":
        manifest["entries"].append(copy.deepcopy(manifest["entries"][0]))
    elif mutation == "status":
        manifest["entries"][0]["status"] = "Skipped"
    elif mutation == "extra":
        manifest["waiver"] = True
    else:
        manifest["overall"] = "Fail"
    result = evaluate(manifest, tmp_path)
    assert result["overall"] == "Incomplete"
    assert result["errors"]


@pytest.mark.parametrize(
    "path",
    [
        "../outside.json",
        "/tmp/a.json",
        "https://host/a",
        "a/../b",
        "a\\b",
        "./a",
        "a//b",
    ],
)
def test_unsafe_refs_rejected(manifest, tmp_path, path):
    manifest["entries"][0]["evidence"][0]["path"] = path
    assert evaluate(manifest, tmp_path)["overall"] == "Incomplete"


@pytest.mark.parametrize(
    "damage",
    [
        "absent",
        "hash",
        "empty",
        "symlink",
        "category",
        "subject",
        "verdict",
        "synthetic",
        "fake-platform",
    ],
)
def test_invalid_artifact_cannot_satisfy_hardware(manifest, tmp_path, damage):
    ref = manifest["entries"][0]["evidence"][1]
    path = tmp_path / ref["path"]
    if damage == "absent":
        path.unlink()
    elif damage == "hash":
        ref["sha256"] = "0" * 64
    elif damage == "empty":
        path.write_bytes(b"")
    elif damage == "symlink":
        original = tmp_path / "original.json"
        path.rename(original)
        path.symlink_to(original)
    else:
        payload = json.loads(path.read_text())
        field = "source" if damage in {"synthetic", "fake-platform"} else damage
        payload[field] = damage if field == "source" else "wrong"
        raw = json.dumps(payload).encode()
        path.write_bytes(raw)
        ref["sha256"] = hashlib.sha256(raw).hexdigest()
    assert evaluate(manifest, tmp_path)["overall"] == "Incomplete"


@pytest.mark.parametrize("category", ["ci", "acceptance", "hardware"])
def test_missing_category_never_passes(manifest, tmp_path, category):
    for entry in manifest["entries"]:
        entry["evidence"] = [
            r for r in entry["evidence"] if not r["path"].endswith(f"-{category}.json")
        ]
    assert evaluate(manifest, tmp_path)["overall"] == "Incomplete"


def test_duplicate_json_keys_rejected(tmp_path):
    path = tmp_path / "manifest.json"
    path.write_text('{"schema_version":1,"schema_version":1}')
    result = subprocess.run(
        [sys.executable, str(SCRIPT), str(path)], capture_output=True, text=True
    )
    assert result.returncode == 2
    assert json.loads(result.stdout)["overall"] == "Incomplete"


def test_real_seed_stays_incomplete():
    path = ROOT / "docs/evidence/release-manifest.json"
    seed = json.loads(path.read_text())
    result = CHECKER.evaluate(seed, ROOT)
    assert result["overall"] == "Incomplete"
    assert not result["errors"]
    assert all(
        e["status"] == "Incomplete" and e["reason"] and not e["evidence"]
        for e in seed["entries"]
    )


@pytest.mark.parametrize("identifier", CHECKER.REQUIRED)
def test_every_required_entry_is_mandatory(manifest, tmp_path, identifier):
    manifest["entries"] = [e for e in manifest["entries"] if e["id"] != identifier]
    result = evaluate(manifest, tmp_path)
    assert result["overall"] == "Incomplete"
    assert identifier in result["incomplete"]


@pytest.mark.parametrize("bad", [None, [], True, 1, "Pass"])
def test_wrong_root_types_are_incomplete(tmp_path, bad):
    assert evaluate(bad, tmp_path)["overall"] == "Incomplete"


@pytest.mark.parametrize("field", ["id", "status", "evidence", "reason"])
def test_wrong_entry_types_are_incomplete(manifest, tmp_path, field):
    manifest["entries"][0][field] = {}
    assert evaluate(manifest, tmp_path)["overall"] == "Incomplete"


def test_attestation_duplicate_keys_rejected(manifest, tmp_path):
    ref = manifest["entries"][0]["evidence"][0]
    path = tmp_path / ref["path"]
    raw = path.read_bytes().replace(
        b'"schema_version": 1', b'"schema_version": 1, "schema_version": 1'
    )
    path.write_bytes(raw)
    ref["sha256"] = hashlib.sha256(raw).hexdigest()
    assert evaluate(manifest, tmp_path)["overall"] == "Incomplete"


def test_oversized_attestation_rejected(manifest, tmp_path):
    ref = manifest["entries"][0]["evidence"][0]
    raw = b" " * (CHECKER.LIMIT + 1)
    (tmp_path / ref["path"]).write_bytes(raw)
    ref["sha256"] = hashlib.sha256(raw).hexdigest()
    assert evaluate(manifest, tmp_path)["overall"] == "Incomplete"


def test_duplicate_refs_rejected(manifest, tmp_path):
    refs = manifest["entries"][0]["evidence"]
    refs.append(copy.deepcopy(refs[0]))
    assert evaluate(manifest, tmp_path)["overall"] == "Incomplete"


def test_cli_matches_verdict(manifest, tmp_path):
    path = tmp_path / "manifest.json"
    for verdict, exit_code in [("Pass", 0), ("Incomplete", 2)]:
        if verdict == "Incomplete":
            manifest["entries"][0].update(status=verdict, evidence=[])
            manifest["overall"] = verdict
        path.write_text(json.dumps(manifest))
        result = subprocess.run(
            [sys.executable, str(SCRIPT), str(path), "--evidence-root", str(tmp_path)],
            capture_output=True,
            text=True,
        )
        assert result.returncode == exit_code
        assert json.loads(result.stdout)["overall"] == verdict


def test_cli_preserves_real_failure_exit(manifest, tmp_path):
    entry = manifest["entries"][0]
    entry["status"] = "Fail"
    for ref in entry["evidence"]:
        path = tmp_path / ref["path"]
        payload = json.loads(path.read_text())
        payload["verdict"] = "Fail"
        raw = json.dumps(payload).encode()
        path.write_bytes(raw)
        ref["sha256"] = hashlib.sha256(raw).hexdigest()
    manifest["overall"] = "Fail"
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(manifest))
    result = subprocess.run(
        [sys.executable, str(SCRIPT), str(path), "--evidence-root", str(tmp_path)],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 1
    assert json.loads(result.stdout)["failures"] == ["SC-001"]
