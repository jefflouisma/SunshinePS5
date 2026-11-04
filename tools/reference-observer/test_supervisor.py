"""Synthetic process supervision tests; no host/device acceptance."""

import json
import os
import shutil
import signal
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

import supervisor
from observer import encoded
from supervisor import cancel_result, snapshot, supervise
from test_observer import (
    attachment_identity,
    attachment_value,
    binding,
    runner_binding,
    scenario,
)


class SupervisorTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        for name, value in (
            ("observer-binding.json", runner_binding()),
            ("binding.json", binding()),
            ("scenario.json", scenario()),
        ):
            (self.root / name).write_bytes(encoded(value))

    def tearDown(self):
        self.temporary.cleanup()

    def run_supervisor(self, success=True, *, command=None, timeout=3, emit=True):
        errors = []

        def signal_after_ready():
            until = time.monotonic() + timeout
            while time.monotonic() < until:
                if (self.root / "ready.json").exists():
                    value = {
                        "schema_version": "1.0.0",
                        "state": "host_cancel_completed",
                        "binding": runner_binding(),
                        "request_succeeded": success,
                    }
                    temporary = self.root / "pending"
                    temporary.write_bytes(encoded(value))
                    temporary.rename(self.root / "observer-cancel.json")
                    return
                time.sleep(0.005)
            errors.append("no READY")

        thread = threading.Thread(target=signal_after_ready) if emit else None
        if thread:
            thread.start()
        result = supervise(
            self.root / "observer-binding.json",
            self.root / "binding.json",
            self.root / "scenario.json",
            self.root / "ready.json",
            self.root / "supervised",
            timeout,
            command=command
            or [sys.executable, str(Path(__file__).with_name("supervisor_fixture.py"))],
            provenance="synthetic_fixture",
        )
        if thread:
            thread.join()
            self.assertEqual(errors, [])
        return result

    def test_success_and_failure_use_real_collector_finish_protocol(self):
        for success in (True, False):
            with self.subTest(success=success):
                if not success:
                    self.tearDown()
                    self.setUp()
                result = self.run_supervisor(success)
                self.assertEqual(result["coordination"], "published", result)
                self.assertEqual(result["acceptance"], "Incomplete")
                self.assertEqual(result["request_succeeded"], success)
                raw = json.loads(
                    (self.root / "supervised/collector/raw.json").read_bytes()
                )
                records = raw["records"]
                if success:
                    self.assertEqual(
                        records[-1]["termination_reason"], "finish_command"
                    )
                else:
                    self.assertTrue(any(r.get("kind") == "error" for r in records))
                self.assertEqual(raw["provenance"], "synthetic_fixture")
                self.assertTrue((self.root / "supervised/supervisor.json").is_file())
                if evidence := os.environ.get("SUPERVISOR_EVIDENCE"):
                    shutil.copytree(
                        self.root / "supervised", Path(evidence) / str(success)
                    )

    def test_missing_signal_is_bounded_and_child_reaped(self):
        started = time.monotonic()
        real_killpg = os.killpg
        with patch.object(supervisor.os, "killpg", wraps=real_killpg) as kill:
            result = self.run_supervisor(timeout=0.3, emit=False)
        process = json.loads((self.root / "supervised/process.json").read_bytes())
        kill.assert_called_once_with(process["pid"], signal.SIGKILL)
        self.assertLess(time.monotonic() - started, 2.5)
        self.assertEqual(result["coordination"], "incomplete")
        self.assertIn("deadline", result["reason"])
        self.assertIsNotNone(result["returncode"])
        with self.assertRaises(ProcessLookupError):
            os.kill(process["pid"], 0)

    def test_exited_leaders_are_never_group_signalled(self):
        for status in (0, 3):
            with self.subTest(status=status):
                if status:
                    self.tearDown()
                    self.setUp()
                with patch.object(supervisor.os, "killpg") as kill:
                    result = self.run_supervisor(
                        emit=False,
                        command=[sys.executable, "-c", f"raise SystemExit({status})"],
                    )
                self.assertEqual(result["returncode"], status)
                kill.assert_not_called()
                process = json.loads(
                    (self.root / "supervised/process.json").read_bytes()
                )
                with self.assertRaises(ProcessLookupError):
                    os.kill(process["pid"], 0)

    def test_published_collector_is_never_group_signalled(self):
        with patch.object(supervisor.os, "killpg") as kill:
            result = self.run_supervisor()
        self.assertEqual(result["coordination"], "published", result)
        self.assertEqual(result["returncode"], 0)
        kill.assert_not_called()

    def test_interruption_still_kills_and_reaps_live_child(self):
        real_selector = supervisor.selectors.DefaultSelector
        real_killpg = os.killpg

        def interrupt(timeout):
            raise KeyboardInterrupt

        def selector():
            selected = real_selector()
            selected.select = interrupt
            return selected

        with (
            patch.object(supervisor.selectors, "DefaultSelector", selector),
            patch.object(supervisor.os, "killpg", wraps=real_killpg) as kill,
            self.assertRaises(KeyboardInterrupt),
        ):
            self.run_supervisor(
                emit=False,
                command=[sys.executable, "-c", "import time; time.sleep(30)"],
            )
        process = json.loads((self.root / "supervised/process.json").read_bytes())
        kill.assert_called_once_with(process["pid"], signal.SIGKILL)
        result = json.loads((self.root / "supervised/supervisor.json").read_bytes())
        self.assertIsNotNone(result["returncode"])
        with self.assertRaises(ProcessLookupError):
            os.kill(process["pid"], 0)

    def test_log_flood_is_bounded(self):
        result = self.run_supervisor(
            emit=False,
            command=[sys.executable, "-c", "import os; os.write(1, b'x' * 2000000)"],
        )
        self.assertIn("log quota", result["reason"])
        self.assertLessEqual(
            (self.root / "supervised/stdout.log").stat().st_size, 1048576
        )

    def test_descendant_holding_pipe_leaves_cleanup_unconfirmed(self):
        code = (
            "import os,time; p=os.fork(); print(os.getpid(),flush=True); "
            "time.sleep(0.7) if p==0 else None"
        )
        with patch.object(supervisor.os, "killpg") as kill:
            result = self.run_supervisor(
                emit=False, timeout=0.3, command=[sys.executable, "-c", code]
            )
        kill.assert_not_called()
        self.assertIn("deadline", result["reason"])
        self.assertEqual(result["coordination"], "incomplete")
        self.assertEqual(result["descendant_cleanup"], "unconfirmed")

    def test_foreign_ready_is_terminal(self):
        # No collector process is allowed to authorize a different run.
        # Publish atomically so this tests the binding, not a partial-file race.
        code = (
            f"import sys,time; sys.path.insert(0,{str(Path(__file__).parent)!r}); "
            "from observer import encoded,publish_envelope; "
            "p=sys.argv[sys.argv.index('--ready-output')+1]; "
            "publish_envelope(p,encoded({'state':'ready'})); time.sleep(30)"
        )
        result = self.run_supervisor(emit=False, command=[sys.executable, "-c", code])
        self.assertIn("READY mismatch", result["reason"])
        self.assertEqual(result["coordination"], "incomplete")
        if evidence := os.environ.get("SUPERVISOR_EVIDENCE"):
            shutil.copytree(self.root / "supervised", Path(evidence) / "foreign-ready")

    def test_invalid_timeout_refused(self):
        for timeout in (0, -1, 301, float("nan"), float("inf")):
            with self.assertRaises(ValueError):
                self.run_supervisor(timeout=timeout, emit=False)

    def test_publication_rejects_foreign_revision(self):
        fixture = str(Path(__file__).with_name("supervisor_fixture.py"))
        code = (
            f"import sys; sys.path.insert(0,{str(Path(fixture).parent)!r}); "
            "import runpy,observer; original=observer.publish; "
            "observer.publish=lambda r,b,s,records,p: "
            "original(r,b|{'test_client_revision':'foreign'},s,records,p); "
            f"runpy.run_path({fixture!r},run_name='__main__')"
        )
        result = self.run_supervisor(command=[sys.executable, "-c", code])
        self.assertIn("manifest binding mismatch", result["reason"])

    def test_stale_paths_refused_before_process(self):
        (self.root / "ready.json").write_bytes(b"stale")
        with self.assertRaisesRegex(ValueError, "fresh"):
            self.run_supervisor(emit=False)
        self.assertFalse((self.root / "supervised").exists())

    def test_signals_reject_foreign_unknown_duplicate_and_non_boolean(self):
        value = {
            "schema_version": "1.0.0",
            "state": "host_cancel_completed",
            "binding": runner_binding(),
            "request_succeeded": True,
        }
        path = self.root / "signal.json"
        for changed in (
            value | {"request_succeeded": 1},
            value | {"extra": 1},
            value | {"binding": runner_binding() | {"nonce": "other"}},
        ):
            path.write_bytes(encoded(changed))
            with self.assertRaises(ValueError):
                cancel_result(path, runner_binding())
        path.write_bytes(b'{"state":1,"state":2}')
        with self.assertRaises(ValueError):
            cancel_result(path, runner_binding())

    def test_snapshot_rejects_symlinks_hardlinks_and_oversize(self):
        source = self.root / "binding.json"
        alias = self.root / "alias"
        alias.symlink_to(source)
        with self.assertRaises(ValueError):
            snapshot(alias)
        alias.unlink()
        os.link(source, alias)
        with self.assertRaises(ValueError):
            snapshot(source)
        alias.unlink()
        with self.assertRaises(ValueError):
            snapshot(source, 1)

    def test_uncertain_close_never_retries_and_still_reaps_and_reports(self):
        for mode in ("true", "false", "watchdog", "reader"):
            with self.subTest(mode=mode):
                self.tearDown()
                self.setUp()
                real_pipe, real_close = os.pipe, os.close
                owned = {}
                attempts = []

                def pipe(real_pipe=real_pipe, owned=owned, mode=mode):
                    pair = real_pipe()
                    owned.setdefault("fd", pair[0 if mode == "reader" else 1])
                    return pair

                def close(fd, owned=owned, attempts=attempts, real_close=real_close):
                    if fd == owned.get("fd"):
                        attempts.append(fd)
                        real_close(fd)
                        raise OSError("uncertain owned close")
                    real_close(fd)

                with (
                    patch.object(supervisor.os, "pipe", pipe),
                    patch.object(supervisor.os, "close", close),
                ):
                    result = self.run_supervisor(
                        success=mode == "true",
                        emit=mode in ("true", "false"),
                        timeout=0.3 if mode == "watchdog" else 3,
                    )
                self.assertEqual(len(attempts), 1)
                self.assertEqual(result["coordination"], "incomplete")
                self.assertIsNotNone(result["returncode"])
                saved = json.loads(
                    (self.root / "supervised/supervisor.json").read_bytes()
                )
                self.assertEqual(saved, result)
                process = json.loads(
                    (self.root / "supervised/process.json").read_bytes()
                )
                with self.assertRaises(ProcessLookupError):
                    os.kill(process["pid"], 0)
                if evidence := os.environ.get("SUPERVISOR_EVIDENCE"):
                    shutil.copytree(
                        self.root / "supervised", Path(evidence) / ("close-" + mode)
                    )

    def test_selector_and_log_sync_errors_do_not_skip_report(self):
        real_selector = supervisor.selectors.DefaultSelector
        real_sync = os.fsync
        faults = {"selector": False, "sync": False}

        def selector():
            selected = real_selector()
            original_close = selected.close

            def close():
                original_close()
                faults["selector"] = True
                raise OSError("selector close uncertainty")

            selected.close = close
            return selected

        def sync(fd):
            if faults["selector"] and not faults["sync"]:
                faults["sync"] = True
                raise OSError("log sync failure")
            real_sync(fd)

        with (
            patch.object(supervisor.selectors, "DefaultSelector", selector),
            patch.object(supervisor.os, "fsync", sync),
        ):
            result = self.run_supervisor()
        self.assertEqual(result["coordination"], "incomplete")
        self.assertEqual(len(result["cleanup_errors"]), 2)
        self.assertEqual(result["returncode"], 0)
        self.assertEqual(
            json.loads((self.root / "supervised/supervisor.json").read_bytes()), result
        )
        if evidence := os.environ.get("SUPERVISOR_EVIDENCE"):
            shutil.copytree(self.root / "supervised", Path(evidence) / "cleanup-order")


