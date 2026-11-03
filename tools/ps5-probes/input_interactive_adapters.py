"""Explicit command adapters for the nonshipping T042 neutral/stop diagnostic.

No device is accessed by import or by direct execution. Commands are reviewed
operator bindings, never shell strings. Postflight JSON is an operator trust
boundary; command execution and hashes do not authenticate its assertions.
"""

import asyncio
import hashlib
import json
import math
import os
import re
import time
from pathlib import Path

import input_interactive_driver as D

MAX_OUTPUT = 65536
MAX_SEGMENT = 32 * 1024 * 1024


def publish_json(artifacts, name, value):
    """Manifest-last atomic no-replace visibility in a private owned directory."""
    pending = name + ".pending"
    artifacts.json(pending, value)
    os.link(
        pending,
        name,
        src_dir_fd=artifacts.fd,
        dst_dir_fd=artifacts.fd,
        follow_symlinks=False,
    )
    # Record the committed name before cleanup/sync, which cannot revoke bytes
    # another reader might already have retained. No overwrite or retry occurs.
    artifacts.refs[-1]["path"] = name
    os.unlink(pending, dir_fd=artifacts.fd)
    artifacts.sync_directory()


def argv(value):
    D.require(
        isinstance(value, (tuple, list))
        and 0 < len(value) <= 64
        and all(
            isinstance(x, str) and 0 < len(x) <= 8192 and "\0" not in x for x in value
        ),
        "invalid command argv",
    )
    D.require(Path(value[0]).is_absolute(), "executable path must be absolute")
    D.require(
        Path(value[0]).is_file() and os.access(value[0], os.X_OK),
        "executable unavailable",
    )
    return tuple(value)


def executable_hash(path):
    digest = hashlib.sha256()
    total = 0
    with Path(path).open("rb") as binary:
        while chunk := binary.read(1024 * 1024):
            total += len(chunk)
            D.require(total <= 256 * 1024 * 1024, "executable exceeds provenance bound")
            digest.update(chunk)
    return digest.hexdigest()


async def spawn(arguments):
    task = asyncio.create_task(
        asyncio.create_subprocess_exec(
            *argv(arguments),
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            start_new_session=True,
        )
    )
    try:
        return await asyncio.shield(task)
    except asyncio.CancelledError:
        process = await task
        await D.ProcessTransport(process).close()
        raise


async def drain(stream, target, limit=MAX_OUTPUT):
    while chunk := await stream.read(4096):
        D.require(len(target) + len(chunk) <= limit, "command output limit exceeded")
        target.extend(chunk)


class Commands:
    """Bounded commands with durable intent, exact argv, output and cleanup."""

    def __init__(self, artifacts, timeout=5):
        D.require(
            math.isfinite(timeout) and 0 < timeout <= 15, "invalid command timeout"
        )
        self.artifacts = artifacts
        self.timeout = timeout

    async def run(self, name, arguments, data=b""):
        arguments = argv(arguments)
        D.require(len(data) <= MAX_SEGMENT, "command input limit exceeded")
        self.artifacts.json(
            f"{name}-intent.json", {"argv": arguments, "at_ns": time.monotonic_ns()}
        )
        record = {
            "argv": arguments,
            "started_ns": time.monotonic_ns(),
            "executable_sha256": await asyncio.to_thread(executable_hash, arguments[0]),
        }
        process = None
        jobs = []
        output, errors = bytearray(), bytearray()
        try:
            async with asyncio.timeout(self.timeout):
                process = await spawn(arguments)
                record["pid"] = process.pid

                async def feed():
                    # A version/inspection command can exit before its stdin is
                    # scheduled. Empty input requires only EOF, never a drain
                    # against a pipe that may already have closed successfully.
                    if data:
                        process.stdin.write(data)
                        await process.stdin.drain()
                    process.stdin.close()

                jobs = [
                    asyncio.create_task(drain(process.stdout, output)),
                    asyncio.create_task(drain(process.stderr, errors)),
                    asyncio.create_task(feed()),
                ]
                await asyncio.gather(*jobs)
                record["returncode"] = await process.wait()
                D.require(record["returncode"] == 0, f"{name} command failed")
                return bytes(output)
        finally:
            for job in jobs:
                job.cancel()
            await asyncio.gather(*jobs, return_exceptions=True)
            if process is not None:
                record["cleanup"] = await D.ProcessTransport(process).close()
            record["ended_ns"] = time.monotonic_ns()
            self.artifacts.write(f"{name}-stdout.dat", bytes(output))
            self.artifacts.write(f"{name}-stderr.dat", bytes(errors))
            self.artifacts.json(f"{name}.json", record)


