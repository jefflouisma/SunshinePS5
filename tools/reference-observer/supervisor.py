"""Local bounded collector owner; coordination never establishes acceptance."""

import argparse
import hashlib
import json
import math
import os
import selectors
import signal
import stat
import subprocess
import sys
import time
import uuid
from pathlib import Path

from observer import (
    RuntimeAttachment,
    encoded,
    file_identity,
    path_parent,
    publish_envelope,
    read_runner_binding,
    require_parent,
    strict_document,
    unique_object,
)


def snapshot(path, limit=16384, *, with_identity=False):
    """Read once, refusing aliases, special files and mutation during acquisition."""
    path, directory = path_parent(path)
    fd = None
    try:
        before = os.stat(path.name, dir_fd=directory, follow_symlinks=False)
        if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
            raise ValueError("unaliased regular file required")
        if not 0 < before.st_size <= limit:
            raise ValueError("snapshot size")
        fd = os.open(
            path.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory
        )
        opened = os.fstat(fd)
        data = bytearray()
        while len(data) <= limit:
            part = os.read(fd, limit + 1 - len(data))
            if not part:
                break
            data.extend(part)
        after = os.stat(path.name, dir_fd=directory, follow_symlinks=False)
        if (
            len(data) != before.st_size
            or file_identity(before) != file_identity(opened)
            or file_identity(opened) != file_identity(os.fstat(fd))
            or file_identity(opened) != file_identity(after)
        ):
            raise ValueError("snapshot changed")
        require_parent(path, directory)
        return (bytes(data), file_identity(opened)) if with_identity else bytes(data)
    finally:
        try:
            if fd is not None:
                owned, fd = fd, None
                os.close(owned)
        finally:
            os.close(directory)


def document(path, limit=16384):
    return json.loads(
        snapshot(path, limit).decode("utf-8"), object_pairs_hook=unique_object
    )


def cancel_result(path, binding):
    value = document(path)
    if (
        not isinstance(value, dict)
        or set(value) != {"schema_version", "state", "binding", "request_succeeded"}
        or value["schema_version"] != "1.0.0"
        or value["state"] != "host_cancel_completed"
        or value["binding"] != binding
        or type(value["request_succeeded"]) is not bool
    ):
        raise ValueError("invalid cancellation signal")
    return value


def absent(path):
    path, directory = path_parent(path)
    try:
        try:
            os.stat(path.name, dir_fd=directory, follow_symlinks=False)
        except FileNotFoundError:
            return
        raise ValueError("fresh destination required")
    finally:
        os.close(directory)


