"""Synthetic coordinator tests; no console or capture device is contacted."""

import asyncio
import hashlib
import importlib.util
import json
import os
import signal
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "input_driver", ROOT / "tools/ps5-probes/input_interactive_driver.py"
)
D = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = D
SPEC.loader.exec_module(D)
ELF = ROOT / ".omo/evidence/t042-interactive-helper-20261031/input-interactive.elf"
READY = (
    b"READY version=1 pid=123 firmware=0x13600007 "
    b"neutral=accepted next=1 delivery=unverified\n"
)
ACK = b"ACK seq=1 mask=0x0000 neutral=accepted begin_ms=10 release_ms=11 end_ms=12\n"
DONE = (
    b"DONE code=0 owned=1 neutral_rc=0 remove_rc=0 close_rc=0 "
    b"terminate_rc=0 hardware_acceptance=incomplete\n"
)


@pytest.fixture(autouse=True)
def synthetic_binary(tmp_path, monkeypatch):
    """Portable CI has no retained PS5 binaries; this is an explicit fixture pin."""
    path = tmp_path / "fixture.elf"
    data = b"synthetic ELF fixture; never deploy"
    path.write_bytes(data)
    monkeypatch.setattr(sys.modules[__name__], "ELF", path)
    monkeypatch.setattr(D, "ELF_SHA256", hashlib.sha256(data).hexdigest())


class FakeTransport:
    def __init__(self, responses=None):
        self.responses = list(responses or [READY, ACK, DONE, b""])
        self.sent = []
        self.closed = False

    async def read(self, size):
        return self.responses.pop(0) if self.responses else b""

    async def send(self, data):
        self.sent.append(data)

    async def wait(self):
        return 0

    async def finish(self):
        return None

    async def close(self):
        self.closed = True
        return {"reaped": True, "returncode": 0}


class FakeCapture:
    def __init__(self):
        self.started = None
        self.closed = False

    async def start(self):
        self.started = time.monotonic_ns()

    async def segment(self, after_ns):
        await asyncio.sleep(0.001)
        end = time.monotonic_ns()
        segment = D.Segment(
            self.started, end, b"synthetic video", b'{"frames":1,"decode_exit":0}'
        )
        self.started = end
        return segment

    async def check(self):
        return None

    async def close(self):
        self.closed = True
        return {"reaped": True, "returncode": 0}


def preflight():
    return {
        "schema_version": 1,
        "observed_ns": time.monotonic_ns(),
        "firmware": "0x13600007",
        "t050_cleanup_complete": True,
        "no_active_game_or_probe": True,
        "ipv4_ipv6_isolated": True,
        "exclusive_capture": True,
        "sources": {"inspection.txt": b"synthetic preflight"},
    }


async def postflight(pid):
    return {
        "pid": pid,
        "observed_ns": time.monotonic_ns(),
        "process_absent": True,
        "services_healthy": True,
        "sources": {"inspection.txt": b"synthetic independent postflight"},
    }


def execute(tmp_path, transport=None, capture=None, observation=None, **kwargs):
    tmp_path.mkdir(parents=True, exist_ok=True)
    transport = transport or FakeTransport()
    capture = capture or FakeCapture()
    launches = []

    async def launch(path):
        launches.append(path)
        assert path.read_bytes() == ELF.read_bytes()
        return transport

    async def scenario():
        return await D.run(
            tmp_path / "run",
            ELF,
            observation or preflight(),
            capture,
            launch,
            postflight,
            synthetic=True,
            **kwargs,
        )

    result = asyncio.run(scenario())
    return result, transport, capture, launches


def test_neutral_stop_and_manifest(tmp_path):
    result, transport, capture, launches = execute(tmp_path)
    assert result["protocol_complete"] is True
    assert result["hardware_acceptance"] == "Incomplete"
    assert result["physical_removal"] == "unverified"
    assert transport.sent == [b"1 neutral\n", b"2 stop\n"]
    assert len(launches) == 1 and transport.closed and capture.closed
    assert json.loads((tmp_path / "run/manifest.json").read_text()) == result
    assert result["capture"][0]["end_ns"] <= result["dispatch_ns"]
    assert result["capture"][-1]["end_ns"] > result["postflight_ns"]


@pytest.mark.parametrize(
    "responses",
    [
        [READY[:10], READY[10:], ACK[:3], ACK[3:], DONE, b""],
        [READY, ACK, DONE[:40], DONE[40:], b""],
    ],
)
def test_fragmented_input(tmp_path, responses):
    assert execute(tmp_path, FakeTransport(responses))[0]["protocol_complete"]


