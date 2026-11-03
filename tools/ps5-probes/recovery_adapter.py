"""Opt-in T050 acquisition and fixed-endpoint transfer; no device I/O on import.

Callbacks are reviewed acquisition code, not remote attestation. Every callback
must acquire new evidence within its deadline and echo the request nonce. Missing
bindings refuse before journal creation. TCP EOF never establishes process exit.
"""

import asyncio
import base64
import hashlib
import json
import math
import os
import re
import socket
import time
import uuid
from pathlib import Path

import recovery as R

LOCAL = "192.168.2.1"
REST_STARTED_AT = 1793483365  # Retained transition: 2026-10-31T21:49:25Z.
COMPONENTS = ("services", "boot", "process", "isolation", "hdmi", "inventory")
SOURCE_LIMIT = 8 * 1024 * 1024
DISCOVERY_REQUEST = b"SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n\0"


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


class Evidence:
    """Private exclusive attempt directory; held descriptor, durable raw records."""

    def __init__(self, parent):
        self.fd = None
        parent_fd = R.durable_journal_directory(parent)
        try:
            self.path = Path(parent) / str(uuid.uuid4())
            os.mkdir(self.path.name, 0o700, dir_fd=parent_fd)
            self.fd = os.open(
                self.path.name,
                os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                dir_fd=parent_fd,
            )
            os.fsync(parent_fd)
            closing_parent, parent_fd = parent_fd, None
            os.close(closing_parent)
        except BaseException:
            child_fd, self.fd = self.fd, None
            for fd in (child_fd, parent_fd):
                if fd is not None:
                    try:
                        os.close(fd)
                    except OSError:
                        # Preserve the construction failure; never retry close.
                        pass
            raise
        self.refs = []

    def write(self, name, raw):
        R.require(self.fd is not None, "evidence is closed")
        R.require(
            isinstance(raw, bytes) and len(raw) <= SOURCE_LIMIT,
            "source bytes/quota",
        )
        R.require(
            name.isascii() and name.replace("-", "").replace(".", "").isalnum(),
            "unsafe evidence name",
        )
        fd = os.open(
            name,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
            0o600,
            dir_fd=self.fd,
        )
        with os.fdopen(fd, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        os.fsync(self.fd)
        self.refs.append({"path": name, "sha256": sha(raw), "bytes": len(raw)})
        return sha(raw)

    def close(self):
        if self.fd is not None:
            fd, self.fd = self.fd, None
            os.close(fd)


async def discover(deadline):
    """Read-only unicast request, bound to the isolated adapter and exact peer."""
    loop = asyncio.get_running_loop()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setblocking(False)
        sock.bind((LOCAL, 0))
        async with asyncio.timeout_at(deadline):
            sent = await loop.sock_sendto(sock, DISCOVERY_REQUEST, (R.CONSOLE, 9302))
            R.require(sent == len(DISCOVERY_REQUEST), "partial discovery request")
            raw, peer = await loop.sock_recvfrom(sock, 4097)
        R.require(peer == (R.CONSOLE, 9302), "foreign discovery peer")
        R.require(len(raw) <= 4096, "discovery quota")
        return raw


def envelope(raw, nonce, started, now):
    R.require(isinstance(raw, bytes) and len(raw) <= SOURCE_LIMIT, "source quota/type")
    value = json.loads(raw, object_pairs_hook=R.unique_object)
    R.require(
        isinstance(value, dict)
        and set(value)
        == {"nonce", "console", "host_id", "firmware", "root", "observed_at", "value"},
        "source envelope fields",
    )
    R.require(
        value["nonce"] == nonce
        and value["console"] == R.CONSOLE
        and value["host_id"] == R.HOST_ID
        and value["firmware"] == R.FIRMWARE
        and value["root"] == R.REMOTE_ROOT,
        "source identity/nonce mismatch",
    )
    stamp = value["observed_at"]
    R.require(
        type(stamp) in (float, int)
        and math.isfinite(stamp)
        and started <= stamp <= now
        and now - stamp <= 120,
        "source predates acquisition or is future/stale",
    )
    R.require(isinstance(value["value"], dict), "source value type")
    return value["value"]


class Adapter:
    """Reviewed callbacks are mandatory; the default instance cannot arm a run.

    Each async callback receives (nonce, absolute_loop_deadline) and returns raw
    UTF-8 JSON in the envelope above. It must honor cancellation. The exit callback
    additionally receives mode, payload SHA-256 and stdout SHA-256; its evidence
    must independently establish exit for that exact invocation.
    """

    def __init__(
        self,
        evidence_parent,
        callbacks=None,
        *,
        exit_observer=None,
        baseline=None,
        journal=R.JOURNAL,
        clock=time.time,
    ):
        self.evidence_parent = Path(evidence_parent)
        self.callbacks = dict(callbacks or {})
        self.exit_observer = exit_observer
        self.baseline = baseline
        self.journal = Path(journal)
        self.clock = clock
        self.last_evidence = None

    def bindings(self):
        R.require(
            set(self.callbacks) == set(COMPONENTS), "unbound observation callbacks"
        )
        R.require(all(callable(c) for c in self.callbacks.values()), "invalid callback")
        R.require(callable(self.exit_observer), "independent exit observer required")
        R.require(
            isinstance(self.baseline, bytes) and len(self.baseline) <= 4096,
            "retained pre-rest boot identity required",
        )
        baseline = json.loads(self.baseline, object_pairs_hook=R.unique_object)
        R.require(
            isinstance(baseline, dict)
            and set(baseline)
            == {"console", "host_id", "firmware", "boot_id", "observed_at"}
            and baseline["console"] == R.CONSOLE
            and baseline["host_id"] == R.HOST_ID
            and baseline["firmware"] == R.FIRMWARE
            and isinstance(baseline["boot_id"], str)
            and 1 <= len(baseline["boot_id"]) <= 128
            and baseline["boot_id"].isascii()
            and baseline["boot_id"].isprintable()
            and type(baseline["observed_at"]) in (int, float)
            and math.isfinite(baseline["observed_at"])
            and 0 <= baseline["observed_at"] < REST_STARTED_AT
            and baseline["observed_at"] < self.clock(),
            "invalid pre-rest boot source",
        )
        return baseline

    async def collect(self):
        baseline = self.bindings()
        evidence = Evidence(self.evidence_parent)
        self.last_evidence = evidence.path
        started = self.clock()
        nonce = str(uuid.uuid4())
        deadline = asyncio.get_running_loop().time() + 30
        try:
            evidence.write("boot-before.json", self.baseline)
            async with asyncio.timeout_at(deadline):
                raw = await discover(deadline)
                evidence.write("discovery.bin", raw)
                discovery = raw.decode("ascii")
                R.discovery_ready(discovery)
                values, hashes = {}, {}
                for name in COMPONENTS:
                    raw = await self.callbacks[name](nonce, deadline)
                    hashes[name] = evidence.write(name + ".json", raw)
                    values[name] = envelope(raw, nonce, started, self.clock())
            now = self.clock()
            boot = values["boot"]
            R.require(
                set(boot) == {"boot_id", "kind"}
                and boot["kind"] == "resume"
                and boot["boot_id"] == baseline["boot_id"],
                "resume continuity unavailable",
            )
            services = values["services"]
            R.require(set(services) == set(R.SERVICES), "service census")
            for name, port in R.SERVICES.items():
                item = services[name]
                R.require(
                    isinstance(item, dict)
                    and set(item) == {"port", "protocol", "verified", "transcript"}
                    and item["port"] == port
                    and item["protocol"] == name
                    and item["verified"] is True
                    and isinstance(item["transcript"], str)
                    and 0 < len(item["transcript"].encode()) <= 65536,
                    "service protocol evidence absent",
                )
                services[name] = {
                    "port": port,
                    "protocol": name,
                    "verified": True,
                    "evidence_sha256": hashes["services"],
                }
            for name, flags in (
                ("process", {"no_game", "no_writer"}),
                ("isolation", {"ipv4", "ipv6", "exclusive"}),
            ):
                R.require(
                    set(values[name]) == flags
                    and all(values[name][f] is True for f in flags),
                    name + " observation refused",
                )
                values[name]["evidence_sha256"] = hashes[name]
            hdmi = values["hdmi"]
            R.require(
                set(hdmi) == {"healthy", "image_base64", "image_sha256"}
                and hdmi["healthy"] is True,
                "HDMI health unavailable",
            )
            image = base64.b64decode(hdmi["image_base64"], validate=True)
            R.require(
                image.startswith(b"\x89PNG\r\n\x1a\n")
                and sha(image) == hdmi["image_sha256"],
                "HDMI source hash/format",
            )
            evidence.write("hdmi.png", image)
            inventory = values["inventory"]
            R.require(
                set(inventory) == {"inventory", "public_fingerprint"},
                "inventory source fields",
            )
            R.validate_inventory(inventory["inventory"])
            index = R.encoded(
                {
                    "nonce": nonce,
                    "started_at": started,
                    "finished_at": now,
                    "sources": list(evidence.refs),
                }
            )
            bundle = {
                "schema_version": 1,
                "console": R.CONSOLE,
                "host_id": R.HOST_ID,
                "firmware": R.FIRMWARE,
                "root": R.REMOTE_ROOT,
                "observed_at": started,
                "evidence_sha256": evidence.write("sources.json", index),
                "discovery": discovery,
                "services": services,
                "resume": {
                    "kind": "resume",
                    "assessed_at": started,
                    "evidence_sha256": hashes["boot"],
                },
                "process": values["process"],
                "isolation": values["isolation"],
                "hdmi": {"healthy": True, "evidence_sha256": hashes["hdmi"]},
                "inventory": inventory["inventory"],
                "public_fingerprint": inventory["public_fingerprint"],
            }
            R.validate_bundle(bundle, now)
            evidence.write("observations.json", R.encoded(bundle))
            return bundle
        except BaseException as error:
            evidence.write("failure.json", R.encoded({"type": type(error).__name__}))
            raise
        finally:
            evidence.close()

    async def _transfer(self, mode, payload, timeout, quota):
        R.require(
            mode in R.ARTIFACTS and sha(payload) == R.ARTIFACTS[mode][1],
            "unapproved payload",
        )
        R.require(timeout == 30 and quota == 65536, "unexpected transfer bounds")
        self.bindings()
        evidence = Evidence(self.evidence_parent)
        self.last_evidence = evidence.path
        loop = asyncio.get_running_loop()
        start, wall = loop.time(), self.clock()
        deadline = start + timeout
        writer, output = None, bytearray()
        nonce = str(uuid.uuid4())
        try:
            evidence.write(
                "transfer.json",
                R.encoded(
                    {
                        "mode": mode,
                        "payload_sha256": sha(payload),
                        "bytes": len(payload),
                        "nonce": nonce,
                        "started_at": wall,
                    }
                ),
            )
            async with asyncio.timeout_at(deadline):
                reader, writer = await asyncio.open_connection(
                    R.CONSOLE, 9021, local_addr=(LOCAL, 0), limit=quota + 1
                )
                R.require(
                    writer.get_extra_info("peername") == (R.CONSOLE, 9021),
                    "foreign loader peer",
                )
                writer.write(payload)
                await writer.drain()
                writer.write_eof()
                while True:
                    chunk = await reader.read(min(8192, quota + 1 - len(output)))
                    if not chunk:
                        break
                    output.extend(chunk)
                    R.require(len(output) <= quota, "loader output quota")
                evidence.write("stdout.bin", bytes(output))
                raw = await self.exit_observer(
                    nonce, deadline, mode, sha(payload), sha(bytes(output))
                )
                evidence.write("exit.json", raw)
                exited = envelope(raw, nonce, wall, self.clock())
                final = re.search(
                    rb"T050 mode=" + mode.encode() + rb" pid=([1-9][0-9]*) ",
                    bytes(output),
                )
                R.require(final is not None, "payload process identity absent")
                R.require(
                    set(exited)
                    == {
                        "mode",
                        "payload_sha256",
                        "stdout_sha256",
                        "exit_code",
                        "complete",
                        "pid",
                        "boot_id",
                        "process_start_id",
                    }
                    and exited["mode"] == mode
                    and exited["payload_sha256"] == sha(payload)
                    and exited["stdout_sha256"] == sha(bytes(output))
                    and type(exited["exit_code"]) is int
                    and exited["exit_code"] == 0
                    and exited["complete"] is True
                    and type(exited["pid"]) is int
                    and exited["pid"] == int(final[1])
                    and exited["boot_id"] == self.bindings()["boot_id"]
                    and isinstance(exited["process_start_id"], str)
                    and 0 < len(exited["process_start_id"]) <= 128,
                    "independent exit evidence mismatch",
                )
                result = {
                    "stdout": output.decode("utf-8"),
                    "stderr": "",
                    "exit_code": 0,
                    "complete": True,
                    "elapsed_seconds": loop.time() - start,
                }
                R.validate_result(mode, result)
                evidence.write("result.json", R.encoded(result))
                return result
        except BaseException as error:
            evidence.write("partial-stdout.bin", bytes(output))
            evidence.write("failure.json", R.encoded({"type": type(error).__name__}))
            raise
        finally:
            try:
                if writer is not None:
                    try:
                        writer.close()
                    finally:
                        # Local abort adds no grace period beyond the deadline.
                        writer.transport.abort()
            finally:
                evidence.close()

    def dispatch(self, mode, *, armed=False):
        """Fresh acquisition precedes journal creation; dispatch never retries."""
        R.require(
            mode in ("verify", "cleanup") and armed is True,
            "explicit mode arming required",
        )
        bundle = asyncio.run(self.collect())
        coordinator = R.Coordinator(self.journal, clock=self.clock)
        try:

            def transfer(kind, payload, timeout, quota):
                return asyncio.run(self._transfer(kind, payload, timeout, quota))

            return getattr(coordinator, mode)(bundle, transfer, armed=True)
        finally:
            coordinator.close()

    def record_postverify(self):
        """Acquire another inventory after successful verify, before cleanup arming."""
        bundle = asyncio.run(self.collect())
        coordinator = R.Coordinator(self.journal, clock=self.clock)
        try:
            coordinator.record_postverify(bundle)
        finally:
            coordinator.close()
