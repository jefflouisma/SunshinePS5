"""Device-free coordinator regressions; all observations are synthetic."""

import copy
import hashlib
import importlib.util
import json
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "ps5_recovery", ROOT / "tools/ps5-probes/recovery.py"
)
recovery = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(recovery)


@pytest.fixture
def bundle():
    entries = []
    for index, (path, kind, mode) in enumerate(recovery.expected_layout()):
        entries.append(
            dict(
                path=path,
                kind=kind,
                mode=mode,
                uid=0,
                device=1,
                inode=index + 1,
                links=1 if kind == "file" else 2,
                size=8 if path == "truncated-identity/identity.bin" else 61,
                sha256="a" * 64 if kind == "file" else None,
            )
        )
    return dict(
        schema_version=1,
        console=recovery.CONSOLE,
        host_id=recovery.HOST_ID,
        firmware=recovery.FIRMWARE,
        root=recovery.REMOTE_ROOT,
        observed_at=1000,
        evidence_sha256="a" * 64,
        discovery=(
            "HTTP/1.1 200 Ok\n"
            "host-id:00E421FDEC6B\nhost-type:PS5\nhost-name:PS5-945\n"
            "host-request-port:997\ndevice-discovery-protocol-version:00030010\n"
            "system-version:13600007\n\n"
        ),
        services={
            name: dict(
                port=port, protocol=name, verified=True, evidence_sha256="a" * 64
            )
            for name, port in recovery.SERVICES.items()
        },
        resume=dict(kind="resume", assessed_at=1000, evidence_sha256="a" * 64),
        process=dict(no_game=True, no_writer=True, evidence_sha256="a" * 64),
        isolation=dict(ipv4=True, ipv6=True, exclusive=True, evidence_sha256="a" * 64),
        hdmi=dict(healthy=True, evidence_sha256="a" * 64),
        inventory=dict(complete=True, no_follow=True, entries=entries),
        public_fingerprint=recovery.FINGERPRINT,
    )


@pytest.fixture
def coordinator(tmp_path, monkeypatch):
    files = {}
    for mode in ("verify", "cleanup"):
        path = tmp_path / (mode + ".elf")
        path.write_bytes(mode.encode())
        files[mode] = path
        monkeypatch.setitem(
            recovery.ARTIFACTS, mode, (path, hashlib.sha256(mode.encode()).hexdigest())
        )
    return recovery.Coordinator(tmp_path / "journal", clock=lambda: 1000)


def result(mode):
    line = (
        "T050 load stage=initial status=0 reason=none\n"
        f"T050 mode=verify pid=368 euid=0 sha256={recovery.FINGERPRINT} "
        "paired=1 metrics=0 pass=1\n"
        if mode == "verify"
        else "T050 mode=cleanup pid=369 removed=1 pass=1\n"
    )
    return dict(stdout=line, stderr="", exit_code=0, complete=True, elapsed_seconds=1)


def test_inspection_never_dispatches(coordinator, bundle):
    assert (
        coordinator.inspect(bundle)["status"]
        == "inspection complete; verification not armed"
    )
    assert not list(coordinator.journal.glob("*.intent.json"))


@pytest.mark.parametrize(
    "mutation",
    [
        lambda b: b.update(console="192.168.2.3"),
        lambda b: b.update(firmware="13600008"),
        lambda b: b.update(observed_at=800),
        lambda b: b["resume"].update(kind="boot"),
        lambda b: b["process"].update(no_writer=False),
        lambda b: b["isolation"].update(ipv6=False),
        lambda b: b["services"]["shsrv"].update(verified=False),
        lambda b: b["services"]["shsrv"].update(protocol="open-port"),
        lambda b: b["inventory"].update(complete=False),
        lambda b: b["inventory"]["entries"].pop(),
        lambda b: b["inventory"]["entries"][1].update(kind="symlink"),
        lambda b: b["inventory"]["entries"][1].update(uid=1),
        lambda b: b["inventory"]["entries"][1].update(mode=511),
        lambda b: b["inventory"]["entries"][2].update(links=2),
        lambda b: b.update(public_fingerprint="b" * 64),
    ],
)
def test_inspection_rejects_unsafe_or_incomplete(coordinator, bundle, mutation):
    mutation(bundle)
    with pytest.raises(recovery.Refused):
        coordinator.inspect(bundle)