def save(path, data):
    with path.open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def supervise(
    binding_path,
    collector_path,
    scenario_path,
    ready_path,
    output,
    timeout=60,
    *,
    command=None,
    provenance="reference_host",
    runtime_attachment=None,
    attachment_ready=None,
    device_feed=None,
    provider_ready=None,
):
    """All processes/signals must share a trusted local filesystem and clock.

    command is an internal test seam, never a CLI or untrusted runner argument.
    One absolute deadline covers binding wait, startup, collection and publication.
    Forced cleanup has a separate, at most two-second process-reaping allowance.
    """
    if not math.isfinite(timeout) or not 0 < timeout <= 300:
        raise ValueError("timeout must be finite, positive and at most 300 seconds")
    deadline = time.monotonic() + timeout
    if (runtime_attachment is None) != (attachment_ready is None):
        raise ValueError(
            "runtime attachment and acknowledgement paths required together"
        )
    if runtime_attachment is not None:
        absent(Path(runtime_attachment))
        absent(Path(attachment_ready))
    if device_feed is not None:
        if runtime_attachment is None:
            raise ValueError("device feed requires runtime attachment")
        absent(Path(device_feed))
    if provider_ready is not None:
        if runtime_attachment is None:
            raise ValueError("provider READY requires runtime attachment")
        absent(Path(provider_ready))
    binding_path, collector_path, scenario_path, ready_path, output = map(
        Path, (binding_path, collector_path, scenario_path, ready_path, output)
    )
    output = output.absolute()
    absent(output)
    absent(ready_path)
    cancel_path = binding_path.parent / "observer-cancel.json"
    absent(cancel_path)
    collector = document(collector_path)
    scenario = document(scenario_path, 1048576)
    output.mkdir(mode=0o700)
    save(output / "scenario.json", encoded(scenario))
    result = {
        "schema_version": "1.0.0",
        "acceptance": "Incomplete",
        "coordination": "incomplete",
        "reason": None,
        "request_succeeded": None,
        "returncode": None,
        "artifacts": {},
        "cleanup_errors": [],
    }
    process = None
    reader = writer = None
    selector = selectors.DefaultSelector()
    logs = {}
    try:
        while True:
            if time.monotonic() >= deadline:
                raise TimeoutError("binding deadline")
            try:
                binding = read_runner_binding(binding_path, collector)
                break
            except FileNotFoundError:
                time.sleep(min(0.02, max(0, deadline - time.monotonic())))
        save(output / "observer-binding.json", encoded(binding))
        if runtime_attachment is not None:
            identity = observer_identity(binding)
            publish_envelope(output / "observer-identity.json", encoded(identity))
            collector["runtime_identity"] = "observer:" + identity["observer_id"]
        if provider_ready is not None:
            from fenrir_provider import provider_ready as read_provider_ready

            while True:
                if time.monotonic() >= deadline:
                    raise TimeoutError("provider baseline deadline")
                try:
                    snapshot(Path(provider_ready), 16384)
                except FileNotFoundError:
                    time.sleep(min(0.02, max(0, deadline - time.monotonic())))
                    continue
                ready_bytes, baseline_bytes = read_provider_ready(
                    provider_ready, binding, identity
                )
                break
            save(output / "provider-ready.json", ready_bytes)
            save(output / "provider-baseline.json", baseline_bytes)
            if time.monotonic() >= deadline:
                raise TimeoutError("provider baseline retention deadline")
        save(output / "collector-binding.json", encoded(collector))
        reader, writer = os.pipe()
        os.set_blocking(reader, False)
        os.set_blocking(writer, False)
        argv = list(
            command or [sys.executable, str(Path(__file__).with_name("observer.py"))]
        )
        argv += [
            "--binding",
            str(output / "collector-binding.json"),
            "--scenario",
            str(output / "scenario.json"),
            "--runner-binding",
            str(output / "observer-binding.json"),
            "--ready-output",
            str(ready_path.absolute()),
            "--output",
            str(output / "collector"),
            "--duration",
            str(max(0.001, deadline - time.monotonic())),
            "--finish-fd",
            str(reader),
        ]
        if runtime_attachment is not None:
            argv += [
                "--observer-identity",
                str(output / "observer-identity.json"),
                "--runtime-attachment",
                str(Path(runtime_attachment).absolute()),
                "--attachment-ready",
                str(Path(attachment_ready).absolute()),
                "--launch-marker",
                str(binding_path.absolute().parent / "observer-launch.json"),
            ]
        if device_feed is not None:
            argv += ["--device-feed", str(Path(device_feed).absolute())]
        process = subprocess.Popen(
            argv,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            pass_fds=(reader,),
            start_new_session=True,
        )
        save(output / "process.json", encoded({"pid": process.pid, "argv": argv}))
        owned, reader = reader, None
        os.close(owned)
        for name, stream in (("stdout", process.stdout), ("stderr", process.stderr)):
            os.set_blocking(stream.fileno(), False)
            logs[name] = (output / (name + ".log")).open("xb")
            selector.register(stream, selectors.EVENT_READ, name)
        ready = False
        signalled = False
        pending = b""
        total = 0
        while True:
            if time.monotonic() >= deadline:
                raise TimeoutError("supervisor deadline")
            if not ready:
                try:
                    value = document(ready_path)
                except FileNotFoundError:
                    pass
                else:
                    if value != {
                        "schema_version": "1.0.0",
                        "state": "ready",
                        "binding": binding,
                    }:
                        raise ValueError("READY mismatch")
                    save(output / "observer-ready.json", encoded(value))
                    ready = True
            if not signalled:
                try:
                    value = cancel_result(cancel_path, binding)
                except FileNotFoundError:
                    pass
                else:
                    if not ready:
                        raise ValueError("cancellation before READY")
                    save(output / "observer-cancel.json", encoded(value))
                    result["request_succeeded"] = value["request_succeeded"]
                    signalled = True
                    if value["request_succeeded"]:
                        pending = encoded(
                            {
                                "schema_version": "1.0.0",
                                "command": "finish",
                                "binding": binding,
                            }
                        )
                    else:
                        owned, writer = writer, None
                        os.close(owned)
            if pending:
                try:
                    pending = pending[os.write(writer, pending) :]
                except BlockingIOError:
                    pass
                if not pending:
                    owned, writer = writer, None
                    os.close(owned)
            for key, _ in selector.select(
                min(0.02, max(0, deadline - time.monotonic()))
            ):
                data = os.read(key.fileobj.fileno(), 65536)
                if not data:
                    selector.unregister(key.fileobj)
                else:
                    total += len(data)
                    if total > 1048576:
                        raise ValueError("collector log quota")
                    logs[key.data].write(data)
            if process.poll() is not None and not selector.get_map():
                if process.returncode != 0 or not ready or not signalled or pending:
                    raise ValueError(
                        "collector did not complete controlled publication"
                    )
                manifest = document(output / "collector/manifest.json", 65536)
                for key in (
                    "session_id",
                    "scenario_id",
                    "host_version",
                    "test_client_version",
                    "test_client_revision",
                    "official_client_version",
                ):
                    if manifest[key] != collector[key]:
                        raise ValueError("manifest binding mismatch")
                if manifest["provenance"] != provenance:
                    raise ValueError("manifest provenance mismatch")
                if {a["path"] for a in manifest["artifacts"]} != {
                    "raw.json",
                    "host.jsonl",
                } or len(manifest["artifacts"]) != 2:
                    raise ValueError("unexpected collector artifacts")
                for artifact in manifest["artifacts"]:
                    data = snapshot(output / "collector" / artifact["path"], 1048576)
                    if hashlib.sha256(data).hexdigest() != artifact["sha256"]:
                        raise ValueError("collector artifact digest mismatch")
                    if artifact["path"] == "raw.json":
                        raw = json.loads(
                            data.decode("utf-8"), object_pairs_hook=unique_object
                        )
                        if (
                            raw["binding"] != collector
                            or raw["provenance"] != provenance
                            or raw["scenario_sha256"]
                            != hashlib.sha256(encoded(scenario)).hexdigest()
                        ):
                            raise ValueError("raw collector binding mismatch")
                        if runtime_attachment is not None:
                            attached = [
                                row
                                for row in raw["records"]
                                if row.get("kind") == "runtime_attachment"
                            ]
                            if (
                                len(attached) != 1
                                or attached[0]["observer_identity"] != identity
                            ):
                                raise ValueError(
                                    "raw runtime attachment identity missing"
                                )
                            row = attached[0]
                            retained = row["attachment_bytes"].encode("utf-8")
                            validator = RuntimeAttachment(
                                identity, binding, runtime_attachment, attachment_ready
                            )
                            validator.launch = document(
                                binding_path.parent / "observer-launch.json"
                            )
                            validator.validate(strict_document(retained))
                            if (
                                row["attachment"] != strict_document(retained)
                                or row["launch_marker"] != validator.launch
                                or row["continuity"] != "unverified"
                            ):
                                raise ValueError("raw runtime attachment mismatch")
                            for source in row["sources"]:
                                if (
                                    hashlib.sha256(
                                        source["text"].encode("utf-8")
                                    ).hexdigest()
                                    != source["sha256"]
                                ):
                                    raise ValueError(
                                        "raw runtime source digest mismatch"
                                    )
                            if [
                                {"path": source["path"], "sha256": source["sha256"]}
                                for source in row["sources"]
                            ] != row["attachment"]["sources"]:
                                raise ValueError(
                                    "raw runtime source inventory mismatch"
                                )
                            if document(Path(attachment_ready)) != {
                                "schema_version": "1.0.0",
                                "state": "attached",
                                "binding": binding,
                                "launch_nonce": validator.launch["launch_nonce"],
                                "observer_id": identity["observer_id"],
                                "association": "exclusive_launch_candidate",
                                "authenticated": False,
                                "runtime_attachment_sha256": hashlib.sha256(
                                    retained
                                ).hexdigest(),
                            }:
                                raise ValueError(
                                    "retained attachment acknowledgement mismatch"
                                )
                            feeds = [
                                r
                                for r in raw["records"]
                                if r.get("kind") == "late_device_attachment"
                            ]
                            if len(feeds) > 1 or (feeds and device_feed is None):
                                raise ValueError("unexpected late device feed")
                            if feeds:
                                feed = feeds[0]
                                feed_index = raw["records"].index(feed)
                                if (
                                    feed_index + 1 >= len(raw["records"])
                                    or raw["records"][feed_index + 1].get("kind")
                                    != "late_attach"
                                ):
                                    raise ValueError("late device diagnostic missing")
                                validator.data, validator.value = (
                                    retained,
                                    row["attachment"],
                                )
                                validator.acknowledged = True
                                validator.acknowledged_ns = feed["acknowledged_ns"]
                                data = feed["feed_bytes"].encode("utf-8")
                                value = strict_document(data)
                                validator.validate_feed(value)
                                if (
                                    value != feed["feed"]
                                    or snapshot(Path(device_feed), 65536) != data
                                ):
                                    raise ValueError(
                                        "retained late device feed mismatch"
                                    )
                                if (
                                    feed["continuity"] != "unverified"
                                    or feed["early_event_coverage"] != "unavailable"
                                ):
                                    raise ValueError("late device coverage overclaim")
                                sources = feed["sources"]
                                if [
                                    {"path": s["path"], "sha256": s["sha256"]}
                                    for s in sources
                                ] != value["sources"]:
                                    raise ValueError(
                                        "late device source inventory mismatch"
                                    )
                                for source in sources:
                                    if (
                                        hashlib.sha256(
                                            source["text"].encode("utf-8")
                                        ).hexdigest()
                                        != source["sha256"]
                                    ):
                                        raise ValueError(
                                            "late device source digest mismatch"
                                        )
                if time.monotonic() >= deadline:
                    raise TimeoutError("publication validation deadline")
                result["coordination"] = "published"
                break
    except (OSError, ValueError, KeyError, TypeError, TimeoutError) as error:
        result["reason"] = type(error).__name__ + ": " + str(error)
    finally:

        def cleanup(label, operation):
            try:
                return operation()
            except BaseException as error:  # noqa: BLE001 - cleanup must reach report
                result["cleanup_errors"].append(
                    {
                        "operation": label,
                        "error": type(error).__name__ + ": " + str(error),
                    }
                )
                result["coordination"] = "incomplete"
                return None

        owned_fds, reader, writer = (("reader", reader), ("writer", writer)), None, None
        for name, fd in owned_fds:
            if fd is not None:
                cleanup(name + " close", lambda fd=fd: os.close(fd))
        if process is not None:
            result["descendant_cleanup"] = "unconfirmed"

            def kill_group():
                # poll() may reap the leader. Its numeric PGID is no longer
                # owned after that; descendants require separate supervision.
                if process.poll() is not None:
                    return
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass

            cleanup("process group kill", kill_group)
            result["returncode"] = cleanup(
                "child reap", lambda: process.wait(timeout=2)
            )
            for name in ("stdout", "stderr"):
                stream = getattr(process, name)
                setattr(process, name, None)
                if stream is not None:
                    cleanup(name + " close", stream.close)
        cleanup("selector close", selector.close)
        while logs:
            name, stream = logs.popitem()
            cleanup(name + " log flush", stream.flush)
            cleanup(name + " log sync", lambda stream=stream: os.fsync(stream.fileno()))
            cleanup(name + " log close", stream.close)
        paths = cleanup("artifact inventory", lambda: list(output.iterdir())) or []
        for path in paths:

            def hash_artifact(path=path):
                if path.is_file():
                    result["artifacts"][path.name] = hashlib.sha256(
                        path.read_bytes()
                    ).hexdigest()

            cleanup("artifact hash " + path.name, hash_artifact)
        cleanup(
            "final report publication",
            lambda: save(output / "supervisor.json", encoded(result)),
        )
    return result


