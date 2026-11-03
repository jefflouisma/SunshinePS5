"""Opt-in acquisition building blocks, NOT a complete live recovery provider.

Protocol observations are not authenticated service/process identity. The loader
accepts ELF bytes and has no reviewed nondestructive health request. Consequently
services, isolation, boot, process, inventory and exit callbacks always refuse.
Only HDMI can be bound, with an explicitly reviewed asynchronous classifier.
No import-time I/O. All network operations use literal fixed endpoints.
"""

import asyncio
import base64
import hashlib
import inspect
import json
import os
import re
import signal
import stat
import sys
import time
from contextlib import asynccontextmanager
from pathlib import Path

import recovery as R
import recovery_adapter as A

CAMERA = Path("/Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/screen.png")
DEVICES = {"Elgato 4K X", "Cam Link 4K"}
LIMIT = 65536


@asynccontextmanager
async def connection(host, port, deadline):
    """One deadline, exact IPv4 peer and immediate transport cleanup."""
    writer = None
    try:
        async with asyncio.timeout_at(deadline):
            reader, writer = await asyncio.open_connection(
                host,
                port,
                local_addr=(A.LOCAL if host == R.CONSOLE else "127.0.0.1", 0),
                limit=LIMIT,
            )
            R.require(
                writer.get_extra_info("peername") == (host, port), "peer mismatch"
            )
            yield reader, writer
    finally:
        if writer is not None:
            writer.close()
            # No additional unbounded wait after the shared deadline/cancellation.
            writer.transport.abort()


async def until(reader, marker, limit=LIMIT):
    raw = bytearray()
    while not raw.endswith(marker):
        part = await reader.read(1)
        R.require(bool(part), "premature EOF")
        raw.extend(part)
        R.require(len(raw) <= limit, "transcript quota")
    return bytes(raw)


async def http(host, port, path, deadline):
    R.require(
        (host, port, path)
        in {
            ("127.0.0.1", 8765, "/shot"),
            (R.CONSOLE, 8080, "/index.html"),
            (R.CONSOLE, 8080, "/fs/data/ps5-dev/services/?fmt=json"),
        },
        "HTTP endpoint not allowlisted",
    )
    async with connection(host, port, deadline) as (reader, writer):
        writer.write(
            f"GET {path} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n".encode()
        )
        await writer.drain()
        head = await until(reader, b"\r\n\r\n", 8192)
        lines = head.decode("ascii").split("\r\n")
        R.require(re.fullmatch(r"HTTP/1\.[01] 200(?: .*)?", lines[0]), "HTTP status")
        headers = {}
        for line in lines[1:-2]:
            key, sep, value = line.partition(":")
            key = key.lower()
            R.require(sep and key not in headers, "HTTP malformed/duplicate header")
            headers[key] = value.strip()
        size = headers.get("content-length", "")
        R.require(size.isascii() and size.isdecimal(), "HTTP requires content length")
        R.require(
            "transfer-encoding" not in headers and int(size) <= LIMIT,
            "HTTP quota/encoding",
        )
        body = await reader.readexactly(int(size))
        R.require(await reader.read(1) == b"", "HTTP trailing bytes")
        return head + body, body


async def unavailable(*args):
    """Never converts caller-supplied claims or TCP connectivity into truth."""
    raise R.Refused("no reviewed independent live proof provider")