@pytest.mark.parametrize(
    "responses",
    [
        [READY + READY],
        [ACK],
        [READY.replace(b"13600007", b"13500007")],
        [READY.replace(b"pid=123", b"pid=0")],
        [READY.replace(b"version=1", b"version=2")],
        [READY, ACK.replace(b"seq=1", b"seq=2")],
        [READY, ACK.replace(b"mask=0x0000", b"mask=0x0040")],
        [READY, ACK.replace(b"release_ms=11", b"release_ms=9")],
        [READY, ACK, DONE.replace(b"remove_rc=0", b"remove_rc=-1")],
        [READY, ACK, b""],
        [READY, ACK, DONE + READY],
        [READY, ACK, DONE, b"extra\n"],
        [b"x" * 513],
    ],
)
def test_protocol_refusal(tmp_path, responses):
    result, transport, capture, launches = execute(tmp_path, FakeTransport(responses))
    assert not result["protocol_complete"]
    assert not (tmp_path / "run/manifest.json").exists()
    assert (tmp_path / "run/incomplete.json").exists()
    assert transport.closed and capture.closed and len(launches) == 1


@pytest.mark.parametrize(
    "field,value",
    [
        ("t050_cleanup_complete", False),
        ("no_active_game_or_probe", False),
        ("ipv4_ipv6_isolated", False),
        ("exclusive_capture", False),
        ("firmware", "0x13500007"),
        ("observed_ns", 0),
        ("sources", {}),
    ],
)
def test_preflight_refuses_before_dispatch(tmp_path, field, value):
    observation = preflight()
    observation[field] = value
    result, _, capture, launches = execute(tmp_path, observation=observation)
    assert not result["protocol_complete"] and not launches and capture.closed
    assert not (tmp_path / "run/dispatch-intent.json").exists()


def test_hash_and_disabled_execution(tmp_path, monkeypatch):
    monkeypatch.setattr(D, "ELF_SHA256", "0" * 64)
    assert not execute(tmp_path)[3]
    with pytest.raises(ValueError, match="disabled"):
        asyncio.run(D.run(tmp_path / "disabled", ELF, preflight(), None, None, None))
    assert not (tmp_path / "disabled").exists()


def test_capture_failure_before_dispatch(tmp_path):
    class Broken(FakeCapture):
        async def segment(self, after_ns):
            raise RuntimeError("capture lost")

    result, _, capture, launches = execute(tmp_path, capture=Broken())
    assert not result["protocol_complete"] and not launches and capture.closed


def test_capture_failure_during_stalled_transport(tmp_path):
    class Stalled(FakeTransport):
        async def read(self, size):
            await asyncio.sleep(10)

    class Broken(FakeCapture):
        async def check(self):
            if self.started and time.monotonic_ns() - self.started > 10_000_000:
                raise RuntimeError("capture lost")

    started = time.monotonic()
    result, transport, capture, launches = execute(
        tmp_path, Stalled(), Broken(), total_seconds=1
    )
    assert "capture lost" in result["error"]
    assert time.monotonic() - started < 1
    assert transport.closed and capture.closed and len(launches) == 1


def test_timeout_output_quota_and_existing_output(tmp_path):
    class Stalled(FakeTransport):
        async def read(self, size):
            await asyncio.sleep(10)

    result, transport, capture, _ = execute(tmp_path, Stalled(), total_seconds=0.02)
    assert "TimeoutError" in result["error"] and transport.closed and capture.closed
    with pytest.raises(FileExistsError):
        execute(tmp_path)
    result, transport, _, _ = execute(tmp_path / "quota", output_limit=1)
    assert "output quota" in result["error"] and not transport.sent


@pytest.mark.parametrize("fault", ["decode", "stale", "gap", "postflight"])
def test_capture_coverage_refusal(tmp_path, fault):
    class Broken(FakeCapture):
        async def segment(self, after_ns):
            item = await super().segment(after_ns)
            if fault == "decode":
                return D.Segment(
                    item.start_ns,
                    item.end_ns,
                    item.data,
                    b'{"frames":1,"decode_exit":1}',
                )
            if fault == "stale":
                return D.Segment(0, 1, item.data, item.decode)
            if fault == "gap" and after_ns > item.start_ns:
                return D.Segment(item.start_ns + 1, item.end_ns, item.data, item.decode)
            if fault == "postflight" and after_ns > item.start_ns:
                raise RuntimeError("postflight capture failed")
            return item

    result, _, capture, _ = execute(tmp_path, capture=Broken())
    assert not result["protocol_complete"] and capture.closed


