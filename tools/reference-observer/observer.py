"""Bounded read-only evdev collector; observations never assert session ownership."""

import argparse
import ctypes
import errno
import fcntl
import hashlib
import json
import os
import re
import select
import stat
import struct
import sys
import tempfile
import time
import uuid
from pathlib import Path

MAX_BYTES = 1_048_576
MAX_EVENTS = 4096
MAX_BINDING_BYTES = 8192
RUNNER_FIELDS = {
    "schema_version",
    "run_id",
    "session_id",
    "scenario_id",
    "scenario_sha256",
    "host_version",
    "test_client_version",
    "nonce",
}
CHECKS = (
    "pairing_proof_flow",
    "pairing_invalid_proofs_rejected",
    "pairing_official_client_cross_check",
    "teardown_session_terminated",
    "teardown_controllers_released",
    "teardown_resources_reclaimed",
)


def identifier(value):
    if not isinstance(value, str) or not re.fullmatch(
        r"[A-Za-z0-9][A-Za-z0-9._:-]{0,127}", value
    ):
        raise ValueError("invalid identifier")
    return value


class Decoder:
    """Decode Linux input_event with explicit word size and byte order."""

    def __init__(self, word_bytes, byteorder):
        if word_bytes not in (4, 8) or byteorder not in ("little", "big"):
            raise ValueError("unsupported input_event ABI")
        self.layout = struct.Struct(
            ("<" if byteorder == "little" else ">")
            + ("ii" if word_bytes == 4 else "qq")
            + "HHi"
        )
        self.pending = bytearray()
        self.count = 0

    def feed(self, data):
        if len(data) > self.layout.size * 64:
            raise ValueError("read quota exceeded")
        self.pending.extend(data)
        result = []
        while len(self.pending) >= self.layout.size:
            sec, usec, kind, code, value = self.layout.unpack_from(self.pending)
            del self.pending[: self.layout.size]
            self.count += 1
            if sec < 0 or not 0 <= usec < 1_000_000 or self.count > MAX_EVENTS:
                raise ValueError("invalid time or event quota exceeded")
            result.append(
                {
                    "kernel_seconds": sec,
                    "kernel_microseconds": usec,
                    "type": kind,
                    "code": code,
                    "value": value,
                }
            )
        return result

    def finish(self):
        if self.pending:
            raise ValueError("truncated input_event")


class Correlator:
    """Match diagnostic candidates without asserting authenticated ownership."""

    def __init__(self, session, scenario):
        self.session = identifier(session)
        self.scenario = identifier(scenario["id"])
        pads = scenario.get("controllers", [])
        if len(pads) != 1 or pads[0]["id"] != 0:
            raise ValueError("this observer supports exactly controller 0")
        self.actions = pads[0]["actions"]
        if not 1 <= len(self.actions) <= 128:
            raise ValueError("action quota")
        ids = [identifier(a["event_id"]) for a in self.actions]
        if len(set(ids)) != len(ids):
            raise ValueError("duplicate event id")
        self.matches = []
        self.errors = []
        self.device = None
        self.seen = set()
        self.axes = {}

    def observe(self, record):
        if record.get("session_id") != self.session:
            raise ValueError("foreign session")
        kind = record["kind"]
        device = record.get("device")
        if kind in ("error", "overflow", "late_attach"):
            self.errors.append(kind)
        elif kind == "attach":
            if self.device is not None or device in self.seen:
                self.errors.append("ambiguous or reused device")
            self.seen.add(device)
            self.device = device
            self.axes = dict(record.get("axes", {}))
            self.match("connect")
        elif kind == "remove":
            if device != self.device:
                self.errors.append("foreign removal")
            else:
                self.match("disconnect")
                self.device = None
        elif kind == "evdev":
            if device != self.device:
                self.errors.append("unbound event")
            elif record["type"] == 0 and record["code"] == 3:
                self.errors.append("SYN_DROPPED")
            elif record["type"] == 3 and record["code"] in (0, 1):
                self.axes[str(record["code"])] = record["value"]
            elif record["type"] == 0 and record["code"] == 0:
                self.match("stick")

    def match(self, kind):
        if self.errors or len(self.matches) == len(self.actions):
            return
        action = self.actions[len(self.matches)]
        if action["type"] != kind:
            # A report without a state change is ordinary; lifecycle reordering is not.
            if kind != "stick":
                self.errors.append("reordered lifecycle")
            return
        if kind == "stick":
            if action.get("stick") != "left" or set(self.axes) != {"0", "1"}:
                return
            # Wolf mapping needs independent calibration before candidates are evidence.
            expected = [round(action[k] * 32767) for k in ("x", "y")]
            if any(abs(self.axes[str(i)] - expected[i]) > 1 for i in range(2)):
                return
        self.matches.append(action["event_id"])


def encoded(value):
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def path_parent(path):
    """Require a canonical parent, then pin it for relative no-follow operations."""
    path = Path(path).absolute()
    if ".." in path.parts or path.parent.resolve(strict=True) != path.parent:
        raise ValueError("path aliases or symlink parents forbidden")
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        require_parent(path, directory)
        return path, directory
    except BaseException:
        os.close(directory)
        raise


def require_parent(path, directory):
    before = path.parent.stat(follow_symlinks=False)
    opened = os.fstat(directory)
    if path.parent.resolve(strict=True) != path.parent or (
        before.st_dev,
        before.st_ino,
    ) != (opened.st_dev, opened.st_ino):
        raise ValueError("parent changed during acquisition/publication")


