"""Synthetic fixtures verify collection mechanics, never reference acceptance."""

import hashlib
import io
import json
import os
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from observer import (
    DEPLOYED_REVISION,
    Correlator,
    Decoder,
    FinishControl,
    RuntimeAttachment,
    collect,
    main,
    prepare_readiness,
    publish,
    publish_ready,
    read_axes,
    read_runner_binding,
    rename_noreplace,
    strict_document,
)


def scenario():
    return {
        "id": "scenario",
        "controllers": [
            {
                "id": 0,
                "actions": [
                    {"event_id": "connect", "type": "connect"},
                    {
                        "event_id": "deflect",
                        "type": "stick",
                        "stick": "left",
                        "x": 0.25,
                        "y": 0,
                    },
                    {
                        "event_id": "neutral",
                        "type": "stick",
                        "stick": "left",
                        "x": 0,
                        "y": 0,
                    },
                    {"event_id": "disconnect", "type": "disconnect"},
                ],
            }
        ],
    }


def binding():
    return {
        "session_id": "session",
        "scenario_id": "scenario",
        "host_version": "fixture-host",
        "test_client_version": "0.2.0",
        "test_client_revision": "fixture",
        "official_client_version": "fixture",
        "runtime_identity": "synthetic pod/image",
    }


def record(kind, **fields):
    return dict(session_id="session", kind=kind, device="event1", **fields)


def population():
    return [
        record("attach", axes={"0": 0, "1": 0}),
        record("evdev", type=3, code=0, value=8192),
        record("evdev", type=0, code=0, value=0),
        record("evdev", type=3, code=0, value=0),
        record("evdev", type=0, code=0, value=0),
        record("remove"),
    ]


def runner_binding():
    return {
        "schema_version": "1.0.0",
        "run_id": "run",
        **{
            key: binding()[key]
            for key in (
                "session_id",
                "scenario_id",
                "host_version",
                "test_client_version",
            )
        },
        "scenario_sha256": "a" * 64,
        "nonce": "018887b2-499d-4c11-9ea2-4f093aaa087a",
    }


def readiness_contract(root):
    """Offline bridge test: mock Linux acquisition, exercise the real publisher."""
    root = Path(root)
    collector = json.loads((root / "collector-binding.json").read_bytes())
    readiness = prepare_readiness(
        root / "observer-binding.json", root / "ready.json", collector
    )
    with (
        patch("observer.sys.platform", "linux"),
        patch("observer.inventory", return_value={}),
        patch("observer.time.monotonic", side_effect=[0, 1]),
    ):
        records = collect(collector["session_id"], 0.5, readiness)
    assert [row["kind"] for row in records] == ["end"]


class ReadinessTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "runner.json"
        self.destination = self.root / "ready.json"
        self.source.write_text(json.dumps(runner_binding()))

    def prepare(self):
        return prepare_readiness(self.source, self.destination, binding())

    def test_snapshot_echoes_opaque_digest_and_is_not_reopened(self):
        readiness = self.prepare()
        self.source.write_text("replacement after snapshot")
        publish_ready(*readiness)
        self.assertEqual(
            json.loads(self.destination.read_bytes()),
            {"schema_version": "1.0.0", "state": "ready", "binding": runner_binding()},
        )
        self.assertEqual(
            {p.name for p in self.root.iterdir()}, {"runner.json", "ready.json"}
        )

    def test_binding_fields_schema_and_duplicate_keys_refuse(self):
        changes = {
            key: "foreign"
            for key in (
                "session_id",
                "scenario_id",
                "host_version",
                "test_client_version",
            )
        }
        changes.update(
            schema_version="2", run_id="../bad", nonce="bad", scenario_sha256="A" * 64
        )
        for key, value in changes.items():
            with self.subTest(key=key):
                self.source.write_text(json.dumps(runner_binding() | {key: value}))
                with self.assertRaises(ValueError):
                    self.prepare()
                self.assertFalse(self.destination.exists())
        for data in (
            runner_binding() | {"extra": "value"},
            {k: v for k, v in runner_binding().items() if k != "nonce"},
            runner_binding() | {"host_version": "x" * 1025},
            [],
        ):
            self.source.write_text(json.dumps(data))
            with self.assertRaises(ValueError):
                self.prepare()
        self.source.write_text('{"nonce":"one","nonce":"two"}')
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.prepare()

    def test_binding_special_files_links_empty_and_oversize_refuse(self):
        for kind in ("symlink", "hardlink", "directory", "fifo", "empty", "oversize"):
            with self.subTest(kind=kind):
                source = self.root / kind
                if kind == "symlink":
                    source.symlink_to(self.source)
                elif kind == "hardlink":
                    os.link(self.source, source)
                elif kind == "directory":
                    source.mkdir()
                elif kind == "fifo":
                    os.mkfifo(source)
                else:
                    source.write_bytes(b"" if kind == "empty" else b"x" * 8193)
                with self.assertRaises((ValueError, OSError)):
                    read_runner_binding(source, binding())
                if kind == "hardlink":
                    source.unlink()

    def test_binding_replacement_during_open_or_read_refuses_and_closes(self):
        for point in ("open", "read"):
            with self.subTest(point=point):
                self.source.write_text(json.dumps(runner_binding()))
                original = getattr(os, point)
                replaced = False

                def replace(*args, original=original, point=point, **kwargs):
                    nonlocal replaced
                    result = original(*args, **kwargs)
                    if not replaced and (
                        point == "read" or args[0] == self.source.name
                    ):
                        replaced = True
                        other = self.root / "replacement"
                        other.write_text(json.dumps(runner_binding()))
                        os.replace(other, self.source)
                    return result

                with (
                    patch(f"observer.os.{point}", side_effect=replace),
                    patch("observer.os.close", wraps=os.close) as close,
                    self.assertRaisesRegex(ValueError, "changed"),
                ):
                    self.prepare()
                self.assertGreaterEqual(close.call_count, 2)
                for call in close.call_args_list:
                    with self.assertRaises(OSError):
                        os.fstat(call.args[0])

    def test_existing_destinations_and_parent_aliases_refuse(self):
        for kind in ("file", "symlink", "dangling", "fifo", "directory"):
            destination = self.root / kind
            if kind == "file":
                destination.write_bytes(b"preserve")
            elif kind in ("symlink", "dangling"):
                destination.symlink_to(
                    self.source if kind == "symlink" else self.root / "missing"
                )
            elif kind == "fifo":
                os.mkfifo(destination)
            else:
                destination.mkdir()
            with self.subTest(kind=kind), self.assertRaises((ValueError, OSError)):
                prepare_readiness(self.source, destination, binding())
        alias = self.root / "alias"
        alias.symlink_to(self.root, target_is_directory=True)
        for source, destination in (
            (alias / "runner.json", self.destination),
            (self.source, alias / "ready.json"),
            (self.source, self.source),
        ):
            with self.assertRaises((ValueError, OSError)):
                prepare_readiness(source, destination, binding())

    def test_destination_race_never_replaces_winner(self):
        readiness = self.prepare()
        self.destination.write_bytes(b"winner")
        with self.assertRaises((ValueError, OSError)):
            publish_ready(*readiness)
        self.assertEqual(self.destination.read_bytes(), b"winner")

    def test_no_replace_rename_handles_race_after_absence_check(self):
        for kind in ("file", "symlink", "directory"):
            with self.subTest(kind=kind):
                readiness = self.prepare()

                def compete(*args, kind=kind):
                    if kind == "file":
                        self.destination.write_bytes(b"competitor")
                    elif kind == "symlink":
                        self.destination.symlink_to(self.root / "absent")
                    else:
                        self.destination.mkdir()
                    return rename_noreplace(*args)

                with (
                    patch("observer.rename_noreplace", side_effect=compete),
                    self.assertRaises(FileExistsError),
                ):
                    publish_ready(*readiness)
                if kind == "file":
                    self.assertEqual(self.destination.read_bytes(), b"competitor")
                elif kind == "symlink":
                    self.assertEqual(self.destination.readlink(), self.root / "absent")
                else:
                    self.assertTrue(self.destination.is_dir())
                self.assertEqual(
                    {p.name for p in self.root.iterdir()}, {"runner.json", "ready.json"}
                )
                if kind == "directory":
                    self.destination.rmdir()
                else:
                    self.destination.unlink()

    def test_in_place_binding_mutation_during_read_refuses(self):
        read = os.read
        modified = False

        def mutate(*args):
            nonlocal modified
            data = read(*args)
            if not modified:
                modified = True
                self.source.write_text(json.dumps(runner_binding() | {"run_id": "new"}))
            return data

        with (
            patch("observer.os.read", side_effect=mutate),
            self.assertRaisesRegex(ValueError, "changed"),
        ):
            self.prepare()
        self.assertFalse(self.destination.exists())

    def test_parent_replacement_before_publication_refuses_and_closes(self):
        parent = self.root / "parent"
        parent.mkdir()
        destination = parent / "ready.json"
        readiness = prepare_readiness(self.source, destination, binding())
        moved = self.root / "moved"
        sync = os.fsync

        def replace_parent(fd):
            sync(fd)
            parent.rename(moved)
            parent.mkdir()

        with (
            patch("observer.os.fsync", side_effect=replace_parent),
            patch("observer.os.close", wraps=os.close) as close,
            self.assertRaisesRegex(ValueError, "parent changed"),
        ):
            publish_ready(*readiness)
        self.assertEqual(list(parent.iterdir()), [])
        self.assertEqual(list(moved.iterdir()), [])
        self.assertEqual(close.call_count, 2)
        for call in close.call_args_list:
            with self.assertRaises(OSError):
                os.fstat(call.args[0])

    def test_partial_writes_produce_complete_envelope(self):
        write = os.write

        def partial(fd, data):
            return write(fd, data[:7])

        with patch("observer.os.write", side_effect=partial):
            publish_ready(*self.prepare())
        self.assertEqual(
            json.loads(self.destination.read_bytes())["binding"], runner_binding()
        )

    def test_publication_flushes_file_before_rename_and_directory_after(self):
        readiness = self.prepare()
        calls = []
        fsync = os.fsync

        def sync(fd):
            calls.append("directory" if os.path.isdir(f"/dev/fd/{fd}") else "file")
            return fsync(fd)

        def publish_rename(*args, **kwargs):
            calls.append("rename")
            return rename_noreplace(*args, **kwargs)

        with (
            patch("observer.os.fsync", side_effect=sync),
            patch("observer.rename_noreplace", side_effect=publish_rename),
        ):
            publish_ready(*readiness)
        self.assertEqual(calls, ["file", "rename", "directory"])

    def test_publication_failures_abort_cleanup_and_close_descriptors(self):
        for point in ("write", "file_sync", "rename"):
            with self.subTest(point=point):
                readiness = self.prepare()
                target = {
                    "write": "observer.os.write",
                    "file_sync": "observer.os.fsync",
                    "rename": "observer.rename_noreplace",
                }[point]

                with (
                    patch(target, side_effect=OSError("injected failure")),
                    patch("observer.os.close", wraps=os.close) as close,
                    self.assertRaises(OSError),
                ):
                    publish_ready(*readiness)
                self.assertEqual({p.name for p in self.root.iterdir()}, {"runner.json"})
                self.assertGreaterEqual(close.call_count, 2)
                for call in close.call_args_list:
                    with self.assertRaises(OSError):
                        os.fstat(call.args[0])

    def test_visible_ready_remains_stable_and_acquires_after_housekeeping_failure(self):
        for point in ("directory_sync", "directory_close", "none"):
            with self.subTest(point=point):
                readiness = self.prepare()
                retained = {}
                fsync, close = os.fsync, os.close
                acquired, completed = threading.Event(), threading.Event()

                def consume(retained=retained, acquired=acquired, completed=completed):
                    try:
                        with self.destination.open("rb") as stream:
                            retained["identity"] = os.fstat(stream.fileno())
                            retained["bytes"] = stream.read()
                            acquired.set()
                            if not completed.wait(5):
                                raise TimeoutError("publisher did not finish")
                            retained["after"] = os.fstat(stream.fileno())
                    except (OSError, ValueError) as error:
                        retained["error"] = repr(error)
                        acquired.set()

                reader = threading.Thread(target=consume, daemon=True)

                def expose(*args, reader=reader, acquired=acquired, retained=retained):
                    rename_noreplace(*args)
                    reader.start()
                    self.assertTrue(acquired.wait(5))
                    self.assertNotIn("error", retained)
                    self.assertEqual(retained["identity"].st_nlink, 1)
                    self.assertEqual(
                        {p.name for p in self.root.iterdir()},
                        {"runner.json", "ready.json"},
                    )

                def sync(fd, point=point, retained=retained, fsync=fsync):
                    if stat.S_ISDIR(os.fstat(fd).st_mode):
                        self.assertTrue(retained)
                        if point == "directory_sync":
                            raise OSError("failure after consumer retained READY")
                    return fsync(fd)

                def close_descriptor(fd, point=point, retained=retained, close=close):
                    directory = stat.S_ISDIR(os.fstat(fd).st_mode)
                    close(fd)
                    if directory and retained and point == "directory_close":
                        raise OSError("close failure after consumer retained READY")

                with (
                    patch("observer.sys.platform", "linux"),
                    patch("observer.inventory", return_value={}) as inventory,
                    patch("observer.time.monotonic", side_effect=[0, 0.01, 1]),
                    patch("observer.select.select", return_value=([], [], [])),
                    patch("observer.rename_noreplace", side_effect=expose),
                    patch("observer.os.fsync", side_effect=sync),
                    patch("observer.os.close", side_effect=close_descriptor),
                    patch("observer.sys.stdout", new_callable=io.StringIO) as output,
                ):
                    try:
                        records = collect("session", 0.5, readiness)
                    finally:
                        completed.set()
                        reader.join(5)
                self.assertFalse(reader.is_alive())
                self.assertNotIn("error", retained)
                self.assertEqual(inventory.call_count, 2)
                self.assertEqual(records[-1]["kind"], "end")
                self.assertEqual(
                    json.loads(output.getvalue())["ready_directory_synced"],
                    point != "directory_sync",
                )
                before, after = retained["identity"], retained["after"]
                for field in (
                    "st_dev",
                    "st_ino",
                    "st_size",
                    "st_mtime_ns",
                    "st_ctime_ns",
                    "st_nlink",
                ):
                    self.assertEqual(getattr(before, field), getattr(after, field))
                self.assertEqual(retained["bytes"], self.destination.read_bytes())
                self.assertEqual(
                    json.loads(retained["bytes"])["binding"], runner_binding()
                )
                artifact_root = os.environ.get("OBSERVER_PUBLISH_TEST_EVIDENCE")
                if artifact_root:
                    artifact = Path(artifact_root) / point
                    artifact.mkdir(parents=True)
                    (artifact / "ready.json").write_bytes(retained["bytes"])
                    (artifact / "observable.json").write_text(
                        json.dumps(
                            {
                                "fault": point,
                                "reader_retained_before_housekeeping": True,
                                "stable_identity": True,
                                "identity_at_visibility": {
                                    name: getattr(before, name)
                                    for name in (
                                        "st_dev",
                                        "st_ino",
                                        "st_size",
                                        "st_mtime_ns",
                                        "st_ctime_ns",
                                        "st_nlink",
                                    )
                                },
                                "identity_after_housekeeping": {
                                    name: getattr(after, name)
                                    for name in (
                                        "st_dev",
                                        "st_ino",
                                        "st_size",
                                        "st_mtime_ns",
                                        "st_ctime_ns",
                                        "st_nlink",
                                    )
                                },
                                "single_link": after.st_nlink == 1,
                                "inventory_calls": inventory.call_count,
                                "records": records,
                                "stdout": json.loads(output.getvalue()),
                                "ready_still_present": self.destination.exists(),
                            },
                            indent=2,
                        )
                        + "\n"
                    )
                self.destination.unlink()

    def test_writable_close_failure_prevents_publication(self):
        readiness = self.prepare()
        close = os.close

        def fail_file_close(fd):
            regular = stat.S_ISREG(os.fstat(fd).st_mode)
            close(fd)
            if regular:
                raise OSError("injected pre-publication close failure")

        with (
            patch("observer.os.close", side_effect=fail_file_close),
            self.assertRaises(OSError),
        ):
            publish_ready(*readiness)
        self.assertEqual({p.name for p in self.root.iterdir()}, {"runner.json"})

    def test_file_ready_acquisition_survives_broken_stdout(self):
        readiness = self.prepare()
        with (
            patch("observer.sys.platform", "linux"),
            patch("observer.inventory", return_value={}) as inventory,
            patch("observer.time.monotonic", side_effect=[0, 0.01, 1]),
            patch("observer.select.select", return_value=([], [], [])),
            patch("builtins.print", side_effect=BrokenPipeError("closed reader")),
        ):
            records = collect("session", 0.5, readiness)
        self.assertEqual(inventory.call_count, 2)
        self.assertEqual(records[-1]["kind"], "end")
        self.assertEqual(
            json.loads(self.destination.read_bytes())["binding"], runner_binding()
        )

    def test_unsupported_atomic_rename_aborts_without_publication(self):
        readiness = self.prepare()
        for failure in ("system", "symbol", "filesystem"):
            with self.subTest(failure=failure):
                if failure == "system":
                    mock = patch(
                        "observer.os.uname",
                        return_value=SimpleNamespace(sysname="unsupported"),
                    )
                elif failure == "symbol":
                    mock = patch("observer.ctypes.CDLL", return_value=SimpleNamespace())
                else:
                    mock = patch(
                        "observer.rename_noreplace",
                        side_effect=OSError("unsupported filesystem"),
                    )
                with mock, self.assertRaises(OSError):
                    publish_ready(*readiness)
                self.assertEqual({p.name for p in self.root.iterdir()}, {"runner.json"})

    def test_ready_ordering_empty_inventory_publication_stdout_then_acquisition(self):
        readiness = self.prepare()
        calls = []

        def inventory():
            calls.append("inventory")
            if len(calls) > 1:
                self.assertTrue(self.destination.exists())
                self.assertIn('"READY"', output.getvalue())
            return {}

        with (
            patch("observer.sys.platform", "linux"),
            patch("observer.inventory", side_effect=inventory),
            patch("observer.time.monotonic", side_effect=[0, 0.01, 1]),
            patch("observer.select.select", return_value=([], [], [])),
            patch("observer.sys.stdout", new_callable=io.StringIO) as output,
        ):
            records = collect("session", 0.5, readiness)
        self.assertEqual(calls, ["inventory", "inventory"])
        self.assertEqual(records[-1]["kind"], "end")

    def test_initial_device_and_publish_failure_emit_no_ready_and_abort(self):
        for initial in ({"event1": (1, 2, 3)}, {}):
            with (
                patch("observer.sys.platform", "linux"),
                patch("observer.inventory", return_value=initial),
                patch(
                    "observer.publish_ready", side_effect=OSError("failure")
                ) as publish,
                patch("observer.sys.stdout", new_callable=io.StringIO) as output,
                self.assertRaises((ValueError, OSError)),
            ):
                collect("session", 0.5, self.prepare())
            self.assertEqual(output.getvalue(), "")
            self.assertEqual(publish.call_count, 0 if initial else 1)
            self.assertFalse(self.destination.exists())

    def test_cli_requires_pair_and_refuses_fixture_before_reading_inputs(self):
        base = [
            "observer.py",
            "--binding",
            "absent",
            "--scenario",
            "absent",
            "--output",
            "absent",
        ]
        for extra in (
            ["--runner-binding", str(self.source)],
            ["--ready-output", str(self.destination)],
            [
                "--runner-binding",
                str(self.source),
                "--ready-output",
                str(self.destination),
                "--fixture",
                "absent",
            ],
        ):
            with (
                patch("observer.sys.argv", base + extra),
                patch("observer.sys.stderr", new_callable=io.StringIO),
                self.assertRaises(SystemExit) as error,
            ):
                main()
            self.assertEqual(error.exception.code, 2)
            self.assertFalse(self.destination.exists())