@pytest.mark.parametrize(
    "text",
    [
        "HTTP/1.1 620 Server Standby\n",
        "HTTP/1.1 200 Ok\nhost-id:00E421FDEC6B\nhost-id:00E421FDEC6B\n\n",
        "HTTP/1.1 200 Ok\nbad\n\n",
        "x" * 4097,
    ],
)
def test_discovery_rejects_standby_malformed_duplicate(text):
    with pytest.raises(recovery.Refused):
        recovery.discovery_ready(text)


def test_inspection_historical_inventory_cannot_arm(coordinator, bundle):
    bundle["inventory"] = json.loads(
        (
            ROOT / ".omo/evidence/t050-rest-device-20261031/postinit-inventory.json"
        ).read_text()
    )
    with pytest.raises(recovery.Refused):
        coordinator.verify(bundle, lambda *_: pytest.fail("dispatched"), armed=True)


def test_verify_requires_separate_arming_and_original_hash(coordinator, bundle):
    with pytest.raises(recovery.Refused):
        coordinator.verify(bundle, lambda *_: pytest.fail("dispatched"))
    recovery.ARTIFACTS["verify"][0].write_bytes(b"changed")
    with pytest.raises(recovery.Refused):
        coordinator.verify(bundle, lambda *_: pytest.fail("dispatched"), armed=True)
    assert not (coordinator.journal / "verify.intent.json").exists()


def test_successful_sequence_exact_bytes_and_postflight(coordinator, bundle):
    calls = []

    def dispatch(mode, payload, timeout, quota):
        assert (coordinator.journal / f"{mode}.intent.json").exists()
        assert payload == mode.encode() and timeout == 30 and quota == 65536
        calls.append(mode)
        return result(mode)

    coordinator.verify(bundle, dispatch, armed=True)
    coordinator.record_postverify(bundle)
    coordinator.cleanup(bundle, dispatch, armed=True)
    postflight = copy.deepcopy(bundle)
    postflight["http"] = dict(
        url=f"http://{recovery.CONSOLE}:8080{recovery.REMOTE_ROOT}", status=404
    )
    assert coordinator.finish(postflight)["status"] == "cleanup independently confirmed"
    assert calls == ["verify", "cleanup"]
    with pytest.raises(recovery.Refused):
        coordinator.verify(bundle, dispatch, armed=True)


@pytest.mark.parametrize("failure", [TimeoutError, RuntimeError, KeyboardInterrupt])
def test_intent_blocks_replay_after_failure(coordinator, bundle, failure):
    def dispatch(*_):
        raise failure("synthetic failure after intent")

    with pytest.raises(failure):
        coordinator.verify(bundle, dispatch, armed=True)
    assert json.loads((coordinator.journal / "verify.failure.json").read_text()) == {
        "type": failure.__name__
    }
    reopened = recovery.Coordinator(coordinator.journal, clock=lambda: 1000)
    with pytest.raises(recovery.Refused):
        reopened.verify(bundle, lambda *_: pytest.fail("replayed"), armed=True)


@pytest.mark.parametrize(
    "mutation",
    [
        lambda r: r.update(exit_code=None),
        lambda r: r.update(exit_code=1),
        lambda r: r.update(complete=False),
        lambda r: r.update(stdout="prefix " + r["stdout"]),
        lambda r: r.update(stdout=r["stdout"] + r["stdout"]),
        lambda r: r.update(stdout=r["stdout"].replace("metrics=0", "metrics=1")),
        lambda r: r.update(stdout=r["stdout"].replace(recovery.FINGERPRINT, "b" * 64)),
        lambda r: r.update(stderr="T050 fail line=1"),
        lambda r: r.update(elapsed_seconds=31),
        lambda r: r.update(stdout="x" * 65537),
    ],
)
def test_incomplete_or_misleading_result_consumes_attempt(
    coordinator, bundle, mutation
):
    response = result("verify")
    mutation(response)
    with pytest.raises(recovery.Refused):
        coordinator.verify(bundle, lambda *_: response, armed=True)
    assert (coordinator.journal / "verify.intent.json").exists()
    with pytest.raises(recovery.Refused):
        coordinator.cleanup(bundle, lambda *_: pytest.fail("cleanup"), armed=True)