def file_identity(info):
    return (
        info.st_dev,
        info.st_ino,
        info.st_size,
        info.st_mtime_ns,
        info.st_ctime_ns,
        info.st_nlink,
    )


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON field")
        result[key] = value
    return result


def read_runner_binding(path, collector_binding):
    """Take one bounded snapshot. The Rust scenario hash is an opaque token."""
    path, directory = path_parent(path)
    fd = None
    try:
        before = os.stat(path.name, dir_fd=directory, follow_symlinks=False)
        if (
            not stat.S_ISREG(before.st_mode)
            or before.st_nlink != 1
            or not 0 < before.st_size <= MAX_BINDING_BYTES
        ):
            raise ValueError("runner binding must be a bounded regular unaliased file")
        fd = os.open(
            path.name, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW, dir_fd=directory
        )
        opened = os.fstat(fd)
        if file_identity(before) != file_identity(opened):
            raise ValueError("runner binding changed before read")
        data = bytearray()
        while len(data) <= MAX_BINDING_BYTES:
            part = os.read(fd, MAX_BINDING_BYTES + 1 - len(data))
            if not part:
                break
            data.extend(part)
        after = os.stat(path.name, dir_fd=directory, follow_symlinks=False)
        # Timestamps can miss a same-size rewrite within one coarse clock tick
        # (Linux), so compare the bytes themselves as well.
        reread = os.pread(fd, MAX_BINDING_BYTES + 1, 0)
        if (
            len(data) != opened.st_size
            or reread != data
            or len(data) > MAX_BINDING_BYTES
            or file_identity(opened) != file_identity(os.fstat(fd))
            or file_identity(opened) != file_identity(after)
        ):
            raise ValueError("runner binding changed during read")
        require_parent(path, directory)
    finally:
        if fd is not None:
            os.close(fd)
        os.close(directory)
    binding = json.loads(data, object_pairs_hook=unique_object)
    if not isinstance(binding, dict) or set(binding) != RUNNER_FIELDS:
        raise ValueError("runner binding schema fields mismatch")
    if any(
        not isinstance(value, str) or not 1 <= len(value) <= 1024
        for value in binding.values()
    ):
        raise ValueError("runner binding field type or size")
    if binding["schema_version"] != "1.0.0":
        raise ValueError("runner binding schema version")
    for key in ("run_id", "session_id", "scenario_id"):
        identifier(binding[key])
    nonce = uuid.UUID(binding["nonce"])
    if nonce.version != 4 or str(nonce) != binding["nonce"]:
        raise ValueError("runner binding requires canonical UUID-v4 nonce")
    if not re.fullmatch(r"[0-9a-f]{64}", binding["scenario_sha256"]):
        raise ValueError("runner binding digest format")
    for key in ("session_id", "scenario_id", "host_version", "test_client_version"):
        if binding[key] != collector_binding.get(key):
            raise ValueError(f"runner/collector {key} mismatch")
    return binding


def require_absent(path, directory):
    try:
        os.stat(path.name, dir_fd=directory, follow_symlinks=False)
    except FileNotFoundError:
        return
    raise ValueError("READY destination must be absent")


def prepare_readiness(source, destination, collector_binding):
    binding = read_runner_binding(source, collector_binding)
    path, directory = path_parent(destination)
    try:
        require_absent(path, directory)
    finally:
        os.close(directory)
    return path, binding


def rename_noreplace(directory, source, destination):
    """Atomically expose one inode, without replacing any existing directory entry."""
    system = os.uname().sysname
    if system == "Darwin":
        symbol, flag = "renameatx_np", 0x00000004  # RENAME_EXCL
    elif system == "Linux":
        symbol, flag = "renameat2", 1  # RENAME_NOREPLACE
    else:
        raise OSError(errno.ENOTSUP, "atomic no-replace rename unavailable")
    library = ctypes.CDLL(None, use_errno=True)
    try:
        rename = getattr(library, symbol)
    except AttributeError as error:
        raise OSError(errno.ENOTSUP, "atomic no-replace rename unavailable") from error
    rename.argtypes = [
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_uint,
    ]
    rename.restype = ctypes.c_int
    if rename(
        directory, os.fsencode(source), directory, os.fsencode(destination), flag
    ):
        failure = ctypes.get_errno()
        raise OSError(failure, os.strerror(failure), destination)


def publish_ready(destination, binding):
    """Commit at visibility; return whether subsequent directory syncing succeeded."""
    data = encoded({"schema_version": "1.0.0", "state": "ready", "binding": binding})
    return publish_envelope(destination, data)


def publish_envelope(destination, data):
    """Publish a bounded immutable coordination envelope."""
    if len(data) > 16384:
        raise ValueError("READY envelope quota")
    path, directory = path_parent(destination)
    temporary = f".observer-ready-{uuid.uuid4().hex}.pending"
    fd, published = None, False
    try:
        require_absent(path, directory)
        fd = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
            0o600,
            dir_fd=directory,
        )
        remaining = memoryview(data)
        while remaining:
            written = os.write(fd, remaining)
            if written <= 0:
                raise OSError("short READY write")
            remaining = remaining[written:]
        os.fsync(fd)
        # Finish every mutation and writable-descriptor operation before visibility.
        closing, fd = fd, None
        os.close(closing)
        require_parent(path, directory)
        rename_noreplace(directory, temporary, path.name)
        published = True
        # The runner can already retain READY. Housekeeping cannot revoke it.
        try:
            os.fsync(directory)
        except OSError:
            return False
        return True
    finally:
        try:
            if fd is not None:
                try:
                    os.close(fd)
                except OSError:
                    pass
            if not published:
                try:
                    os.unlink(temporary, dir_fd=directory)
                except OSError:
                    pass
        finally:
            try:
                os.close(directory)
            except OSError:
                if not published:
                    raise