def test_no_manifest_when_publication_or_quota_fails(tmp_path, monkeypatch):
    def fail(self, manifest):
        raise OSError("publication failed")

    monkeypatch.setattr(D.Artifacts, "publish", fail)
    result = execute(tmp_path)[0]
    assert not result["protocol_complete"] and "publication_error" in result
    assert not (tmp_path / "run/manifest.json").exists()
    result, _, _, launches = execute(tmp_path / "quota", artifact_quota=1)
    assert not result["protocol_complete"] and not launches


def test_real_subprocess_protocol_and_reaping(tmp_path):
    script = (
        "import sys\n"
        f"sys.stdout.buffer.write({READY!r});sys.stdout.flush()\n"
        "assert sys.stdin.readline() == '1 neutral\\n'\n"
        f"sys.stdout.buffer.write({ACK!r});sys.stdout.flush()\n"
        "assert sys.stdin.readline() == '2 stop\\n'\n"
        f"sys.stdout.buffer.write({DONE!r});sys.stdout.flush()\n"
        "assert sys.stdin.read() == ''\n"
    )
    children = []

    async def launch(path):
        transport = await D.ProcessTransport.spawn([sys.executable, "-c", script])
        children.append(transport.process)
        return transport

    async def scenario():
        return await D.run(
            tmp_path / "run",
            ELF,
            preflight(),
            FakeCapture(),
            launch,
            postflight,
            synthetic=True,
        )

    result = asyncio.run(scenario())
    assert result["protocol_complete"] and children[0].returncode == 0
    with pytest.raises(ProcessLookupError):
        os.kill(children[0].pid, 0)


@pytest.mark.parametrize("status", [0, 3])
def test_close_never_signals_known_exited_child(tmp_path, monkeypatch, status):
    signals = []
    monkeypatch.setattr(D.os, "killpg", lambda pid, sig: signals.append((pid, sig)))

    async def scenario():
        transport = await D.ProcessTransport.spawn(
            [sys.executable, "-c", f"raise SystemExit({status})"]
        )
        assert await transport.wait() == status
        with pytest.raises(ProcessLookupError):
            os.kill(transport.process.pid, 0)
        result = await transport.close()
        (tmp_path / "cleanup.json").write_text(
            json.dumps({"pid": transport.process.pid, "signals": signals, **result})
        )
        assert signals == []
        assert result["reaped"] and result["returncode"] == status

    asyncio.run(scenario())


def test_close_stops_escalation_after_term_exit(tmp_path, monkeypatch):
    signals = []
    original = os.killpg

    async def scenario():
        transport = await D.ProcessTransport.spawn(
            [
                sys.executable,
                "-c",
                (
                    "import signal,time;"
                    "signal.signal(signal.SIGTERM,lambda *_: exit(0));"
                    "print('ready',flush=True);time.sleep(30)"
                ),
            ]
        )
        assert await transport.read(100) == b"ready\n"

        def record(pid, sig):
            signals.append((pid, sig))
            # The pre-fix regression records the unsafe attempt without sending it.
            if transport.process.returncode is None:
                original(pid, sig)

        monkeypatch.setattr(D.os, "killpg", record)
        started = time.monotonic()
        result = await transport.close()
        (tmp_path / "cleanup.json").write_text(
            json.dumps({"pid": transport.process.pid, "signals": signals, **result})
        )
        assert time.monotonic() - started < 3
        assert signals == [(transport.process.pid, signal.SIGTERM)]
        assert result["reaped"] and result["returncode"] == 0
        with pytest.raises(ProcessLookupError):
            os.kill(transport.process.pid, 0)

    asyncio.run(scenario())


def test_real_subprocess_kill_fallback(tmp_path, monkeypatch):
    signals = []
    original = os.killpg

    async def scenario():
        transport = await D.ProcessTransport.spawn(
            [
                sys.executable,
                "-c",
                (
                    "import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);"
                    "print('ready',flush=True);time.sleep(30)"
                ),
            ]
        )
        assert await transport.read(100) == b"ready\n"

        def record(pid, sig):
            assert transport.process.returncode is None
            signals.append((pid, sig))
            original(pid, sig)

        monkeypatch.setattr(D.os, "killpg", record)
        started = time.monotonic()
        result = await transport.close()
        assert time.monotonic() - started < 3
        assert signals == [
            (transport.process.pid, signal.SIGTERM),
            (transport.process.pid, signal.SIGKILL),
        ]
        assert result["reaped"] and result["returncode"] == -signal.SIGKILL
        (tmp_path / "cleanup.json").write_text(
            json.dumps(
                {
                    "pid": transport.process.pid,
                    "signals": signals,
                    **result,
                }
            )
        )
        with pytest.raises(ProcessLookupError):
            os.kill(transport.process.pid, 0)

    asyncio.run(scenario())