class ParserTests(unittest.TestCase):
    def test_all_abis_and_every_fragment_boundary(self):
        for size, words in ((4, "ii"), (8, "qq")):
            for order, prefix in (("little", "<"), ("big", ">")):
                wire = struct.pack(prefix + words + "HHi", 12, 345, 3, 0, -8192)
                for cut in range(len(wire) + 1):
                    decoder = Decoder(size, order)
                    events = decoder.feed(wire[:cut]) + decoder.feed(wire[cut:])
                    decoder.finish()
                    self.assertEqual(
                        events,
                        [
                            {
                                "kernel_seconds": 12,
                                "kernel_microseconds": 345,
                                "type": 3,
                                "code": 0,
                                "value": -8192,
                            }
                        ],
                    )

    def test_truncation_and_quota_refuse(self):
        decoder = Decoder(8, "little")
        decoder.feed(b"x")
        with self.assertRaisesRegex(ValueError, "truncated"):
            decoder.finish()
        with self.assertRaisesRegex(ValueError, "quota"):
            decoder.feed(b"x" * 1537)
        with self.assertRaises(ValueError):
            Decoder(2, "little")

    def test_invalid_timestamp_refuses(self):
        for seconds, micros in ((-1, 0), (0, 1000000), (0, -1)):
            with self.assertRaises(ValueError):
                Decoder(8, "little").feed(
                    struct.pack("<qqHHi", seconds, micros, 0, 0, 0)
                )


class CorrelationTests(unittest.TestCase):
    def run_records(self, records):
        result = Correlator("session", scenario())
        for item in records:
            result.observe(item)
        return result

    def test_complete_ordered_candidates_include_neutral_and_disconnect(self):
        result = self.run_records(population())
        self.assertEqual(
            result.matches, ["connect", "deflect", "neutral", "disconnect"]
        )
        self.assertEqual(result.errors, [])

    def test_missing_or_reordered_actions_never_infer_completion(self):
        for records in (
            population()[:3],
            [population()[0], population()[-1]],
            population()[1:],
        ):
            self.assertLess(len(self.run_records(records).matches), 4)

    def test_ambiguity_reuse_late_attach_and_syn_dropped_are_sticky(self):
        faults = [
            record("attach", device_alias="second"),
            record("late_attach"),
            record("evdev", type=0, code=3, value=0),
        ]
        for fault in faults:
            result = self.run_records(population()[:1] + [fault] + population()[1:])
            self.assertTrue(result.errors)
            self.assertNotIn("disconnect", result.matches)
        result = self.run_records(population() + [record("attach")])
        self.assertTrue(result.errors)

    def test_foreign_session_refuses(self):
        item = record("attach")
        item["session_id"] = "other"
        with self.assertRaisesRegex(ValueError, "foreign session"):
            self.run_records([item])

    def test_syn_report_required_for_matching(self):
        result = self.run_records(population()[:2])
        self.assertEqual(result.matches, ["connect"])


class AcquisitionTests(unittest.TestCase):
    def test_real_pipe_fragments_and_eof(self):
        read_fd, write_fd = os.pipe()
        decoder = Decoder(8, "little")
        wire = struct.pack("<qqHHi", 4, 0, 3, 1, -100)
        try:
            os.write(write_fd, wire[:7])
            self.assertEqual(decoder.feed(os.read(read_fd, 24)), [])
            os.write(write_fd, wire[7:])
            self.assertEqual(decoder.feed(os.read(read_fd, 24))[0]["value"], -100)
            os.close(write_fd)
            write_fd = None
            self.assertEqual(os.read(read_fd, 24), b"")
            decoder.finish()
        finally:
            os.close(read_fd)
            if write_fd is not None:
                os.close(write_fd)

    def test_collector_ready_initial_snapshot_remove_and_descriptor_cleanup(self):
        identity = (1, 2, 3)
        with (
            patch("observer.sys.platform", "linux"),
            patch("observer.inventory", side_effect=[{}, {"event1": identity}, {}]),
            patch("observer.time.monotonic", side_effect=[0, 0.01, 0.02, 1]),
            patch("observer.os.open", return_value=42),
            patch(
                "observer.os.fstat",
                return_value=SimpleNamespace(st_dev=1, st_ino=2, st_rdev=3),
            ),
            patch("observer.read_axes", return_value={"0": 0, "1": 0}),
            patch("observer.select.select", return_value=([], [], [])),
            patch("observer.os.close") as close,
            patch("observer.sys.stdout", new_callable=io.StringIO) as output,
        ):
            records = collect("session", 0.5)
            self.assertEqual(json.loads(output.getvalue())["state"], "READY")
            self.assertEqual([r["kind"] for r in records], ["attach", "remove", "end"])
            self.assertEqual(records[0]["axes"], {"0": 0, "1": 0})
            close.assert_called_once_with(42)

    def test_ioctl_failure_closes_open_descriptor_and_records_error(self):
        identity = (1, 2, 3)
        with (
            patch("observer.sys.platform", "linux"),
            patch("observer.inventory", side_effect=[{}, {"event1": identity}]),
            patch("observer.time.monotonic", side_effect=[0, 0.01]),
            patch("observer.os.open", return_value=42),
            patch(
                "observer.os.fstat",
                return_value=SimpleNamespace(st_dev=1, st_ino=2, st_rdev=3),
            ),
            patch("observer.read_axes", side_effect=OSError("removed during ioctl")),
            patch("observer.os.close") as close,
            patch("observer.sys.stdout", new_callable=io.StringIO),
        ):
            records = collect("session", 0.5)
            self.assertEqual(records[-1]["kind"], "error")
            close.assert_called_once_with(42)

    def test_axis_read_requests_and_range_validation(self):
        requests = []

        def ioctl(fd, request, data, mutate):
            self.assertEqual((fd, mutate), (42, True))
            requests.append(request)
            data[:] = struct.pack("=6i", 0, -32768, 32767, 0, 0, 0)

        with patch("observer.fcntl.ioctl", side_effect=ioctl):
            self.assertEqual(read_axes(42), {"0": 0, "1": 0})
        self.assertEqual(requests, [0x80184540, 0x80184541])


class FinishTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "runner.json"
        self.source.write_text(json.dumps(runner_binding()))
        self.readiness = prepare_readiness(
            self.source, self.root / "ready.json", binding()
        )
        self.reader, self.writer = os.pipe()
        os.set_blocking(self.reader, False)
        self.addCleanup(self.close_pipe)
        evidence = os.environ.get("OBSERVER_FINISH_TEST_EVIDENCE")
        if evidence:
            Path(evidence).mkdir(parents=True, exist_ok=True)

    def close_pipe(self):
        for descriptor in (self.reader, self.writer):
            try:
                os.close(descriptor)
            except OSError:
                pass

    def command(self, **changes):
        return (
            json.dumps(
                {
                    "schema_version": "1.0.0",
                    "command": "finish",
                    "binding": runner_binding(),
                }
                | changes
            ).encode()
            + b"\n"
        )

    def run_finish(self, wire=None, *, close=True, duration=300, inventory=None):
        output = io.StringIO()
        write = output.write
        sent = False
        sender = None

        def send_large():
            try:
                remaining = memoryview(wire)
                while remaining:
                    remaining = remaining[os.write(self.writer, remaining) :]
            except BrokenPipeError:
                pass
            finally:
                if close:
                    os.close(self.writer)
                    self.writer = -1

        def on_ready(text):
            nonlocal sent, sender
            result = write(text)
            if not sent and '"READY"' in text:
                sent = True
                if wire is not None and len(wire) > 4096:
                    sender = threading.Thread(target=send_large)
                    sender.start()
                elif wire is not None:
                    os.write(self.writer, wire)
                if close and sender is None:
                    os.close(self.writer)
                    self.writer = -1
            return result

        start = time.monotonic()
        with (
            patch("observer.sys.platform", "linux"),
            patch("observer.inventory", side_effect=inventory or dict),
            patch("observer.sys.stdout", output),
            patch.object(output, "write", side_effect=on_ready),
        ):
            records = collect("session", duration, self.readiness, self.reader)
        if sender is not None:
            sender.join(timeout=2)
            self.assertFalse(sender.is_alive())
        elapsed = time.monotonic() - start
        with self.assertRaises(OSError):
            os.fstat(self.reader)
        self.reader = -1
        return records, elapsed

    def test_valid_finish_publishes_promptly_and_retains_boundary(self):
        records, elapsed = self.run_finish(self.command())
        self.assertLess(elapsed, 1)
        self.assertEqual(records[-1]["termination_reason"], "finish_command")
        self.assertEqual(records[-1]["devices"], {})
        self.assertEqual(records[-1]["finish_binding"], runner_binding())
        destination = self.root / "finished"
        rows, manifest = publish(
            destination, binding(), scenario(), records, "synthetic_fixture"
        )
        self.assertTrue(all(row["validity"] == "unavailable" for row in rows))
        self.assertTrue(
            all(row["satisfied"] is None for row in manifest["observations"])
        )
        evidence = os.environ.get("OBSERVER_FINISH_TEST_EVIDENCE")
        if evidence:
            destination = Path(evidence) / "valid-finish"
            destination.parent.mkdir(parents=True, exist_ok=True)
            publish(destination, binding(), scenario(), records, "synthetic_fixture")
            (destination / "observable.json").write_text(
                json.dumps({"elapsed_seconds": elapsed, "descriptor_closed": True})
            )

    def test_foreign_stale_malformed_duplicate_and_oversize_commands_fail(self):
        wires = [
            b"broken\n",
            b"\xff\n",
            b"[" * 2000 + b"]" * 2000 + b"\n",
            self.command().rstrip(b"\n"),
            self.command(command="stop"),
            self.command(extra=True),
            self.command() * 2,
            b"x" * 16385,
            b'{"command":"finish","command":"finish"}\n',
        ]
        wires.extend(
            self.command(binding=runner_binding() | {field: "foreign"})
            for field in runner_binding()
        )
        for index, wire in enumerate(wires):
            with self.subTest(index=index):
                # Each run has a fresh readiness destination and pipe.
                if index:
                    self.readiness = (
                        self.root / f"ready-{index}.json",
                        runner_binding(),
                    )
                    self.reader, self.writer = os.pipe()
                    os.set_blocking(self.reader, False)
                records, elapsed = self.run_finish(wire)
                self.assertLess(elapsed, 1)
                self.assertEqual(records[-1]["kind"], "error")
                self.assertEqual(records[-1]["termination_reason"], "control_error")
                self.assertFalse(
                    any(
                        row.get("termination_reason") == "finish_command"
                        for row in records
                    )
                )

    def test_finish_rejects_utf16_utf32_and_all_bom_variants(self):
        text = self.command().decode("utf-8")
        wires = [("utf-8-bom", b"\xef\xbb\xbf" + self.command())]
        for encoding, bom in (
            ("utf-16-be", b"\xfe\xff"),
            ("utf-16-le", b"\xff\xfe"),
            ("utf-32-be", b"\x00\x00\xfe\xff"),
            ("utf-32-le", b"\xff\xfe\x00\x00"),
        ):
            encoded = text.encode(encoding)
            wires.extend(((encoding, encoded), (encoding + "-bom", bom + encoded)))
        for index, (encoding, wire) in enumerate(wires):
            with self.subTest(encoding=encoding):
                if index:
                    self.reader, self.writer = os.pipe()
                    os.set_blocking(self.reader, False)
                    self.readiness = (
                        self.root / f"encoding-{index}.json",
                        runner_binding(),
                    )
                records, _ = self.run_finish(wire)
                self.assertEqual(records[-1]["termination_reason"], "control_error")
                self.assertFalse(any(row["kind"] == "end" for row in records))

    def control_close_fault(self):
        target = self.reader
        original_close = os.close
        attempts = []

        def close(descriptor):
            original_close(descriptor)
            if descriptor == target:
                attempts.append(descriptor)
                raise OSError("injected consumed control descriptor close failure")

        return patch("observer.os.close", side_effect=close), attempts

    def test_control_close_failure_preserves_final_records_and_manifest(self):
        fault, attempts = self.control_close_fault()
        with fault:
            records, _ = self.run_finish(self.command())
        self.assertEqual(len(attempts), 1)
        self.assertEqual(records[-2]["termination_reason"], "finish_command")
        self.assertEqual(records[-1]["termination_reason"], "control_close_error")
        destination = self.root / "close-error-run"
        publish(destination, binding(), scenario(), records, "synthetic_fixture")
        self.assertTrue((destination / "manifest.json").is_file())

    def test_control_close_failure_preserves_primary_error_before_collection(self):
        os.write(self.writer, self.command())
        fault, attempts = self.control_close_fault()
        with (
            fault,
            patch("observer.sys.platform", "linux"),
            self.assertRaisesRegex(ValueError, "premature finish input"),
        ):
            collect("session", 300, self.readiness, self.reader)
        self.assertEqual(len(attempts), 1)
        with self.assertRaises(OSError):
            os.fstat(self.reader)
        self.reader = -1
        self.assertFalse((self.root / "ready.json").exists())

    def test_control_close_failure_at_quota_refuses_publication_without_retry(self):
        fault, attempts = self.control_close_fault()
        with (
            fault,
            patch("observer.MAX_EVENTS", 1),
            self.assertRaisesRegex(ValueError, "quota"),
        ):
            self.run_finish(self.command())
        self.assertEqual(len(attempts), 1)
        with self.assertRaises(OSError):
            os.fstat(self.reader)
        self.reader = -1

    def test_multiplexed_select_failure_is_neutral_poll_error(self):
        import select

        original_select = select.select

        def fail_multiplexed(readers, writers, exceptional, timeout):
            if self.reader in readers:
                raise OSError("injected multiplexed poll failure")
            return original_select(readers, writers, exceptional, timeout)

        with patch("observer.select.select", side_effect=fail_multiplexed):
            records, _ = self.run_finish(close=False)
        self.assertEqual(records[-1]["termination_reason"], "poll_error")
        self.assertIn("multiplexed", records[-1]["reason"])

    def test_eof_without_command_and_duration_watchdog_are_distinct(self):
        records, _ = self.run_finish()
        self.assertEqual(records[-1]["termination_reason"], "control_error")
        self.assertIn("EOF", records[-1]["reason"])
        self.reader, self.writer = os.pipe()
        os.set_blocking(self.reader, False)
        self.readiness = (self.root / "watchdog.json", runner_binding())
        records, elapsed = self.run_finish(close=False, duration=0.05)
        self.assertLess(elapsed, 1)
        self.assertEqual(records[-1]["termination_reason"], "duration_limit")

    def test_premature_data_or_eof_prevents_ready_and_closes(self):
        os.write(self.writer, self.command())
        with (
            patch("observer.sys.platform", "linux"),
            self.assertRaisesRegex(ValueError, "premature"),
        ):
            collect("session", 300, self.readiness, self.reader)
        self.assertFalse((self.root / "ready.json").exists())
        with self.assertRaises(OSError):
            os.fstat(self.reader)
        self.reader = -1
        self.reader, self.writer = os.pipe()
        os.set_blocking(self.reader, False)
        os.close(self.writer)
        self.writer = -1
        with (
            patch("observer.sys.platform", "linux"),
            self.assertRaisesRegex(ValueError, "premature control EOF"),
        ):
            collect("session", 300, self.readiness, self.reader)
        self.assertFalse((self.root / "ready.json").exists())
        with self.assertRaises(OSError):
            os.fstat(self.reader)
        self.reader = -1

    def test_control_descriptor_rejects_blocking_regular_and_write_end(self):
        for descriptor in (self.writer,):
            with self.assertRaises(ValueError):
                FinishControl(descriptor, runner_binding())
        os.set_blocking(self.reader, True)
        with self.assertRaises(ValueError):
            FinishControl(self.reader, runner_binding())
        with self.source.open("rb") as stream, self.assertRaises(ValueError):
            FinishControl(stream.fileno(), runner_binding())

    def test_fragmented_command_requires_eof_and_rejects_late_duplicate(self):
        control = FinishControl(self.reader, runner_binding())
        control.preflight()
        wire = self.command()
        os.write(self.writer, wire[:15])
        self.assertFalse(control.poll())
        os.write(self.writer, wire[15:])
        self.assertFalse(control.poll())
        os.write(self.writer, wire)
        os.close(self.writer)
        self.writer = -1
        self.assertFalse(control.poll())
        with self.assertRaises(ValueError):
            control.poll()

    def test_fragment_timeout_is_bounded(self):
        control = FinishControl(self.reader, runner_binding())
        os.write(self.writer, b"{")
        with patch("observer.time.monotonic", return_value=10):
            self.assertFalse(control.poll())
        with (
            patch("observer.time.monotonic", return_value=11),
            self.assertRaisesRegex(ValueError, "deadline"),
        ):
            control.poll()

    def device_case(self, payload, *, final_only=False, close_failure=False):
        device_read, device_write = os.pipe()
        os.set_blocking(device_read, False)
        self.addCleanup(os.close, device_read)
        self.addCleanup(os.close, device_write)
        if payload:
            os.write(device_write, payload)
        info = os.fstat(device_read)
        identity = (info.st_dev, info.st_ino, info.st_rdev)
        calls, descriptors = [], []
        original = os.open
        original_close = os.close

        def inventory():
            calls.append("inventory")
            return (
                {"event1": identity} if len(calls) >= (4 if final_only else 2) else {}
            )

        def open_device(path, *args, **kwargs):
            if path != "event1":
                return original(path, *args, **kwargs)
            descriptor = os.dup(device_read)
            descriptors.append(descriptor)
            return descriptor

        def close_device(descriptor):
            original_close(descriptor)
            if close_failure and descriptor in descriptors:
                raise OSError("injected consumed-descriptor close failure")

        with (
            patch("observer.os.open", side_effect=open_device),
            patch("observer.os.close", side_effect=close_device),
            patch("observer.read_axes", return_value={"0": 0, "1": 0}),
        ):
            records, elapsed = self.run_finish(self.command(), inventory=inventory)
        for descriptor in descriptors:
            with self.assertRaises(OSError):
                os.fstat(descriptor)
        return records, elapsed, calls

    def test_close_failure_retains_error_after_final_snapshot(self):
        records, elapsed, _ = self.device_case(b"", close_failure=True)
        self.assertLess(elapsed, 1)
        self.assertEqual(records[-2]["termination_reason"], "finish_command")
        self.assertEqual(records[-1]["termination_reason"], "close_error")
        self.assertIn("consumed-descriptor", records[-1]["reason"])

    def test_final_boundary_retains_present_device_and_events_without_removal(self):
        wire = struct.pack("=qqHHi", 1, 0, 3, 0, 8192)
        records, elapsed, calls = self.device_case(wire, final_only=True)
        self.assertLess(elapsed, 1)
        self.assertEqual(len(calls), 4)
        self.assertEqual([row["kind"] for row in records], ["attach", "evdev", "end"])
        self.assertIn("event1", records[-1]["devices"])
        self.assertEqual(records[-1]["termination_reason"], "finish_command")
        evidence = os.environ.get("OBSERVER_FINISH_TEST_EVIDENCE")
        if evidence:
            publish(
                Path(evidence) / "present-device",
                binding(),
                scenario(),
                records,
                "synthetic_fixture",
            )

    def test_final_partial_record_and_unread_queue_retain_acquisition_errors(self):
        for index, payload in enumerate(
            (b"x", struct.pack("=qqHHi", 1, 0, 3, 0, 1) * 65)
        ):
            if index:
                self.reader, self.writer = os.pipe()
                os.set_blocking(self.reader, False)
                self.readiness = (self.root / "second.json", runner_binding())
            records, elapsed, _ = self.device_case(payload, final_only=True)
            self.assertLess(elapsed, 1)
            self.assertEqual(records[-1]["termination_reason"], "acquisition_error")
            self.assertIn("event1", records[-1]["devices"])
            self.assertFalse(any(row["kind"] == "end" for row in records))
            evidence = os.environ.get("OBSERVER_FINISH_TEST_EVIDENCE")
            if evidence:
                publish(
                    Path(evidence) / f"acquisition-error-{index}",
                    binding(),
                    scenario(),
                    records,
                    "synthetic_fixture",
                )

    def test_quota_failure_refuses_completion_and_closes_control(self):
        with (
            patch("observer.MAX_EVENTS", 0),
            self.assertRaisesRegex(ValueError, "quota"),
        ):
            self.run_finish(self.command())
        with self.assertRaises(OSError):
            os.fstat(self.reader)
        self.reader = -1

    def test_cli_rejects_finish_without_readiness_or_with_stdio(self):
        for extra in (
            ["--finish-fd", "3"],
            [
                "--runner-binding",
                "absent",
                "--ready-output",
                "absent",
                "--finish-fd",
                "0",
            ],
        ):
            with (
                patch(
                    "observer.sys.argv",
                    [
                        "observer.py",
                        "--binding",
                        "absent",
                        "--scenario",
                        "absent",
                        "--output",
                        "absent",
                        *extra,
                    ],
                ),
                patch("observer.sys.stderr", io.StringIO()),
                self.assertRaises(SystemExit) as error,
            ):
                main()
            self.assertEqual(error.exception.code, 2)

    def test_inherited_pipe_cli_finishes_and_publishes_manifest(self):
        self.run_cli()

    def test_cli_control_close_failure_publishes_incomplete_manifest(self):
        self.run_cli(close_failure=True)

    def run_cli(self, close_failure=False):
        evidence = os.environ.get("OBSERVER_FINISH_TEST_EVIDENCE")
        name = "cli-close-error" if close_failure else "cli"
        root = (Path(evidence) if evidence else self.root) / name
        root.mkdir(parents=True)
        (root / "binding.json").write_text(json.dumps(binding()))
        (root / "scenario.json").write_text(json.dumps(scenario()))
        (root / "runner.json").write_text(json.dumps(runner_binding()))
        script = (
            "import observer; observer.sys.platform = 'linux'; "
            "observer.inventory = dict; publish = observer.publish; "
            "observer.publish = lambda root, binding, scenario, records, "
            "provenance: publish(root, binding, scenario, records, "
            "'synthetic_fixture')\n"
        )
        if close_failure:
            script += (
                f"target = {self.reader}; attempts = []; close = observer.os.close\n"
                "def fail_close(descriptor):\n"
                "    close(descriptor)\n"
                "    if descriptor == target and not attempts:\n"
                "        attempts.append(descriptor)\n"
                "        raise OSError('injected consumed control close error')\n"
                "observer.os.close = fail_close\n"
            )
        script += "observer.main()\n"
        if close_failure:
            script += "assert attempts == [target]\n"
        command = [
            sys.executable,
            "-c",
            script,
            "--binding",
            str(root / "binding.json"),
            "--scenario",
            str(root / "scenario.json"),
            "--output",
            str(root / "run"),
            "--runner-binding",
            str(root / "runner.json"),
            "--ready-output",
            str(root / "ready.json"),
            "--finish-fd",
            str(self.reader),
            "--duration",
            "300",
        ]
        process = subprocess.Popen(
            command,
            cwd=Path(__file__).parent,
            pass_fds=(self.reader,),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            import select

            self.assertTrue(select.select([process.stdout], [], [], 5)[0])
            ready = process.stdout.readline()
            self.assertEqual(json.loads(ready)["state"], "READY")
            start = time.monotonic()
            os.write(self.writer, self.command())
            os.close(self.writer)
            self.writer = -1
            stdout, stderr = process.communicate(timeout=5)
            elapsed = time.monotonic() - start
            self.assertEqual(process.returncode, 0, stderr.decode())
            self.assertLess(elapsed, 2)
            self.assertEqual(json.loads(stdout)["state"], "DONE")
            self.assertEqual(
                json.loads(stdout)["finish_status"],
                "incomplete" if close_failure else "finished",
            )
            raw = json.loads((root / "run/raw.json").read_bytes())
            self.assertEqual(
                raw["records"][-1]["termination_reason"],
                "control_close_error" if close_failure else "finish_command",
            )
            if close_failure:
                self.assertEqual(
                    raw["records"][-2]["termination_reason"], "finish_command"
                )
            self.assertEqual(raw["provenance"], "synthetic_fixture")
            (root / "stdout.log").write_bytes(ready + stdout)
            (root / "observable.json").write_text(
                json.dumps(
                    {
                        "exit": process.returncode,
                        "elapsed_after_finish_seconds": elapsed,
                        "synthetic_acquisition": True,
                        "injected_control_close_failure": close_failure,
                        "command": command,
                    }
                )
            )
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

    def test_cli_does_not_report_finish_success_for_error_or_watchdog(self):
        for reason in (
            "control_error",
            "acquisition_error",
            "duration_limit",
            "close_error",
            "control_close_error",
            "poll_error",
        ):
            with (
                patch(
                    "observer.sys.argv",
                    [
                        "observer.py",
                        "--binding",
                        "binding",
                        "--scenario",
                        "scenario",
                        "--output",
                        "output",
                        "--runner-binding",
                        "runner",
                        "--ready-output",
                        "ready",
                        "--finish-fd",
                        "3",
                    ],
                ),
                patch("observer.read_json", side_effect=[binding(), scenario()]),
                patch("observer.prepare_readiness", return_value=self.readiness),
                patch(
                    "observer.collect",
                    return_value=[record("error", termination_reason=reason)],
                ),
                patch("observer.publish"),
                patch("observer.sys.stdout", new_callable=io.StringIO) as output,
            ):
                main()
            self.assertEqual(
                json.loads(output.getvalue())["finish_status"], "incomplete"
            )


class PublisherTests(unittest.TestCase):
    def test_atomic_manifest_hash_and_unavailable_contract(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            rows, manifest = publish(
                root, binding(), scenario(), population(), "synthetic_fixture"
            )
            self.assertEqual(
                manifest, json.loads((root / "manifest.json").read_bytes())
            )
            artifacts = {
                entry["path"]: entry["sha256"] for entry in manifest["artifacts"]
            }
            self.assertEqual(len(manifest["artifacts"]), 2)
            self.assertEqual(set(artifacts), {"raw.json", "host.jsonl"})
            for name, digest in artifacts.items():
                data = (root / name).read_bytes()
                self.assertEqual(digest, hashlib.sha256(data).hexdigest())
                self.assertNotEqual(digest, hashlib.sha256(data + b"\n").hexdigest())
            self.assertEqual(len(rows), 4)
            self.assertTrue(
                all(
                    r["validity"] == "unavailable" and not r["authenticated"]
                    for r in rows
                )
            )
            self.assertTrue(
                all(o["satisfied"] is None for o in manifest["observations"])
            )
            self.assertEqual(
                {p.name for p in root.iterdir()},
                {"raw.json", "host.jsonl", "manifest.json"},
            )
            with self.assertRaises(FileExistsError):
                publish(root, binding(), scenario(), population(), "synthetic_fixture")

    def test_failed_artifact_write_never_publishes_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            with (
                patch(
                    "observer.os.replace",
                    side_effect=OSError("injected publish failure"),
                ),
                self.assertRaises(OSError),
            ):
                publish(root, binding(), scenario(), population(), "synthetic_fixture")
            self.assertFalse((root / "manifest.json").exists())
            self.assertEqual(list(root.iterdir()), [])

    def test_second_artifact_failure_leaves_no_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            replace = os.replace

            def fail_host(source, destination):
                if destination.name == "host.jsonl":
                    raise OSError("injected host publication failure")
                replace(source, destination)

            with (
                patch("observer.os.replace", side_effect=fail_host),
                self.assertRaises(OSError),
            ):
                publish(root, binding(), scenario(), population(), "synthetic_fixture")
            self.assertEqual({p.name for p in root.iterdir()}, {"raw.json"})

    def test_wrong_binding_and_quota_do_not_create_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            wrong = binding() | {"scenario_id": "other"}
            for metadata, records in (
                (wrong, population()),
                (binding(), [record("end")] * 4097),
            ):
                with self.assertRaises(ValueError):
                    publish(root, metadata, scenario(), records, "synthetic_fixture")
                self.assertFalse(root.exists())


def attachment_identity():
    return {
        "schema_version": "1.0.0",
        "runner_binding": runner_binding(),
        "observer_id": "018887b2-499d-4c11-9ea2-4f093aaa087a",
        "host_boot_id": "fixture-boot",
        "pid": 123,
        "process_start_ticks": 1,
        "collector_sha256": "a" * 64,
        "supervisor_sha256": "b" * 64,
        "clock_domain": "host_monotonic",
        "created_ns": 0,
        "authenticated": False,
    }


def attachment_value():
    value = {
        "schema_version": "1.0.0",
        "state": "attached",
        "runner_binding": runner_binding(),
        "launch_nonce": "018887b2-499d-4c11-9ea2-4f093aaa087a",
        "observer_id": attachment_identity()["observer_id"],
        "observed_ns": time.monotonic_ns(),
        "association": "exclusive_launch_candidate",
        "authenticated": False,
        "source_revision": DEPLOYED_REVISION,
        "cluster_identity": "fixture-cluster",
        "session": {
            "namespace": "house",
            "name": "session",
            "uid": "session-uid",
            "resource_version": "1",
            "pairing_name": "fixture",
            "user_name": "fixture",
            "app_name": "fixture",
        },
        "containers": [
            {
                "name": name,
                "container_id": "containerd://fixture-" + name,
                "image_id": "sha256:" + "a" * 64,
                "restart_count": 0,
                "started_at": "fixture-time",
            }
            for name in ("wolf-agent", "wolf", "app")
        ],
        "wolf_session_id": "123",
        "devices": [
            {
                "session_id": "123",
                "path": "/dev/input/event1",
                "major": 13,
                "minor": 65,
                "generation": 1,
            }
        ],
        "sources": [
            {"path": "source.json", "sha256": hashlib.sha256(b"{}").hexdigest()}
        ],
    }
    parent = value["session"]
    for kind, owner in (
        ("deployment", "Session"),
        ("replicaset", "Deployment"),
        ("pod", "ReplicaSet"),
    ):
        item = {
            "namespace": "house",
            "name": kind,
            "uid": kind + "-uid",
            "resource_version": "1",
            "owner_references": [
                {
                    "kind": owner,
                    "name": parent["name"],
                    "uid": parent["uid"],
                    "controller": True,
                }
            ],
        }
        if kind == "pod":
            item.update(node_name="node", node_uid="node-uid")
        value[kind] = item
        parent = item
    return value


class RuntimeAttachmentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name).resolve()
        self.path = self.root / "runtime.json"
        self.state = RuntimeAttachment(
            attachment_identity(),
            runner_binding(),
            self.path,
            self.root / "attached.json",
        )
        self.reader, self.writer = os.pipe()
        os.set_blocking(self.reader, False)
        self.control = FinishControl(self.reader, runner_binding())
        (self.root / "source.json").write_bytes(b"{}")
        self.launch = {
            "schema_version": "1.0.0",
            "state": "launched",
            "binding": runner_binding(),
            "launch_nonce": "018887b2-499d-4c11-9ea2-4f093aaa087a",
        }
        (self.root / "observer-launch.json").write_text(json.dumps(self.launch))

    def tearDown(self):
        os.close(self.reader)
        if self.writer is not None:
            os.close(self.writer)
        self.temp.cleanup()

    def test_owner_chain_and_device_validation(self):
        self.state.launch = self.launch
        self.state.validate(attachment_value())
        mutations = [
            lambda v: v["pod"]["owner_references"][0].update(uid="foreign"),
            lambda v: v["deployment"]["owner_references"][0].update(controller=1),
            lambda v: v["devices"][0].update(session_id="foreign"),
            lambda v: v["devices"][0].update(path="/dev/input/../event1"),
            lambda v: v["devices"].append(
                dict(v["devices"][0], path="/dev/input/event2")
            ),
            lambda v: v["containers"][0].update(restart_count=True),
            lambda v: v.update(authenticated=True),
            lambda v: v.update(source_revision="local-checkout"),
            lambda v: v.update(extra=True),
            lambda v: v["sources"][0].update(path="../secret"),
            lambda v: v["pod"].update(namespace="foreign"),
            lambda v: v["containers"][0].update(image_id="mutable-tag"),
        ]
        for change in mutations:
            value = attachment_value()
            change(value)
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.state.validate(value)

    def test_strict_encoding_duplicates_and_bounds(self):
        for data in (
            b"{}" * 40000,
            b'{"a":1,"a":2}',
            b"\xef\xbb\xbf{}",
            "{}".encode("utf-16"),
            b"[]",
        ):
            with self.subTest(data=data[:30]), self.assertRaises(ValueError):
                self.state.validate(strict_document(data))

    def test_missing_cancelled_and_foreign_attachment(self):
        with self.assertRaisesRegex(ValueError, "deadline"):
            self.state.await_attachment(time.monotonic() + 0.02, self.control)
        os.close(self.writer)
        self.writer = None
        with self.assertRaises(ValueError):
            self.state.await_attachment(time.monotonic() + 1, self.control)
        self.assertFalse((self.root / "attached.json").exists())

    def test_postlaunch_nonce_and_prelaunch_refusal(self):
        launch = self.root / "observer-launch.json"
        launch.unlink()
        self.path.write_text(json.dumps(attachment_value()))
        with self.assertRaisesRegex(ValueError, "before launch"):
            self.state.await_attachment(time.monotonic() + 1, self.control)
        launch.write_text(json.dumps(self.launch))
        value = attachment_value()
        value["launch_nonce"] = "foreign"
        self.path.write_text(json.dumps(value))
        with self.assertRaisesRegex(ValueError, "binding mismatch"):
            self.state.await_attachment(time.monotonic() + 1, self.control)
        self.assertFalse((self.root / "attached.json").exists())

    def test_attachment_alias_and_source_alias_refusal(self):
        actual = self.root / "actual.json"
        actual.write_text(json.dumps(attachment_value()))
        self.path.symlink_to(actual)
        with self.assertRaises(ValueError):
            self.state.await_attachment(time.monotonic() + 1, self.control)
        self.path.unlink()
        self.path.write_bytes(actual.read_bytes())
        os.link(self.root / "source.json", self.root / "source-alias.json")
        with self.assertRaises(ValueError):
            self.state.await_attachment(time.monotonic() + 1, self.control)
        self.assertFalse((self.root / "attached.json").exists())

    def test_identical_bytes_new_inode_is_terminal(self):
        self.path.write_text(json.dumps(attachment_value()))
        with patch.object(self.state, "inventory", return_value={}):
            self.state.await_attachment(time.monotonic() + 1, self.control)
        replacement = self.root / "replacement"
        replacement.write_bytes(self.path.read_bytes())
        replacement.replace(self.path)
        with self.assertRaisesRegex(ValueError, "changed"):
            self.state.inventory()

    def test_descriptor_and_axes_precede_attachment_ack(self):
        from observer import collect_controlled

        self.state.data = b"{}"
        self.state.value = attachment_value()
        self.state.launch = self.launch
        fake = SimpleNamespace(descriptor=99, binding=runner_binding())
        polls = iter((False, True))
        fake.poll = lambda: next(polls)
        events = []

        def acknowledge():
            self.assertEqual(events, ["axes"])
            events.append("ack")
            self.state.acknowledged = True

        with (
            patch.object(self.state, "await_attachment"),
            patch.object(
                self.state, "inventory", return_value={"/dev/input/event1": (1, 2, 3)}
            ),
            patch.object(
                self.state,
                "acknowledge",
                side_effect=lambda: None if self.state.acknowledged else acknowledge(),
            ),
            patch("observer.os.open", return_value=42),
            patch(
                "observer.os.fstat",
                return_value=SimpleNamespace(st_dev=1, st_ino=2, st_rdev=3),
            ),
            patch(
                "observer.read_axes",
                side_effect=lambda fd: events.append("axes") or {"0": 0, "1": 0},
            ),
            patch("observer.select.select", return_value=([], [], [])),
            patch("observer.os.close"),
        ):
            records = collect_controlled("session", 1, fake, self.state)
        self.assertEqual(events, ["axes", "ack"])
        self.assertEqual(records[-1]["termination_reason"], "finish_command")
        self.assertFalse(any(row["kind"] == "remove" for row in records))

    def test_retained_attachment_and_runner_envelope(self):
        self.path.write_bytes(json.dumps(attachment_value()).encode())
        with patch.object(self.state, "inventory", return_value={}):
            self.state.await_attachment(time.monotonic() + 1, self.control)
        self.state.acknowledge()
        envelope = json.loads((self.root / "attached.json").read_bytes())
        self.assertEqual(
            envelope["runtime_attachment_sha256"],
            hashlib.sha256(self.path.read_bytes()).hexdigest(),
        )
        self.assertFalse(envelope["authenticated"])
        self.assertEqual(self.state.evidence()["continuity"], "unverified")
        records = [record("runtime_attachment", **self.state.evidence())]
        publish(
            self.root / "published", binding(), scenario(), records, "synthetic_fixture"
        )
        raw = json.loads((self.root / "published/raw.json").read_bytes())
        self.assertEqual(
            raw["records"][0]["attachment_bytes"].encode(), self.path.read_bytes()
        )
        rows = (self.root / "published/host.jsonl").read_text().splitlines()
        self.assertTrue(all(json.loads(row)["authenticated"] is False for row in rows))
        if evidence := os.environ.get("ATTACHMENT_EVIDENCE"):
            import shutil

            shutil.copytree(self.root / "published", Path(evidence) / "attachment")

    def test_source_hash_alias_and_attachment_replacement(self):
        value = attachment_value()
        self.path.write_bytes(json.dumps(value).encode())
        (self.root / "source.json").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "digest"):
            self.state.await_attachment(time.monotonic() + 1, self.control)
        (self.root / "source.json").write_bytes(b"{}")
        with patch.object(self.state, "inventory", return_value={}):
            self.state.await_attachment(time.monotonic() + 1, self.control)
        for change in (
            lambda v: v["pod"].update(uid="replacement"),
            lambda v: v["containers"][0].update(restart_count=1),
        ):
            value = attachment_value()
            change(value)
            self.path.write_bytes(json.dumps(value).encode())
            with self.assertRaisesRegex(ValueError, "changed"):
                self.state.inventory()

    def test_exact_device_identity_no_name_scan(self):
        self.path.write_bytes(json.dumps(attachment_value()).encode())
        self.state.data = self.path.read_bytes()
        self.state.value = strict_document(self.state.data)
        from observer import file_identity

        self.state.file_identity = file_identity(self.path.stat())
        original = os.stat
        info = SimpleNamespace(
            st_mode=stat.S_IFCHR, st_dev=1, st_ino=2, st_rdev=os.makedev(13, 65)
        )

        def read(path, *args, **kwargs):
            return (
                info
                if str(path) == "/dev/input/event1"
                else original(path, *args, **kwargs)
            )

        with (
            patch("observer.os.stat", side_effect=read),
            patch(
                "observer.Path.resolve",
                side_effect=lambda strict=True: Path("/dev/input"),
            ),
            patch("observer.inventory") as scan,
        ):
            # Only device-parent resolution is bypassed on the non-Linux test host.
            with patch("supervisor.snapshot", return_value=self.state.data):
                self.assertEqual(list(self.state.inventory()), ["/dev/input/event1"])
                info.st_ino = 3
                with self.assertRaisesRegex(ValueError, "incarnation"):
                    self.state.inventory()
            scan.assert_not_called()


