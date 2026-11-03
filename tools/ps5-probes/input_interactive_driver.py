"""Bounded neutral/stop coordinator. Importable API; no implicit device access.

Capture and postflight providers are explicit trust boundaries. They must collect
independent observations; their assertions do not establish hardware acceptance.
All callbacks are async, cancellation-cooperative, and bounded by this coordinator.
"""

import asyncio
import hashlib
import json
import os
import re
import signal
import stat
import time
from dataclasses import dataclass
from pathlib import Path

ELF_SHA256 = "3f0fa25d8bf4d4e6aee59b0459e507f6ee9c71b51b1027210971ec0607f94f2b"
FIRMWARE = "0x13600007"
READY = re.compile(
    rb"READY version=1 pid=([1-9][0-9]{0,9}) firmware=0x13600007 "
    rb"neutral=accepted next=1 delivery=unverified"
)
ACK = re.compile(
    rb"ACK seq=1 mask=0x0000 neutral=accepted begin_ms=([0-9]{1,20}) "
    rb"release_ms=([0-9]{1,20}) end_ms=([0-9]{1,20})"
)
DONE = (
    b"DONE code=0 owned=1 neutral_rc=0 remove_rc=0 close_rc=0 "
    b"terminate_rc=0 hardware_acceptance=incomplete"
)


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


@dataclass(frozen=True)
class Segment:
    """Completed segment plus independent decoder output on the host clock."""

    start_ns: int
    end_ns: int
    data: bytes
    decode: bytes


