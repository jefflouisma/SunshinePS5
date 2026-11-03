"""Opt-in SSH subsystem relay. Transport completion never establishes acceptance.

Install this module and its siblings on Linux, then configure an administrator-
owned SSH subsystem named sunshine-reference-observer-v1 to run --serve. The
client never transmits a remote shell command. Configuration travels over stdin.
"""

import argparse
import base64
import hashlib
import json
import math
import os
import re
import selectors
import signal
import subprocess
import sys
import time
import uuid
from pathlib import Path

from observer import (
    encoded,
    path_parent,
    publish_envelope,
    read_runner_binding,
    rename_noreplace,
    require_absent,
    require_parent,
    unique_object,
)
from supervisor import absent, cancel_result, snapshot

SUBSYSTEM = "sunshine-reference-observer-v1"
MODULES = ("remote_relay.py", "supervisor.py", "observer.py", "fenrir_provider.py")
FILE_LIMIT = 1048576
FRAME_LIMIT = 2 * FILE_LIMIT
TOTAL_LIMIT = 16 * FILE_LIMIT
PROVIDER_FIELDS = {
    "kubectl",
    "kubeconfig",
    "context",
    "namespace",
    "agent_port",
    "user",
    "app",
    "pairing",
}
# Order matters: source material precedes the envelope that refers to it.
REMOTE_FILES = (
    "supervised/observer-identity.json",
    "provider/baseline.json",
    "provider/provider-ready.json",
    "ready.json",
    "provider/runtime-source.json",
    "provider/runtime.json",
    "attached.json",
    "provider/devices-source.json",
    "provider/devices.json",
    "supervised/collector-binding.json",
    "supervised/scenario.json",
    "supervised/observer-binding.json",
    "supervised/observer-ready.json",
    "supervised/provider-ready.json",
    "supervised/provider-baseline.json",
    "supervised/observer-cancel.json",
    "supervised/collector/raw.json",
    "supervised/collector/host.jsonl",
    "supervised/collector/manifest.json",
    "supervised/supervisor.json",
    *[f"provider/commands-{index}.json" for index in range(16)],
    "provider/provider-report.json",
)


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def publish_artifact(destination, data):
    """One-MiB artifact publication; coordination keeps its separate 16-KiB limit."""
    require(0 < len(data) <= FILE_LIMIT, "artifact publication quota")
    path, directory = path_parent(destination)
    temporary = f".relay-{uuid.uuid4().hex}.pending"
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
            require(written > 0, "short artifact write")
            remaining = remaining[written:]
        os.fsync(fd)
        owned, fd = fd, None
        os.close(owned)
        require_parent(path, directory)
        rename_noreplace(directory, temporary, path.name)
        published = True
        try:
            os.fsync(directory)
        except OSError:
            return False
        return True
    finally:
        try:
            if fd is not None:
                owned, fd = fd, None
                try:
                    os.close(owned)
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


def parse(data):
    require(0 < len(data) <= FRAME_LIMIT, "frame quota")
    return json.loads(data.decode("utf-8"), object_pairs_hook=unique_object)


def timeout_value(value):
    require(
        type(value) in (int, float) and math.isfinite(value) and 0 < value <= 300,
        "timeout must be finite and between zero and 300 seconds",
    )
    return value


def code_hashes():
    return {
        name: digest(snapshot(Path(__file__).with_name(name), FILE_LIMIT))
        for name in MODULES
    }


def directory_identity(path):
    path = Path(path)
    require(
        path.is_absolute() and str(path) == os.path.normpath(path), "absolute directory"
    )
    # Open every component without following symlinks, including ancestors.
    fd = os.open("/", os.O_RDONLY | os.O_DIRECTORY)
    try:
        for part in path.parts[1:]:
            next_fd = os.open(
                part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd
            )
            os.close(fd)
            fd = next_fd
        value = os.fstat(fd)
        require(
            value.st_uid == os.geteuid() and value.st_mode & 0o077 == 0,
            "private owned directory required",
        )
        return value.st_dev, value.st_ino
    finally:
        os.close(fd)


