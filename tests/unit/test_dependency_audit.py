"""Offline audit regressions; mocked network checks never establish live coverage."""

import copy
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/audit-dependencies.sh"


@pytest.fixture
def audit(monkeypatch):
    source = SCRIPT.read_text().split("<<'PYTHON'\n", 1)[1].rsplit("\nPYTHON", 1)[0]
    monkeypatch.setattr(sys, "argv", [str(SCRIPT), str(ROOT)])
    namespace = {"__name__": "dependency_audit_test"}
    # Import the repository-owned heredoc with its main entry point disabled.
    exec(compile(source, str(SCRIPT), "exec"), namespace)  # noqa: S102
    return namespace


@pytest.fixture
def host_lock():
    return json.loads((ROOT / "cmake/dependencies.lock.json").read_text())


def inventory(audit, monkeypatch, lock):
    monkeypatch.setitem(audit, "load", lambda *_: copy.deepcopy(lock))
    monkeypatch.setitem(audit, "git_pin", lambda *_: "mock tag pin")
    monkeypatch.setitem(audit, "host_archive", lambda *_: "mock archive integrity")
    return audit["host_inventory"]()


def test_locked_enet_commit_is_queried_without_invented_tag(
    audit, monkeypatch, host_lock
):
    queries = inventory(audit, monkeypatch, host_lock)
    enet = host_lock["dependencies"]["enet"]
    assert len(queries) == 8
    assert [item for item in queries if item[0].startswith("host enet@")] == [
        (f"host enet@{enet['version']} commit", {"commit": enet["source_commit"]})
    ]
    checks = audit["REPORT"]["checks"]
    provenance = next(check for check in checks if check["name"].endswith("source pin"))
    assert provenance["status"] == "pass"
    for exact in (enet["version"], enet["repository"], enet["source_commit"]):
        assert exact in provenance["detail"]
    assert any(
        "enet@" in check["name"] and "source_archive" in check["name"]
        for check in checks
    )


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("version", "1.3.17"),
        ("version", "1.3.18-moonlight-aca8784"),
        ("version", "1.3.17-other-aca8784"),
        ("version", "1.3.17-moonlight-aca8785"),
        ("version", "1.3.17-moonlight-aca8784;id"),
        ("version", "1.3.17-moonlight-aca8784\n"),
        ("version", None),
        ("repository", "https://github.com/other/enet"),
        ("repository", "http://github.com/cgutman/enet"),
        ("source_commit", "aca8784"),
        ("source_commit", "b" * 40),
        ("source_commit", None),
        ("tag", "v1.3.17"),
    ],
)
def test_enet_rejects_unsupported_or_inconsistent_identity(
    audit, monkeypatch, host_lock, field, value
):
    host_lock["dependencies"]["enet"][field] = value
    with pytest.raises(ValueError):
        inventory(audit, monkeypatch, host_lock)


def test_enet_rejects_archive_for_another_commit(audit, monkeypatch, host_lock):
    host_lock["dependencies"]["enet"]["source_archive"]["url"] = (
        "https://codeload.github.com/cgutman/enet/tar.gz/main"
    )
    with pytest.raises(ValueError):
        inventory(audit, monkeypatch, host_lock)


@pytest.mark.parametrize("dependency", ["ps5_payload_sdk", "llvm", "openssl"])
def test_other_dependencies_still_require_numeric_version_and_tag(
    audit, monkeypatch, host_lock, dependency
):
    host_lock["dependencies"][dependency]["version"] += "-moonlight-aca8784"
    with pytest.raises(ValueError):
        inventory(audit, monkeypatch, host_lock)


def test_host_queries_reach_osv_with_exact_commit(audit, monkeypatch, host_lock):
    queries = inventory(audit, monkeypatch, host_lock)
    requests = []

    def response(url, data):
        requests.append((url, copy.deepcopy(data)))
        return {"results": [{} for _ in data["queries"]]}

    monkeypatch.setitem(audit, "json_request", response)
    assert audit["vulnerabilities"](queries).startswith("8 complete live queries;")
    assert requests == [
        (audit["OSV"] + "querybatch", {"queries": [q for _, q in queries]})
    ]
    assert requests[0][1]["queries"][-1] == {
        "commit": host_lock["dependencies"]["enet"]["source_commit"]
    }