class LateDeviceFeedTests(unittest.TestCase):
    def setUp(self):
        RuntimeAttachmentTests.setUp(self)
        self.feed = self.root / "devices.json"
        self.state.feed_path = self.feed
        value = attachment_value()
        value["devices"] = []
        self.path.write_text(json.dumps(value))
        self.state.await_attachment(time.monotonic() + 1, self.control)

    tearDown = RuntimeAttachmentTests.tearDown

    def value(self):
        return {
            "schema_version": "1.0.0",
            "state": "devices_attached",
            "runner_binding": runner_binding(),
            "launch_nonce": self.launch["launch_nonce"],
            "observer_id": self.state.identity["observer_id"],
            "runtime_attachment_sha256": hashlib.sha256(self.state.data).hexdigest(),
            "observed_ns": time.monotonic_ns(),
            "continuity": "unverified",
            "devices": attachment_value()["devices"],
            "sources": attachment_value()["sources"],
        }

    def test_preexisting_feed_refuses_acknowledgement(self):
        self.feed.write_text(json.dumps(self.value()))
        with self.assertRaises(ValueError):
            self.state.acknowledge()
        self.assertFalse(self.state.ready.exists())

    def test_strict_feed_and_sources(self):
        self.state.acknowledge()
        for change in (
            {"launch_nonce": "foreign"},
            {"runtime_attachment_sha256": "a" * 64},
            {"continuity": "verified"},
            {"observed_ns": True},
            {"observed_ns": 0},
            {"devices": []},
            {"extra": 1},
            {"sources": [{"path": "source.json", "sha256": "b" * 64}]},
            {"devices": [dict(attachment_value()["devices"][0], session_id="other")]},
            {"devices": attachment_value()["devices"] * 2},
        ):
            self.feed.write_text(json.dumps(self.value() | change))
            with self.assertRaises(ValueError):
                self.state.poll_feed()
        for data in (
            b"\xef\xbb\xbf{}",
            b'{"x":1,"x":2}',
            b"x" * 65537,
            "{}".encode("utf-16"),
        ):
            self.feed.write_bytes(data)
            with self.assertRaises(ValueError):
                self.state.poll_feed()

    def test_alias_and_identical_byte_replacement(self):
        self.state.acknowledge()
        target = self.root / "target"
        target.write_text(json.dumps(self.value()))
        self.feed.symlink_to(target)
        with self.assertRaises(ValueError):
            self.state.poll_feed()
        self.feed.unlink()
        os.link(target, self.feed)
        with self.assertRaises(ValueError):
            self.state.poll_feed()
        self.feed.unlink()
        target.rename(self.feed)
        self.state.poll_feed()
        target.write_bytes(self.feed.read_bytes())
        target.replace(self.feed)
        with self.assertRaisesRegex(ValueError, "feed changed"):
            self.state.inventory()

    def test_finish_without_feed_publishes_unavailable(self):
        from observer import collect_controlled

        polls = iter((False, True))
        control = SimpleNamespace(
            descriptor=99, binding=runner_binding(), poll=lambda: next(polls)
        )
        with (
            patch.object(self.state, "await_attachment"),
            patch("observer.select.select", return_value=([], [], [])),
        ):
            records = collect_controlled("session", 1, control, self.state)
        self.assertEqual(records[-1]["termination_reason"], "finish_command")
        self.assertFalse(
            any(r["kind"] in ("attach", "evdev", "remove") for r in records)
        )

    def test_missing_feed_watchdog_and_control_loss(self):
        from observer import collect_controlled

        self.state.acknowledge()
        with patch.object(self.state, "await_attachment"):
            records = collect_controlled("session", 0.03, self.control, self.state)
        self.assertEqual(records[-1]["termination_reason"], "duration_limit")
        os.close(self.writer)
        self.writer = None
        with patch.object(self.state, "await_attachment"):
            records = collect_controlled("session", 1, self.control, self.state)
        self.assertEqual(records[-1]["termination_reason"], "control_error")
        self.assertFalse(any(r["kind"] == "attach" for r in records))

    def test_feed_descriptor_mismatch_and_incarnation(self):
        self.state.acknowledge()
        self.feed.write_text(json.dumps(self.value()))
        self.state.poll_feed()
        original_stat, original_resolve = os.stat, Path.resolve
        info = SimpleNamespace(
            st_mode=stat.S_IFCHR, st_dev=1, st_ino=2, st_rdev=os.makedev(13, 66)
        )
        with (
            patch(
                "observer.os.stat",
                side_effect=lambda path, *a, **kw: (
                    info
                    if str(path) == "/dev/input/event1"
                    else original_stat(path, *a, **kw)
                ),
            ),
            patch.object(
                Path,
                "resolve",
                lambda path, *a, **kw: (
                    path
                    if str(path) == "/dev/input"
                    else original_resolve(path, *a, **kw)
                ),
            ),
        ):
            with self.assertRaisesRegex(ValueError, "descriptor mismatch"):
                self.state.inventory()
            info.st_rdev = os.makedev(13, 65)
            self.assertIn("/dev/input/event1", self.state.inventory())
            info.st_ino = 3
            with self.assertRaisesRegex(ValueError, "incarnation"):
                self.state.inventory()

    def test_late_feed_real_events_remain_unavailable(self):
        from observer import collect_controlled

        descriptor = 42
        identity = (1, 2, os.makedev(13, 65))
        info = SimpleNamespace(
            st_dev=1, st_ino=2, st_rdev=identity[2], st_mode=stat.S_IFCHR
        )
        original_stat = os.stat
        original_resolve = Path.resolve
        event = struct.Struct("@llHHi")
        payload = event.pack(1, 0, 3, 0, 8192) + event.pack(1, 1, 0, 0, 0)
        sent = False
        reads = 0

        def poll():
            nonlocal sent
            if not self.state.acknowledged:
                return False
            if not sent:
                self.feed.write_text(json.dumps(self.value()))
                sent = True
                return False
            return True

        def select_ready(fds, *_):
            return ([descriptor] if descriptor in fds and reads == 0 else [], [], [])

        def read(fd, size):
            nonlocal reads
            self.assertEqual(fd, descriptor)
            reads += 1
            return payload

        real_read = os.read
        with (
            patch.object(self.state, "await_attachment"),
            patch(
                "observer.os.stat",
                side_effect=lambda path, *a, **kw: (
                    info
                    if str(path) == "/dev/input/event1"
                    else original_stat(path, *a, **kw)
                ),
            ),
            patch.object(
                Path,
                "resolve",
                lambda path, *a, **kw: (
                    path
                    if str(path) == "/dev/input"
                    else original_resolve(path, *a, **kw)
                ),
            ),
            patch("observer.os.open", wraps=os.open) as opening,
            patch("observer.os.fstat", wraps=os.fstat) as fstat,
            patch("observer.os.close", wraps=os.close) as closing,
            patch(
                "observer.os.read",
                side_effect=lambda fd, size: (
                    read(fd, size) if fd == descriptor else real_read(fd, size)
                ),
            ),
            patch("observer.read_axes", return_value={"0": 0, "1": 0}),
            patch("observer.select.select", side_effect=select_ready),
            patch("observer.inventory") as scan,
        ):
            original_open, original_fstat, original_close = (
                opening._mock_wraps,
                fstat._mock_wraps,
                closing._mock_wraps,
            )
            opening.side_effect = lambda path, *a, **kw: (
                descriptor
                if str(path) == "/dev/input/event1"
                else original_open(path, *a, **kw)
            )
            fstat.side_effect = lambda fd: (
                info if fd == descriptor else original_fstat(fd)
            )
            closing.side_effect = lambda fd: (
                None if fd == descriptor else original_close(fd)
            )
            records = collect_controlled(
                "session",
                1,
                SimpleNamespace(descriptor=99, binding=runner_binding(), poll=poll),
                self.state,
            )
            scan.assert_not_called()
        self.assertEqual(records[-1]["termination_reason"], "finish_command", records)
        self.assertEqual(sum(r["kind"] == "evdev" for r in records), 2)
        self.assertEqual(sum(r["kind"] == "late_device_attachment" for r in records), 1)
        correlator = Correlator("session", scenario())
        for row in records:
            correlator.observe(row)
        self.assertIn("late_attach", correlator.errors)
        self.assertEqual(correlator.matches, [])
        publish(
            self.root / "published", binding(), scenario(), records, "synthetic_fixture"
        )
        if evidence := os.environ.get("ATTACHMENT_EVIDENCE"):
            import shutil

            shutil.copytree(
                self.root / "published", Path(evidence) / "late-device-feed"
            )


if __name__ == "__main__":
    unittest.main()