def private_parents(root, relative):
    """Create only literal child directories through pinned no-follow descriptors."""
    fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        for part in Path(relative).parts:
            require(part not in {"", ".", "..", "/"}, "child directory name")
            try:
                os.mkdir(part, mode=0o700, dir_fd=fd)
            except FileExistsError:
                pass
            next_fd = os.open(
                part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd
            )
            os.close(fd)
            fd = next_fd
            info = os.fstat(fd)
            require(
                info.st_uid == os.geteuid() and info.st_mode & 0o077 == 0,
                "private artifact parent required",
            )
    finally:
        os.close(fd)


class Frozen:
    """Repeated snapshots detect replacement even when the bytes match."""

    def __init__(self):
        self.files = {}

    def read(self, path, limit=FILE_LIMIT):
        path = Path(path)
        value = snapshot(path, limit, with_identity=True)
        mode = path.lstat()
        require(
            mode.st_uid == os.geteuid() and mode.st_mode & 0o022 == 0,
            "file ownership or writable permissions",
        )
        require(
            path not in self.files or self.files[path] == value,
            "immutable file replaced or changed",
        )
        self.files[path] = value
        return value[0]

    def check(self):
        for path in tuple(self.files):
            self.read(path)


def packet(name, data):
    require(0 < len(data) <= FILE_LIMIT, "artifact size")
    return {
        "name": name,
        "sha256": digest(data),
        "data": base64.b64encode(data).decode("ascii"),
    }


def unpack(value, names):
    require(
        isinstance(value, dict) and set(value) == {"name", "sha256", "data"},
        "artifact envelope",
    )
    require(value["name"] in names and isinstance(value["data"], str), "artifact name")
    data = base64.b64decode(value["data"], validate=True)
    require(
        0 < len(data) <= FILE_LIMIT and digest(data) == value["sha256"],
        "artifact hash or size",
    )
    return value["name"], data


def launch_value(data, binding):
    value = parse(data)
    require(
        isinstance(value, dict)
        and set(value) == {"schema_version", "state", "binding", "launch_nonce"},
        "launch fields",
    )
    nonce = uuid.UUID(value["launch_nonce"])
    require(
        value["schema_version"] == "1.0.0"
        and value["state"] == "launched"
        and value["binding"] == binding
        and nonce.version == 4
        and str(nonce) == value["launch_nonce"],
        "launch binding",
    )
    return value


class Wire:
    """One deadline bounds pipe reads and writes, including partial frames."""

    def __init__(self, reader, writer, deadline):
        self.reader, self.writer, self.deadline = reader, writer, deadline
        self.buffer = bytearray()
        os.set_blocking(reader, False)
        os.set_blocking(writer, False)

    def wait(self, fd, event):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("relay deadline")
        with selectors.DefaultSelector() as selector:
            selector.register(fd, event)
            if not selector.select(remaining):
                raise TimeoutError("relay deadline")

    def send(self, value):
        data = encoded(value)
        require(len(data) <= FRAME_LIMIT, "frame quota")
        while data:
            self.wait(self.writer, selectors.EVENT_WRITE)
            try:
                data = data[os.write(self.writer, data) :]
            except BlockingIOError:
                pass

    def receive(self):
        while b"\n" not in self.buffer:
            self.wait(self.reader, selectors.EVENT_READ)
            try:
                data = os.read(
                    self.reader, min(65536, FRAME_LIMIT + 1 - len(self.buffer))
                )
            except BlockingIOError:
                continue
            if not data:
                raise EOFError("transport closed")
            self.buffer.extend(data)
            require(len(self.buffer) <= FRAME_LIMIT, "frame quota")
        line, _, remaining = self.buffer.partition(b"\n")
        self.buffer = bytearray(remaining)
        return parse(line)


def reap(process):
    """Reap the leader; descendants after known leader exit are unconfirmed.

    SIGINT allows Python owners to run their finally blocks. Only a still-owned,
    unreaped leader permits a group signal; its numeric PGID may be reused after
    poll or wait observes exit.
    """
    errors = []
    for sig in (signal.SIGINT, signal.SIGKILL):
        if process.poll() is not None:
            break
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            pass
        except OSError as error:
            errors.append(error)
        try:
            process.wait(timeout=2)
            break
        except subprocess.TimeoutExpired:
            if sig == signal.SIGKILL:
                raise
    if errors:
        raise errors[0]


