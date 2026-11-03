"""Synthetic callback and socket fixtures; these tests never contact a PS5."""

import asyncio
import base64
import importlib.util
import json
import os
import sys
from pathlib import Path

import pytest

PROBES = Path(__file__).resolve().parents[2] / "tools/ps5-probes"
sys.path.insert(0, str(PROBES))
SPEC = importlib.util.spec_from_file_location(
    "recovery_adapter", PROBES / "recovery_adapter.py"
)
A = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(A)
R = A.R
READY = (
    b"HTTP/1.1 200 Ok\nhost-id:00E421FDEC6B\nhost-type:PS5\nhost-name:PS5-945\n"
    b"host-request-port:997\ndevice-discovery-protocol-version:00030010\nsystem-version:13600007\n\n"
)


def wrapped(value, nonce, **changes):
    body = {
        "nonce": nonce,
        "console": R.CONSOLE,
        "host_id": R.HOST_ID,
        "firmware": R.FIRMWARE,
        "root": R.REMOTE_ROOT,
        "observed_at": 1000,
        "value": value,
    }
    body.update(changes)
    return R.encoded(body)


def inventory():
    return {
        "complete": True,
        "no_follow": True,
        "entries": [
            {
                "path": path,
                "kind": kind,
                "mode": mode,
                "uid": 0,
                "device": 1,
                "inode": i + 1,
                "links": 1 if kind == "file" else 2,
                "size": 61,
                "sha256": "a" * 64 if kind == "file" else None,
            }
            for i, (path, kind, mode) in enumerate(R.expected_layout())
        ],
    }


def output(mode):
    return (
        (
            "T050 load stage=initial status=0 reason=none\n"
            f"T050 mode=verify pid=368 euid=0 sha256={R.FINGERPRINT} "
            "paired=1 metrics=0 pass=1\n"
        )
        if mode == "verify"
        else "T050 mode=cleanup pid=369 removed=1 pass=1\n"
    ).encode()


@pytest.fixture
def setup(tmp_path, monkeypatch):
    image = b"\x89PNG\r\n\x1a\nsynthetic classifier-owned bytes"
    values = {
        "services": {
            name: {
                "port": port,
                "protocol": name,
                "verified": True,
                "transcript": "synthetic protocol check",
            }
            for name, port in R.SERVICES.items()
        },
        "boot": {"boot_id": "boot-before-rest", "kind": "resume"},
        "process": {"no_game": True, "no_writer": True},
        "isolation": {"ipv4": True, "ipv6": True, "exclusive": True},
        "hdmi": {
            "healthy": True,
            "image_base64": base64.b64encode(image).decode(),
            "image_sha256": A.sha(image),
        },
        "inventory": {"inventory": inventory(), "public_fingerprint": R.FINGERPRINT},
    }
    callbacks = {}
    for name in A.COMPONENTS:

        async def callback(nonce, deadline, name=name):
            assert deadline > asyncio.get_running_loop().time()
            return wrapped(values[name], nonce)

        callbacks[name] = callback

    async def discovery(deadline):
        return READY

    monkeypatch.setattr(A, "discover", discovery)

    async def exit_observer(nonce, deadline, mode, payload_hash, stdout_hash):
        return wrapped(
            {
                "mode": mode,
                "payload_sha256": payload_hash,
                "stdout_sha256": stdout_hash,
                "exit_code": 0,
                "complete": True,
                "pid": 368 if mode == "verify" else 369,
                "boot_id": "boot-before-rest",
                "process_start_id": "synthetic-start-1",
            },
            nonce,
        )

    baseline = R.encoded(
        {
            "console": R.CONSOLE,
            "host_id": R.HOST_ID,
            "firmware": R.FIRMWARE,
            "boot_id": "boot-before-rest",
            "observed_at": 900,
        }
    )
    adapter = A.Adapter(
        tmp_path / "evidence",
        callbacks,
        exit_observer=exit_observer,
        baseline=baseline,
        journal=tmp_path / "journal",
        clock=lambda: 1000,
    )
    for mode in ("verify", "cleanup"):
        file = tmp_path / (mode + ".elf")
        file.write_bytes(mode.encode())
        monkeypatch.setitem(R.ARTIFACTS, mode, (file, A.sha(mode.encode())))
    return adapter, values


class Writer:
    def __init__(self, mode, fail=""):
        self.mode, self.fail = mode, fail
        self.payload, self.closed, self.aborted, self.eof = b"", False, False, False
        self.transport = self

    def get_extra_info(self, key):
        assert key == "peername"
        return (R.CONSOLE, 9021) if self.fail != "peer" else ("192.168.2.3", 9021)

    def write(self, data):
        self.payload += data

    async def drain(self):
        if self.fail == "send":
            raise ConnectionResetError("synthetic partial send")

    def write_eof(self):
        self.eof = True

    def close(self):
        self.closed = True
        if self.fail == "close":
            raise OSError("synthetic close error")

    def abort(self):
        self.aborted = True