def avfoundation_argv(ffmpeg, device, width=320, height=180):
    """Acquire continuous 1080p30 video and spatially reduce every frame."""
    D.require(
        isinstance(device, str) and device and ":" not in device, "invalid video device"
    )
    D.require(
        type(width) is int
        and type(height) is int
        and 1 <= width <= 1920
        and 1 <= height <= 1080,
        "invalid capture geometry",
    )
    return (
        str(ffmpeg),
        "-hide_banner",
        "-loglevel",
        "error",
        "-f",
        "avfoundation",
        "-framerate",
        "30",
        "-video_size",
        "1920x1080",
        "-i",
        f"{device}:none",
        "-an",
        "-vf",
        f"scale={width}:{height}",
        "-fps_mode",
        "passthrough",
        "-c:v",
        "ppm",
        "-f",
        "image2pipe",
        "pipe:1",
    )


def decoder_argv(ffmpeg):
    return (
        str(ffmpeg),
        "-hide_banner",
        "-loglevel",
        "error",
        "-xerror",
        "-f",
        "image2pipe",
        "-c:v",
        "ppm",
        "-i",
        "pipe:0",
        "-progress",
        "pipe:1",
        "-nostats",
        "-f",
        "null",
        "-",
    )


class PpmCapture:
    """One continuous process. Segments retain complete PPM frames and receipt times.

    The bound is on host receipt gaps, not source timestamps or console delivery.
    The initial segment contains at least two frames before helper dispatch.
    """

    def __init__(
        self,
        commands,
        capture_argv,
        decode_argv,
        version_argv,
        device,
        width,
        height,
        frame_gap_seconds,
    ):
        self.commands = commands
        self.capture_argv = argv(capture_argv)
        self.decode_argv = argv(decode_argv)
        self.version_argv = argv(version_argv)
        D.require(
            isinstance(device, str) and 0 < len(device) <= 256,
            "device identity missing",
        )
        D.require(
            type(width) is int
            and type(height) is int
            and 1 <= width <= 1920
            and 1 <= height <= 1080,
            "invalid frame geometry",
        )
        D.require(
            math.isfinite(frame_gap_seconds) and 0 < frame_gap_seconds <= 1,
            "invalid frame gap bound",
        )
        self.device = device
        self.header = f"P6\n{width} {height}\n255\n".encode()
        self.pixels = width * height * 3
        self.gap_ns = int(frame_gap_seconds * 1e9)
        self.process = None
        self.reader = self.stderr_reader = None
        self.frames = []
        self.buffered = 0
        self.stderr = bytearray()
        self.boundary = self.last_ns = None
        self.started_ns = None
        self.closing = False
        self.index = 0
        self.version = b""
        self.retained = None
        self.batches = []
        self.retention_errors = []

    def retain_batch(self, frames, origin):
        """Commit complete bytes before a decoder or coordinator can fail."""
        name = f"batch-{len(self.batches)}"
        data = b"".join(frame for _, frame in frames)
        item = {
            "raw": name + ".ppm",
            "metadata": name + ".json",
            "decode": name + "-decode.json",
            "origin": origin,
        }
        self.batches.append(item)
        metadata = {
            "status": "partial_unverified",
            "hardware_acceptance": "Incomplete",
            "origin": origin,
            "frames": len(frames),
            "frame_receipt_ns": [at for at, _ in frames],
            "sha256": hashlib.sha256(data).hexdigest(),
            "bytes": len(data),
            "device": self.device,
            "clock_boundary": "host_frame_receipt_only",
            "source_timestamp_continuity": "unverified",
        }
        failures = []
        for target, content in (
            (item["raw"], data),
            (item["metadata"], json.dumps(metadata).encode()),
        ):
            try:
                self.retained.write(target, content)
            except (OSError, ValueError) as error:
                failures.append(
                    {
                        "path": target,
                        "error": type(error).__name__,
                        "reason": str(error)[:256],
                    }
                )
        self.retention_errors.extend(failures)
        D.require(not failures, "capture retention failed")
        return item, data

    async def decode_batch(self, item, data, count, *, final=False):
        command_name = f"decode-{self.index}"
        self.index += 1
        status = {
            "status": "failed",
            "attempted": True,
            "command_record": command_name + ".json",
            "raw_sha256": hashlib.sha256(data).hexdigest(),
            "expected_frames": count,
            "hardware_acceptance": "Incomplete",
        }
        runner = (
            Commands(self.commands.artifacts, timeout=1) if final else self.commands
        )
        try:
            decoded = await runner.run(command_name, self.decode_argv, data)
            counts = re.findall(rb"(?m)^frame=([0-9]+)\s*$", decoded)
            D.require(
                counts
                and int(counts[-1]) == count
                and b"progress=end" in decoded.splitlines(),
                "decoder did not confirm every retained frame",
            )
            status.update(status="decoded", decode_exit=0, frames=count)
        except (OSError, ValueError, TimeoutError, asyncio.CancelledError) as error:
            status["error"] = type(error).__name__
            raise
        finally:
            try:
                self.retained.json(item["decode"], status)
            except (OSError, ValueError) as error:
                self.retention_errors.append(
                    {"path": item["decode"], "error": type(error).__name__}
                )
                raise

    async def finalize(self):
        """After owned-process cleanup, preserve the final buffer and bind sources.

        This never upgrades protocol completion. A bounded diagnostic decode is
        attempted only after raw retention; failure leaves those raw bytes intact.
        """
        if self.retained is None:
            return {"retention_complete": True, "batches": [], "errors": []}
        try:
            if self.frames:
                frames, self.frames = self.frames, []
                self.buffered = 0
                try:
                    item, data = self.retain_batch(frames, "final_buffer")
                    if self.process is not None and self.process.returncode is not None:
                        await self.decode_batch(item, data, len(frames), final=True)
                    else:
                        self.retained.json(
                            item["decode"],
                            {
                                "status": "unavailable",
                                "attempted": False,
                                "reason": "capture process cleanup unconfirmed",
                            },
                        )
                except (
                    OSError,
                    ValueError,
                    TimeoutError,
                    asyncio.CancelledError,
                ) as error:
                    self.retention_errors.append(
                        {"phase": "final_buffer", "error": type(error).__name__}
                    )
            evidence = {
                "schema_version": 1,
                "status": "partial_unverified",
                "hardware_acceptance": "Incomplete",
                "batches": self.batches,
                "artifacts": list(self.retained.refs),
                "errors": self.retention_errors,
                "retention_complete": not self.retention_errors,
            }
            try:
                publish_json(self.retained, "manifest.json", evidence)
            except (OSError, ValueError) as error:
                self.retention_errors.append(
                    {"phase": "manifest_publication", "error": type(error).__name__}
                )
            return {
                **evidence,
                "retention_complete": not self.retention_errors,
                "artifacts": [
                    {**ref, "path": "capture-retained/" + ref["path"]}
                    for ref in self.retained.refs
                ],
            }
        finally:
            self.retained.close()

    async def start(self):
        D.require(self.started_ns is None, "capture cannot restart")
        # Two requested segments plus the final complete-frame buffer, with an
        # independent metadata allowance. Command logs keep their separate quota.
        self.retained = D.Artifacts(
            self.commands.artifacts.root / "capture-retained",
            3 * MAX_SEGMENT + 1024 * 1024,
        )
        self.version = await self.commands.run("capture-version", self.version_argv)
        D.require(bool(self.version.strip()), "capture version missing")
        self.commands.artifacts.json(
            "capture-intent.json",
            {
                "argv": self.capture_argv,
                "decode_argv": self.decode_argv,
                "device": self.device,
                "clock_boundary": "host_frame_receipt_only",
                "max_receipt_gap_ns": self.gap_ns,
                "executable_sha256": await asyncio.to_thread(
                    executable_hash, self.capture_argv[0]
                ),
            },
        )
        self.started_ns = time.monotonic_ns()
        self.process = await spawn(self.capture_argv)
        self.reader = asyncio.create_task(self.acquire())
        self.stderr_reader = asyncio.create_task(
            drain(self.process.stderr, self.stderr)
        )

    async def acquire(self):
        while True:
            try:
                header = await self.process.stdout.readexactly(len(self.header))
            except asyncio.IncompleteReadError as error:
                if self.closing and not error.partial:
                    return
                raise ValueError("capture ended or truncated") from error
            D.require(header == self.header, "unexpected PPM frame geometry/header")
            try:
                pixels = await self.process.stdout.readexactly(self.pixels)
            except asyncio.IncompleteReadError as error:
                raise ValueError("truncated capture frame") from error
            received = time.monotonic_ns()
            previous = self.last_ns
            frame = header + pixels
            D.require(len(self.frames) < 1024, "capture frame count exceeded")
            D.require(
                self.buffered + len(frame) <= MAX_SEGMENT,
                "capture segment quota exceeded",
            )
            self.frames.append((received, frame))
            self.buffered += len(frame)
            if self.last_ns is not None:
                D.require(
                    0 < received - previous <= self.gap_ns,
                    "capture receipt timeline gap",
                )
            else:
                self.boundary = received
            self.last_ns = received

    async def check(self):
        for job in (self.reader, self.stderr_reader):
            if job is not None and job.done():
                await job
                D.require(self.closing, "capture pipe ended unexpectedly")
        if self.process is not None and not self.closing:
            D.require(self.process.returncode is None, "capture process exited")
            last = self.last_ns if self.last_ns is not None else self.started_ns
            # Allow device startup up to the enclosing watchdog. Once the first
            # frame arrives, enforce the declared receipt gap continuously.
            if self.last_ns is not None:
                D.require(
                    time.monotonic_ns() - last <= self.gap_ns, "capture receipt stalled"
                )

    async def segment(self, after_ns):
        while True:
            await self.check()
            if (
                self.frames
                and self.boundary < self.frames[-1][0]
                and self.frames[-1][0] > after_ns
            ):
                break
            await asyncio.sleep(0.005)
        frames, self.frames = self.frames, []
        self.buffered = 0
        start, end = self.boundary, frames[-1][0]
        self.boundary = end
        item, data = self.retain_batch(frames, "segment_candidate")
        await self.decode_batch(item, data, len(frames))
        await self.check()
        report = {
            "frames": len(frames),
            "decode_exit": 0,
            "sha256": hashlib.sha256(data).hexdigest(),
            "capture_argv": self.capture_argv,
            "decode_argv": self.decode_argv,
            "capture_version_sha256": hashlib.sha256(self.version).hexdigest(),
            "device": self.device,
            "frame_receipt_ns": [at for at, _ in frames],
            "clock_boundary": "host_frame_receipt_only",
            "source_timestamp_continuity": "unverified",
            "max_receipt_gap_ns": self.gap_ns,
        }
        return D.Segment(start, end, data, json.dumps(report).encode())

    async def close(self):
        if self.process is None:
            return {"reaped": True, "returncode": 0}
        self.closing = True
        clean = False
        error_name = None
        try:
            self.process.stdin.write(b"q\n")
            await self.process.stdin.drain()
            self.process.stdin.close()
            await asyncio.wait_for(
                asyncio.gather(self.reader, self.stderr_reader, self.process.wait()), 1
            )
            clean = self.process.returncode == 0
        except (OSError, ValueError, TimeoutError, asyncio.CancelledError) as error:
            error_name = type(error).__name__
        finally:
            for job in (self.reader, self.stderr_reader):
                job.cancel()
            await asyncio.gather(
                self.reader, self.stderr_reader, return_exceptions=True
            )
            cleanup = await D.ProcessTransport(self.process).close()
            self.commands.artifacts.write("capture-stderr.dat", bytes(self.stderr))
            self.commands.artifacts.json(
                "capture-close.json",
                {
                    "pid": self.process.pid,
                    "clean_eof": clean,
                    "error": error_name,
                    **cleanup,
                },
            )
        return {**cleanup, "returncode": 0 if clean else -1}