class Owner:
    """Linux-side owner. No remote deployment or cluster mutation is performed."""

    def __init__(self, config, relay_id, deadline):
        require(
            set(config) == {"root", "provider", "binding", "collector", "scenario"},
            "owner configuration",
        )
        require(
            isinstance(config["provider"], dict)
            and set(config["provider"]) == PROVIDER_FIELDS,
            "provider configuration",
        )
        require(
            all(
                isinstance(v, str) and 0 < len(v) <= 1024 and "\x00" not in v
                for v in config["provider"].values()
            ),
            "provider values",
        )
        require(sys.platform == "linux", "remote owner requires Linux")
        self.base = Path(config["root"])
        self.base_identity = directory_identity(self.base)
        self.root = self.base / relay_id
        self.root.mkdir(mode=0o700)  # Deliberately refuses a replay/existing session.
        self.identity = directory_identity(self.root)
        self.frozen = Frozen()
        self.sent, self.total, self.processes = set(), 0, []
        self.deadline = deadline
        self.binding = config["binding"]
        self.launch = None
        self.cancel = None
        self.check()
        for name, value in (
            ("observer-binding.json", self.binding),
            ("binding.json", config["collector"]),
            ("scenario.json", config["scenario"]),
        ):
            publish_artifact(self.root / name, encoded(value))
            self.frozen.read(self.root / name)
        read_runner_binding(self.root / "observer-binding.json", config["collector"])
        self.provider = config["provider"]

    def start(self):
        tool = Path(__file__).parent
        remaining = str(max(0.001, self.deadline - time.monotonic()))
        shared = [
            "--runner-binding",
            str(self.root / "observer-binding.json"),
            "--attachment-ready",
            str(self.root / "attached.json"),
            "--timeout",
            remaining,
        ]
        supervisor_args = [
            sys.executable,
            str(tool / "supervisor.py"),
            *shared,
            "--binding",
            str(self.root / "binding.json"),
            "--scenario",
            str(self.root / "scenario.json"),
            "--ready-output",
            str(self.root / "ready.json"),
            "--output",
            str(self.root / "supervised"),
            "--runtime-attachment",
            str(self.root / "provider/runtime.json"),
            "--device-feed",
            str(self.root / "provider/devices.json"),
            "--provider-ready",
            str(self.root / "provider/provider-ready.json"),
        ]
        provider_args = [
            sys.executable,
            str(tool / "fenrir_provider.py"),
            *shared,
            "--enable-provider",
            "--output",
            str(self.root / "provider"),
            "--observer-identity",
            str(self.root / "supervised/observer-identity.json"),
            "--launch-marker",
            str(self.root / "observer-launch.json"),
        ]
        for key, value in self.provider.items():
            provider_args += ["--" + key.replace("_", "-"), value]
        for argv in (provider_args, supervisor_args):
            self.processes.append(
                subprocess.Popen(
                    argv,
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    start_new_session=True,
                )
            )
            if argv is provider_args:
                self.await_provider_directory()

    def await_provider_directory(self):
        # supervisor.absent() requires the provider's parent directory to exist.
        # The provider cannot publish baseline READY until supervisor identity exists.
        while not (self.root / "provider").exists():
            self.check()
            require(self.processes[0].poll() is None, "provider failed before startup")
            time.sleep(0.005)
        directory_identity(self.root / "provider")

    def check(self):
        require(
            directory_identity(self.base) == self.base_identity
            and directory_identity(self.root) == self.identity,
            "remote directory replaced",
        )
        self.frozen.check()
        if time.monotonic() >= self.deadline:
            raise TimeoutError("remote lease expired")

    def put(self, value):
        self.check()
        name, data = unpack(value, {"observer-launch.json", "observer-cancel.json"})
        if name == "observer-launch.json":
            require(
                "ready.json" in self.sent and self.launch is None, "launch ordering"
            )
            self.launch = launch_value(data, self.binding)
        else:
            require(
                "ready.json" in self.sent and self.cancel is None, "cancel ordering"
            )
        publish_envelope(self.root / name, data)
        self.frozen.read(self.root / name)
        if name == "observer-cancel.json":
            self.cancel = cancel_result(self.root / name, self.binding)
        return {"stored": name, "sha256": digest(data)}

    def poll(self):
        self.check()
        # Use one exit snapshot for both artifact eligibility and completion.
        # An owner may exit during the scan; its files must get another scan
        # before that exit can trigger the final publication check.
        exited = [process.poll() is not None for process in self.processes]
        for name in REMOTE_FILES:
            if name in self.sent:
                continue
            # supervisor.save() writes final names directly. Its identity alone
            # uses atomic publication; all other supervisor-owned files become
            # immutable only after the supervisor process has finished.
            if (
                name.startswith("supervised/")
                and name != "supervised/observer-identity.json"
                and (len(exited) < 2 or not exited[1])
            ):
                continue
            try:
                data = self.frozen.read(self.root / name)
            except FileNotFoundError:
                continue
            self.total += len(data)
            require(self.total <= TOTAL_LIMIT, "remote artifact quota")
            self.sent.add(name)
            return {"artifact": packet(name, data)}
        if exited and all(exited):
            require("supervised/supervisor.json" in self.sent, "remote owner failed")
            self.close()
            return {"done": True, "cleanup": "owners_reaped"}
        return {"pending": True}

    def close(self):
        errors = []
        while self.processes:
            process = self.processes.pop()
            try:
                reap(process)
            except (OSError, subprocess.SubprocessError) as error:
                errors.append(type(error).__name__)
        require(not errors, "remote owner cleanup failed")


