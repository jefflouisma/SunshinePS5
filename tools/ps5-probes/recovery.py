"""Offline T050 recovery checkpoints and one-shot injected dispatch coordination.

The CLI only inspects supplied observations. There is no socket, shell, payload
deployment or private-key reader here. recovery_adapter.py provides opt-in
discovery/transfer mechanics but requires reviewed live observation and exit
callbacks. Supplied evidence hashes bind assertions; they are not attestation.
"""

import argparse
import hashlib
import json
import math
import os
import re
import stat
import time
from pathlib import Path

CONSOLE = "192.168.2.2"
HOST_ID = "00E421FDEC6B"
FIRMWARE = "13600007"
REMOTE_ROOT = "/data/ps5-dev/t050-20261031-codex6"
FINGERPRINT = "c25403d792a58565794dcd9ced9c12ef8b2e862e6f4c20e430d8b51e3c5f47b9"
REPO = Path(__file__).resolve().parents[2]
ARTIFACTS = {
    "verify": (
        REPO / ".omo/evidence/t050-rest-device-20261031/target-next-verify.elf",
        "5cf7efd1ca26f92664d01982601d9362b723a465e83fe5f4a98c8a121be45f54",
    ),
    "cleanup": (
        REPO / ".omo/evidence/t050-storage-probe-20261031/target-next-cleanup.elf",
        "0e47941c800912212ef8661b17a08cd31ed9d653beb2efd35b34ac435b3f6bb3",
    ),
}
JOURNAL = REPO / ".omo/evidence/t050-rest-device-20261031/recovery-journal"
SERVICES = {
    "loader": 9021,
    "websrv": 8080,
    "gdbsrv": 2159,
    "klogsrv": 3232,
    "shsrv": 2323,
}
CHILDREN = (
    "state",
    "corrupt-identity",
    "truncated-identity",
    "missing-identity",
    "corrupt-settings",
    "unreadable-registry",
    "identity-permissions",
    "identity-symlink",
    "identity-multilink",
    "directory-permissions",
)


class Refused(ValueError):
    """A required observation, durable record or dispatch result is unavailable."""


def require(condition, message):
    if not condition:
        raise Refused(message)


def encoded(value):
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), allow_nan=False
    ).encode()


def digest(value):
    return hashlib.sha256(encoded(value)).hexdigest()


def hash_value(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON field")
        result[key] = value
    return result


def read_bounded(path, limit):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, "unsafe local file")
        data = stream.read(limit + 1)
    require(len(data) <= limit, "local file exceeds quota")
    return data


def read_json(path):
    return json.loads(read_bounded(path, 1024 * 1024), object_pairs_hook=unique_object)


def discovery_ready(raw):
    require(isinstance(raw, str) and len(raw) <= 4096, "discovery quota/type")
    lines = raw.replace("\r\n", "\n").split("\n")
    require(lines[0] == "HTTP/1.1 200 Ok", "console is not ready")
    require(lines[-2:] == ["", ""], "incomplete discovery response")
    fields = {}
    for line in lines[1:-2]:
        key, separator, value = line.partition(":")
        require(
            separator and key not in fields and value,
            "invalid/duplicate discovery field",
        )
        fields[key] = value
    expected = {
        "host-id": HOST_ID,
        "host-type": "PS5",
        "host-request-port": "997",
        "device-discovery-protocol-version": "00030010",
        "system-version": FIRMWARE,
    }
    require(
        all(fields.get(k) == v for k, v in expected.items()),
        "foreign discovery identity",
    )
    require(set(fields) == set(expected) | {"host-name"}, "unexpected discovery fields")
    require(re.fullmatch(r"[ -~]{1,64}", fields["host-name"]), "invalid console name")


def expected_layout():
    """Exact codex6 target layout with unsupported link fixtures restored."""
    yield ".", "directory", 0o700
    for child in CHILDREN:
        yield child, "directory", 0o700
        for name in ("identity.bin", "settings.bin", "devices.bin"):
            if child == "missing-identity" and name == "identity.bin":
                continue
            mode = 0o600
            if (child, name) in (
                ("unreadable-registry", "devices.bin"),
                ("identity-permissions", "identity.bin"),
            ):
                mode = 0
            yield f"{child}/{name}", "file", mode