def socket_fixture(monkeypatch, fail=""):
    writers = []

    async def connect(host, port, *, local_addr, limit):
        assert (host, port, local_addr, limit) == (R.CONSOLE, 9021, (A.LOCAL, 0), 65537)
        mode = "verify" if not writers else "cleanup"
        writer = Writer(mode, fail)
        writers.append(writer)
        reader = asyncio.StreamReader()
        reader.feed_data(b"x" * 65537 if fail == "quota" else output(mode))
        if fail == "read":
            reader.set_exception(TimeoutError("synthetic read deadline"))
        else:
            reader.feed_eof()
        return reader, writer

    monkeypatch.setattr(A.asyncio, "open_connection", connect)
    return writers


def test_collect_retains_sources_and_no_journal(setup):
    adapter, _ = setup
    bundle = asyncio.run(adapter.collect())
    R.validate_bundle(bundle, 1000)
    assert not adapter.journal.exists()
    index = json.loads((adapter.last_evidence / "sources.json").read_bytes())
    assert len(index["sources"]) == 9
    for source in index["sources"]:
        raw = (adapter.last_evidence / source["path"]).read_bytes()
        assert (len(raw), A.sha(raw)) == (source["bytes"], source["sha256"])


@pytest.mark.parametrize("kind", ["callbacks", "exit", "baseline"])
def test_unbound_refuses_before_any_acquisition_or_intent(setup, kind):
    adapter, _ = setup
    if kind == "callbacks":
        adapter.callbacks.pop("services")
    elif kind == "exit":
        adapter.exit_observer = None
    else:
        adapter.baseline = None
    with pytest.raises(R.Refused):
        adapter.dispatch("verify", armed=True)
    assert not adapter.journal.exists()
    assert adapter.last_evidence is None


@pytest.mark.parametrize(
    "raw",
    [
        READY.replace(b"200 Ok", b"620 Server Standby"),
        READY.replace(b"13600007", b"13600008"),
        READY.replace(b"00E421FDEC6B", b"00E421FDEC6C"),
        READY + b"partial",
        READY.replace(b"host-type:PS5", b"host-type:PS5\nhost-type:PS5"),
    ],
)
def test_discovery_refusal_retains_raw_without_intent(setup, monkeypatch, raw):
    adapter, _ = setup

    async def bad(deadline):
        return raw

    monkeypatch.setattr(A, "discover", bad)
    with pytest.raises(R.Refused):
        adapter.dispatch("verify", armed=True)
    assert (adapter.last_evidence / "discovery.bin").read_bytes() == raw
    assert not adapter.journal.exists()


@pytest.mark.parametrize(
    "change",
    [
        {"nonce": "replayed"},
        {"observed_at": 999},
        {"observed_at": 1001},
        {"console": "192.168.2.3"},
        {"root": "/data"},
        {"firmware": "wrong"},
    ],
)
def test_callback_envelope_binding_and_freshness(setup, change):
    adapter, values = setup

    async def bad(nonce, deadline):
        return (
            wrapped(values["process"], nonce, **change)
            if "nonce" not in change
            else wrapped(values["process"], "replayed")
        )

    adapter.callbacks["process"] = bad
    with pytest.raises(R.Refused):
        adapter.dispatch("verify", armed=True)
    assert not adapter.journal.exists()


@pytest.mark.parametrize(
    "mutation",
    [
        lambda v: v["boot"].update(boot_id="new-boot"),
        lambda v: v["boot"].update(kind="unknown"),
        lambda v: v["process"].update(no_writer=False),
        lambda v: v["process"].update(no_game=False),
        lambda v: v["services"]["loader"].update(protocol="tcp-open"),
        lambda v: v["services"]["gdbsrv"].update(transcript=""),
        lambda v: v["hdmi"].update(healthy=False),
        lambda v: v["hdmi"].update(image_sha256="0" * 64),
        lambda v: v["inventory"]["inventory"].update(complete=False),
        lambda v: v["inventory"]["inventory"]["entries"].pop(),
        lambda v: v["inventory"]["inventory"]["entries"][2].update(kind="symlink"),
        lambda v: v["inventory"]["inventory"]["entries"][2].update(links=2),
        lambda v: v["inventory"].update(public_fingerprint="0" * 64),
    ],
)
def test_observation_failures_cannot_arm(setup, mutation):
    adapter, values = setup
    mutation(values)
    with pytest.raises(R.Refused):
        adapter.dispatch("verify", armed=True)
    assert not adapter.journal.exists()