class LiveComponents:
    def __init__(
        self, evidence, *, enabled=False, classifier=None, classifier_review=None
    ):
        R.require(enabled is True, "explicit live acquisition opt-in required")
        R.require(
            classifier is None
            or (
                callable(classifier)
                and isinstance(classifier_review, str)
                and re.fullmatch(r"[0-9a-f]{64}", classifier_review)
            ),
            "classifier requires explicit reviewed source digest",
        )
        self.evidence = evidence
        self.classifier = classifier
        self.classifier_path = None
        if classifier is not None:
            R.require(
                inspect.isfunction(classifier), "classifier must be a reviewed function"
            )
            source = inspect.getsourcefile(classifier)
            R.require(source is not None, "classifier source unavailable")
            self.classifier_path = Path(source)
            R.require(
                hashlib.sha256(
                    R.read_bounded(self.classifier_path, A.SOURCE_LIMIT)
                ).hexdigest()
                == classifier_review,
                "classifier source digest mismatch",
            )
        self.classifier_review = classifier_review
        self.sequence = 0

    def record(self, label, raw):
        self.sequence += 1
        return self.evidence.write(f"live-{self.sequence}-{label}.raw", raw)

    def callbacks(self):
        result = dict.fromkeys(A.COMPONENTS, unavailable)
        if self.classifier is not None:
            result["hdmi"] = self.hdmi
        return result

    exit_observer = staticmethod(unavailable)

    async def service_observation(self, name, deadline):
        """Collect diagnostic bytes; does NOT return an adapter success envelope."""
        R.require(name in R.SERVICES and name != "loader", "loader has no safe probe")
        async with asyncio.timeout_at(deadline):
            if name == "websrv":
                raw, body = await http(R.CONSOLE, 8080, "/index.html", deadline)
                self.record("web-index", raw)
                R.require(
                    b"<title>Homebrew Launcher</title>" in body, "web identity marker"
                )
                raw, body = await http(
                    R.CONSOLE, 8080, "/fs/data/ps5-dev/services/?fmt=json", deadline
                )
                self.record("web-files", raw)
                items = json.loads(body, object_pairs_hook=R.unique_object)
                R.require(isinstance(items, list), "file endpoint format")
                names = {
                    i.get("name")
                    for i in items
                    if isinstance(i, dict) and i.get("mode") == "-"
                }
                R.require(
                    {f"{n}-ps5.elf" for n in ("websrv", "gdbsrv", "klogsrv", "shsrv")}
                    <= names,
                    "file endpoint contents",
                )
                return {"protocol_observed": True, "verified": False}
            async with connection(R.CONSOLE, R.SERVICES[name], deadline) as (
                reader,
                writer,
            ):
                if name == "gdbsrv":
                    payload = b"qSupported"
                    writer.write(b"$" + payload + f"#{sum(payload) % 256:02x}".encode())
                    await writer.drain()
                    prefix = await reader.readexactly(1)
                    if prefix == b"+":
                        prefix = await reader.readexactly(1)
                    R.require(prefix == b"$", "RSP packet start")
                    raw = await until(reader, b"#", 4096)
                    checksum = await reader.readexactly(2)
                    self.record("gdb-rsp", prefix + raw + checksum)
                    R.require(
                        re.fullmatch(b"[0-9a-fA-F]{2}", checksum)
                        and int(checksum, 16) == sum(raw[:-1]) % 256,
                        "RSP checksum",
                    )
                    R.require(
                        raw[:-1] and all(32 <= c < 127 for c in raw[:-1]), "RSP payload"
                    )
                    writer.write(b"+")
                    await writer.drain()
                elif name == "klogsrv":
                    raw = await until(reader, b"\n", 8192)
                    self.record("klog-line", raw)
                    R.require(
                        raw.strip()
                        and all(c in (9, 10, 13) or 32 <= c < 127 for c in raw),
                        "invalid klog line",
                    )
                else:
                    raw = await until(reader, b"/$ ", 4096)
                    self.record("shell-prompt", raw)
                    R.require(raw == b"/$ ", "unexpected shell greeting/prompt")
                    writer.write(b"help\n")
                    await writer.drain()
                    raw = await until(reader, b"/$ ", 16384)
                    self.record("shell-help", raw)
                    R.require(
                        all(
                            re.search(rb"\b" + c + rb"\b", raw[:-3])
                            for c in (b"help", b"pwd", b"exit")
                        ),
                        "shell help response",
                    )
                    writer.write(b"pwd\n")
                    await writer.drain()
                    raw = await until(reader, b"/$ ", 4096)
                    self.record("shell-pwd", raw)
                    R.require(
                        raw in (b"/\n/$ ", b"/\r\n/$ ", b"pwd\r\n/\r\n/$ "),
                        "shell pwd response",
                    )
                    writer.write(b"exit\n")
                    await writer.drain()
                return {"protocol_observed": True, "verified": False}

    async def hdmi(self, nonce, deadline):
        R.require(self.classifier is not None, "reviewed signal classifier missing")
        R.require(
            hashlib.sha256(
                R.read_bounded(self.classifier_path, A.SOURCE_LIMIT)
            ).hexdigest()
            == self.classifier_review,
            "classifier source changed",
        )
        R.require(isinstance(nonce, str) and 0 < len(nonce) <= 256, "nonce")
        async with asyncio.timeout_at(deadline):
            before = snapshot(CAMERA)
            started = time.time_ns()
            raw, body = await http("127.0.0.1", 8765, "/shot", deadline)
            self.record("camshot-response", raw)
            device = body.decode("utf-8").removeprefix("ok\ndevice=").removesuffix("\n")
            R.require(
                device in DEVICES and body == f"ok\ndevice={device}\n".encode(),
                "camera device metadata",
            )
            after = snapshot(CAMERA)
            R.require(
                after[0] != before[0] and after[1] >= started, "stale camera inode/time"
            )
            image = R.read_bounded(CAMERA, 4 * 1024 * 1024)
            R.require(
                snapshot(CAMERA) == after
                and hashlib.sha256(image).hexdigest() == after[3],
                "camera changed during read",
            )
            R.require(image.startswith(b"\x89PNG\r\n\x1a\n"), "camera PNG header")
            self.record("camera", image)
            self.record(
                "camera-freshness",
                R.encoded(
                    {
                        "before": before,
                        "after": after,
                        "device": device,
                        "classifier_review": self.classifier_review,
                    }
                ),
            )
            R.require(
                await self.classifier(image, device, deadline) is True,
                "classifier did not establish healthy HDMI",
            )
            return R.encoded(
                {
                    "nonce": nonce,
                    "console": R.CONSOLE,
                    "host_id": R.HOST_ID,
                    "firmware": R.FIRMWARE,
                    "root": R.REMOTE_ROOT,
                    "observed_at": time.time(),
                    "value": {
                        "healthy": True,
                        "image_base64": base64.b64encode(image).decode(),
                        "image_sha256": hashlib.sha256(image).hexdigest(),
                    },
                }
            )