def validate_inventory(inventory):
    require(isinstance(inventory, dict), "missing inventory")
    require(
        inventory.get("complete") is True and inventory.get("no_follow") is True,
        "inventory is incomplete or follows links",
    )
    entries = inventory.get("entries")
    expected = {path: (kind, mode) for path, kind, mode in expected_layout()}
    require(
        isinstance(entries, list) and len(entries) == len(expected),
        "inventory entry count",
    )
    seen, inodes, devices = set(), set(), set()
    for entry in entries:
        require(isinstance(entry, dict), "inventory entry type")
        require(
            set(entry)
            == {
                "path",
                "kind",
                "mode",
                "uid",
                "device",
                "inode",
                "links",
                "size",
                "sha256",
            },
            "unexpected inventory fields",
        )
        path = entry.get("path")
        require(
            isinstance(path, str) and path in expected and path not in seen,
            "unexpected/duplicate inventory path",
        )
        seen.add(path)
        kind, mode = expected[path]
        require(
            entry.get("kind") == kind
            and type(entry.get("mode")) is int
            and entry["mode"] == mode
            and type(entry.get("uid")) is int
            and entry["uid"] == 0,
            "inventory type/owner/mode mismatch",
        )
        for key in ("device", "inode", "links", "size"):
            require(
                type(entry.get(key)) is int and entry[key] >= 0, "missing stat metadata"
            )
        identity = (entry["device"], entry["inode"])
        require(
            entry["inode"] > 0 and identity not in inodes, "aliased inventory inode"
        )
        inodes.add(identity)
        devices.add(entry["device"])
        if kind == "file":
            require(
                entry["links"] == 1
                and 0 < entry["size"] <= 40000
                and hash_value(entry.get("sha256")),
                "unsafe/incomplete file metadata",
            )
        else:
            require(
                entry["links"] >= 2 and entry.get("sha256") is None,
                "invalid directory metadata",
            )
    require(len(devices) == 1, "inventory crosses filesystems")


def validate_bundle(bundle, now, inventory=True):
    require(
        isinstance(bundle, dict) and len(encoded(bundle)) <= 1024 * 1024,
        "observation quota/type",
    )
    keys = {
        "schema_version",
        "console",
        "host_id",
        "firmware",
        "root",
        "observed_at",
        "evidence_sha256",
        "discovery",
        "services",
        "resume",
        "process",
        "isolation",
        "hdmi",
        "inventory",
        "public_fingerprint",
    }
    require(
        set(bundle) == keys | (set() if inventory else {"http"}),
        "unexpected bundle fields",
    )
    expected = {
        "schema_version": 1,
        "console": CONSOLE,
        "host_id": HOST_ID,
        "firmware": FIRMWARE,
        "root": REMOTE_ROOT,
    }
    require(
        all(bundle.get(k) == v for k, v in expected.items()),
        "wrong observation binding",
    )
    stamp = bundle.get("observed_at")
    require(
        type(stamp) in (int, float)
        and math.isfinite(stamp)
        and -5 <= now - stamp <= 120,
        "observation stale or future",
    )
    require(
        hash_value(bundle.get("evidence_sha256")), "missing observation source hash"
    )
    discovery_ready(bundle.get("discovery"))
    services = bundle.get("services")
    require(
        isinstance(services, dict) and set(services) == set(SERVICES),
        "service census missing",
    )
    for name, port in SERVICES.items():
        service = services[name]
        require(
            isinstance(service, dict)
            and service.get("port") == port
            and service.get("protocol") == name
            and service.get("verified") is True
            and hash_value(service.get("evidence_sha256")),
            "service protocol unverified",
        )
    for key, flags in (
        ("process", ("no_game", "no_writer")),
        ("isolation", ("ipv4", "ipv6", "exclusive")),
        ("hdmi", ("healthy",)),
    ):
        observation = bundle.get(key)
        require(
            isinstance(observation, dict)
            and hash_value(observation.get("evidence_sha256"))
            and all(observation.get(flag) is True for flag in flags),
            f"{key} unverified",
        )
    resume = bundle.get("resume")
    require(
        isinstance(resume, dict)
        and resume.get("kind") == "resume"
        and resume.get("assessed_at") == stamp
        and hash_value(resume.get("evidence_sha256")),
        "fresh resume assessment required; boot/unknown cannot arm verification",
    )
    if inventory:
        validate_inventory(bundle.get("inventory"))
        require(
            bundle.get("public_fingerprint") == FINGERPRINT,
            "public identity digest mismatch",
        )