class AttachmentSupervisorTests(unittest.TestCase):
    setUp = SupervisorTests.setUp
    tearDown = SupervisorTests.tearDown

    def test_attachment_subprocess_and_immutable_raw(self):
        errors = []

        def adapter():
            try:
                until = time.monotonic() + 3
                while not (self.root / "ready.json").exists():
                    if time.monotonic() >= until:
                        raise TimeoutError("no prelaunch READY")
                    time.sleep(0.005)
                value = attachment_value()
                # Wolf may create devices only after RTSP/controller arrival.
                value["devices"] = []
                # The polling observer must never see a created-but-unwritten
                # marker; like the runner, publish it atomically.
                temporary = self.root / "launch.pending"
                temporary.write_bytes(
                    encoded(
                        {
                            "schema_version": "1.0.0",
                            "state": "launched",
                            "binding": runner_binding(),
                            "launch_nonce": value["launch_nonce"],
                        }
                    )
                )
                temporary.rename(self.root / "observer-launch.json")
                (self.root / "source.json").write_bytes(b"{}")
                temporary = self.root / "runtime.pending"
                temporary.write_bytes(encoded(value))
                temporary.rename(self.root / "runtime.json")
                while not (self.root / "attached.json").exists():
                    if time.monotonic() >= until:
                        raise TimeoutError("no postlaunch attachment")
                    time.sleep(0.005)
                (self.root / "controller-arrived-after-ack.json").write_bytes(
                    encoded(
                        {
                            "synthetic": True,
                            "device_path": "/dev/input/event1",
                            "observed_after_attachment_ack": True,
                        }
                    )
                )
                temporary = self.root / "cancel.pending"
                temporary.write_bytes(
                    encoded(
                        {
                            "schema_version": "1.0.0",
                            "state": "host_cancel_completed",
                            "binding": runner_binding(),
                            "request_succeeded": True,
                        }
                    )
                )
                temporary.rename(self.root / "observer-cancel.json")
            except (OSError, ValueError, TimeoutError) as error:
                errors.append(str(error))

        thread = threading.Thread(target=adapter)
        thread.start()
        with patch("supervisor.observer_identity", return_value=attachment_identity()):
            result = supervise(
                self.root / "observer-binding.json",
                self.root / "binding.json",
                self.root / "scenario.json",
                self.root / "ready.json",
                self.root / "supervised",
                3,
                command=[
                    sys.executable,
                    str(Path(__file__).with_name("supervisor_fixture.py")),
                ],
                provenance="synthetic_fixture",
                runtime_attachment=self.root / "runtime.json",
                attachment_ready=self.root / "attached.json",
                device_feed=self.root / "devices.json",
            )
        thread.join()
        self.assertEqual(errors, [])
        self.assertEqual(result["coordination"], "published", result)
        raw = json.loads((self.root / "supervised/collector/raw.json").read_bytes())
        self.assertEqual(
            raw["binding"]["runtime_identity"],
            "observer:" + attachment_identity()["observer_id"],
        )
        attached = [r for r in raw["records"] if r["kind"] == "runtime_attachment"]
        self.assertEqual(len(attached), 1)
        self.assertEqual(attached[0]["attachment"]["devices"], [])
        self.assertEqual(
            attached[0]["device_acquisition"], "unavailable_pending_later_device_feed"
        )
        self.assertFalse(
            any(r["kind"] in ("attach", "evdev", "remove") for r in raw["records"])
        )
        self.assertTrue((self.root / "controller-arrived-after-ack.json").exists())
        self.assertEqual(
            attached[0]["attachment_bytes"].encode(),
            (self.root / "runtime.json").read_bytes(),
        )
        self.assertEqual(raw["records"][-1]["termination_reason"], "finish_command")
        self.assertFalse((self.root / "devices.json").exists())
        if evidence := os.environ.get("ATTACHMENT_EVIDENCE"):
            shutil.copy2(
                self.root / "controller-arrived-after-ack.json",
                self.root / "supervised/controller-arrived-after-ack.json",
            )
            shutil.copytree(
                self.root / "supervised", Path(evidence) / "supervised-attachment"
            )

    def test_preexisting_feed_refused_before_spawn(self):
        feed = self.root / "devices.json"
        feed.write_bytes(b"stale")
        with self.assertRaises(ValueError):
            supervise(
                self.root / "observer-binding.json",
                self.root / "binding.json",
                self.root / "scenario.json",
                self.root / "ready.json",
                self.root / "supervised",
                runtime_attachment=self.root / "runtime.json",
                attachment_ready=self.root / "attached.json",
                device_feed=feed,
            )
        self.assertFalse((self.root / "supervised").exists())


if __name__ == "__main__":
    unittest.main()
