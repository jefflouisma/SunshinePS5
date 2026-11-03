"""Local deployment gate fixtures; no root installation or SSH connection."""

import hashlib
import json
import os
import pwd
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

import deployment_entrypoint as gate


class DeploymentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.modules = self.root / "modules"
        self.modules.mkdir(mode=0o755)
        hashes = {}
        for name in gate.MODULES:
            data = b"# synthetic reviewed module\n"
            (self.modules / name).write_bytes(data)
            hashes[name] = hashlib.sha256(data).hexdigest()
        self.sessions = self.root / "sessions"
        self.sessions.mkdir(mode=0o700)
        self.kube = self.root / "kubeconfig"
        self.kube.write_bytes(b"synthetic credential\n")
        self.kube.chmod(0o600)
        self.python = self.root / "python"
        self.python.write_bytes(b"synthetic executable\n")
        self.python.chmod(0o755)
        self.uid = os.getuid()
        self.config = {
            "schema_version": 1,
            "account": pwd.getpwuid(self.uid).pw_name,
            "uid": self.uid,
            "module_directory": str(self.modules),
            "hashes": hashes,
            "python": str(self.python),
            "remote_root": str(self.sessions),
            "provider": {key: "fixture" for key in gate.PROVIDER_FIELDS},
        }
        self.config["provider"].update(
            kubectl=str(self.python), kubeconfig=str(self.kube)
        )
        self.path = self.root / "config.json"
        # The fixture uses the invoking user's files. Only administrator ownership
        # and ancestor policy are modeled; terminal modes/type/hash checks are real.
        self.real_inspect = gate.inspect
        self.ancestors = patch.object(gate, "ancestor")
        self.ancestors.start()
        self.addCleanup(self.ancestors.stop)

    def inspect_fixture(self, path, owner, **kwargs):
        return self.real_inspect(path, self.uid, **kwargs)

    def run_preflight(self):
        self.path.write_text(json.dumps(self.config))
        with (
            patch.object(gate, "inspect", side_effect=self.inspect_fixture),
            patch.object(gate.sys, "executable", str(self.python)),
        ):
            return gate.preflight(str(self.path))

    def test_success(self):
        self.assertEqual(self.run_preflight(), self.config)

    def test_schema_and_account(self):
        for key, value in (
            ("extra", 1),
            ("uid", 0),
            ("uid", True),
            ("account", "wrong-account"),
            ("schema_version", True),
        ):
            with self.subTest(key=key, value=value):
                original = self.config.copy()
                self.config[key] = value
                with self.assertRaises((ValueError, KeyError)):
                    self.run_preflight()
                self.config = original

    def test_hash_mismatch(self):
        (self.modules / "observer.py").write_bytes(b"modified\n")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            self.run_preflight()

    def test_private_modes_and_executable(self):
        for path, bad, good in (
            (self.sessions, 0o755, 0o700),
            (self.kube, 0o644, 0o600),
            (self.python, 0o644, 0o755),
            (self.modules, 0o777, 0o755),
        ):
            with self.subTest(path=path):
                path.chmod(bad)
                with self.assertRaises(ValueError):
                    self.run_preflight()
                path.chmod(good)

    def test_symlink_hardlink_and_owner(self):
        link = self.root / "link"
        link.symlink_to(self.kube)
        with self.assertRaises(OSError):
            gate.inspect(str(link), self.uid)
        link.unlink()
        os.link(self.kube, link)
        with self.assertRaisesRegex(ValueError, "alias"):
            gate.inspect(str(self.kube), self.uid)
        with self.assertRaisesRegex(ValueError, "owner"):
            gate.inspect(str(self.python), self.uid + 1)

    def test_ancestor_policy(self):
        self.ancestors.stop()
        for owner, mode in ((self.uid + 1, 0o40755), (self.uid, 0o40777)):
            with self.assertRaisesRegex(ValueError, "ancestor"):
                gate.ancestor(Mock(st_uid=owner, st_mode=mode), {self.uid})

    def test_noncanonical_paths(self):
        for value in (
            "relative",
            str(self.root) + "/../x",
            str(self.root) + "//x",
            str(self.root) + "/./x",
            "/",
        ):
            with self.subTest(path=value), self.assertRaises(ValueError):
                gate.inspect(value, self.uid)

    def test_duplicate_json(self):
        self.path.write_bytes(b'{"uid":1,"uid":2}')
        with (
            patch.object(gate, "inspect", side_effect=self.inspect_fixture),
            self.assertRaisesRegex(ValueError, "duplicate"),
        ):
            gate.preflight(str(self.path))

    def test_parent_symlink_and_quota(self):
        link = self.root / "linked-modules"
        link.symlink_to(self.modules, target_is_directory=True)
        with self.assertRaises(OSError):
            gate.inspect(str(link / "observer.py"), self.uid)
        with self.assertRaisesRegex(ValueError, "quota"):
            gate.inspect(str(self.python), self.uid, limit=1)

    def test_force_command_rejected_before_preflight(self):
        for command in ("", "id", "sunshine-reference-observer-v1"):
            with (
                patch.dict(os.environ, SSH_ORIGINAL_COMMAND=command),
                patch.object(gate, "preflight") as preflight,
            ):
                with self.assertRaisesRegex(ValueError, "ForceCommand"):
                    gate.serve(str(self.path))
                preflight.assert_not_called()

    def test_serve_pins_request_before_owner(self):
        relay = Mock()
        good = {"root": str(self.sessions), "provider": self.config["provider"]}

        def exercise(owner_factory):
            for request in ({}, dict(good, root="/other"), dict(good, provider={})):
                with self.assertRaisesRegex(ValueError, "binding"):
                    owner_factory(request, "id", 10)
            relay.Owner.assert_not_called()
            owner_factory(good, "id", 10)
            relay.Owner.assert_called_once_with(good, "id", 10)

        relay.serve.side_effect = exercise
        relay.__file__ = str(self.modules / "remote_relay.py")

        def imported(name):
            return (
                relay
                if name == "remote_relay"
                else Mock(__file__=str(self.modules / (name + ".py")))
            )

        with (
            patch.dict(os.environ, {}, clear=True),
            patch.object(gate.sys, "flags", Mock(isolated=1, no_site=1)),
            patch.object(gate.sys, "platform", "linux"),
            patch.object(gate, "preflight", return_value=self.config),
            patch.object(gate, "inspect", return_value=(1, 2)),
            patch.object(gate.importlib, "import_module", side_effect=imported),
            patch.object(gate.sys, "path", list(gate.sys.path)),
            patch.dict(gate.sys.modules),
        ):
            for name in gate.MODULES:
                gate.sys.modules.pop(name[:-3], None)
            gate.serve(str(self.path))
            relay.__file__ = str(self.modules / "__pycache__/remote_relay.pyc")
            with self.assertRaisesRegex(ValueError, "module path mismatch"):
                gate.serve(str(self.path))
            self.assertEqual(relay.serve.call_count, 1)

    def test_bytecode_and_import_shadow_entries(self):
        for name in (
            "__pycache__",
            "remote_relay.pyc",
            "remote_relay.pyo",
            "arbitrary.pyc",
            "json.py",
            "package",
        ):
            with self.subTest(name=name):
                extra = self.modules / name
                if name in ("__pycache__", "package"):
                    extra.mkdir()
                else:
                    extra.write_bytes(b"untrusted import material")
                with self.assertRaisesRegex(ValueError, "entry/bytecode"):
                    self.run_preflight()
                if extra.is_dir():
                    extra.rmdir()
                else:
                    extra.unlink()

    def test_close_failure_not_retried_and_remaining_fds_closed(self):
        real_open, real_close = os.open, os.close
        opened, closed = [], []

        def opening(*args, **kwargs):
            fd = real_open(*args, **kwargs)
            opened.append(fd)
            return fd

        def closing(fd):
            closed.append(fd)
            real_close(fd)
            if len(closed) == 1:
                raise OSError("consumed close failure")

        with (
            patch.object(gate.os, "open", side_effect=opening),
            patch.object(gate.os, "close", side_effect=closing),
            self.assertRaisesRegex(OSError, "consumed close"),
        ):
            gate.inspect(str(self.python), self.uid)
        self.assertEqual(closed, opened)

    def test_terminal_close_failure_closes_parent(self):
        real_close, real_open = os.close, os.open
        terminal = None
        attempts = []

        def opening(path, *args, **kwargs):
            nonlocal terminal
            fd = real_open(path, *args, **kwargs)
            if path == "python":
                terminal = fd
            return fd

        def closing(fd):
            attempts.append(fd)
            real_close(fd)
            if fd == terminal:
                raise OSError("terminal close failure")

        with (
            patch.object(gate.os, "open", side_effect=opening),
            patch.object(gate.os, "close", side_effect=closing),
            self.assertRaisesRegex(OSError, "terminal close"),
        ):
            gate.inspect(str(self.python), self.uid)
        self.assertEqual(attempts[-2], terminal)
        self.assertNotEqual(attempts[-1], terminal)

    def test_directory_replacement_during_hashing(self):
        self.path.write_text(json.dumps(self.config))
        reads = 0

        def changing(path, owner, **kwargs):
            nonlocal reads
            result = self.inspect_fixture(path, owner, **kwargs)
            if path == str(self.modules):
                reads += 1
                if reads == 2:
                    return ("replacement",)
            return result

        with (
            patch.object(gate, "inspect", side_effect=changing),
            self.assertRaisesRegex(ValueError, "directory replaced"),
        ):
            gate.preflight(str(self.path))


if __name__ == "__main__":
    unittest.main()