def test_cleanup_requires_postverify_and_separate_arm(coordinator, bundle):
    coordinator.verify(bundle, lambda *_: result("verify"), armed=True)
    with pytest.raises(recovery.Refused):
        coordinator.cleanup(bundle, lambda *_: pytest.fail("cleanup"), armed=True)
    coordinator.record_postverify(bundle)
    with pytest.raises(recovery.Refused):
        coordinator.cleanup(bundle, lambda *_: pytest.fail("cleanup"))
    changed = copy.deepcopy(bundle)
    changed["inventory"]["entries"][2]["inode"] += 100
    with pytest.raises(recovery.Refused):
        coordinator.cleanup(changed, lambda *_: pytest.fail("cleanup"), armed=True)


@pytest.mark.parametrize("failure", ["root", "service", "hdmi", "wrong_url"])
def test_finish_requires_independent_postflight(coordinator, bundle, failure):
    coordinator.verify(bundle, lambda *_: result("verify"), armed=True)
    coordinator.record_postverify(bundle)
    coordinator.cleanup(bundle, lambda *_: result("cleanup"), armed=True)
    bundle["http"] = dict(
        url=f"http://{recovery.CONSOLE}:8080{recovery.REMOTE_ROOT}", status=404
    )
    if failure == "root":
        bundle["http"]["status"] = 200
    elif failure == "wrong_url":
        bundle["http"]["url"] += "-other"
    elif failure == "service":
        bundle["services"]["websrv"]["verified"] = False
    else:
        bundle["hdmi"]["healthy"] = False
    with pytest.raises(recovery.Refused):
        coordinator.finish(bundle)


def test_truncated_existing_intent_is_nonretryable(coordinator, bundle):
    (coordinator.journal / "verify.intent.json").write_text("")
    with pytest.raises(recovery.Refused):
        coordinator.verify(bundle, lambda *_: pytest.fail("replayed"), armed=True)


def test_fsync_failure_never_dispatches(coordinator, bundle, monkeypatch):
    def fail(_):
        raise OSError("synthetic durability failure")

    monkeypatch.setattr(recovery.os, "fsync", fail)
    with pytest.raises(OSError):
        coordinator.verify(bundle, lambda *_: pytest.fail("dispatched"), armed=True)
    assert (coordinator.journal / "verify.intent.json").exists()


def test_pre_dispatch_inventory_cannot_be_reused_as_postflight(coordinator, bundle):
    coordinator.clock = lambda: 1001
    coordinator.verify(bundle, lambda *_: result("verify"), armed=True)
    with pytest.raises(recovery.Refused, match="precedes"):
        coordinator.record_postverify(bundle)


def test_failed_cleanup_cannot_finish_or_retry(coordinator, bundle):
    coordinator.verify(bundle, lambda *_: result("verify"), armed=True)
    coordinator.record_postverify(bundle)
    bad = result("cleanup")
    bad["stdout"] = bad["stdout"].replace("removed=1", "removed=0")
    with pytest.raises(recovery.Refused):
        coordinator.cleanup(bundle, lambda *_: bad, armed=True)
    with pytest.raises(recovery.Refused):
        coordinator.cleanup(bundle, lambda *_: pytest.fail("replayed"), armed=True)
    with pytest.raises(recovery.Refused):
        coordinator.finish(bundle)


def test_cli_inspects_without_creating_journal(tmp_path, bundle):
    import subprocess
    import sys
    import time

    bundle["observed_at"] = time.time()
    bundle["resume"]["assessed_at"] = bundle["observed_at"]
    path = tmp_path / "synthetic-observations.json"
    path.write_text(json.dumps(bundle))
    completed = subprocess.run(
        [sys.executable, "-B", str(ROOT / "tools/ps5-probes/recovery.py"), str(path)],
        capture_output=True,
        text=True,
        timeout=5,
    )
    (tmp_path / "cli.stdout").write_text(completed.stdout)
    (tmp_path / "cli.stderr").write_text(completed.stderr)
    assert completed.returncode == 0
    assert completed.stdout == "inspection complete; verification not armed\n"