def test_duplicate_json_retained_and_refused(setup):
    adapter, _ = setup

    async def bad(nonce, deadline):
        return b'{"nonce":"a","nonce":"b"}'

    adapter.callbacks["process"] = bad
    with pytest.raises(R.Refused, match="duplicate"):
        asyncio.run(adapter.collect())
    assert (
        adapter.last_evidence / "process.json"
    ).read_bytes() == b'{"nonce":"a","nonce":"b"}'


def test_exact_transfer_and_separate_cleanup_gate(setup, monkeypatch):
    adapter, _ = setup
    writers = socket_fixture(monkeypatch)
    result = adapter.dispatch("verify", armed=True)
    assert result["complete"] is True
    assert (
        writers[0].payload == b"verify"
        and writers[0].eof
        and writers[0].closed
        and writers[0].aborted
    )
    with pytest.raises(R.Refused, match="postverify"):
        adapter.dispatch("cleanup", armed=True)
    assert len(writers) == 1
    adapter.record_postverify()
    with pytest.raises(R.Refused, match="arming"):
        adapter.dispatch("cleanup")
    adapter.dispatch("cleanup", armed=True)
    assert writers[1].payload == b"cleanup" and writers[1].closed and writers[1].aborted
    with pytest.raises(R.Refused, match="existing journal"):
        adapter.dispatch("verify", armed=True)
    assert len(writers) == 2


@pytest.mark.parametrize("fail", ["send", "read", "quota", "peer", "close"])
def test_transfer_failure_consumes_attempt_and_closes(setup, monkeypatch, fail):
    adapter, _ = setup
    writers = socket_fixture(monkeypatch, fail)
    with pytest.raises((R.Refused, OSError)):
        adapter.dispatch("verify", armed=True)
    assert writers[0].closed and writers[0].aborted
    assert (adapter.journal / "verify.intent.json").exists()
    assert not (adapter.journal / "verify.completed.json").exists()
    assert (adapter.last_evidence / "transfer.json").exists()
    with pytest.raises(R.Refused, match="existing journal"):
        adapter.dispatch("verify", armed=True)
    assert len(writers) == 1


@pytest.mark.parametrize(
    "mutation",
    [
        lambda v: v.update(exit_code=False),
        lambda v: v.update(exit_code=1),
        lambda v: v.update(pid=999),
        lambda v: v.update(complete=False),
        lambda v: v.update(boot_id="replacement"),
        lambda v: v.update(stdout_sha256="0" * 64),
    ],
)
def test_eof_cannot_establish_exit(setup, monkeypatch, mutation):
    adapter, _ = setup
    writers = socket_fixture(monkeypatch)
    original = adapter.exit_observer

    async def bad(*args):
        value = json.loads(await original(*args))
        mutation(value["value"])
        return R.encoded(value)

    adapter.exit_observer = bad
    with pytest.raises(R.Refused, match="exit evidence"):
        adapter.dispatch("verify", armed=True)
    assert writers[0].closed and writers[0].aborted
    assert (adapter.last_evidence / "stdout.bin").read_bytes() == output("verify")
    assert not (adapter.journal / "verify.completed.json").exists()


def test_tampered_artifact_never_opens_socket(setup, monkeypatch):
    adapter, _ = setup
    writers = socket_fixture(monkeypatch)
    R.ARTIFACTS["verify"][0].write_bytes(b"tampered")
    with pytest.raises(R.Refused, match="hash mismatch"):
        adapter.dispatch("verify", armed=True)
    assert not writers and not list(adapter.journal.glob("*.intent.json"))


def test_source_storage_symlink_refused(tmp_path):
    parent = tmp_path / "real"
    parent.mkdir()
    link = tmp_path / "link"
    link.symlink_to(parent, target_is_directory=True)
    with pytest.raises(OSError):
        A.Evidence(link)


def test_evidence_no_replace(tmp_path):
    evidence = A.Evidence(tmp_path / "evidence")
    try:
        evidence.write("source.bin", b"first")
        with pytest.raises(FileExistsError):
            evidence.write("source.bin", b"replacement")
        assert (evidence.path / "source.bin").read_bytes() == b"first"
    finally:
        evidence.close()


