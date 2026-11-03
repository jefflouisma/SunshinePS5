"""Synthetic cross-process relay verification; no SSH server or device acceptance."""

import io
import json
import os
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

import remote_relay as relay
from observer import encoded, publish_envelope
from test_observer import binding, runner_binding, scenario


class RelayTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        self.remote = self.root / "remote"
        self.remote.mkdir(mode=0o700)
        for name, data in (
            ("observer-binding.json", encoded(runner_binding())),
            ("collector.json", encoded(binding())),
            ("scenario.json", encoded(scenario())),
            ("known_hosts", b"observer.test ssh-ed25519 Zml4dHVyZQ==\n"),
            ("identity", b"synthetic-not-a-real-key\n"),
        ):
            (self.root / name).write_bytes(data)
        (self.root / "identity").chmod(0o600)
        self.args = {
            "host": "observer.test",
            "user": "observer",
            "port": 22,
            "known_hosts": self.root / "known_hosts",
            "identity_file": self.root / "identity",
            "runner_binding": self.root / "observer-binding.json",
            "collector": self.root / "collector.json",
            "scenario": self.root / "scenario.json",
            "ready": self.root / "ready.json",
            "attached": self.root / "attached.json",
            "output": self.root / "evidence",
            "remote_root": self.remote,
            "provider": {key: "fixture" for key in relay.PROVIDER_FIELDS},
            "expected_hashes": relay.code_hashes(),
            "timeout": 6,
            "enable": True,
            "ssh_command": [
                sys.executable,
                str(Path(__file__).with_name("remote_relay_fixture.py")),
            ],
        }

    def tearDown(self):
        self.temporary.cleanup()

    def population(self, name, success=True, fault=None, signal_runner=True):
        stop = threading.Event()
        errors = []

        def runner():
            try:
                for file, output in (
                    ("ready.json", "observer-launch.json"),
                    ("attached.json", "observer-cancel.json"),
                ):
                    while not (self.root / file).exists():
                        if stop.wait(0.005):
                            return
                    if output == "observer-launch.json":
                        value = {
                            "schema_version": "1.0.0",
                            "state": "launched",
                            "binding": runner_binding(),
                            "launch_nonce": "018887b2-499d-4c11-9ea2-4f093aaa087a",
                        }
                    else:
                        value = {
                            "schema_version": "1.0.0",
                            "state": "host_cancel_completed",
                            "binding": runner_binding(),
                            "request_succeeded": success,
                        }
                    publish_envelope(self.root / output, encoded(value))
            except (OSError, ValueError) as error:
                errors.append(str(error))

        thread = threading.Thread(target=runner) if signal_runner else None
        if thread:
            thread.start()
        started = time.monotonic()
        try:
            with patch.dict(os.environ, {"RELAY_FAULT": fault or ""}):
                result = relay.relay(**self.args)
        finally:
            stop.set()
            if thread:
                thread.join(timeout=1)
        self.assertEqual(errors, [])
        self.assertLess(time.monotonic() - started, self.args["timeout"] + 5)
        self.assertEqual(result["acceptance"], "Incomplete")
        self.assertEqual(result["cleanup_errors"], [])
        self.assertEqual(
            json.loads((self.root / "evidence/relay.json").read_bytes()), result
        )
        # A connected success confirms remote reaping; error paths only report
        # unconfirmed remote cleanup because a lost network cannot prove reaping.
        if evidence := os.environ.get("RELAY_EVIDENCE"):
            destination = Path(evidence) / name
            destination.mkdir(parents=True)
            shutil.copytree(self.root / "evidence", destination / "local")
            shutil.copytree(self.remote, destination / "remote")
            (destination / "observable.json").write_bytes(
                encoded(
                    {
                        "transport": result["transport"],
                        "acceptance": result["acceptance"],
                        "elapsed_seconds": time.monotonic() - started,
                        "remote_cleanup": result["remote_cleanup"],
                        "request_succeeded": result["request_succeeded"],
                        "runner_errors": errors,
                    }
                )
            )
        return result

    def test_success_true_and_false_preserve_real_supervisor_population(self):
        for success in (True, False):
            with self.subTest(success=success):
                if not success:
                    self.tearDown()
                    self.setUp()
                result = self.population("cancel-" + str(success).lower(), success)
                self.assertEqual(result["transport"], "published", result)
                self.assertEqual(result["request_succeeded"], success)
                self.assertEqual(result["remote_cleanup"], "owners_reaped")
                for name, expected in result["artifacts"].items():
                    self.assertEqual(
                        relay.digest((self.root / "evidence" / name).read_bytes()),
                        expected,
                    )
                local = self.root / "evidence/remote"
                raw = json.loads((local / "supervised/collector/raw.json").read_bytes())
                self.assertEqual(raw["provenance"], "synthetic_fixture")
                self.assertTrue(
                    any(row["kind"] == "runtime_attachment" for row in raw["records"])
                )
                self.assertEqual(
                    (self.root / "ready.json").read_bytes(),
                    (local / "ready.json").read_bytes(),
                )
                self.assertEqual(
                    (self.root / "attached.json").read_bytes(),
                    (local / "attached.json").read_bytes(),
                )
                # This is the real collector PID retained by the real supervisor.
                session = next(self.remote.iterdir())
                pid = json.loads((session / "supervised/process.json").read_bytes())[
                    "pid"
                ]
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)

    def test_timeout_and_partial_frame_are_bounded(self):
        for fault in ("timeout", "partial"):
            with self.subTest(fault=fault):
                if fault == "partial":
                    self.tearDown()
                    self.setUp()
                self.args["timeout"] = 0.3
                result = self.population(fault, fault=fault, signal_runner=False)
                self.assertEqual(result["transport"], "incomplete")
                self.assertEqual(result["reason"], "TimeoutError")
                self.assertEqual(result["remote_cleanup"], "unconfirmed")

    def test_large_real_collector_artifact_preserved(self):
        result = self.population("large", fault="large")
        self.assertEqual(result["transport"], "published", result)
        path = self.root / "evidence/remote/supervised/collector/raw.json"
        self.assertGreater(path.stat().st_size, 16384)
        self.assertEqual(
            relay.digest(path.read_bytes()),
            result["artifacts"]["remote/supervised/collector/raw.json"],
        )

    def test_supervisor_in_progress_files_are_not_read_or_frozen(self):
        config = {
            "root": str(self.remote),
            "provider": self.args["provider"],
            "binding": runner_binding(),
            "collector": binding(),
            "scenario": scenario(),
        }
        with patch("remote_relay.sys.platform", "linux"):
            owner = relay.Owner(
                config, "018887b2-499d-4c11-9ea2-4f093aaa087a", time.monotonic() + 5
            )
        owner.processes = [Mock(), Mock()]
        for process in owner.processes:
            process.poll.return_value = None
        directory = owner.root / "supervised"
        directory.mkdir(mode=0o700)
        path = directory / "collector-binding.json"
        with path.open("xb") as writer:
            writer.write(b'{"partial":')
            writer.flush()
            self.assertEqual(owner.poll(), {"pending": True})
            self.assertNotIn(path, owner.frozen.files)
            writer.write(b"true}\n")
        self.assertEqual(owner.poll(), {"pending": True})
        self.assertNotIn(path, owner.frozen.files)
        owner.processes[1].poll.return_value = 2
        name, data = relay.unpack(owner.poll()["artifact"], relay.REMOTE_FILES)
        self.assertEqual(name, "supervised/collector-binding.json")
        self.assertEqual(data, b'{"partial":true}\n')

    def test_supervisor_exit_during_scan_defers_completion_until_export(self):
        config = {
            "root": str(self.remote),
            "provider": self.args["provider"],
            "binding": runner_binding(),
            "collector": binding(),
            "scenario": scenario(),
        }
        with patch("remote_relay.sys.platform", "linux"):
            owner = relay.Owner(
                config, "018887b2-499d-4c11-9ea2-4f093aaa087a", time.monotonic() + 5
            )
        directory = owner.root / "supervised"
        directory.mkdir(mode=0o700)
        report = directory / "supervisor.json"
        child = subprocess.Popen(
            [
                sys.executable,
                "-c",
                (
                    "import pathlib,sys; "
                    "pathlib.Path(sys.argv[1]).write_bytes(b'{}\\n'); "
                    "print('ready', flush=True); sys.stdin.read(1)"
                ),
                str(report),
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            start_new_session=True,
        )
        self.addCleanup(child.stdout.close)
        self.addCleanup(child.stdin.close)
        provider = Mock()
        provider.poll.return_value = 0
        owner.processes = [provider, child]
        original = owner.frozen.read
        released = False

        def read(path, *args, **kwargs):
            nonlocal released
            if path.name == "provider-report.json" and not released:
                # The supervisor exits after its files were skipped in this scan.
                child.stdin.write(b"x")
                child.stdin.flush()
                self.assertEqual(child.wait(timeout=2), 0)
                released = True
            return original(path, *args, **kwargs)

        try:
            self.assertTrue(select.select([child.stdout], [], [], 2)[0])
            self.assertEqual(child.stdout.readline(), b"ready\n")
            self.assertIsNone(child.poll())
            with patch.object(owner.frozen, "read", side_effect=read):
                first = owner.poll()
                self.assertEqual(first, {"pending": True})
                self.assertTrue(released)
                self.assertNotIn(report, owner.frozen.files)
                second = owner.poll()
                self.assertEqual(
                    relay.unpack(second["artifact"], relay.REMOTE_FILES),
                    ("supervised/supervisor.json", b"{}\n"),
                )
                third = owner.poll()
                self.assertEqual(third, {"done": True, "cleanup": "owners_reaped"})
            if evidence := os.environ.get("RELAY_EVIDENCE"):
                destination = Path(evidence) / "exit-during-scan"
                destination.mkdir(parents=True)
                shutil.copytree(self.remote, destination / "remote")
                (destination / "observable.json").write_bytes(
                    encoded({"first": first, "second": second, "third": third})
                )
        finally:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=2)

    def test_large_publication_no_replace_and_quota(self):
        destination = self.root / "large"
        data = b"x" * relay.FILE_LIMIT
        self.assertTrue(relay.publish_artifact(destination, data))
        self.assertEqual(relay.snapshot(destination, relay.FILE_LIMIT), data)
        with self.assertRaises((FileExistsError, ValueError)):
            relay.publish_artifact(destination, b"replacement")
        self.assertEqual(destination.read_bytes(), data)
        with self.assertRaises(ValueError):
            relay.publish_artifact(self.root / "oversize", data + b"x")
        self.assertFalse((self.root / "oversize").exists())

    def test_hash_tamper_replacement_and_foreign_nonce_refuse(self):
        for fault in ("hash", "replace", "directory", "nonce"):
            with self.subTest(fault=fault):
                if fault != "hash":
                    self.tearDown()
                    self.setUp()
                result = self.population(fault, fault=fault)
                self.assertEqual(result["transport"], "incomplete")
                self.assertFalse((self.root / "attached.json").exists())
                self.assertEqual(result["remote_cleanup"], "unconfirmed")

    def test_wrong_host_and_code_pin_fail_before_ready(self):
        for mismatch in ("host", "hash"):
            with self.subTest(mismatch=mismatch):
                if mismatch == "hash":
                    self.tearDown()
                    self.setUp()
                    self.args["expected_hashes"]["observer.py"] = "0" * 64
                else:
                    self.args["host"] = "wrong.example"
                result = self.population("wrong-" + mismatch, signal_runner=False)
                self.assertEqual(result["transport"], "incomplete")
                self.assertFalse((self.root / "ready.json").exists())
                self.assertEqual(list(self.remote.iterdir()), [])

    def test_stale_launch_refused_before_transport(self):
        (self.root / "observer-launch.json").write_bytes(b"stale")
        result = self.population("stale", signal_runner=False)
        self.assertEqual(result["transport"], "incomplete")
        self.assertEqual(list(self.remote.iterdir()), [])

    def test_opt_in_timeout_and_ssh_syntax_are_strict(self):
        for value in (False,):
            with self.assertRaises(ValueError):
                relay.relay(**(self.args | {"enable": value}))
        for value in (0, -1, 301, float("nan"), float("inf")):
            with self.assertRaises(ValueError):
                relay.relay(**(self.args | {"timeout": value}))
        for host in ("-proxy", "host;id", "host $(id)", "host\n", "user@host"):
            with self.assertRaises(ValueError):
                relay.ssh_argv(host, "observer", 22, "/file", "/identity")
        self.assertFalse((self.root / "evidence").exists())

    def test_frozen_refuses_symlink_hardlink_nonregular_mutation_and_permissions(self):
        source = self.root / "source"
        source.write_bytes(b"safe")
        frozen = relay.Frozen()
        frozen.read(source)
        replacement = self.root / "replacement"
        replacement.write_bytes(b"safe")
        replacement.replace(source)
        with self.assertRaises(ValueError):
            frozen.read(source)
        for kind in (
            "symlink",
            "hardlink",
            "fifo",
            "directory",
            "writable",
            "empty",
            "large",
        ):
            path = self.root / kind
            if kind == "symlink":
                path.symlink_to(source)
            elif kind == "hardlink":
                os.link(source, path)
            elif kind == "fifo":
                os.mkfifo(path)
            elif kind == "directory":
                path.mkdir()
            else:
                path.write_bytes(
                    b""
                    if kind == "empty"
                    else b"x" * (relay.FILE_LIMIT + 1 if kind == "large" else 1)
                )
                if kind == "writable":
                    path.chmod(0o666)
            with self.subTest(kind=kind), self.assertRaises((OSError, ValueError)):
                relay.Frozen().read(path)

    def test_directory_replacement_and_alias_fail(self):
        identity = relay.directory_identity(self.remote)
        self.remote.rename(self.root / "old")
        self.remote.mkdir(mode=0o700)
        self.assertNotEqual(identity, relay.directory_identity(self.remote))
        alias = self.root / "alias"
        alias.symlink_to(self.remote)
        with self.assertRaises(OSError):
            relay.directory_identity(alias)
        self.remote.chmod(0o755)
        with self.assertRaises(ValueError):
            relay.directory_identity(self.remote)

    def test_private_parent_creation_never_follows_alias(self):
        (self.root / "alias").symlink_to(self.remote)
        with self.assertRaises(OSError):
            relay.private_parents(self.root, "alias/escape")
        self.assertFalse((self.remote / "escape").exists())

    def cleanup_child(self, script):
        process = subprocess.Popen(
            [sys.executable, "-c", script],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            text=True,
        )

        def close():
            if process.poll() is None:
                process.kill()
            process.wait(timeout=3)
            process.stdout.close()

        self.addCleanup(close)
        return process

    def ready_cleanup_child(self, handler):
        process = self.cleanup_child(
            "import signal, sys, time\n"
            f"signal.signal(signal.SIGINT, {handler})\n"
            "print('ready', flush=True)\n"
            "time.sleep(60)\n"
        )
        self.assertTrue(select.select([process.stdout], [], [], 3)[0])
        self.assertEqual(process.stdout.readline(), "ready\n")
        return process

    def test_reap_never_signals_already_reaped_child(self):
        for status in (0, 3):
            with self.subTest(status=status):
                process = self.cleanup_child(f"raise SystemExit({status})")
                self.assertEqual(process.wait(timeout=3), status)
                with patch("remote_relay.os.killpg") as group_signal:
                    relay.reap(process)
                group_signal.assert_not_called()
                self.assertEqual(process.returncode, status)

    def test_reap_stops_escalation_after_sigint_exit(self):
        process = self.ready_cleanup_child("lambda *_: sys.exit(0)")
        with patch("remote_relay.os.killpg", wraps=os.killpg) as group_signal:
            relay.reap(process)
        self.assertEqual(process.returncode, 0)
        self.assertEqual(
            group_signal.call_args_list,
            [unittest.mock.call(process.pid, signal.SIGINT)],
        )

    def test_reap_kills_unresponsive_live_child_within_bound(self):
        process = self.ready_cleanup_child("signal.SIG_IGN")
        started = time.monotonic()
        with patch("remote_relay.os.killpg", wraps=os.killpg) as group_signal:
            relay.reap(process)
        self.assertLess(time.monotonic() - started, 5)
        self.assertEqual(process.returncode, -signal.SIGKILL)
        self.assertEqual(
            group_signal.call_args_list,
            [
                unittest.mock.call(process.pid, signal.SIGINT),
                unittest.mock.call(process.pid, signal.SIGKILL),
            ],
        )

    def test_reap_still_waits_and_reports_uncertain_signal(self):
        process = self.ready_cleanup_child("signal.SIG_IGN")
        group_signal = os.killpg
        attempted = []

        def uncertain_signal(pid, sig):
            attempted.append(sig)
            if sig == signal.SIGINT:
                raise PermissionError("injected signal failure")
            group_signal(pid, sig)

        with (
            patch("remote_relay.os.killpg", side_effect=uncertain_signal),
            self.assertRaisesRegex(PermissionError, "injected signal failure"),
        ):
            relay.reap(process)
        self.assertEqual(process.returncode, -signal.SIGKILL)
        self.assertEqual(attempted, [signal.SIGINT, signal.SIGKILL])

    def test_envelope_quota_name_hash_duplicate_json_and_launch_replay(self):
        for value in (
            relay.packet("../escape", b"{}"),
            {"name": "ready.json", "sha256": "0" * 64, "data": "e30="},
        ):
            with self.assertRaises(ValueError):
                relay.unpack(value, relay.REMOTE_FILES)
        with self.assertRaises(ValueError):
            relay.parse(b'{"a":1,"a":2}')
        with self.assertRaises(ValueError):
            relay.packet("ready.json", b"x" * (relay.FILE_LIMIT + 1))
        with self.assertRaises(ValueError):
            relay.launch_value(
                encoded(
                    {
                        "schema_version": "1.0.0",
                        "state": "launched",
                        "binding": runner_binding() | {"nonce": "foreign"},
                        "launch_nonce": "018887b2-499d-4c11-9ea2-4f093aaa087a",
                    }
                ),
                runner_binding(),
            )

    def test_subsystem_rejects_replayed_sequence_before_owner_start(self):
        relay_id = "018887b2-499d-4c11-9ea2-4f093aaa087a"
        messages = io.BytesIO(
            encoded({"op": "hello", "relay_id": relay_id, "timeout": 1})
            + encoded({"relay_id": relay_id, "seq": 2, "op": "begin", "value": {}})
        )
        reader, writer = os.pipe()
        result_reader, result_writer = os.pipe()
        try:
            os.write(writer, messages.read())
            with self.assertRaisesRegex(ValueError, "sequence"):
                relay.serve(reader, result_writer)
        finally:
            for fd in (reader, writer, result_reader, result_writer):
                os.close(fd)

    def test_fake_ssh_rejects_insecure_options_wrong_host_and_shell_command(self):
        argv = relay.ssh_argv(
            "observer.test",
            "observer",
            22,
            self.root / "known_hosts",
            self.root / "identity",
        )
        for marker, replacement in (
            ("StrictHostKeyChecking=yes", "StrictHostKeyChecking=no"),
            ("observer.test", "wrong.example"),
            (relay.SUBSYSTEM, "sh -c id"),
        ):
            altered = [replacement if item == marker else item for item in argv]
            result = subprocess.run(
                self.args["ssh_command"] + altered,
                input=b"",
                capture_output=True,
                timeout=3,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
