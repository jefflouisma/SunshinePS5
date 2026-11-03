"""Local wrapper fixtures; never invoke the installed k3s or contact a cluster."""

import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import deployment_entrypoint as gate


class K3sKubectlTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.source = Path(__file__).with_name("k3s-kubectl.sh")
        self.script = self.source.read_text()
        # Substitute only the fixed executable in a private copy. The shipped
        # wrapper deliberately has no target override for this test or callers.
        self.assertEqual(self.script.count("/usr/local/bin/k3s"), 1)
        self.assertEqual(
            shlex.split(self.script, comments=True),
            ["exec", "/usr/local/bin/k3s", "kubectl", "$@"],
        )
        self.target = self.root / "fixture k3s"
        self.wrapper = self.root / "kubectl"
        self.wrapper.write_text(
            self.script.replace("/usr/local/bin/k3s", shlex.quote(str(self.target)))
        )
        self.wrapper.chmod(0o755)

    def invoke(self, label, arguments):
        env = dict(os.environ, PATH="/nonexistent", K3S="/wrong", KUBECTL="/wrong")
        process = subprocess.Popen(
            [str(self.wrapper), *arguments],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        stdout, stderr = process.communicate(timeout=5)
        record = {
            "source_sha256": hashlib.sha256(self.source.read_bytes()).hexdigest(),
            "fixture_substituted_literal_target": str(self.target),
            "arguments": arguments,
            "spawn_pid": process.pid,
            "returncode": process.returncode,
            "stdout": stdout,
            "stderr": stderr,
        }
        destination = os.environ.get("K3S_WRAPPER_EVIDENCE")
        if destination:
            with (Path(destination) / (label + ".json")).open("x") as stream:
                json.dump(record, stream, indent=2)
                stream.write("\n")
        return record

    def test_arguments_exit_and_process_replacement(self):
        arguments = ["--kubeconfig", "/private/with spaces", "", "*", "$(false)", "--"]
        for status in (0, 37):
            with self.subTest(status=status):
                self.target.write_text(
                    f"#!{sys.executable}\n"
                    "import json, os, sys\n"
                    "print(json.dumps({'argv': sys.argv[1:], 'pid': os.getpid()}))\n"
                    f"sys.exit({status})\n"
                )
                self.target.chmod(0o755)
                result = self.invoke(f"exit-{status}", arguments)
                self.assertEqual(result["returncode"], status)
                self.assertEqual(result["stderr"], "")
                actual = json.loads(result["stdout"])
                self.assertEqual(actual["argv"], ["kubectl", *arguments])
                self.assertEqual(actual["pid"], result["spawn_pid"])

    def test_missing_fixed_target_fails(self):
        result = self.invoke("missing-target", ["get", "pods"])
        self.assertNotEqual(result["returncode"], 0)
        self.assertEqual(result["stdout"], "")
        self.assertTrue(result["stderr"])

    def test_regular_executable_gate_and_symlink_refusal(self):
        # Model administrator ownership/ancestors; exercise real terminal-file
        # checks. This does not validate a root-owned Linux installation.
        with patch.object(gate, "ancestor"):
            self.assertEqual(
                gate.inspect(str(self.source.resolve()), os.getuid(), executable=True),
                self.source.read_bytes(),
            )
            alias = self.root / "alias"
            alias.symlink_to(self.wrapper)
            with self.assertRaises(OSError):
                gate.inspect(str(alias), os.getuid(), executable=True)
            self.wrapper.chmod(0o644)
            with self.assertRaisesRegex(ValueError, "executable bit absent"):
                gate.inspect(str(self.wrapper), os.getuid(), executable=True)


if __name__ == "__main__":
    unittest.main()