def validate_result(mode, result):
    require(isinstance(result, dict), "transport result missing")
    require(
        type(result.get("exit_code")) is int
        and result["exit_code"] == 0
        and result.get("complete") is True,
        "complete successful exit evidence missing",
    )
    elapsed = result.get("elapsed_seconds")
    require(
        type(elapsed) in (int, float) and math.isfinite(elapsed) and 0 <= elapsed <= 30,
        "transport deadline exceeded",
    )
    stdout, stderr = result.get("stdout"), result.get("stderr")
    require(
        isinstance(stdout, str)
        and isinstance(stderr, str)
        and len(stdout.encode()) + len(stderr.encode()) <= 65536,
        "transport output quota/type",
    )
    require(not stderr and "T050 fail" not in stdout, "payload error output")
    success = (
        rf"T050 mode=verify pid=[1-9][0-9]* euid=0 sha256={FINGERPRINT} "
        r"paired=1 metrics=0 pass=1"
        if mode == "verify"
        else r"T050 mode=cleanup pid=[1-9][0-9]* removed=1 pass=1"
    )
    lines = stdout.splitlines()
    require(
        lines and re.fullmatch(success, lines[-1]) is not None,
        "exact final success missing",
    )
    require(
        sum(line.startswith("T050 mode=") for line in lines) == 1,
        "duplicate payload result",
    )
    load = "T050 load stage=initial status=0 reason=none"
    trace = (
        r"T050 trace stage=[a-zA-Z0-9_.]+ event=(begin|end) file=-?[0-9]+ "
        r"result=-?[0-9]+ reason=[a-zA-Z0-9_-]+ errno=[0-9]+ openssl=[0-9]+"
    )
    require(
        lines.count(load) == (1 if mode == "verify" else 0),
        "initial load result missing",
    )
    require(
        all(line == load or re.fullmatch(trace, line) for line in lines[:-1]),
        "unrecognized payload output",
    )


def durable_journal_directory(path):
    """Open a no-follow directory chain and persist every containing entry.

    Sync existing links too: a prior interrupted creation may have left a visible
    directory whose parent was never synced. No dispatch may depend on that link.
    All operations use held parent descriptors, including creation and validation.
    """
    path = Path(path).absolute()
    require(".." not in path.parts and len(path.parts) > 1, "invalid journal path")
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    parent = os.open(path.anchor, flags)
    try:
        for index, component in enumerate(path.parts[1:], start=1):
            created = False
            try:
                os.mkdir(component, mode=0o700, dir_fd=parent)
                created = True
            except FileExistsError:
                pass
            child = os.open(component, flags, dir_fd=parent)
            try:
                info = os.fstat(child)
                require(
                    stat.S_ISDIR(info.st_mode), "journal ancestor is not a directory"
                )
                if created or index == len(path.parts) - 1:
                    require(
                        info.st_uid == os.geteuid()
                        and stat.S_IMODE(info.st_mode) == 0o700,
                        "unsafe journal directory",
                    )
                os.fsync(child)
                os.fsync(parent)
            except BaseException:
                os.close(child)
                raise
            os.close(parent)
            parent = child
        result, parent = parent, None
        return result
    finally:
        if parent is not None:
            os.close(parent)