def test_cancellation_closes_owned_resources(tmp_path):
    class Stalled(FakeTransport):
        async def read(self, size):
            await asyncio.sleep(10)

    transport = Stalled()
    capture = FakeCapture()
    launched = asyncio.Event()

    async def launch(path):
        launched.set()
        return transport

    async def scenario():
        task = asyncio.create_task(
            D.run(
                tmp_path / "run",
                ELF,
                preflight(),
                capture,
                launch,
                postflight,
                synthetic=True,
            )
        )
        await launched.wait()
        task.cancel()
        return await task

    result = asyncio.run(scenario())
    assert "CancelledError" in result["error"]
    assert transport.closed and capture.closed and not transport.sent


def test_uncertain_send_is_never_repeated(tmp_path):
    class Broken(FakeTransport):
        async def send(self, data):
            self.sent.append(data)
            raise ConnectionError("uncertain write")

    result, transport, _, launches = execute(tmp_path, Broken())
    assert not result["protocol_complete"]
    assert transport.sent == [b"1 neutral\n"] and len(launches) == 1
    assert (tmp_path / "run/neutral-intent.json").exists()


@pytest.mark.parametrize("kind", ["exit", "postflight", "reap", "capture_exit"])
def test_final_verification_failures(tmp_path, monkeypatch, kind):
    transport = FakeTransport()
    capture = FakeCapture()

    async def bad_exit():
        return 1

    async def bad_postflight(pid):
        value = await postflight_original(pid)
        value["process_absent"] = False
        return value

    async def bad_reap():
        return {"reaped": False, "returncode": None}

    async def bad_capture_exit():
        return {"reaped": True, "returncode": 1}

    postflight_original = postflight
    if kind == "exit":
        transport.wait = bad_exit
    elif kind == "postflight":
        monkeypatch.setattr(sys.modules[__name__], "postflight", bad_postflight)
    elif kind == "reap":
        transport.close = bad_reap
    else:
        capture.close = bad_capture_exit
    result = execute(tmp_path, transport, capture)[0]
    assert not result["protocol_complete"]
    assert not (tmp_path / "run/manifest.json").exists()


def test_manifest_directory_sync_failure_removes_success(tmp_path, monkeypatch):
    original = D.Artifacts.sync_directory

    def fail_final(self):
        if (self.root / "manifest.json").exists():
            raise OSError("directory sync failed")
        original(self)

    monkeypatch.setattr(D.Artifacts, "sync_directory", fail_final)
    result = execute(tmp_path)[0]
    assert not result["protocol_complete"] and "publication_error" in result
    assert not (tmp_path / "run/manifest.json").exists()


def test_manifest_checksums_and_order(tmp_path):
    result = execute(tmp_path)[0]
    for ref in result["artifacts"]:
        assert (
            hashlib.sha256((tmp_path / "run" / ref["path"]).read_bytes()).hexdigest()
            == ref["sha256"]
        )
    assert (tmp_path / "run/manifest.json").stat().st_mtime_ns >= max(
        (tmp_path / "run" / ref["path"]).stat().st_mtime_ns
        for ref in result["artifacts"]
    )


def test_cancel_during_spawn_recovers_child(tmp_path, monkeypatch):
    original = asyncio.create_subprocess_exec
    created = asyncio.Event()
    children = []

    async def delayed(*args, **kwargs):
        child = await original(*args, **kwargs)
        children.append(child)
        created.set()
        await asyncio.sleep(0.02)
        return child

    monkeypatch.setattr(asyncio, "create_subprocess_exec", delayed)

    async def scenario():
        task = asyncio.create_task(
            D.ProcessTransport.spawn(
                [
                    sys.executable,
                    "-c",
                    "import time;time.sleep(30)",
                ]
            )
        )
        await created.wait()
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert children[0].returncode is not None
        with pytest.raises(ProcessLookupError):
            os.kill(children[0].pid, 0)
        (tmp_path / "cleanup.json").write_text(
            json.dumps(
                {
                    "pid": children[0].pid,
                    "returncode": children[0].returncode,
                    "reaped": True,
                }
            )
        )

    asyncio.run(scenario())