def strict_json(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            D.require(key not in result, "duplicate observation field")
            result[key] = value
        return result

    result = json.loads(data.decode("utf-8"), object_pairs_hook=unique)
    D.require(
        isinstance(result, dict)
        and type(result.get("schema_version")) is int
        and result["schema_version"] == 1,
        "invalid observation schema",
    )
    return result


class Postflight:
    def __init__(self, commands, bindings):
        D.require(
            set(bindings) == {"discovery", "services", "process", "hdmi"},
            "all four postflight commands required",
        )
        self.commands = commands
        self.bindings = {key: argv(value) for key, value in bindings.items()}

    async def __call__(self, pid):
        sources, values = {}, {}
        for name, arguments in self.bindings.items():
            bound = tuple(
                str(pid) if token == "{pid}" else token for token in arguments
            )
            raw = await self.commands.run(f"postflight-{name}", bound)
            sources[name] = raw
            values[name] = strict_json(raw)
        D.require(
            values["discovery"].get("ready") is True
            and values["discovery"].get("firmware") == D.FIRMWARE,
            "discovery unavailable or wrong firmware",
        )
        D.require(values["services"].get("healthy") is True, "services unavailable")
        D.require(
            type(values["process"].get("pid")) is int
            and values["process"]["pid"] == pid
            and values["process"].get("absent") is True,
            "exact process absence unconfirmed",
        )
        D.require(
            values["hdmi"].get("signal_present") is True, "HDMI postflight unavailable"
        )
        return {
            "pid": pid,
            "observed_ns": time.monotonic_ns(),
            "process_absent": True,
            "services_healthy": True,
            "sources": sources,
            "trust_boundary": "operator_supplied_read_only_commands",
        }


async def run_with_commands(
    output,
    elf,
    preflight,
    *,
    capture_argv,
    decode_argv,
    version_argv,
    launch_argv,
    launch_version_argv,
    postflight_commands,
    device,
    width=320,
    height=180,
    frame_gap_seconds=0.25,
    enable_device=False,
    synthetic=False,
    total_seconds=30,
):
    """One opt-in run; all executable bindings are supplied by the trusted caller.

    `{elf}` must be one complete launch argument; `{pid}` may be one complete
    postflight argument. Neither substitution invokes a shell. This wrapper never
    retries dispatch and never performs automatic console recovery.
    """
    D.require(synthetic or enable_device, "device execution disabled")
    launch_argv = argv(launch_argv)
    D.require(launch_argv.count("{elf}") == 1, "one literal ELF argument required")
    D.require(
        math.isfinite(total_seconds) and 0 < total_seconds <= 300, "invalid watchdog"
    )
    artifacts = D.Artifacts(Path(output), 4 * 1024 * 1024)
    commands = Commands(artifacts)
    result = {"protocol_complete": False, "hardware_acceptance": "Incomplete"}
    capture = None
    try:
        capture = PpmCapture(
            commands,
            capture_argv,
            decode_argv,
            version_argv,
            device,
            width,
            height,
            frame_gap_seconds,
        )
        postflight = Postflight(commands, postflight_commands)
        version = await commands.run("launch-version", launch_version_argv)
        D.require(bool(version.strip()), "launch version missing")

        async def launch(snapshot):
            arguments = tuple(
                str(snapshot) if token == "{elf}" else token for token in launch_argv
            )
            artifacts.json(
                "launch-intent.json",
                {
                    "argv": arguments,
                    "at_ns": time.monotonic_ns(),
                    "executable_sha256": await asyncio.to_thread(
                        executable_hash, arguments[0]
                    ),
                },
            )
            return await D.ProcessTransport.spawn(arguments)

        result = await D.run(
            artifacts.root / "coordinator",
            elf,
            preflight,
            capture,
            launch,
            postflight,
            enable_device=enable_device,
            synthetic=synthetic,
            total_seconds=total_seconds,
        )
    finally:
        try:
            evidence = (
                await capture.finalize()
                if capture is not None
                else {
                    "retention_complete": True,
                    "artifacts": [],
                    "errors": [],
                }
            )
            if not evidence["retention_complete"]:
                result["protocol_complete"] = False
                result["capture_retention_errors"] = evidence["errors"]
            refs = list(artifacts.refs) + evidence.get("artifacts", [])
            for name in (
                "manifest.json",
                "incomplete.json",
                "publication-failure.json",
            ):
                path = artifacts.root / "coordinator" / name
                if path.is_file():
                    refs.append(
                        {
                            "path": "coordinator/" + name,
                            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                        }
                    )
            publish_json(
                artifacts,
                "adapter-manifest.json",
                {
                    "schema_version": 1,
                    "provenance": "synthetic_fixture"
                    if synthetic
                    else "operator_bound",
                    "protocol_complete": result["protocol_complete"],
                    "hardware_acceptance": "Incomplete",
                    "clock_boundary": "host_frame_receipt_only",
                    "capture_retention_complete": evidence["retention_complete"],
                    "capture_retention_errors": evidence["errors"],
                    "artifacts": refs,
                },
            )
        finally:
            artifacts.close()
    return result


if __name__ == "__main__":
    raise SystemExit(
        "Device execution disabled: import run_with_commands() with reviewed bindings"
    )