def serve(reader=None, writer=None, *, owner_factory=Owner):
    """Subsystem entrypoint; connection loss triggers owner cleanup."""
    wire = Wire(
        reader if reader is not None else sys.stdin.fileno(),
        writer if writer is not None else sys.stdout.fileno(),
        time.monotonic() + 10,
    )
    owner = None
    try:
        hello = wire.receive()
        require(
            set(hello) == {"op", "relay_id", "timeout"} and hello["op"] == "hello",
            "hello protocol",
        )
        relay_id = hello["relay_id"]
        require(
            str(uuid.UUID(relay_id)) == relay_id and uuid.UUID(relay_id).version == 4,
            "relay UUID",
        )
        wire.deadline = time.monotonic() + timeout_value(hello["timeout"])
        wire.send({"relay_id": relay_id, "seq": 0, "hashes": code_hashes()})
        seq = 1
        while True:
            value = wire.receive()
            require(
                isinstance(value, dict)
                and set(value) == {"relay_id", "seq", "op", "value"}
                and value["relay_id"] == relay_id
                and type(value["seq"]) is int
                and value["seq"] == seq,
                "request sequence or identity",
            )
            operation = value["op"]
            if operation == "begin":
                require(owner is None and seq == 1, "session replay")
                owner = owner_factory(value["value"], relay_id, wire.deadline)
                owner.start()
                response = {"started": True}
            elif operation == "put":
                require(owner is not None, "owner not started")
                response = owner.put(value["value"])
            elif operation == "poll":
                require(owner is not None and value["value"] is None, "poll protocol")
                response = owner.poll()
            else:
                raise ValueError("unknown operation")
            wire.send({"relay_id": relay_id, "seq": seq, "value": response})
            seq += 1
            if response.get("done"):
                break
    finally:
        if owner is not None:
            owner.close()


def ssh_argv(host, user, port, known_hosts, identity_file):
    require(
        isinstance(host, str)
        and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.-]{0,252}", host),
        "SSH host syntax",
    )
    require(
        isinstance(user, str) and re.fullmatch(r"[a-z_][a-z0-9_-]{0,63}", user),
        "SSH user syntax",
    )
    require(type(port) is int and 0 < port <= 65535, "SSH port")
    options = {
        "StrictHostKeyChecking": "yes",
        "BatchMode": "yes",
        "ForwardAgent": "no",
        "ClearAllForwardings": "yes",
        "PasswordAuthentication": "no",
        "KbdInteractiveAuthentication": "no",
        "IdentitiesOnly": "yes",
        "UserKnownHostsFile": str(known_hosts),
        "GlobalKnownHostsFile": "/dev/null",
        "PermitLocalCommand": "no",
        "RequestTTY": "no",
        "ProxyCommand": "none",
        "ProxyJump": "none",
        "ConnectTimeout": "5",
        "ConnectionAttempts": "1",
        "ServerAliveInterval": "2",
        "ServerAliveCountMax": "2",
        "ControlMaster": "no",
        "ControlPath": "none",
        "UpdateHostKeys": "no",
        "VerifyHostKeyDNS": "no",
        "HostKeyAlias": host,
        "LogLevel": "ERROR",
        "IdentityAgent": "none",
    }
    return [
        "-F",
        "/dev/null",
        "-T",
        "-p",
        str(port),
        "-l",
        user,
        "-i",
        str(identity_file),
        *[item for key, value in options.items() for item in ("-o", key + "=" + value)],
        "-s",
        host,
        SUBSYSTEM,
    ]