def test_parent_sync_failure_prevents_capture_and_launch(tmp_path, monkeypatch):
    parent_identity = (tmp_path.stat().st_dev, tmp_path.stat().st_ino)
    original = os.fsync
    capture = FakeCapture()
    transport = FakeTransport()
    syncs = []

    def reject_parent(fd):
        info = os.fstat(fd)
        identity = (info.st_dev, info.st_ino)
        syncs.append(identity)
        if identity == parent_identity and (tmp_path / "run").exists():
            raise OSError("injected parent fsync failure")
        return original(fd)

    monkeypatch.setattr(os, "fsync", reject_parent)
    with pytest.raises(OSError, match="parent fsync"):
        execute(tmp_path, transport, capture)
    assert capture.started is None and not transport.sent
    assert not (tmp_path / "run/dispatch-intent.json").exists()
    monkeypatch.setattr(os, "fsync", original)
    with pytest.raises(FileExistsError):
        execute(tmp_path, transport, capture)
    (tmp_path / "directory-result.json").write_text(
        json.dumps(
            {
                "parent_identity": parent_identity,
                "syncs": syncs,
                "capture_started": False,
                "dispatch_intent_exists": False,
            }
        )
    )


def test_parent_sync_precedes_capture_and_dispatch(tmp_path, monkeypatch):
    parent_identity = (tmp_path.stat().st_dev, tmp_path.stat().st_ino)
    original = os.fsync
    events = []

    def record(fd):
        info = os.fstat(fd)
        if (info.st_dev, info.st_ino) == parent_identity and (
            tmp_path / "run"
        ).exists():
            events.append("parent_synced_after_mkdir")
        return original(fd)

    class Capture(FakeCapture):
        async def start(self):
            assert events == ["parent_synced_after_mkdir"]
            events.append("capture_started")
            await super().start()

    monkeypatch.setattr(os, "fsync", record)
    result, _, _, launches = execute(tmp_path, capture=Capture())
    assert result["protocol_complete"] and len(launches) == 1
    assert events[:2] == ["parent_synced_after_mkdir", "capture_started"]
    (tmp_path / "directory-result.json").write_text(json.dumps(events))


@pytest.mark.parametrize(
    "kind", ["parent_symlink", "root_symlink", "unsafe_parent", "traversal"]
)
def test_unsafe_output_refused_before_capture(tmp_path, kind):
    capture = FakeCapture()
    transport = FakeTransport()
    destination = tmp_path / "destination"
    destination.mkdir(mode=0o700)
    if kind == "parent_symlink":
        (tmp_path / "alias").symlink_to(destination, target_is_directory=True)
        output = tmp_path / "alias/run"
    elif kind == "root_symlink":
        (tmp_path / "alias").symlink_to(destination, target_is_directory=True)
        output = tmp_path / "alias"
    elif kind == "unsafe_parent":
        destination.chmod(0o777)
        output = destination / "run"
    else:
        output = destination / "../run"

    async def launch(path):
        raise AssertionError("unsafe output must never reach launch")

    with pytest.raises((OSError, ValueError)):
        asyncio.run(
            D.run(output, ELF, preflight(), capture, launch, postflight, synthetic=True)
        )
    assert capture.started is None and not transport.sent
    destination.chmod(0o700)


def test_created_directory_permissions_checked_and_fds_closed(tmp_path, monkeypatch):
    import fcntl

    original_mkdir = os.mkdir
    original_open = os.open
    opened = []

    def unsafe_mkdir(path, mode=0o777, *, dir_fd=None):
        original_mkdir(path, mode=mode, dir_fd=dir_fd)
        if path == "run":
            os.chmod(path, 0o755, dir_fd=dir_fd)

    def track_open(*args, **kwargs):
        fd = original_open(*args, **kwargs)
        opened.append(fd)
        return fd

    monkeypatch.setattr(os, "mkdir", unsafe_mkdir)
    monkeypatch.setattr(os, "open", track_open)
    with pytest.raises(ValueError, match="unsafe output directory"):
        D.Artifacts(tmp_path / "run", 1000)
    for fd in set(opened):
        with pytest.raises(OSError):
            fcntl.fcntl(fd, fcntl.F_GETFD)
    (tmp_path / "directory-result.json").write_text(
        json.dumps(
            {
                "unsafe_mode_refused": True,
                "opened_fds_closed": True,
            }
        )
    )