def observer_identity(binding):
    """Identify this persistent Linux owner independently of the future pod."""
    boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    fields = Path("/proc/self/stat").read_text().rsplit(")", 1)[1].split()
    return {
        "schema_version": "1.0.0",
        "runner_binding": binding,
        "observer_id": str(uuid.uuid4()),
        "host_boot_id": boot,
        "pid": os.getpid(),
        "process_start_ticks": int(fields[19]),
        "collector_sha256": hashlib.sha256(
            Path(__file__).with_name("observer.py").read_bytes()
        ).hexdigest(),
        "supervisor_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "clock_domain": "host_monotonic",
        "created_ns": time.monotonic_ns(),
        "authenticated": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runner-binding", "binding", "scenario", "ready-output", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--runtime-attachment", type=Path)
    parser.add_argument("--attachment-ready", type=Path)
    parser.add_argument("--device-feed", type=Path)
    parser.add_argument("--provider-ready", type=Path)
    args = parser.parse_args()
    result = supervise(
        args.runner_binding,
        args.binding,
        args.scenario,
        args.ready_output,
        args.output,
        args.timeout,
        runtime_attachment=args.runtime_attachment,
        attachment_ready=args.attachment_ready,
        device_feed=args.device_feed,
        provider_ready=args.provider_ready,
    )
    print(json.dumps(result))
    # Publication is coordination only. Every outcome remains acceptance Incomplete.
    return 2


if __name__ == "__main__":
    sys.exit(main())