def publish(root, binding, scenario, records, provenance):
    """Commit the manifest last in a fresh directory; partial runs lack it."""
    if provenance not in ("reference_host", "synthetic_fixture"):
        raise ValueError("unsupported provenance")
    correlation = Correlator(binding["session_id"], scenario)
    if binding["scenario_id"] != correlation.scenario:
        raise ValueError("scenario mismatch")
    for key in (
        "host_version",
        "test_client_version",
        "test_client_revision",
        "official_client_version",
        "runtime_identity",
    ):
        if not isinstance(binding.get(key), str) or not 1 <= len(binding[key]) <= 1024:
            raise ValueError("missing version/runtime binding")
    if len(records) > MAX_EVENTS:
        raise ValueError("record quota")
    for record in records:
        correlation.observe(record)
    raw = encoded(
        {
            "provenance": provenance,
            "binding": binding,
            "scenario_sha256": hashlib.sha256(encoded(scenario)).hexdigest(),
            "records": records,
            "diagnostic_matches": correlation.matches,
            "errors": correlation.errors,
        }
    )
    if len(raw) > MAX_BYTES:
        raise ValueError("artifact quota")
    reason = (
        "evdev has no authenticated session/event ownership bridge; "
        "diagnostic matches cannot establish delivery"
    )
    rows = []
    for n, action in enumerate(correlation.actions):
        rows.append(
            {
                "schema_version": "1.0.0",
                "id": f"observer-{n}",
                "session_id": correlation.session,
                "provenance": provenance,
                "source": "host_pipeline",
                "kind": "input",
                "event_id": action["event_id"],
                "controller_id": 0,
                "boundary": "host_received",
                "authenticated": False,
                "clock_domain": None,
                "source_clock": None,
                "observed_at": None,
                "population": {
                    "expected": 1,
                    "observed": 0,
                    "represented": 0,
                    "excluded": 0,
                    "missing": 1,
                    "overflow": None,
                    "failed_delivery": None,
                },
                "coverage": {
                    "state": "unavailable",
                    "enabled_intervals": [],
                    "gaps": [],
                    "reason": reason,
                },
                "validity": "unavailable",
                "reason": reason,
                "artifact_refs": ["raw.json"],
                "measurements": [],
            }
        )
    host_bytes = b"".join(map(encoded, rows))
    manifest = {
        key: binding[key]
        for key in (
            "session_id",
            "scenario_id",
            "host_version",
            "test_client_version",
            "test_client_revision",
            "official_client_version",
        )
    }
    manifest.update(
        schema_version="1.0.0",
        provenance=provenance,
        artifacts=[
            {"path": "raw.json", "sha256": hashlib.sha256(raw).hexdigest()},
            {"path": "host.jsonl", "sha256": hashlib.sha256(host_bytes).hexdigest()},
        ],
        observations=[
            {
                "check": check,
                "satisfied": None,
                "validity": "unavailable",
                "reason": (
                    "read-only evdev acquisition cannot attest "
                    "this complete lifecycle check"
                ),
                "artifacts": ["raw.json"],
            }
            for check in CHECKS
        ],
    )
    root = Path(root)
    root.mkdir(mode=0o700, parents=False, exist_ok=False)
    for name, data in (
        ("raw.json", raw),
        ("host.jsonl", host_bytes),
        ("manifest.json", encoded(manifest)),
    ):
        fd, temporary = tempfile.mkstemp(prefix=".publish-", dir=root)
        try:
            with os.fdopen(fd, "wb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, root / name)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
    directory = os.open(root, os.O_RDONLY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    return rows, manifest


def inventory():
    result = {}
    for path in Path("/sys/class/input").glob("event*/device/name"):
        with path.open() as stream:
            name = stream.read(129).strip()
        if name != "Wolf X-Box One (virtual) pad":
            continue
        device = Path("/dev/input") / path.parts[-3]
        info = device.stat()
        if not stat.S_ISCHR(info.st_mode):
            raise ValueError("evdev path is not a character device")
        result[str(device)] = (info.st_dev, info.st_ino, info.st_rdev)
        if len(result) > 4:
            raise ValueError("device quota exceeded")
    return result


def read_axes(fd):
    """Linux EVIOCGABS(ABS_X/Y), asm-generic _IOR('E', 0x40+axis, input_absinfo)."""
    axes = {}
    for axis in (0, 1):
        data = bytearray(24)
        fcntl.ioctl(fd, 0x80184540 + axis, data, True)
        value, minimum, maximum, _, _, _ = struct.unpack("=6i", data)
        if (minimum, maximum) != (-32768, 32767):
            raise ValueError("unsupported axis range")
        axes[str(axis)] = value
    return axes


class FinishControl:
    """One bounded JSON line, committed by EOF on an inherited nonblocking pipe."""

    def __init__(self, descriptor, binding):
        if type(descriptor) is not int or descriptor <= 2:
            raise ValueError("finish descriptor must be an inherited pipe above stdio")
        flags = fcntl.fcntl(descriptor, fcntl.F_GETFL)
        if (
            not stat.S_ISFIFO(os.fstat(descriptor).st_mode)
            or flags & os.O_ACCMODE != os.O_RDONLY
            or not flags & os.O_NONBLOCK
        ):
            raise ValueError("finish descriptor must be a nonblocking read-only pipe")
        self.descriptor = descriptor
        self.binding = binding
        self.pending = bytearray()
        self.started = None

    def preflight(self):
        try:
            data = os.read(self.descriptor, 1)
        except BlockingIOError:
            return
        raise ValueError("premature finish input" if data else "premature control EOF")

    def poll(self):
        if self.started is not None and time.monotonic() - self.started >= 1:
            raise ValueError("finish command/EOF deadline exceeded")
        try:
            data = os.read(self.descriptor, 16385 - len(self.pending))
        except BlockingIOError:
            return False
        if data:
            if self.started is None:
                self.started = time.monotonic()
            self.pending.extend(data)
            if len(self.pending) > 16384:
                raise ValueError("finish command quota exceeded")
            return False
        if not self.pending:
            raise ValueError("control EOF before finish command")
        if self.pending.count(b"\n") != 1 or not self.pending.endswith(b"\n"):
            raise ValueError("finish requires exactly one JSON line then EOF")
        try:
            message = self.pending.decode("utf-8", errors="strict")
            if message.startswith("\ufeff"):
                raise ValueError("finish command UTF-8 BOM forbidden")
            command = json.loads(message, object_pairs_hook=unique_object)
        except RecursionError as error:
            raise ValueError("finish JSON nesting exceeds parser limit") from error
        if command != {
            "schema_version": "1.0.0",
            "command": "finish",
            "binding": self.binding,
        }:
            raise ValueError("finish command or exact runner binding mismatch")
        return True


DEPLOYED_REVISION = "5c8251c8a7a547466226c5f08f72d94a5de05dd0"


def exact_fields(value, fields):
    if not isinstance(value, dict) or set(value) != set(fields.split()):
        raise ValueError("attachment schema fields mismatch")


def natural(value):
    if type(value) is not int or not 0 <= value < 2**63:
        raise ValueError("attachment integer invalid")
    return value


def digest_value(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
        raise ValueError("attachment digest invalid")


def strict_document(data):
    if not 0 < len(data) <= 65536:
        raise ValueError("attachment size invalid")
    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=unique_object)
    except (UnicodeError, RecursionError) as error:
        raise ValueError("attachment encoding or depth invalid") from error


class RuntimeAttachment:
    """One immutable supplied association; it never authenticates ownership.

    The source adapter must publish a complete candidate once. This first slice
    rejects device loss and any changed attachment instead of merging epochs.
    """

    def __init__(
        self, identity, binding, path, ready, launch_marker=None, device_feed=None
    ):
        exact_fields(
            identity,
            "schema_version runner_binding observer_id host_boot_id pid "
            "process_start_ticks collector_sha256 supervisor_sha256 clock_domain "
            "created_ns authenticated",
        )
        observer_id = uuid.UUID(identity["observer_id"])
        if (
            observer_id.version != 4
            or str(observer_id) != identity["observer_id"]
            or identity["runner_binding"] != binding
            or identity["schema_version"] != "1.0.0"
            or identity["authenticated"] is not False
            or identity["clock_domain"] != "host_monotonic"
        ):
            raise ValueError("observer identity mismatch")
        identifier(identity["host_boot_id"])
        for key in ("pid", "process_start_ticks", "created_ns"):
            natural(identity[key])
        if identity["pid"] == 0:
            raise ValueError("observer PID missing")
        for key in ("collector_sha256", "supervisor_sha256"):
            digest_value(identity[key])
        self.identity, self.binding = identity, binding
        self.path, self.ready = Path(path), Path(ready)
        self.launch_path = (
            Path(launch_marker)
            if launch_marker
            else self.path.parent / "observer-launch.json"
        )
        self.launch = None
        self.data = self.value = None
        self.file_identity = None
        self.devices = {}
        self.sources = []
        self.acknowledged = False
        self.acknowledged_ns = None
        self.feed_path = Path(device_feed) if device_feed is not None else None
        self.feed_data = self.feed_value = self.feed_identity = None
        self.feed_sources = []
        self.feed_emitted = False
        if self.feed_path is not None:
            from supervisor import absent

            absent(self.feed_path)

    def validate(self, value):
        exact_fields(
            value,
            "schema_version state runner_binding launch_nonce observer_id observed_ns "
            "association "
            "authenticated source_revision cluster_identity session deployment "
            "replicaset pod containers wolf_session_id devices sources",
        )
        if (
            value["schema_version"] != "1.0.0"
            or value["state"] != "attached"
            or value["runner_binding"] != self.binding
            or value["observer_id"] != self.identity["observer_id"]
            or value["association"] != "exclusive_launch_candidate"
            or value["authenticated"] is not False
            or value["source_revision"] != DEPLOYED_REVISION
            or self.launch is None
            or value["launch_nonce"] != self.launch["launch_nonce"]
        ):
            raise ValueError("runtime attachment binding mismatch")
        natural(value["observed_ns"])
        if (
            not self.identity["created_ns"]
            <= value["observed_ns"]
            <= time.monotonic_ns()
        ):
            raise ValueError("runtime attachment clock mismatch")
        identifier(value["cluster_identity"])
        identifier(value["wolf_session_id"])
        session = value["session"]
        exact_fields(
            session,
            "namespace name uid resource_version pairing_name user_name app_name",
        )
        for item in session.values():
            identifier(item)
        parent = session
        for kind, parent_kind in (
            ("deployment", "Session"),
            ("replicaset", "Deployment"),
            ("pod", "ReplicaSet"),
        ):
            item = value[kind]
            fields = "namespace name uid resource_version owner_references"
            if kind == "pod":
                fields += " node_name node_uid"
            exact_fields(item, fields)
            for key in set(item) - {"owner_references"}:
                identifier(item[key])
            if item["namespace"] != session["namespace"] or item[
                "owner_references"
            ] != [
                {
                    "kind": parent_kind,
                    "name": parent["name"],
                    "uid": parent["uid"],
                    "controller": True,
                }
            ]:
                raise ValueError("runtime owner chain mismatch")
            if item["owner_references"][0]["controller"] is not True:
                raise ValueError("runtime controller reference invalid")
            parent = item
        containers = value["containers"]
        if not isinstance(containers, list) or not 3 <= len(containers) <= 8:
            raise ValueError("runtime containers missing")
        names = set()
        for item in containers:
            exact_fields(item, "name container_id image_id restart_count started_at")
            identifier(item["name"])
            if item["name"] in names:
                raise ValueError("duplicate runtime container")
            names.add(item["name"])
            for key in ("container_id", "image_id", "started_at"):
                if not isinstance(item[key], str) or not 1 <= len(item[key]) <= 512:
                    raise ValueError("runtime container identity invalid")
            if not re.search(r"sha256:[0-9a-f]{64}$", item["image_id"]):
                raise ValueError("runtime image digest missing")
            natural(item["restart_count"])
        if not {"wolf-agent", "wolf"} <= names:
            raise ValueError("agent/Wolf container missing")
        devices = value["devices"]
        if not isinstance(devices, list) or not 0 <= len(devices) <= 4:
            raise ValueError("runtime device inventory invalid")
        paths, generations = set(), set()
        for item in devices:
            exact_fields(item, "session_id path major minor generation")
            if (
                item["session_id"] != value["wolf_session_id"]
                or not isinstance(item["path"], str)
                or not re.fullmatch(r"/dev/input/event[0-9]+", item["path"])
            ):
                raise ValueError("foreign runtime device")
            for key in ("major", "minor", "generation"):
                natural(item[key])
            if item["path"] in paths or item["generation"] in generations:
                raise ValueError("device path or generation reuse")
            paths.add(item["path"])
            generations.add(item["generation"])
        sources = value["sources"]
        if not isinstance(sources, list) or not 1 <= len(sources) <= 8:
            raise ValueError("runtime sources missing")
        seen = set()
        for item in sources:
            exact_fields(item, "path sha256")
            if (
                not isinstance(item["path"], str)
                or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", item["path"])
                or item["path"] in seen
            ):
                raise ValueError("runtime source path invalid")
            seen.add(item["path"])
            digest_value(item["sha256"])

    def await_attachment(self, deadline, control):
        # Import lazily: supervisor already imports the collector's protocol helpers.
        from supervisor import snapshot

        while time.monotonic() < deadline:
            if control.poll():
                raise ValueError("finish before runtime attachment")
            if self.launch is None:
                try:
                    launch = strict_document(snapshot(self.launch_path, 16384))
                except FileNotFoundError:
                    if self.path.exists():
                        raise ValueError(
                            "runtime attachment arrived before launch marker"
                        )
                    select.select([control.descriptor], [], [], 0.02)
                    continue
                exact_fields(launch, "schema_version state binding launch_nonce")
                nonce = uuid.UUID(launch["launch_nonce"])
                if (
                    launch["schema_version"] != "1.0.0"
                    or launch["state"] != "launched"
                    or launch["binding"] != self.binding
                    or nonce.version != 4
                    or str(nonce) != launch["launch_nonce"]
                ):
                    raise ValueError("launch marker mismatch")
                self.launch = launch
            try:
                data, acquired_identity = snapshot(self.path, 65536, with_identity=True)
            except FileNotFoundError:
                select.select([control.descriptor], [], [], 0.02)
                continue
            value = strict_document(data)
            self.validate(value)
            sources = []
            for source in value["sources"]:
                raw = snapshot(self.path.parent / source["path"], 65536)
                if hashlib.sha256(raw).hexdigest() != source["sha256"]:
                    raise ValueError("runtime source digest mismatch")
                # Sources must be reviewed redacted UTF-8 observations.
                sources.append({**source, "text": raw.decode("utf-8")})
            self.file_identity = acquired_identity
            self.data, self.value, self.sources = data, value, sources
            self.inventory()
            if time.monotonic() >= deadline or control.poll():
                raise ValueError("runtime attachment outside acquisition window")
            return
        raise ValueError("runtime attachment deadline")

    def acknowledge(self):
        if not self.acknowledged:
            if self.feed_path is not None:
                from supervisor import absent

                absent(self.feed_path)
                if self.value["devices"]:
                    raise ValueError(
                        "late device feed requires empty runtime inventory"
                    )
            self.acknowledged_ns = time.monotonic_ns()
            publish_envelope(
                self.ready,
                encoded(
                    {
                        "schema_version": "1.0.0",
                        "state": "attached",
                        "binding": self.binding,
                        "launch_nonce": self.launch["launch_nonce"],
                        "observer_id": self.identity["observer_id"],
                        "association": "exclusive_launch_candidate",
                        "authenticated": False,
                        "runtime_attachment_sha256": hashlib.sha256(
                            self.data
                        ).hexdigest(),
                    }
                ),
            )
            self.acknowledged = True

    def validate_feed(self, value):
        exact_fields(
            value,
            "schema_version state runner_binding launch_nonce observer_id "
            "runtime_attachment_sha256 observed_ns continuity devices sources",
        )
        if (
            value["schema_version"] != "1.0.0"
            or value["state"] != "devices_attached"
            or value["runner_binding"] != self.binding
            or value["launch_nonce"] != self.launch["launch_nonce"]
            or value["observer_id"] != self.identity["observer_id"]
            or value["runtime_attachment_sha256"]
            != hashlib.sha256(self.data).hexdigest()
            or value["continuity"] != "unverified"
            or not self.acknowledged
            or self.value["devices"]
            or natural(self.acknowledged_ns) < self.value["observed_ns"]
            or natural(value["observed_ns"]) < self.acknowledged_ns
            or not isinstance(value["devices"], list)
            or not value["devices"]
        ):
            raise ValueError("late device feed binding or boundary mismatch")
        self.validate(
            self.value
            | {key: value[key] for key in ("devices", "sources", "observed_ns")}
        )

    def poll_feed(self):
        from supervisor import snapshot

        if (
            self.feed_path is None
            or not self.acknowledged
            or self.feed_data is not None
        ):
            return
        try:
            data, identity = snapshot(self.feed_path, 65536, with_identity=True)
        except FileNotFoundError:
            return
        value = strict_document(data)
        self.validate_feed(value)
        sources = []
        for source in value["sources"]:
            raw = snapshot(self.feed_path.parent / source["path"], 65536)
            if hashlib.sha256(raw).hexdigest() != source["sha256"]:
                raise ValueError("late device source digest mismatch")
            sources.append({**source, "text": raw.decode("utf-8")})
        self.feed_data, self.feed_identity = data, identity
        self.feed_value, self.feed_sources = value, sources

    def feed_evidence(self):
        return {
            "feed": self.feed_value,
            "feed_bytes": self.feed_data.decode("utf-8"),
            "sources": self.feed_sources,
            "acknowledged_ns": self.acknowledged_ns,
            "continuity": "unverified",
            "early_event_coverage": "unavailable",
        }

    def inventory(self):
        from supervisor import snapshot

        if (
            snapshot(self.path, 65536) != self.data
            or file_identity(self.path.stat(follow_symlinks=False))
            != self.file_identity
        ):
            raise ValueError("runtime attachment changed")
        if self.feed_data is not None and (
            snapshot(self.feed_path, 65536) != self.feed_data
            or file_identity(self.feed_path.stat(follow_symlinks=False))
            != self.feed_identity
        ):
            raise ValueError("late device feed changed")
        result = {}
        for device in (self.feed_value or self.value)["devices"]:
            if (
                Path(device["path"]).parent.resolve(strict=True)
                != Path(device["path"]).parent
            ):
                raise ValueError("runtime device parent alias")
            info = os.stat(device["path"], follow_symlinks=False)
            if not stat.S_ISCHR(info.st_mode) or (
                os.major(info.st_rdev),
                os.minor(info.st_rdev),
            ) != (device["major"], device["minor"]):
                raise ValueError("runtime device descriptor mismatch")
            identity = (info.st_dev, info.st_ino, info.st_rdev)
            if (
                device["path"] in self.devices
                and self.devices[device["path"]] != identity
            ):
                raise ValueError("runtime device incarnation changed")
            result[device["path"]] = identity
        self.devices = result
        return result

    def evidence(self):
        return {
            "observer_identity": self.identity,
            "launch_marker": self.launch,
            "attachment": self.value,
            "attachment_bytes": self.data.decode("utf-8"),
            "sources": self.sources,
            "device_snapshot": self.devices,
            "continuity": "unverified",
            "device_acquisition": "supplied_initial_devices"
            if self.value["devices"]
            else "unavailable_pending_later_device_feed",
        }


def collect_controlled(session, duration, control, attachment=None):
    records, opened = [], {}

    def devices():
        return {device: list(entry[2]) for device, entry in opened.items()}

    def emit(kind, **fields):
        if len(records) >= MAX_EVENTS:
            raise ValueError("record quota exceeded")
        records.append(
            dict(
                session_id=session, kind=kind, observer_ns=time.monotonic_ns(), **fields
            )
        )

    def boundary(final=False):
        if attachment is not None:
            attachment.poll_feed()
            if attachment.feed_data is not None and not attachment.feed_emitted:
                emit("late_device_attachment", **attachment.feed_evidence())
                emit(
                    "late_attach",
                    reason="device feed acquired after runtime acknowledgement",
                )
                attachment.feed_emitted = True
        current = attachment.inventory() if attachment is not None else inventory()
        for device, (fd, decoder, identity) in list(opened.items()):
            if current.get(device) != identity:
                decoder.finish()
                emit("remove", device=device)
                del opened[device]
                os.close(fd)
        for device, identity in current.items():
            if device in opened:
                continue
            fd = os.open(device, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
            try:
                info = os.fstat(fd)
                if (info.st_dev, info.st_ino, info.st_rdev) != identity:
                    raise ValueError("device changed during open")
                axes = read_axes(fd)
            except BaseException:
                os.close(fd)
                raise
            opened[device] = (
                fd,
                Decoder(struct.calcsize("l"), sys.byteorder),
                identity,
            )
            emit("attach", device=device, identity=identity, axes=axes)
        if attachment is not None:
            # Empty inventory permits RTSP/controller creation. A separate feed
            # can later identify exact devices without mutating runtime identity.
            if not attachment.acknowledged and (
                time.monotonic() >= deadline or control.poll()
            ):
                raise ValueError(
                    "attachment cancelled or expired before device readiness"
                )
            attachment.acknowledge()
            emit(
                "device_snapshot",
                sequence=len(records),
                attachment_sha256=hashlib.sha256(attachment.data).hexdigest(),
                devices=devices(),
                continuity="unverified",
            )
        descriptors = [entry[0] for entry in opened.values()]
        ready, _, _ = select.select(descriptors, [], [], 0)
        for device, (fd, decoder, _) in opened.items():
            if fd in ready:
                data = os.read(fd, decoder.layout.size * 64)
                if not data:
                    decoder.finish()
                    raise ValueError("unexpected evdev EOF")
                for event in decoder.feed(data):
                    emit("evdev", device=device, **event)
        if final:
            # A bounded final read cannot claim a drained boundary with unread input.
            if select.select(descriptors, [], [], 0)[0]:
                raise ValueError("unread input at final acquisition boundary")
            for _, decoder, _ in opened.values():
                decoder.finish()

    phase = "acquisition_error"
    try:
        deadline = time.monotonic() + duration
        if attachment is not None:
            emit(
                "observer_identity",
                identity=attachment.identity,
                continuity="unverified",
            )
            attachment.await_attachment(deadline, control)
            emit("runtime_attachment", **attachment.evidence())
        while time.monotonic() < deadline:
            phase = "acquisition_error"
            boundary()
            phase = "control_error"
            if control.poll():
                phase = "acquisition_error"
                boundary(final=True)
                emit(
                    "end",
                    termination_reason="finish_command",
                    devices=devices(),
                    finish_binding=control.binding,
                )
                return records
            phase = "poll_error"
            select.select(
                [control.descriptor, *[entry[0] for entry in opened.values()]],
                [],
                [],
                0.02,
            )
        phase = "acquisition_error"
        boundary(final=True)
        emit("end", termination_reason="duration_limit", devices=devices())
    except (OSError, ValueError) as error:
        emit(
            "error",
            reason=str(error)[:512],
            termination_reason=phase,
            devices=devices(),
        )
    finally:
        # Pop before close: an uncertain close must not be retried on a reused fd.
        close_errors = []
        while opened:
            _, (fd, _, _) = opened.popitem()
            try:
                os.close(fd)
            except OSError as error:
                close_errors.append(str(error)[:512])
        for reason in close_errors:
            emit("error", reason=reason, termination_reason="close_error")
    return records


def collect(session, duration, readiness=None, finish_fd=None, attachment=None):
    # Ownership of a supplied non-stdio descriptor transfers to this call.
    records = None
    try:
        control = None
        if finish_fd is not None:
            if readiness is None:
                raise ValueError("finish control requires runner readiness binding")
            control = FinishControl(finish_fd, readiness[1])
            control.preflight()
        records = _collect(session, duration, readiness, control, attachment)
    finally:
        if type(finish_fd) is int and finish_fd > 2:
            try:
                os.close(finish_fd)
            except OSError as error:
                # Never retry an uncertain close or mask an earlier exception.
                if records is not None:
                    if len(records) >= MAX_EVENTS:
                        raise ValueError(
                            "record quota exceeded retaining control close error"
                        ) from error
                    records.append(
                        {
                            "session_id": session,
                            "kind": "error",
                            "observer_ns": time.monotonic_ns(),
                            "reason": str(error)[:512],
                            "termination_reason": "control_close_error",
                        }
                    )
    return records


def _collect(session, duration, readiness=None, control=None, attachment=None):
    if (
        sys.platform != "linux"
        or os.uname().machine not in ("x86_64", "aarch64", "arm64")
        or struct.calcsize("l") != 8
        or not 0 < duration <= 300
    ):
        raise ValueError("64-bit x86/ARM Linux and duration in (0,300] required")
    records, opened = [], {}
    identifier(session)

    def emit(kind, **fields):
        if len(records) >= MAX_EVENTS:
            raise ValueError("record quota exceeded")
        records.append(
            dict(
                session_id=session, kind=kind, observer_ns=time.monotonic_ns(), **fields
            )
        )

    if attachment is not None and (readiness is None or control is None):
        raise ValueError("attachment requires readiness and finish control")
    initial = {} if attachment is not None else inventory()
    handshake = {"state": "READY", "session_id": session, "initial_devices": initial}
    if attachment is not None:
        handshake["runtime_inventory"] = "deferred_until_postlaunch_attachment"
    if readiness is not None:
        if initial:
            raise ValueError("READY publication requires an empty initial inventory")
        if control is not None:
            control.preflight()
        handshake["ready_directory_synced"] = publish_ready(*readiness)
    if initial:
        emit("late_attach", devices=list(initial))
    # Read the READY stdout handshake before dispatching actions.
    try:
        print(json.dumps(handshake), flush=True)
    except OSError:
        if readiness is None:
            raise
    if control is not None:
        return collect_controlled(session, duration, control, attachment)
    deadline = time.monotonic() + duration
    try:
        while time.monotonic() < deadline:
            current = inventory()
            for device, (fd, decoder, identity) in list(opened.items()):
                if current.get(device) != identity:
                    decoder.finish()
                    emit("remove", device=device)
                    os.close(fd)
                    del opened[device]
            for device, identity in current.items():
                if device in opened:
                    continue
                fd = os.open(device, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
                try:
                    info = os.fstat(fd)
                    if (info.st_dev, info.st_ino, info.st_rdev) != identity:
                        raise ValueError("device changed during open")
                except BaseException:
                    os.close(fd)
                    raise
                opened[device] = (
                    fd,
                    Decoder(struct.calcsize("l"), sys.byteorder),
                    identity,
                )
                emit("attach", device=device, identity=identity, axes=read_axes(fd))
            ready, _, _ = select.select(
                [entry[0] for entry in opened.values()], [], [], 0.02
            )
            for device, (fd, decoder, _) in opened.items():
                if fd not in ready:
                    continue
                data = os.read(fd, decoder.layout.size * 64)
                if not data:
                    decoder.finish()
                    raise ValueError("unexpected evdev EOF")
                for event in decoder.feed(data):
                    emit("evdev", device=device, **event)
        for _, decoder, _ in opened.values():
            decoder.finish()
        emit("end", devices=list(opened))
    except (OSError, ValueError) as error:
        # Refuse publication when no quota remains for an error record.
        if len(records) >= MAX_EVENTS:
            raise
        emit("error", reason=str(error)[:512])
    finally:
        for fd, _, _ in opened.values():
            os.close(fd)
    return records


def read_json(path):
    with open(path, "rb") as stream:
        data = stream.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise ValueError("JSON input quota")
    return json.loads(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binding", required=True)
    parser.add_argument("--scenario", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--duration", type=float, default=30)
    parser.add_argument(
        "--runner-binding", help="immutable runner observer-binding.json"
    )
    parser.add_argument("--ready-output", help="fresh local runner READY envelope path")
    parser.add_argument(
        "--finish-fd",
        type=int,
        help="owned inherited nonblocking read pipe for one bound finish command",
    )
    parser.add_argument(
        "--fixture", help="portable raw record replay, always synthetic_fixture"
    )
    parser.add_argument("--observer-identity", type=Path)
    parser.add_argument("--runtime-attachment", type=Path)
    parser.add_argument("--attachment-ready", type=Path)
    parser.add_argument("--launch-marker", type=Path)
    parser.add_argument("--device-feed", type=Path)
    args = parser.parse_args()
    attachment_options = (
        args.observer_identity,
        args.runtime_attachment,
        args.attachment_ready,
        args.launch_marker,
    )
    if any(attachment_options) and (
        not all(attachment_options) or args.finish_fd is None
    ):
        parser.error("attachment requires all three paths and finish control")
    if args.device_feed is not None and not all(attachment_options):
        parser.error("device feed requires runtime attachment")
    if (args.runner_binding is None) != (args.ready_output is None):
        parser.error("--runner-binding and --ready-output must be supplied together")
    if args.runner_binding is not None and args.fixture:
        parser.error("--fixture cannot publish runner readiness")
    if args.finish_fd is not None and (
        args.runner_binding is None or args.fixture or args.finish_fd <= 2
    ):
        parser.error(
            "--finish-fd requires runner readiness, descriptor >2, and no fixture"
        )
    binding, scenario = read_json(args.binding), read_json(args.scenario)
    Correlator(binding["session_id"], scenario)
    if binding["scenario_id"] != scenario["id"]:
        raise ValueError("scenario mismatch before acquisition")
    readiness = (
        prepare_readiness(args.runner_binding, args.ready_output, binding)
        if args.runner_binding is not None
        else None
    )
    attachment = None
    if args.observer_identity is not None:
        from supervisor import snapshot

        attachment = RuntimeAttachment(
            strict_document(snapshot(args.observer_identity, 65536)),
            readiness[1],
            args.runtime_attachment,
            args.attachment_ready,
            args.launch_marker,
            args.device_feed,
        )
    records = (
        read_json(args.fixture)
        if args.fixture
        else collect(
            binding["session_id"], args.duration, readiness, args.finish_fd, attachment
        )
    )
    publish(
        args.output,
        binding,
        scenario,
        records,
        "synthetic_fixture" if args.fixture else "reference_host",
    )
    print(
        json.dumps(
            {
                "state": "DONE",
                "session_id": binding["session_id"],
                "output": args.output,
                "acceptance": "Incomplete",
                **(
                    {
                        "finish_status": (
                            "finished"
                            if records
                            and records[-1].get("termination_reason")
                            == "finish_command"
                            else "incomplete"
                        )
                    }
                    if args.finish_fd is not None
                    else {}
                ),
            }
        ),
        flush=True,
    )


if __name__ == "__main__":
    main()