@pytest.mark.parametrize("close_failure", ["none", "child", "parent", "both"])
def test_evidence_constructor_fsync_failure_closes_once(
    tmp_path, monkeypatch, close_failure
):
    parent_fd = os.open(tmp_path, os.O_RDONLY | os.O_DIRECTORY)
    original_open, original_close = os.open, os.close
    child_fds, closes = [], []
    primary = OSError("injected parent fsync failure")
    evidence = A.Evidence.__new__(A.Evidence)

    def open_child(*args, **kwargs):
        fd = original_open(*args, **kwargs)
        child_fds.append(fd)
        return fd

    def fail_fsync(fd):
        assert fd == parent_fd
        raise primary

    def close_once(fd):
        closes.append(fd)
        original_close(fd)
        kind = "parent" if fd == parent_fd else "child"
        if close_failure in (kind, "both"):
            raise OSError("injected close failure")

    with monkeypatch.context() as patch:
        patch.setattr(R, "durable_journal_directory", lambda path: parent_fd)
        patch.setattr(A.os, "open", open_child)
        patch.setattr(A.os, "fsync", fail_fsync)
        patch.setattr(A.os, "close", close_once)
        with pytest.raises(OSError) as caught:
            evidence.__init__(tmp_path)
        assert caught.value is primary
        assert len(child_fds) == 1
        assert closes == [child_fds[0], parent_fd]
        assert evidence.fd is None
        evidence.close()
        with pytest.raises(R.Refused, match="closed"):
            evidence.write("forbidden.bin", b"must not be written")
        assert closes == [child_fds[0], parent_fd]
        assert len(child_fds) == 1
    closed_errors = []
    for fd in (child_fds[0], parent_fd):
        with pytest.raises(OSError) as caught:
            os.fstat(fd)
        closed_errors.append(caught.value.errno)
    assert closed_errors == [9, 9]
    (tmp_path / "close-observations.json").write_bytes(
        R.encoded(
            {
                "fault": close_failure,
                "primary_preserved": True,
                "closed_once": closes,
                "child_fd": child_fds[0],
                "parent_fd": parent_fd,
                "fstat_errno": closed_errors,
                "closed_state": evidence.fd is None,
                "write_refused": True,
            }
        )
    )


def shorten_deadline(monkeypatch):
    timeout_at = A.asyncio.timeout_at
    monkeypatch.setattr(
        A.asyncio,
        "timeout_at",
        lambda deadline: timeout_at(
            min(deadline, asyncio.get_running_loop().time() + 0.01)
        ),
    )


def test_acquisition_deadline_cancels_callback_before_intent(setup, monkeypatch):
    adapter, _ = setup
    cancelled = []

    async def blocked(nonce, deadline):
        try:
            await asyncio.sleep(10)
        finally:
            cancelled.append(True)

    adapter.callbacks["process"] = blocked
    shorten_deadline(monkeypatch)
    with pytest.raises(TimeoutError):
        adapter.dispatch("verify", armed=True)
    assert cancelled == [True] and not adapter.journal.exists()
    assert (
        json.loads((adapter.last_evidence / "failure.json").read_bytes())["type"]
        == "TimeoutError"
    )


def test_exit_deadline_preserves_output_and_consumes_intent(setup, monkeypatch):
    adapter, _ = setup
    writers = socket_fixture(monkeypatch)

    async def blocked(*args):
        await asyncio.sleep(10)

    adapter.exit_observer = blocked
    shorten_deadline(monkeypatch)
    with pytest.raises(TimeoutError):
        adapter.dispatch("verify", armed=True)
    assert writers[0].closed and writers[0].aborted
    assert (adapter.last_evidence / "stdout.bin").read_bytes() == output("verify")
    assert (adapter.journal / "verify.intent.json").exists()
    assert not (adapter.journal / "verify.completed.json").exists()


def test_post_rest_baseline_cannot_establish_continuity(setup):
    adapter, _ = setup
    baseline = json.loads(adapter.baseline)
    baseline["observed_at"] = A.REST_STARTED_AT
    adapter.baseline = R.encoded(baseline)
    adapter.clock = lambda: A.REST_STARTED_AT + 1
    with pytest.raises(R.Refused, match="pre-rest"):
        adapter.dispatch("verify", armed=True)
    assert not adapter.journal.exists()


def test_discovery_fixed_socket_and_wrong_peer(monkeypatch):
    import socket

    events = []

    class Datagram:
        def __enter__(self):
            return self

        def __exit__(self, *_):
            events.append("closed")

        def setblocking(self, enabled):
            assert enabled is False

        def bind(self, address):
            assert address == (A.LOCAL, 0)

    async def scenario():
        loop = asyncio.get_running_loop()

        async def send(sock, raw, target):
            assert raw == A.DISCOVERY_REQUEST and target == (R.CONSOLE, 9302)
            events.append("sent")
            return len(raw)

        async def receive(sock, limit):
            assert limit == 4097
            return READY, ("192.168.2.3", 9302)

        monkeypatch.setattr(loop, "sock_sendto", send)
        monkeypatch.setattr(loop, "sock_recvfrom", receive)
        monkeypatch.setattr(socket, "socket", lambda *args: Datagram())
        with pytest.raises(R.Refused, match="peer"):
            await A.discover(loop.time() + 1)

    asyncio.run(scenario())
    assert events == ["sent", "closed"]