class Coordinator:
    """One population, one durable journal; injected transport is never retried.

    Never delete/replace the journal to clear an ambiguous attempt. Real adapters
    must use JOURNAL, pass exact immutable payload bytes, enforce the supplied
    timeout/quota, and report complete exit evidence. recovery_adapter.py keeps
    those mechanics separate from its still-unbound live observation callbacks.
    """

    def __init__(self, journal=JOURNAL, clock=time.time):
        self.dir_fd = None
        self.journal, self.clock = Path(journal), clock
        self.dir_fd = durable_journal_directory(self.journal)

    def close(self):
        if self.dir_fd is not None:
            os.close(self.dir_fd)
            self.dir_fd = None

    def __del__(self):
        self.close()

    def _write_once(self, name, value):
        data = encoded(value)
        require(len(data) <= 1024 * 1024, "journal record quota")
        try:
            fd = os.open(
                name,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                0o600,
                dir_fd=self.dir_fd,
            )
        except FileExistsError as error:
            raise Refused("existing journal record; do not retry dispatch") from error
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.fsync(self.dir_fd)

    def _read(self, name):
        # Journals are private and the held directory descriptor protects writes.
        fd = os.open(
            name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=self.dir_fd
        )
        with os.fdopen(fd, "rb") as stream:
            info = os.fstat(stream.fileno())
            require(
                stat.S_ISREG(info.st_mode)
                and info.st_nlink == 1
                and info.st_uid == os.geteuid()
                and stat.S_IMODE(info.st_mode) == 0o600,
                "unsafe journal record",
            )
            raw = stream.read(1024 * 1024 + 1)
        require(len(raw) <= 1024 * 1024, "journal quota")
        return json.loads(raw, object_pairs_hook=unique_object)

    def inspect(self, bundle):
        validate_bundle(bundle, self.clock())
        return {
            "status": "inspection complete; verification not armed",
            "binding": digest(bundle),
        }

    def _dispatch(self, mode, bundle, transport, armed):
        require(armed is True, f"{mode} requires separate explicit arming")
        bundle = json.loads(encoded(bundle))
        self.inspect(bundle)
        path, expected_hash = ARTIFACTS[mode]
        payload = read_bounded(path, 32 * 1024 * 1024)
        require(
            hashlib.sha256(payload).hexdigest() == expected_hash,
            "approved ELF hash mismatch",
        )
        # The exclusive durable intent precedes any call that can send bytes.
        self._write_once(
            f"{mode}.intent.json",
            {
                "mode": mode,
                "root": REMOTE_ROOT,
                "fingerprint": FINGERPRINT,
                "artifact_sha256": expected_hash,
                "observations": bundle,
                "started_at": self.clock(),
            },
        )
        try:
            response = transport(mode, payload, 30, 65536)
        except BaseException as error:
            # Preserve the exception category without copying arbitrary adapter text.
            self._write_once(f"{mode}.failure.json", {"type": type(error).__name__})
            raise
        # Even malformed, missing, interrupted or oversized results leave the intent.
        self._write_once(f"{mode}.result.json", response)
        validate_result(mode, response)
        self._write_once(f"{mode}.completed.json", {"at": self.clock()})
        return response

    def verify(self, bundle, transport, *, armed=False):
        return self._dispatch("verify", bundle, transport, armed)

    def _successful(self, mode):
        try:
            intent = self._read(f"{mode}.intent.json")
            require(
                intent["mode"] == mode
                and intent["root"] == REMOTE_ROOT
                and intent["fingerprint"] == FINGERPRINT
                and intent["artifact_sha256"] == ARTIFACTS[mode][1],
                "journal binding mismatch",
            )
            validate_result(mode, self._read(f"{mode}.result.json"))
            completed = self._read(f"{mode}.completed.json")
            require(
                type(completed.get("at")) in (int, float)
                and math.isfinite(completed["at"]),
                "completion timestamp missing",
            )
            return completed["at"]
        except (OSError, KeyError, json.JSONDecodeError) as error:
            raise Refused("successful prior dispatch evidence unavailable") from error

    def record_postverify(self, bundle):
        completed = self._successful("verify")
        self.inspect(bundle)
        require(
            bundle["observed_at"] >= completed,
            "postverify observation precedes verification",
        )
        self._write_once("postverify.json", bundle)

    def cleanup(self, bundle, transport, *, armed=False):
        self._successful("verify")
        try:
            previous = self._read("postverify.json")
        except (OSError, json.JSONDecodeError) as error:
            raise Refused("postverify inventory missing") from error
        self.inspect(previous)
        require(
            bundle.get("inventory") == previous["inventory"],
            "postverify layout changed",
        )
        return self._dispatch("cleanup", bundle, transport, armed)

    def finish(self, bundle):
        completed = self._successful("cleanup")
        validate_bundle(bundle, self.clock(), inventory=False)
        require(
            bundle["observed_at"] >= completed,
            "postcleanup observation precedes cleanup",
        )
        require(
            bundle.get("http")
            == {
                "url": f"http://{CONSOLE}:8080{REMOTE_ROOT}",
                "status": 404,
            },
            "exact root absence not independently established",
        )
        receipt = {"status": "cleanup independently confirmed", "observations": bundle}
        self._write_once("postcleanup.json", receipt)
        return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "observations", type=Path, help="fresh JSON observation bundle; no device I/O"
    )
    args = parser.parse_args()
    try:
        bundle = read_json(args.observations)
        validate_bundle(bundle, time.time())
        print("inspection complete; verification not armed")
        return 0
    except (OSError, ValueError, TypeError, KeyError) as error:
        print(f"inspection refused: {error}")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