class Artifacts:
    def __init__(self, root, quota):
        self.fd = None
        self.root = root.absolute()
        require(
            ".." not in self.root.parts and len(self.root.parts) > 1,
            "invalid output path",
        )
        flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
        parent = os.open(self.root.anchor, flags)
        try:
            for component in self.root.parts[1:-1]:
                child = os.open(component, flags, dir_fd=parent)
                try:
                    info = os.fstat(child)
                    require(
                        info.st_uid in (0, os.geteuid()), "untrusted output ancestor"
                    )
                    require(
                        not info.st_mode & 0o002 or info.st_mode & stat.S_ISVTX,
                        "unsafe output ancestor",
                    )
                    # Existing ancestors may themselves be interrupted mkdirs.
                    os.fsync(child)
                    os.fsync(parent)
                except BaseException:
                    os.close(child)
                    raise
                os.close(parent)
                parent = child
            info = os.fstat(parent)
            require(
                info.st_uid == os.geteuid() and not info.st_mode & 0o022,
                "output parent must be owned and not group/world writable",
            )
            # Exclusive creation is the no-retry gate, even for interrupted runs.
            os.mkdir(self.root.name, mode=0o700, dir_fd=parent)
            child = os.open(self.root.name, flags, dir_fd=parent)
            try:
                info = os.fstat(child)
                require(
                    info.st_uid == os.geteuid() and stat.S_IMODE(info.st_mode) == 0o700,
                    "unsafe output directory",
                )
                os.fsync(child)
                os.fsync(parent)  # Persist the containing entry before dispatch.
                self.fd = child
            except BaseException:
                os.close(child)
                raise
        finally:
            os.close(parent)
        self.remaining = quota
        self.refs = []

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def __del__(self):
        self.close()

    def write(self, name, data):
        require(re.fullmatch(r"[a-zA-Z0-9_.-]+", name), "invalid artifact name")
        require(len(data) <= self.remaining, "artifact quota exceeded")
        require(len(self.refs) < 128, "artifact count exceeded")
        fd = os.open(
            name,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
            0o600,
            dir_fd=self.fd,
        )
        with os.fdopen(fd, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        self.sync_directory()
        self.remaining -= len(data)
        self.refs.append({"path": name, "sha256": hashlib.sha256(data).hexdigest()})
        return self.root / name

    def json(self, name, data):
        return self.write(name, json.dumps(data, sort_keys=True).encode() + b"\n")

    def publish(self, manifest):
        self.json("manifest.pending", manifest)
        os.rename(
            "manifest.pending", "manifest.json", src_dir_fd=self.fd, dst_dir_fd=self.fd
        )
        try:
            self.sync_directory()
        except OSError:
            os.unlink("manifest.json", dir_fd=self.fd)
            raise

    def sync_directory(self):
        os.fsync(self.fd)


class Lines:
    def __init__(self, transport, artifacts, limit):
        self.transport = transport
        self.artifacts = artifacts
        self.limit = limit
        self.buffer = b""
        self.raw = bytearray()

    async def line(self):
        while b"\n" not in self.buffer:
            require(len(self.buffer) <= 512, "protocol line exceeds limit")
            chunk = await self.transport.read(4096)
            require(isinstance(chunk, bytes), "transport returned non-bytes")
            require(bool(chunk), "EOF before expected protocol line")
            require(len(self.raw) + len(chunk) <= self.limit, "output quota exceeded")
            self.raw.extend(chunk)
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        require(len(line) <= 512, "protocol line exceeds limit")
        return line

    async def eof(self):
        require(not self.buffer, "unexpected output after DONE")
        chunk = await self.transport.read(4096)
        if chunk:
            require(len(self.raw) + len(chunk) <= self.limit, "output quota exceeded")
            self.raw.extend(chunk)
            raise ValueError("unexpected output after DONE")


class ProcessTransport:
    """Owned subprocess group; stderr is retained in the bounded protocol stream.

    The caller supplies reviewed argv. Spawn itself does not imply authorization.
    The driver receives a factory, calls it once, and never retries it.
    Group signals are best effort while the leader is reported live. Descendants
    surviving its exit are not established absent by direct-child reaping.
    """

    def __init__(self, process):
        self.process = process

    @classmethod
    async def spawn(cls, argv):
        spawning = asyncio.create_task(
            asyncio.create_subprocess_exec(
                *argv,
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.STDOUT,
                start_new_session=True,
            )
        )
        try:
            return cls(await asyncio.shield(spawning))
        except asyncio.CancelledError:
            # Subprocess creation may already have forked. Recover its handle
            # before propagating cancellation so no spawned child is orphaned.
            owned = cls(await spawning)
            await owned.close()
            raise

    async def read(self, size):
        return await self.process.stdout.read(size)

    async def send(self, data):
        self.process.stdin.write(data)
        await self.process.stdin.drain()

    async def wait(self):
        return await self.process.wait()

    async def finish(self):
        self.process.stdin.close()

    async def close(self):
        if self.process.stdin:
            self.process.stdin.close()
        for sig in (signal.SIGTERM, signal.SIGKILL):
            # asyncio may reap independently of wait(); a known exit ends our
            # authority to signal this numeric group identifier.
            if self.process.returncode is not None:
                break
            try:
                os.killpg(self.process.pid, sig)
            except ProcessLookupError:
                pass
            try:
                await asyncio.wait_for(self.process.wait(), 1)
            except TimeoutError:
                continue
            break
        return {
            "reaped": self.process.returncode is not None,
            "returncode": self.process.returncode,
            "descendant_cleanup_confirmed": False,
        }


def retain_observation(artifacts, prefix, observation):
    sources = observation.get("sources")
    require(
        isinstance(sources, dict) and 0 < len(sources) <= 8,
        "independent observation sources missing",
    )
    retained = dict(observation)
    retained["sources"] = []
    for index, (name, data) in enumerate(sources.items()):
        require(
            isinstance(name, str) and isinstance(data, bytes) and data,
            "invalid observation source",
        )
        require(len(data) <= 65536, "observation source exceeds limit")
        path = f"{prefix}-source-{index}.dat"
        artifacts.write(path, data)
        retained["sources"].append({"name": name, "path": path})
    artifacts.json(f"{prefix}.json", retained)


async def run(
    output,
    elf,
    preflight,
    capture,
    launch,
    postflight,
    *,
    enable_device=False,
    synthetic=False,
    total_seconds=60,
    output_limit=16384,
    artifact_quota=64 * 1024 * 1024,
):
    """Run exactly neutral then stop. Live adapters require explicit enable_device.

    capture provides async start(), check(), segment(after_ns), close(). segment
    returns completed contiguous host-monotonic segments with decoder JSON.
    launch(snapshot_path) returns async read/send/finish/wait/close transport.
    postflight receives the READY PID and returns fresh observations/sources.
    Providers must not suppress cancellation or block the event loop. close()
    must reap owned children and return {reaped: true, returncode: integer}.
    """
    require(synthetic or enable_device, "device execution disabled")
    require(0 < total_seconds <= 300, "invalid watchdog")
    require(0 < output_limit <= 65536, "invalid output limit")
    require(0 < artifact_quota <= 256 * 1024 * 1024, "invalid artifact quota")
    artifacts = Artifacts(Path(output), artifact_quota)
    result = {
        "schema_version": 1,
        "provenance": "synthetic_fixture" if synthetic else "operator_bound",
        "protocol_complete": False,
        "hardware_acceptance": "Incomplete",
        "physical_removal": "unverified",
        "delivery": "unverified",
        "timing_correlation": "unverified",
        "api_cleanup_returned": False,
        "process_absent": False,
        "capture": [],
    }
    started = time.monotonic_ns()
    transport = None
    lines = None
    commands = []
    events = []
    monitor = None
    capture_ready = asyncio.Event()

    async def check_capture():
        await capture_ready.wait()
        while True:
            await capture.check()
            await asyncio.sleep(0.05)

    async def segment(after):
        item = await capture.segment(after)
        require(isinstance(item, Segment), "capture segment missing")
        require(
            type(item.start_ns) is int
            and type(item.end_ns) is int
            and started <= item.start_ns < item.end_ns <= time.monotonic_ns()
            and item.end_ns > after,
            "capture timestamps invalid or stale",
        )
        if result["capture"]:
            require(
                item.start_ns == result["capture"][-1]["end_ns"],
                "capture coverage has a gap or overlap",
            )
        require(
            0 < len(item.data) <= 32 * 1024 * 1024 and 0 < len(item.decode) <= 65536,
            "capture size invalid",
        )
        decoded = json.loads(item.decode)
        require(
            type(decoded.get("frames")) is int
            and decoded["frames"] > 0
            and type(decoded.get("decode_exit")) is int
            and decoded.get("decode_exit") == 0,
            "capture not decodable",
        )
        index = len(result["capture"])
        artifacts.write(f"capture-{index}.dat", item.data)
        artifacts.write(f"capture-{index}-decode.json", item.decode)
        result["capture"].append({"start_ns": item.start_ns, "end_ns": item.end_ns})

    async def sequence():
        nonlocal transport, lines
        observed = preflight.get("observed_ns")
        require(
            type(observed) is int and 0 <= started - observed <= 60_000_000_000,
            "preflight is stale or has an invalid host clock",
        )
        require(
            preflight.get("schema_version") == 1
            and preflight.get("firmware") == FIRMWARE,
            "preflight firmware mismatch",
        )
        for field in (
            "t050_cleanup_complete",
            "no_active_game_or_probe",
            "ipv4_ipv6_isolated",
            "exclusive_capture",
        ):
            require(preflight.get(field) is True, f"preflight missing {field}")
        retain_observation(artifacts, "preflight", preflight)
        with Path(elf).open("rb") as source:
            binary = source.read(16 * 1024 * 1024 + 1)
        require(
            len(binary) <= 16 * 1024 * 1024
            and hashlib.sha256(binary).hexdigest() == ELF_SHA256,
            "ELF hash mismatch",
        )
        snapshot = artifacts.write("input-interactive.elf", binary)
        snapshot.chmod(0o400)
        await capture.start()
        capture_ready.set()
        await segment(started)
        await capture.check()
        result["dispatch_ns"] = time.monotonic_ns()
        require(
            result["dispatch_ns"] - observed <= 60_000_000_000,
            "preflight expired before dispatch",
        )
        artifacts.json(
            "dispatch-intent.json",
            {
                "at_ns": result["dispatch_ns"],
                "elf_sha256": ELF_SHA256,
                "retry_permitted": False,
            },
        )
        transport = await launch(snapshot)
        lines = Lines(transport, artifacts, output_limit)
        ready = READY.fullmatch(await lines.line())
        require(ready is not None, "invalid or unexpected READY")
        result["pid"] = int(ready[1])
        events.append({"event": "ready", "host_ns": time.monotonic_ns()})
        # Journal each intent before writing; any uncertain transmission is final.
        for command, label in ((b"1 neutral\n", "neutral"), (b"2 stop\n", "stop")):
            require(not lines.buffer, "unsolicited protocol response before command")
            event = {"event": label, "host_ns": time.monotonic_ns()}
            artifacts.json(f"{label}-intent.json", event)
            commands.append(command.decode())
            await transport.send(command)
            events.append(event)
            response = await lines.line()
            if label == "neutral":
                ack = ACK.fullmatch(response)
                require(ack is not None, "invalid neutral ACK")
                begin, release, end = map(int, ack.groups())
                require(
                    0 <= begin <= release <= end <= (1 << 64) - 1,
                    "unordered or invalid ACK timestamps",
                )
                result["console_ms"] = {"begin": begin, "release": release, "end": end}
            else:
                require(response == DONE, "invalid DONE or failed cleanup")
                result["api_cleanup_returned"] = True
            events.append(
                {"event": f"{label}-response", "host_ns": time.monotonic_ns()}
            )
        await transport.finish()
        await lines.eof()
        require(await transport.wait() == 0, "transport exit failure")
        observation = await postflight(result["pid"])
        post_ns = observation.get("observed_ns")
        require(
            type(post_ns) is int
            and events[-1]["host_ns"] <= post_ns <= time.monotonic_ns(),
            "postflight is stale",
        )
        require(
            observation.get("pid") == result["pid"]
            and observation.get("process_absent") is True
            and observation.get("services_healthy") is True,
            "postflight failed",
        )
        retain_observation(artifacts, "postflight", observation)
        result["postflight_ns"] = post_ns
        result["process_absent"] = True
        await segment(post_ns)
        await capture.check()
        result["protocol_complete"] = True

    task = None
    try:
        async with asyncio.timeout(total_seconds):
            task = asyncio.create_task(sequence())
            monitor = asyncio.create_task(check_capture())
            completed, _ = await asyncio.wait(
                (task, monitor), return_when=asyncio.FIRST_COMPLETED
            )
            if monitor in completed:
                await monitor  # A capture failure aborts even if transport stalls.
                raise ValueError("capture monitor stopped")
            await task
    except (Exception, asyncio.CancelledError) as error:
        result["protocol_complete"] = False
        result["error"] = type(error).__name__ + ": " + str(error)[:256]
    finally:
        for pending in (task, monitor):
            if pending is not None:
                pending.cancel()
        await asyncio.gather(
            *(x for x in (task, monitor) if x is not None), return_exceptions=True
        )
        for label, owned in (("transport", transport), ("capture", capture)):
            if owned is None:
                continue
            try:
                cleanup = await asyncio.wait_for(owned.close(), 3)
                require(cleanup.get("reaped") is True, "owned child not reaped")
                require(
                    not result["protocol_complete"] or cleanup.get("returncode") == 0,
                    "owned child cleanup failed",
                )
                result[f"{label}_cleanup"] = cleanup
            except Exception as error:
                result["protocol_complete"] = False
                result[f"{label}_cleanup_error"] = type(error).__name__
        try:
            artifacts.write("transport.log", bytes(lines.raw) if lines else b"")
            artifacts.json("commands.json", commands)
            artifacts.json("events.json", events)
            result["artifacts"] = list(artifacts.refs)
            if result["protocol_complete"]:
                artifacts.publish(result)
            else:
                artifacts.json("incomplete.json", result)
        except Exception as error:
            result["protocol_complete"] = False
            result["publication_error"] = type(error).__name__
            try:
                artifacts.json("publication-failure.json", result)
            except (OSError, ValueError):
                pass  # Quota/disk failure may prevent even a failure record.
        artifacts.close()
    return result


if __name__ == "__main__":
    raise SystemExit(
        "Device execution disabled: import run() with reviewed adapters and preflight"
    )