class Validation:
    """Validate cross-host binding without comparing independent monotonic clocks."""

    def __init__(self, binding, collector, scenario, hashes=None):
        self.binding, self.collector, self.scenario = binding, collector, scenario
        self.files = {}
        self.launch = self.cancel = None
        self.hashes = hashes

    def accept(self, name, data):
        require(name not in self.files, "duplicate remote artifact")
        value = parse(data) if name.endswith(".json") else None
        if name == "supervised/observer-identity.json":
            require(
                value["runner_binding"] == self.binding
                and value["authenticated"] is False,
                "observer binding",
            )
            require(
                str(uuid.UUID(value["observer_id"])) == value["observer_id"]
                and uuid.UUID(value["observer_id"]).version == 4,
                "observer UUID",
            )
            if self.hashes is not None:
                require(
                    value["collector_sha256"] == self.hashes["observer.py"]
                    and value["supervisor_sha256"] == self.hashes["supervisor.py"],
                    "observer executable identity",
                )
        if name == "provider/provider-ready.json":
            identity = parse(self.files["supervised/observer-identity.json"])
            require(
                value["binding"] == self.binding
                and value["observer_id"] == identity["observer_id"]
                and value["state"] == "baseline_ready"
                and value["baseline_sha256"]
                == digest(self.files["provider/baseline.json"]),
                "provider baseline binding",
            )
        if name == "ready.json":
            require(
                "provider/provider-ready.json" in self.files
                and value
                == {
                    "schema_version": "1.0.0",
                    "state": "ready",
                    "binding": self.binding,
                },
                "READY binding",
            )
        if name in ("provider/runtime.json", "provider/devices.json", "attached.json"):
            identity = parse(self.files["supervised/observer-identity.json"])
            require(
                self.launch is not None
                and value["observer_id"] == identity["observer_id"]
                and value["launch_nonce"] == self.launch["launch_nonce"],
                "attachment identity or nonce",
            )
            require(
                value.get("runner_binding", value.get("binding")) == self.binding,
                "attachment binding",
            )
            if name == "attached.json":
                require(
                    value
                    == {
                        "schema_version": "1.0.0",
                        "state": "attached",
                        "binding": self.binding,
                        "launch_nonce": self.launch["launch_nonce"],
                        "observer_id": identity["observer_id"],
                        "association": "exclusive_launch_candidate",
                        "authenticated": False,
                        "runtime_attachment_sha256": digest(
                            self.files["provider/runtime.json"]
                        ),
                    },
                    "attachment ack",
                )
            else:
                for source in value["sources"]:
                    require(
                        source["path"] in {"runtime-source.json", "devices-source.json"}
                        and source["sha256"]
                        == digest(self.files["provider/" + source["path"]]),
                        "runtime source hash",
                    )
                if name == "provider/devices.json":
                    require(
                        "attached.json" in self.files
                        and value["runtime_attachment_sha256"]
                        == digest(self.files["provider/runtime.json"]),
                        "device feed binding",
                    )
        if name == "supervised/collector/manifest.json":
            for key in (
                "session_id",
                "scenario_id",
                "host_version",
                "test_client_version",
                "test_client_revision",
                "official_client_version",
            ):
                require(value[key] == self.collector[key], "manifest binding")
            require(
                len(value["artifacts"]) == 2
                and {a["path"] for a in value["artifacts"]}
                == {"raw.json", "host.jsonl"},
                "manifest inventory",
            )
            for item in value["artifacts"]:
                require(
                    item["sha256"]
                    == digest(self.files["supervised/collector/" + item["path"]]),
                    "manifest artifact digest",
                )
            raw = parse(self.files["supervised/collector/raw.json"])
            require(
                raw["binding"] == parse(self.files["supervised/collector-binding.json"])
                and raw["scenario_sha256"] == digest(encoded(self.scenario)),
                "raw binding",
            )
            rows = [
                row for row in raw["records"] if row.get("kind") == "runtime_attachment"
            ]
            require(
                len(rows) == 1
                and rows[0]["attachment_bytes"].encode("utf-8")
                == self.files["provider/runtime.json"]
                and rows[0]["observer_identity"]
                == parse(self.files["supervised/observer-identity.json"])
                and rows[0]["launch_marker"] == self.launch,
                "raw runtime population binding",
            )
            feeds = [
                row
                for row in raw["records"]
                if row.get("kind") == "late_device_attachment"
            ]
            require(len(feeds) <= 1, "duplicate raw device feed")
            if feeds:
                require(
                    feeds[0]["feed_bytes"].encode("utf-8")
                    == self.files["provider/devices.json"],
                    "raw device population binding",
                )
        if name == "supervised/supervisor.json":
            require(
                value["acceptance"] == "Incomplete"
                and value["coordination"] == "published"
                and not value["cleanup_errors"]
                and self.cancel is not None
                and value["request_succeeded"] is self.cancel["request_succeeded"]
                and "supervised/collector/manifest.json" in self.files,
                "supervisor publication",
            )
        self.files[name] = data