def snapshot(path):
    """Reject links, stale/replaced reads; retain pre/post inode, time, size, hash."""
    before = os.stat(path, follow_symlinks=False)
    R.require(
        stat.S_ISREG(before.st_mode) and before.st_nlink == 1,
        "camera must be private regular file",
    )
    raw = R.read_bounded(path, 4 * 1024 * 1024)
    after = os.stat(path, follow_symlinks=False)
    R.require(before == after, "camera snapshot raced")
    return (
        before.st_ino,
        before.st_mtime_ns,
        before.st_size,
        hashlib.sha256(raw).hexdigest(),
    )


async def command(argv, deadline, limit=LIMIT):
    """Literal argv and bounded output; best-effort cleanup of a live leader.

    Known-exited leaders are never group-signalled. Descendants after leader
    exit require a separate supervisor; this helper provides no such guarantee.
    """
    process = None
    completed = False
    try:
        async with asyncio.timeout_at(deadline):
            process = await asyncio.create_subprocess_exec(
                *argv,
                stdin=asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.STDOUT,
                env={"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LC_ALL": "C"},
                start_new_session=True,
            )
            data = bytearray()
            while chunk := await process.stdout.read(min(4096, limit + 1)):
                data.extend(chunk)
                R.require(len(data) <= limit, "command output quota")
            status = await process.wait()
            R.require(status == 0, "host diagnostic command failed")
            completed = True
            return bytes(data)
    finally:
        if process is not None and not completed:
            failure = sys.exc_info()[1]
            # asyncio may already have reaped the child, even before wait().
            # Never signal its numeric PGID once that exit is known.
            if process.returncode is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except OSError as error:
                    if failure is not None:
                        failure.add_note(f"group cleanup unconfirmed: {error}")
            elif failure is not None:
                failure.add_note("leader exited; descendant cleanup not established")
            try:
                await asyncio.wait_for(process.wait(), timeout=1)
            except (OSError, TimeoutError) as error:
                if failure is not None:
                    failure.add_note(f"child reap unconfirmed: {error}")


def admit_host_route(interface, route, forwarding):
    """Necessary host conditions only: never asserts console IPv6/exclusivity."""
    addresses = re.findall(rb"^\s*inet (\S+) netmask (\S+)", interface, re.M)
    R.require(addresses == [(b"192.168.2.1", b"0xffffff00")], "host IPv4 ambiguity")
    R.require(
        b"status: active" in interface
        and not re.search(rb"^\s*inet6\s", interface, re.M),
        "host link/IPv6 admission",
    )
    fields = {}
    for line in route.splitlines():
        key, sep, value = line.strip().partition(b":")
        if sep:
            R.require(key not in fields, "ambiguous route fields")
            fields[key] = value.strip()
    R.require(
        fields.get(b"interface") == b"en9"
        and fields.get(b"destination")
        in (b"192.168.2.2", b"192.168.2.0", b"192.168.2"),
        "route not isolated en9",
    )
    R.require(b"GATEWAY" not in fields.get(b"flags", b""), "routed gateway forbidden")
    R.require(
        forwarding.splitlines() == [b"0", b"0"], "host forwarding enabled/unknown"
    )
    return {
        "host_conditions_observed": True,
        "ipv4_isolated": False,
        "ipv6_isolated": False,
        "exclusive": False,
    }


async def host_observation(evidence, deadline, *, enabled=False):
    R.require(enabled is True, "explicit host acquisition opt-in required")
    outputs = []
    for name, argv in (
        ("interface", ("/sbin/ifconfig", "en9")),
        ("route", ("/sbin/route", "-n", "get", R.CONSOLE)),
        (
            "forwarding",
            (
                "/usr/sbin/sysctl",
                "-n",
                "net.inet.ip.forwarding",
                "net.inet6.ip6.forwarding",
            ),
        ),
        ("nat", ("/sbin/pfctl", "-s", "nat")),
    ):
        raw = await command(argv, deadline)
        evidence.write(f"host-{name}.raw", raw)
        outputs.append(raw)
    R.require(not outputs[3].strip(), "NAT rules/diagnostics present; refuse isolation")
    return admit_host_route(*outputs[:3])