def test_duplicate_json_keys_rejected(tmp_path):
    path = tmp_path / "duplicate.json"
    path.write_text('{"root":"a","root":"b"}')
    with pytest.raises(recovery.Refused, match="duplicate"):
        recovery.read_json(path)


def test_pinned_actual_artifacts_match_reviewed_hashes():
    for path, expected in recovery.ARTIFACTS.values():
        assert (
            hashlib.sha256(recovery.read_bounded(path, 32 * 1024 * 1024)).hexdigest()
            == expected
        )


def test_concurrent_dispatchers_send_once(coordinator, bundle):
    from concurrent.futures import ThreadPoolExecutor
    from threading import Barrier

    barrier = Barrier(2)
    calls = []

    def dispatch(*_):
        calls.append("sent")
        return result("verify")

    def attempt():
        barrier.wait(timeout=5)
        try:
            coordinator.verify(bundle, dispatch, armed=True)
            return "sent"
        except recovery.Refused:
            return "refused"

    with ThreadPoolExecutor(max_workers=2) as pool:
        first, second = pool.submit(attempt), pool.submit(attempt)
        assert sorted((first.result(timeout=5), second.result(timeout=5))) == [
            "refused",
            "sent",
        ]
    assert calls == ["sent"]


@pytest.mark.parametrize("existing", [False, True])
def test_parent_directory_fsync_failure_prevents_dispatch(
    tmp_path, bundle, monkeypatch, existing
):
    parent = tmp_path / "private-parent"
    parent.mkdir(mode=0o700)
    journal = parent / "journal"
    if existing:
        journal.mkdir(mode=0o700)
    parent_identity = (parent.stat().st_dev, parent.stat().st_ino)
    real_fsync = recovery.os.fsync
    calls = []

    def fail_parent(fd):
        info = recovery.os.fstat(fd)
        if (info.st_dev, info.st_ino) == parent_identity and journal.exists():
            raise OSError("synthetic journal-parent fsync failure")
        real_fsync(fd)

    monkeypatch.setattr(recovery.os, "fsync", fail_parent)
    with pytest.raises(OSError, match="journal-parent"):
        candidate = recovery.Coordinator(journal, clock=lambda: 1000)
        candidate.verify(bundle, lambda *_: calls.append("sent"), armed=True)
    assert calls == []
    assert journal.is_dir()
    assert not (journal / "verify.intent.json").exists()


def test_nested_journal_creation_is_durable_before_send(
    coordinator, tmp_path, bundle, monkeypatch
):
    journal = tmp_path / "new-parent" / "new-child" / "journal"
    real_fsync = recovery.os.fsync
    synced = []

    def record_sync(fd):
        info = recovery.os.fstat(fd)
        real_fsync(fd)
        synced.append((info.st_dev, info.st_ino))

    monkeypatch.setattr(recovery.os, "fsync", record_sync)
    candidate = recovery.Coordinator(journal, clock=lambda: 1000)

    def dispatch(*_):
        # Every containing directory's entry is persisted before the payload call.
        for path in (tmp_path, journal.parent.parent, journal.parent, journal):
            info = path.stat()
            assert (info.st_dev, info.st_ino) in synced
        intent = (journal / "verify.intent.json").stat()
        assert (intent.st_dev, intent.st_ino) in synced
        return result("verify")

    candidate.verify(bundle, dispatch, armed=True)
    candidate.close()
    reopened = recovery.Coordinator(journal, clock=lambda: 1000)
    with pytest.raises(recovery.Refused):
        reopened.verify(bundle, lambda *_: pytest.fail("replayed"), armed=True)
    (tmp_path / "directory-fsync-identities.json").write_text(json.dumps(synced))


def test_journal_creation_refuses_symlink_ancestor(tmp_path):
    real_parent = tmp_path / "real"
    real_parent.mkdir(mode=0o700)
    alias = tmp_path / "alias"
    alias.symlink_to(real_parent, target_is_directory=True)
    with pytest.raises((OSError, recovery.Refused)):
        recovery.Coordinator(alias / "journal")
    assert not (real_parent / "journal").exists()
