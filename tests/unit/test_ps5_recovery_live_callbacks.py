"""Local TCP/process fixtures only; no console endpoints are contacted."""

import asyncio
import importlib.util
import json
import os
import sys
from contextlib import asynccontextmanager
from pathlib import Path

import pytest

PROBES = Path(__file__).resolve().parents[2] / "tools/ps5-probes"
sys.path.insert(0, str(PROBES))
SPEC = importlib.util.spec_from_file_location(
    "recovery_live_callbacks", PROBES / "recovery_live_callbacks.py"
)
L = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(L)


class Evidence:
    def __init__(self, root):
        self.root = root

    def write(self, name, raw):
        (self.root / name).write_bytes(raw)


@asynccontextmanager
async def fixture_connection(monkeypatch, handler):
    finished = asyncio.Event()

    async def serve(reader, writer):
        try:
            await handler(reader, writer)
        finally:
            writer.close()
            await writer.wait_closed()
            finished.set()

    server = await asyncio.start_server(serve, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    original = L.connection

    @asynccontextmanager
    async def local(host, requested_port, deadline):
        async with original("127.0.0.1", port, deadline) as stream:
            yield stream

    monkeypatch.setattr(L, "connection", local)
    try:
        yield
    finally:
        server.close()
        await server.wait_closed()
        await asyncio.wait_for(finished.wait(), 1)


def test_disabled_and_missing_proofs(tmp_path):
    with pytest.raises(L.R.Refused):
        L.LiveComponents(Evidence(tmp_path))
    live = L.LiveComponents(Evidence(tmp_path), enabled=True)
    assert all(callback is L.unavailable for callback in live.callbacks().values())
    for callback in [*live.callbacks().values(), live.exit_observer]:
        with pytest.raises(L.R.Refused):
            asyncio.run(callback("nonce", 0))
    with pytest.raises(L.R.Refused):
        L.LiveComponents(Evidence(tmp_path), enabled=True, classifier=lambda: True)


@pytest.mark.parametrize(
    "reply,accepted",
    [
        (b"+$PacketSize=4000#f4", True),
        (b"+$OK#9a", True),
        (b"+$OK#00", False),
        (b"+$OK#", False),
        (b"fake", False),
    ],
)
def test_rsp_partial_checksum(monkeypatch, tmp_path, reply, accepted):
    async def scenario():
        async def handler(reader, writer):
            assert await reader.readexactly(14) == b"$qSupported#37"
            for byte in reply:
                writer.write(bytes([byte]))
                await writer.drain()
            if accepted:
                assert await reader.readexactly(1) == b"+"

        async with fixture_connection(monkeypatch, handler):
            live = L.LiveComponents(Evidence(tmp_path), enabled=True)
            if accepted:
                assert await live.service_observation(
                    "gdbsrv", asyncio.get_running_loop().time() + 1
                ) == {"protocol_observed": True, "verified": False}
            else:
                with pytest.raises((L.R.Refused, asyncio.IncompleteReadError)):
                    await live.service_observation(
                        "gdbsrv", asyncio.get_running_loop().time() + 1
                    )

    asyncio.run(scenario())


@pytest.mark.parametrize("cancel", [False, True])
def test_socket_deadline_and_cancellation_cleanup(monkeypatch, tmp_path, cancel):
    async def scenario():
        async def handler(reader, writer):
            assert await reader.read() == b""

        async with fixture_connection(monkeypatch, handler):
            live = L.LiveComponents(Evidence(tmp_path), enabled=True)
            task = asyncio.create_task(
                live.service_observation(
                    "klogsrv",
                    asyncio.get_running_loop().time() + (1 if cancel else 0.03),
                )
            )
            if cancel:
                await asyncio.sleep(0.02)
                task.cancel()
            with pytest.raises(asyncio.CancelledError if cancel else TimeoutError):
                await task

    asyncio.run(scenario())


@pytest.mark.parametrize("spoof", [False, True])
def test_interactive_shell(monkeypatch, tmp_path, spoof):
    async def scenario():
        async def handler(reader, writer):
            writer.write(b"/$ ")
            await writer.drain()
            assert await reader.readline() == b"help\n"
            writer.write(b"help pwd exit\n/$ ")
            await writer.drain()
            assert await reader.readline() == b"pwd\n"
            writer.write(b"fake/$ " if spoof else b"/\n/$ ")
            await writer.drain()
            assert await reader.readline() == (b"" if spoof else b"exit\n")

        async with fixture_connection(monkeypatch, handler):
            live = L.LiveComponents(Evidence(tmp_path), enabled=True)
            if spoof:
                with pytest.raises(L.R.Refused):
                    await live.service_observation(
                        "shsrv", asyncio.get_running_loop().time() + 1
                    )
            else:
                assert not (
                    await live.service_observation(
                        "shsrv", asyncio.get_running_loop().time() + 1
                    )
                )["verified"]

    asyncio.run(scenario())


@pytest.mark.parametrize(
    "body,accepted",
    [(b"kernel: fixture line\n", True), (b"\n", False), (b"a" * 8193, False)],
)
def test_klog_quota(monkeypatch, tmp_path, body, accepted):
    async def scenario():
        async def handler(reader, writer):
            writer.write(body)
            await writer.drain()

        async with fixture_connection(monkeypatch, handler):
            live = L.LiveComponents(Evidence(tmp_path), enabled=True)
            if accepted:
                assert not (
                    await live.service_observation(
                        "klogsrv", asyncio.get_running_loop().time() + 1
                    )
                )["verified"]
            else:
                with pytest.raises(L.R.Refused):
                    await live.service_observation(
                        "klogsrv", asyncio.get_running_loop().time() + 1
                    )

    asyncio.run(scenario())


@pytest.mark.parametrize(
    "headers",
    [
        b"Content-Length: 2\r\n",
        b"Content-Length: 2\r\nContent-Length: 2\r\n",
        b"Transfer-Encoding: chunked\r\nContent-Length: 2\r\n",
        b"Content-Length: 999999\r\n",
    ],
)
def test_http_bounds(monkeypatch, headers):
    async def scenario():
        async def handler(reader, writer):
            assert (await reader.readuntil(b"\r\n\r\n")).startswith(b"GET /shot ")
            writer.write(b"HTTP/1.1 200 OK\r\n" + headers + b"\r\nok")
            await writer.drain()

        async with fixture_connection(monkeypatch, handler):
            if headers == b"Content-Length: 2\r\n":
                assert (
                    await L.http(
                        "127.0.0.1",
                        8765,
                        "/shot",
                        asyncio.get_running_loop().time() + 1,
                    )
                )[1] == b"ok"
            else:
                with pytest.raises(L.R.Refused):
                    await L.http(
                        "127.0.0.1",
                        8765,
                        "/shot",
                        asyncio.get_running_loop().time() + 1,
                    )

    asyncio.run(scenario())


@pytest.mark.parametrize(
    "stale,classifies", [(True, True), (False, False), (False, True)]
)
def test_fresh_camera_requires_classifier(monkeypatch, tmp_path, stale, classifies):
    camera = tmp_path / "screen.png"
    camera.write_bytes(b"\x89PNG\r\n\x1a\nfixture")
    monkeypatch.setattr(L, "CAMERA", camera)

    async def shot(*args):
        if not stale:
            replacement = tmp_path / "replacement"
            replacement.write_bytes(camera.read_bytes())
            os.replace(replacement, camera)
        return b"fixture CamShot", b"ok\ndevice=Elgato 4K X\n"

    async def classifier(image, device, deadline):
        return classifies

    monkeypatch.setattr(L, "http", shot)
    live = L.LiveComponents(
        Evidence(tmp_path),
        enabled=True,
        classifier=classifier,
        classifier_review=L.hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    )

    async def scenario():
        started = L.time.time()
        if stale or not classifies:
            with pytest.raises(L.R.Refused):
                await live.hdmi("nonce", asyncio.get_running_loop().time() + 1)
        else:
            raw = await live.hdmi("nonce", asyncio.get_running_loop().time() + 1)
            assert L.A.envelope(raw, "nonce", started, L.time.time())["healthy"] is True
            assert json.loads(raw)["nonce"] == "nonce"

    asyncio.run(scenario())


def test_route_ambiguity():
    interface = b"inet 192.168.2.1 netmask 0xffffff00\nstatus: active\n"
    route = b"interface: en9\ndestination: 192.168.2.2\nflags: <UP,HOST>\n"
    result = L.admit_host_route(interface, route, b"0\n0\n")
    assert result["host_conditions_observed"] and not result["exclusive"]
    for iface, rt, forwarding in [
        (interface + b"inet 10.0.0.1 netmask 0xffffff00", route, b"0\n0\n"),
        (interface, route + b"interface: en0\n", b"0\n0\n"),
        (interface, route, b"1\n0\n"),
        (interface + b"inet6 fe80::1\n", route, b"0\n0\n"),
    ]:
        with pytest.raises(L.R.Refused):
            L.admit_host_route(iface, rt, forwarding)


@pytest.mark.parametrize("mode", ["timeout", "quota", "cancel"])
def test_subprocess_cleanup(tmp_path, mode):
    pidfile = tmp_path / "pid"
    code = (
        "import os,time,pathlib;"
        f"p=pathlib.Path({str(pidfile)!r});t=p.with_suffix('.tmp');"
        "t.write_text(str(os.getpid()));os.replace(t,p);"
        + ("print('x'*10000,flush=True);" if mode == "quota" else "")
        + "time.sleep(60)"
    )

    async def scenario():
        task = asyncio.create_task(
            L.command(
                (sys.executable, "-c", code),
                asyncio.get_running_loop().time() + (0.15 if mode == "timeout" else 2),
                limit=1024,
            )
        )
        if mode == "cancel":
            while not pidfile.exists():
                await asyncio.sleep(0.005)
            task.cancel()
        with pytest.raises(
            {
                "timeout": TimeoutError,
                "quota": L.R.Refused,
                "cancel": asyncio.CancelledError,
            }[mode]
        ):
            await task
        pid = int(pidfile.read_text())
        with pytest.raises(ProcessLookupError):
            os.kill(pid, 0)

    asyncio.run(scenario())


def test_peer_mismatch_closes(monkeypatch):
    class Writer:
        closed = False
        aborted = False

        def get_extra_info(self, name):
            return ("127.0.0.2", 8765)

        def close(self):
            self.closed = True

        def abort(self):
            self.aborted = True

        @property
        def transport(self):
            return self

    writer = Writer()

    async def connect(*args, **kwargs):
        return None, writer

    monkeypatch.setattr(L.asyncio, "open_connection", connect)

    async def scenario():
        with pytest.raises(L.R.Refused):
            async with L.connection(
                "127.0.0.1", 8765, asyncio.get_running_loop().time() + 1
            ):
                pytest.fail("foreign peer admitted")
        assert writer.closed and writer.aborted

    asyncio.run(scenario())


def test_classifier_digest_must_match_source(tmp_path):
    async def classifier(*args):
        return True

    with pytest.raises(L.R.Refused, match="source digest mismatch"):
        L.LiveComponents(
            Evidence(tmp_path),
            enabled=True,
            classifier=classifier,
            classifier_review="a" * 64,
        )


@pytest.mark.parametrize("tail", [b"okEXTRA", b"o"])
def test_http_terminal_framing(monkeypatch, tail):
    async def scenario():
        async def handler(reader, writer):
            await reader.readuntil(b"\r\n\r\n")
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n" + tail)
            await writer.drain()

        async with fixture_connection(monkeypatch, handler):
            with pytest.raises((L.R.Refused, asyncio.IncompleteReadError)):
                await L.http(
                    "127.0.0.1", 8765, "/shot", asyncio.get_running_loop().time() + 1
                )

    asyncio.run(scenario())


@pytest.mark.parametrize("cancel", [False, True])
def test_descendant_cleanup(tmp_path, cancel):
    pidfile = tmp_path / "child-pid"
    code = (
        "import os,time,pathlib\npid=os.fork()\n"
        "if pid:\n"
        f" p=pathlib.Path({str(pidfile)!r})\n t=p.with_suffix('.tmp')\n"
        " t.write_text(str(pid))\n os.replace(t,p)\n"
        "time.sleep(60)"
    )

    async def scenario():
        task = asyncio.create_task(
            L.command(
                (sys.executable, "-c", code),
                asyncio.get_running_loop().time() + (2 if cancel else 0.15),
            )
        )
        if cancel:
            while not pidfile.exists():
                await asyncio.sleep(0.005)
            task.cancel()
        with pytest.raises(asyncio.CancelledError if cancel else TimeoutError):
            await task
        pid = int(pidfile.read_text())
        # The kernel may retain a zombie until launchd reaps it. It must not run.
        proc = await asyncio.create_subprocess_exec(
            "/bin/ps", "-o", "stat=", "-p", str(pid), stdout=asyncio.subprocess.PIPE
        )
        output, _ = await proc.communicate()
        assert not output.strip() or output.strip().startswith(b"Z")

    asyncio.run(scenario())


def test_success_never_signals_reaped_group(monkeypatch):
    def killpg(*args):
        pytest.fail("normal completed process group must not be signalled")

    monkeypatch.setattr(L.os, "killpg", killpg)

    async def scenario():
        assert (
            await L.command(
                (sys.executable, "-c", "print('ok')"),
                asyncio.get_running_loop().time() + 1,
            )
            == b"ok\n"
        )

    asyncio.run(scenario())


def test_missing_classifier_source_refuses(monkeypatch, tmp_path):
    async def classifier(*args):
        return True

    monkeypatch.setattr(L.inspect, "getsourcefile", lambda callback: None)
    with pytest.raises(L.R.Refused, match="source unavailable"):
        L.LiveComponents(
            Evidence(tmp_path),
            enabled=True,
            classifier=classifier,
            classifier_review="a" * 64,
        )


@pytest.mark.parametrize("valid", [False, True])
def test_web_identity_and_file_endpoint(monkeypatch, tmp_path, valid):
    async def scenario():
        async def handler(reader, writer):
            request = await reader.readuntil(b"\r\n\r\n")
            if b"GET /index.html " in request:
                body = (
                    b"<title>Homebrew Launcher</title>" if valid else b"foreign server"
                )
            else:
                assert b"GET /fs/data/ps5-dev/services/?fmt=json " in request
                body = json.dumps(
                    [
                        {"name": f"{n}-ps5.elf", "mode": "-"}
                        for n in ("websrv", "gdbsrv", "klogsrv", "shsrv")
                    ]
                ).encode()
            writer.write(
                b"HTTP/1.1 200 OK\r\nContent-Length: "
                + str(len(body)).encode()
                + b"\r\n\r\n"
                + body
            )
            await writer.drain()

        async with fixture_connection(monkeypatch, handler):
            live = L.LiveComponents(Evidence(tmp_path), enabled=True)
            if valid:
                assert await live.service_observation(
                    "websrv", asyncio.get_running_loop().time() + 1
                ) == {"protocol_observed": True, "verified": False}
            else:
                with pytest.raises(L.R.Refused, match="identity"):
                    await live.service_observation(
                        "websrv", asyncio.get_running_loop().time() + 1
                    )

    asyncio.run(scenario())


def test_loader_never_connects(monkeypatch, tmp_path):
    def connect(*args):
        pytest.fail("loader must not connect")

    monkeypatch.setattr(L, "connection", connect)
    live = L.LiveComponents(Evidence(tmp_path), enabled=True)
    with pytest.raises(L.R.Refused, match="no safe probe"):
        asyncio.run(live.service_observation("loader", 0))


def test_nonzero_exit_never_signals_reaped_group(monkeypatch):
    def killpg(*args):
        pytest.fail("nonzero exited process group must not be signalled")

    monkeypatch.setattr(L.os, "killpg", killpg)

    async def scenario():
        with pytest.raises(L.R.Refused, match="command failed") as caught:
            await L.command(
                (sys.executable, "-c", "raise SystemExit(3)"),
                asyncio.get_running_loop().time() + 1,
            )
        assert (
            "leader exited; descendant cleanup not established"
            in caught.value.__notes__
        )

    asyncio.run(scenario())


def test_signal_failure_still_reaps_preserving_error(monkeypatch, tmp_path):
    pidfile = tmp_path / "pid"
    code = (
        "import os,time,pathlib;"
        f"p=pathlib.Path({str(pidfile)!r});t=p.with_suffix('.tmp');"
        "t.write_text(str(os.getpid()));os.replace(t,p);"
        "print('x'*10000,flush=True);time.sleep(.05)"
    )

    def killpg(*args):
        raise PermissionError("synthetic signal failure")

    monkeypatch.setattr(L.os, "killpg", killpg)

    async def scenario():
        with pytest.raises(L.R.Refused, match="output quota") as caught:
            await L.command(
                (sys.executable, "-c", code),
                asyncio.get_running_loop().time() + 1,
                limit=1024,
            )
        assert any(
            "group cleanup unconfirmed" in note for note in caught.value.__notes__
        )
        with pytest.raises(ProcessLookupError):
            os.kill(int(pidfile.read_text()), 0)

    asyncio.run(scenario())