def relay(
    *,
    host,
    user,
    port,
    known_hosts,
    identity_file,
    runner_binding,
    collector,
    scenario,
    ready,
    attached,
    output,
    remote_root,
    provider,
    expected_hashes,
    timeout=60,
    enable=False,
    ssh_command=None,
):
    require(enable, "remote relay disabled")
    deadline = time.monotonic() + timeout_value(timeout)
    output = Path(output).absolute()
    absent(output)
    output.mkdir(mode=0o700)
    output_identity = directory_identity(output)
    result = {
        "acceptance": "Incomplete",
        "transport": "incomplete",
        "reason": None,
        "request_succeeded": None,
        "remote_cleanup": "unconfirmed",
        "artifacts": {},
        "cleanup_errors": [],
    }
    process = None
    frozen = Frozen()
    try:
        require(
            set(expected_hashes) == set(MODULES)
            and all(re.fullmatch(r"[0-9a-f]{64}", v) for v in expected_hashes.values()),
            "pinned module hashes",
        )
        for path in (ready, attached):
            absent(Path(path))
        paths = {
            name: Path(runner_binding).parent / name
            for name in ("observer-launch.json", "observer-cancel.json")
        }
        for path in paths.values():
            absent(path)
        known_hosts, identity_file = (
            Path(known_hosts).absolute(),
            Path(identity_file).absolute(),
        )
        require(
            re.fullmatch(r"/[A-Za-z0-9_./-]+", str(known_hosts)),
            "known_hosts must have a literal whitespace-free SSH path",
        )
        hosts = frozen.read(known_hosts, 16384).decode("ascii").splitlines()
        require(
            len(hosts) == 1
            and len(hosts[0].split()) in (3, 4)
            and hosts[0].split()[0] == host
            and hosts[0].split()[1]
            in {"ssh-ed25519", "ecdsa-sha2-nistp256", "ssh-rsa"},
            "one explicit pinned host key required",
        )
        base64.b64decode(hosts[0].split()[2], validate=True)
        frozen.read(identity_file, 65536)
        require(
            identity_file.stat().st_mode & 0o077 == 0,
            "private SSH identity permissions",
        )
        collector_value, scenario_value = (
            parse(frozen.read(collector)),
            parse(frozen.read(scenario)),
        )
        while True:
            if time.monotonic() >= deadline:
                raise TimeoutError("binding deadline")
            try:
                binding_data = frozen.read(runner_binding, 16384)
                binding = read_runner_binding(runner_binding, collector_value)
                require(parse(binding_data) == binding, "binding changed")
                break
            except FileNotFoundError:
                time.sleep(0.02)
        validation = Validation(
            binding, collector_value, scenario_value, expected_hashes
        )
        for name, data in (
            ("observer-binding.json", binding_data),
            ("collector.json", encoded(collector_value)),
            ("scenario.json", encoded(scenario_value)),
        ):
            publish_artifact(output / name, data)
        relay_id = str(uuid.uuid4())
        argv = list(ssh_command or ["/usr/bin/ssh"]) + ssh_argv(
            host, user, port, known_hosts, identity_file
        )
        process = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        wire = Wire(process.stdout.fileno(), process.stdin.fileno(), deadline)
        wire.send(
            {
                "op": "hello",
                "relay_id": relay_id,
                "timeout": max(0.001, deadline - time.monotonic()),
            }
        )
        require(
            wire.receive()
            == {"relay_id": relay_id, "seq": 0, "hashes": expected_hashes},
            "remote code pin or handshake mismatch",
        )
        seq = 0

        def request(operation, value=None):
            nonlocal seq
            frozen.check()
            require(
                directory_identity(output) == output_identity,
                "local directory replaced",
            )
            seq += 1
            wire.send(
                {"relay_id": relay_id, "seq": seq, "op": operation, "value": value}
            )
            response = wire.receive()
            require(
                set(response) == {"relay_id", "seq", "value"}
                and response["relay_id"] == relay_id
                and response["seq"] == seq,
                "response sequence or identity",
            )
            return response["value"]

        require(
            request(
                "begin",
                {
                    "root": str(remote_root),
                    "provider": provider,
                    "binding": binding,
                    "collector": collector_value,
                    "scenario": scenario_value,
                },
            )
            == {"started": True},
            "remote start",
        )
        sent, total = set(), 0
        while True:
            for name, path in paths.items():
                if name in sent:
                    continue
                try:
                    data = frozen.read(path, 16384)
                except FileNotFoundError:
                    continue
                require("ready.json" in validation.files, "signal before READY")
                if name == "observer-launch.json":
                    validation.launch = launch_value(data, binding)
                else:
                    validation.cancel = cancel_result(path, binding)
                    result["request_succeeded"] = validation.cancel["request_succeeded"]
                require(
                    request("put", packet(name, data))
                    == {"stored": name, "sha256": digest(data)},
                    "remote transfer acknowledgement",
                )
                publish_envelope(output / name, data)
                sent.add(name)
            response = request("poll")
            if set(response) == {"artifact"}:
                name, data = unpack(response["artifact"], REMOTE_FILES)
                total += len(data)
                require(total <= TOTAL_LIMIT, "local artifact quota")
                validation.accept(name, data)
                destination = output / "remote" / name
                private_parents(output, Path("remote") / Path(name).parent)
                publish_artifact(destination, data)
                frozen.read(destination)
                result["artifacts"]["remote/" + name] = digest(data)
                if name in ("ready.json", "attached.json"):
                    target = Path(ready if name == "ready.json" else attached)
                    publish_envelope(target, data)
                    frozen.read(target)
            elif response == {"done": True, "cleanup": "owners_reaped"}:
                require(
                    "supervised/supervisor.json" in validation.files,
                    "incomplete final inventory",
                )
                frozen.check()
                result["transport"] = "published"
                result["remote_cleanup"] = "owners_reaped"
                break
            else:
                require(response == {"pending": True}, "unexpected response")
                time.sleep(min(0.02, max(0, deadline - time.monotonic())))
    except (OSError, ValueError, KeyError, TypeError, EOFError, TimeoutError) as error:
        # Never persist SSH/kubeconfig contents or untrusted remote diagnostic text.
        result["reason"] = type(error).__name__
    finally:
        if process is not None:
            for name in ("stdin", "stdout"):
                stream = getattr(process, name)
                setattr(process, name, None)
                try:
                    stream.close()
                except OSError as error:
                    result["cleanup_errors"].append(type(error).__name__)
            try:
                reap(process)
            except (OSError, subprocess.SubprocessError) as error:
                result["cleanup_errors"].append(type(error).__name__)
        if result["cleanup_errors"]:
            result["transport"] = "incomplete"
        if directory_identity(output) == output_identity:
            publish_envelope(output / "relay.json", encoded(result))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serve", action="store_true")
    parser.add_argument("--enable-relay", action="store_true")
    parser.add_argument("--config", type=Path)
    args = parser.parse_args()
    if args.serve:
        require(not args.enable_relay and args.config is None, "subsystem arguments")
        serve()
    else:
        require(
            args.enable_relay and args.config is not None,
            "explicit relay opt-in required",
        )
        config = parse(snapshot(args.config, 65536))
        require(
            isinstance(config, dict)
            and "ssh_command" not in config
            and "enable" not in config,
            "internal test seam unavailable in CLI",
        )
        result = relay(**config, enable=True)
        print(json.dumps(result))
    return 2  # Coordination success does not imply acceptance.


if __name__ == "__main__":
    raise SystemExit(main())
