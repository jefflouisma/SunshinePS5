"""Real local subprocess provider tests; Kubernetes and devices are synthetic."""

import hashlib
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

from fenrir_provider import Commands, Kubernetes, provide, provider_ready
from observer import RuntimeAttachment, encoded, publish_envelope
from supervisor import supervise
from test_observer import (
    attachment_identity,
    attachment_value,
    binding,
    runner_binding,
    scenario,
)


class ProviderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name).resolve()
        self.output = self.root / "provider"
        self.fake = self.root / "kubectl"
        self.fake.write_text(
            f"#!{sys.executable}\n"
            "import json,sys,time\nfrom pathlib import Path\n"
            "root=Path(__file__).parent\n"
            "v=json.loads((root/'objects.json').read_text());args=sys.argv\n"
            "if v.get('sleep'):time.sleep(20)\n"
            "if v.get('flood'):print('x'*70000);sys.exit(0)\n"
            "if 'config' in args:out=v['config']\n"
            "elif '--raw' in args:\n"
            " expected='/api/v1/namespaces/house/pods/'\n"
            " expected+='https:pod:8080/proxy/input-devices'\n"
            " if args[args.index('--raw')+1]!=expected:sys.exit(3)\n"
            " if v.get('proxy_error'):sys.stderr.write('SECRET');sys.exit(1)\n"
            " (root/'queried-proxy').touch();out={'devices':v['devices']}\n"
            "else:\n"
            " resource=args[args.index('get')+1]\n"
            " if resource.startswith('sessions.'):\n"
            "  launched=(root/'observer-launch.json').exists()\n"
            "  items=v['sessions'] if launched else v.get('baseline',[])\n"
            "  out=items[0] if 'session' in args else {'items':items}\n"
            " elif resource in ('namespaces','nodes'):\n"
            "  uid='cluster-uid' if resource=='namespaces' else 'node-uid'\n"
            "  out={'metadata':{'uid':uid}}\n"
            " else:\n"
            "  out={'items':v[resource]}\n"
            "  before=v.get('replace_before') and (root/'attached.json').exists()\n"
            "  if resource=='pods' and before:\n"
            "   out['items'][0]['metadata']['uid']='replacement'\n"
            "  if resource=='pods' and (root/'queried-proxy').exists():\n"
            "   if v.get('replace_after'):\n"
            "    out['items'][0]['metadata']['uid']='replacement'\n"
            "   if v.get('replace'):\n"
            "    out['items'][0]['status']['containerStatuses'][0]['restartCount']=1\n"
            "print(json.dumps(out))\n"
        )
        self.fake.chmod(0o700)
        value = attachment_value()
        session = {
            "metadata": self.meta(value["session"]),
            "spec": {
                "userReference": {"name": "fixture"},
                "gameReference": {"name": "fixture"},
                "pairingReference": {"name": "fixture"},
                "config": {"aesKey": "SECRET", "aesIV": "SECRET"},
            },
            "status": {"wolfSessionID": "123"},
        }
        self.objects = {
            "config": {
                "clusters": [{"cluster": {"server": "https://example.invalid"}}]
            },
            "sessions": [session],
            "devices": value["devices"],
        }
        for kind, plural in (
            ("deployment", "deployments"),
            ("replicaset", "replicasets"),
            ("pod", "pods"),
        ):
            obj = {"metadata": self.meta(value[kind])}
            if kind == "pod":
                obj["spec"] = {
                    "nodeName": "node",
                    "containers": [
                        {
                            "name": "wolf-agent",
                            "ports": [
                                {
                                    "name": "agent",
                                    "containerPort": 8080,
                                    "protocol": "TCP",
                                }
                            ],
                        }
                    ],
                }
                obj["status"] = {
                    "containerStatuses": [
                        {
                            "name": c["name"],
                            "containerID": c["container_id"],
                            "imageID": c["image_id"],
                            "restartCount": 0,
                            "ready": True,
                            "state": {"running": {"startedAt": c["started_at"]}},
                        }
                        for c in value["containers"]
                    ]
                }
            self.objects[plural] = [obj]
        self.save_objects()
        for filename, data in (
            ("runner.json", runner_binding()),
            ("identity.json", attachment_identity()),
            ("binding.json", binding()),
            ("scenario.json", scenario()),
        ):
            (self.root / filename).write_bytes(encoded(data))
        (self.root / "config").write_bytes(b"synthetic config")

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def meta(item):
        value = {key: item[key] for key in ("name", "namespace", "uid")}
        value["resourceVersion"] = item["resource_version"]
        if "owner_references" in item:
            value["ownerReferences"] = item["owner_references"]
        return value

    def save_objects(self):
        (self.root / "objects.json").write_bytes(encoded(self.objects))

    def wait(self, path, timeout=8):
        until = time.monotonic() + timeout
        while not path.exists():
            if time.monotonic() >= until:
                raise TimeoutError(str(path))
            time.sleep(0.005)

    def launch(self):
        publish_envelope(
            self.root / "observer-launch.json",
            encoded(
                {
                    "schema_version": "1.0.0",
                    "state": "launched",
                    "binding": runner_binding(),
                    "launch_nonce": attachment_value()["launch_nonce"],
                }
            ),
        )

    def run_provider(self, *, timeout=6, enable=True, identity=None):
        with patch("fenrir_provider.host_check"):
            return provide(
                self.output,
                self.root / "runner.json",
                identity or self.root / "identity.json",
                self.root / "observer-launch.json",
                self.root / "attached.json",
                kubectl=self.fake,
                kubeconfig=self.root / "config",
                context="fixture",
                namespace="house",
                agent_port=getattr(self, "agent_port", "agent"),
                user="fixture",
                app="fixture",
                pairing="fixture",
                timeout=timeout,
                enable=enable,
            )

    def execute(self, *, expect_runtime=True):
        errors = []

        def runner():
            try:
                self.wait(self.output / "provider-ready.json")
                provider_ready(
                    self.output / "provider-ready.json",
                    runner_binding(),
                    attachment_identity(),
                )
                self.launch()
                if expect_runtime:
                    self.wait(self.output / "runtime.json")
                    state = RuntimeAttachment(
                        attachment_identity(),
                        runner_binding(),
                        self.output / "runtime.json",
                        self.root / "attached.json",
                        self.root / "observer-launch.json",
                    )
                    state.launch = json.loads(
                        (self.root / "observer-launch.json").read_bytes()
                    )
                    state.value = json.loads(
                        (self.output / "runtime.json").read_bytes()
                    )
                    state.data = (self.output / "runtime.json").read_bytes()
                    state.validate(state.value)
                    state.acknowledge()
            except (OSError, ValueError, KeyError, TypeError, AssertionError) as error:
                errors.append(repr(error))

        thread = threading.Thread(target=runner)
        thread.start()
        result = self.run_provider()
        thread.join()
        self.assertEqual(errors, [])
        if evidence := os.environ.get("PROVIDER_EVIDENCE"):
            shutil.copytree(self.output, Path(evidence) / self.id().split(".")[-1])
        return result

    def test_success_publishes_consumer_validated_redacted_payloads(self):
        result = self.execute()
        self.assertEqual(result["state"], "published", result)
        state = RuntimeAttachment(
            attachment_identity(),
            runner_binding(),
            self.output / "runtime.json",
            self.root / "other-ack",
            self.root / "observer-launch.json",
        )
        state.launch = json.loads((self.root / "observer-launch.json").read_bytes())
        state.data = (self.output / "runtime.json").read_bytes()
        state.value = json.loads(state.data)
        state.validate(state.value)
        state.acknowledged, state.acknowledged_ns = True, state.value["observed_ns"]
        state.validate_feed(json.loads((self.output / "devices.json").read_bytes()))
        for path in self.output.iterdir():
            self.assertNotIn(b"SECRET", path.read_bytes())
        for filename, digest in result["artifacts"].items():
            self.assertEqual(
                hashlib.sha256((self.output / filename).read_bytes()).hexdigest(),
                digest,
            )

    def test_ambiguous_new_session(self):
        self.objects["sessions"].append(
            self.objects["sessions"][0]
            | {"metadata": self.objects["sessions"][0]["metadata"] | {"uid": "other"}}
        )
        self.save_objects()
        result = self.execute(expect_runtime=False)
        self.assertEqual(result["state"], "incomplete")
        self.assertFalse((self.output / "runtime.json").exists())

    def test_https_only_proxy_rejects_http_named_and_wrong_ports(self):
        commands = Commands(time.monotonic() + 4)
        kube = Kubernetes(
            commands, self.fake, self.root / "config", "fixture", "house", "agent"
        )
        for target in (
            "pod:8080",
            "http:pod:8080",
            "https:pod:agent",
            "https:pod:8081",
        ):
            with self.subTest(target=target), self.assertRaises(ValueError):
                kube.call(
                    [
                        "get",
                        "--raw",
                        f"/api/v1/namespaces/house/pods/{target}/proxy/input-devices",
                    ]
                )
        self.assertEqual(
            kube.devices("pod", {"container_port": 8080}), self.objects["devices"]
        )

    def test_numeric_agent_port_resolves_from_selected_pod(self):
        self.agent_port = "8080"
        self.assertEqual(self.execute()["state"], "published")
        source = json.loads((self.output / "runtime-source.json").read_bytes())
        self.assertEqual(
            source["agent_endpoint"],
            {
                "container_name": "wolf-agent",
                "container_port": 8080,
                "port_name": "agent",
                "scheme": "https",
            },
        )

    def test_missing_agent_port(self):
        self.objects["pods"][0]["spec"]["containers"][0]["ports"] = []
        self.save_objects()
        self.assertEqual(self.execute(expect_runtime=False)["state"], "incomplete")
        self.assertFalse((self.output / "runtime.json").exists())

    def test_ambiguous_agent_port(self):
        ports = self.objects["pods"][0]["spec"]["containers"][0]["ports"]
        ports.append(ports[0] | {"containerPort": 8443})
        self.save_objects()
        self.assertEqual(self.execute(expect_runtime=False)["state"], "incomplete")

    def test_agent_container_status_mismatch(self):
        statuses = self.objects["pods"][0]["status"]["containerStatuses"]
        for status in statuses:
            if status["name"] == "wolf-agent":
                status["name"] = "foreign-agent"
        self.save_objects()
        self.assertEqual(self.execute(expect_runtime=False)["state"], "incomplete")

    def test_pod_replacement_before_proxy(self):
        self.objects["replace_before"] = True
        self.save_objects()
        self.assertEqual(self.execute()["state"], "incomplete")
        self.assertFalse((self.root / "queried-proxy").exists())
        self.assertFalse((self.output / "devices.json").exists())

    def test_pod_replacement_after_proxy(self):
        self.objects["replace_after"] = True
        self.save_objects()
        self.assertEqual(self.execute()["state"], "incomplete")
        self.assertTrue((self.root / "queried-proxy").exists())
        self.assertFalse((self.output / "devices.json").exists())

    def test_foreign_owner(self):
        self.objects["pods"][0]["metadata"]["ownerReferences"][0]["name"] = "foreign"
        self.save_objects()
        self.assertEqual(self.execute(expect_runtime=False)["state"], "incomplete")

    def test_pod_container_replacement(self):
        self.objects["replace"] = True
        self.save_objects()
        self.assertEqual(self.execute()["state"], "incomplete")
        self.assertFalse((self.output / "devices.json").exists())

    def test_proxy_failure(self):
        self.objects["proxy_error"] = True
        self.save_objects()
        self.assertEqual(self.execute()["state"], "incomplete")
        self.assertNotIn(b"SECRET", (self.output / "provider-report.json").read_bytes())

    def test_device_generation_reuse(self):
        self.objects["devices"].append(
            self.objects["devices"][0] | {"path": "/dev/input/event2"}
        )
        self.save_objects()
        self.assertEqual(self.execute()["state"], "incomplete")
        self.assertFalse((self.output / "devices-source.json").exists())

    def test_insecure_tls_refused_before_baseline(self):
        self.objects["config"]["clusters"][0]["cluster"]["insecure-skip-tls-verify"] = (
            True
        )
        self.save_objects()
        self.assertEqual(self.run_provider()["state"], "incomplete")
        self.assertFalse((self.output / "provider-ready.json").exists())

    def test_preexisting_launch_and_disabled(self):
        with self.assertRaisesRegex(ValueError, "disabled"):
            self.run_provider(enable=False)
        self.assertFalse(self.output.exists())
        self.launch()
        self.assertEqual(self.run_provider()["state"], "incomplete")
        self.assertFalse((self.output / "provider-ready.json").exists())
        with self.assertRaises(ValueError):
            self.run_provider()

    def test_existing_baseline_session_is_not_new_candidate(self):
        self.objects["baseline"] = self.objects["sessions"]
        self.save_objects()

        def runner():
            self.wait(self.output / "provider-ready.json")
            self.launch()

        thread = threading.Thread(target=runner)
        thread.start()
        result = self.run_provider(timeout=0.5)
        thread.join()
        self.assertEqual(result["state"], "incomplete")
        self.assertFalse((self.output / "runtime.json").exists())

    def test_atomic_publication_competitor_is_preserved(self):
        def runner():
            self.wait(self.output / "provider-ready.json")
            (self.output / "runtime.json").write_bytes(b"competitor")
            self.launch()

        thread = threading.Thread(target=runner)
        thread.start()
        result = self.run_provider()
        thread.join()
        self.assertEqual(result["state"], "incomplete")
        self.assertEqual((self.output / "runtime.json").read_bytes(), b"competitor")

    def test_changed_coordination_snapshot_is_terminal(self):
        def runner():
            self.wait(self.output / "provider-ready.json")
            replacement = self.root / "replacement"
            replacement.write_bytes((self.root / "runner.json").read_bytes())
            replacement.replace(self.root / "runner.json")
            self.launch()

        thread = threading.Thread(target=runner)
        thread.start()
        self.assertEqual(self.run_provider()["state"], "incomplete")
        thread.join()
        self.assertFalse((self.output / "runtime.json").exists())

    def test_provider_ready_timeout_foreign_and_missing_source_prevent_spawn(self):
        for mode in ("timeout", "foreign", "missing-source"):
            with self.subTest(mode=mode):
                out = self.root / mode
                source = self.root / (mode + "-provider")
                source.mkdir()

                def publisher(out=out, source=source, mode=mode):
                    self.wait(out / "observer-identity.json")
                    baseline = encoded(
                        {
                            "session_uids": [],
                            "observed_ns": time.monotonic_ns(),
                            "cluster_uid": "cluster",
                        }
                    )
                    if mode == "timeout":
                        return  # An exited provider never publishes its barrier.
                    if mode != "missing-source":
                        (source / "baseline.json").write_bytes(baseline)
                    digest = hashlib.sha256(baseline).hexdigest()
                    publish_envelope(
                        source / "ready.json",
                        encoded(
                            {
                                "schema_version": "1.0.0",
                                "state": "baseline_ready",
                                "binding": runner_binding(),
                                "observer_id": "foreign"
                                if mode == "foreign"
                                else attachment_identity()["observer_id"],
                                "baseline_sha256": digest,
                                "sources": [
                                    {"path": "baseline.json", "sha256": digest}
                                ],
                            }
                        ),
                    )

                thread = threading.Thread(target=publisher)
                thread.start()
                with (
                    patch(
                        "supervisor.observer_identity",
                        return_value=attachment_identity(),
                    ),
                    patch("supervisor.subprocess.Popen") as spawn,
                ):
                    result = supervise(
                        self.root / "runner.json",
                        self.root / "binding.json",
                        self.root / "scenario.json",
                        self.root / (mode + "-ready"),
                        out,
                        0.3,
                        runtime_attachment=source / "runtime.json",
                        attachment_ready=source / "attached.json",
                        provider_ready=source / "ready.json",
                    )
                thread.join()
                self.assertEqual(result["coordination"], "incomplete")
                spawn.assert_not_called()
                self.assertFalse((self.root / (mode + "-ready")).exists())

    def test_provider_ready_preexisting_refused(self):
        path = self.root / "provider-ready.json"
        path.write_bytes(b"stale")
        with (
            self.assertRaises(ValueError),
            patch("supervisor.subprocess.Popen") as spawn,
        ):
            supervise(
                self.root / "runner.json",
                self.root / "binding.json",
                self.root / "scenario.json",
                self.root / "ready.json",
                self.root / "supervised",
                runtime_attachment=self.root / "runtime.json",
                attachment_ready=self.root / "attached.json",
                provider_ready=path,
            )
        spawn.assert_not_called()

    def test_deadline_and_output_quota_reap_real_children(self):
        for code in ("import time;time.sleep(30)", "print('x'*70000)"):
            commands = Commands(time.monotonic() + 0.2)
            started = time.monotonic()
            with (
                patch("fenrir_provider.os.killpg", wraps=os.killpg) as kill_group,
                self.assertRaises(ValueError),
            ):
                commands.run([sys.executable, "-c", code])
            self.assertLess(time.monotonic() - started, 3)
            pid = commands.records[0]["pid"]
            if "sleep" in code:
                kill_group.assert_called_once_with(pid, signal.SIGKILL)
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)

    def test_exited_command_never_signals_reaped_group(self):
        for status in (0, 3):
            with self.subTest(status=status):
                commands = Commands(time.monotonic() + 2)
                with patch("fenrir_provider.os.killpg") as kill_group:
                    argv = [
                        sys.executable,
                        "-c",
                        f"import sys;print('{{}}');sys.exit({status})",
                    ]
                    if status:
                        with self.assertRaisesRegex(ValueError, "command failed"):
                            commands.run(argv)
                    else:
                        self.assertEqual(commands.run(argv), {})
                kill_group.assert_not_called()
                self.assertEqual(commands.records[0]["returncode"], status)
                with self.assertRaises(ProcessLookupError):
                    os.kill(commands.records[0]["pid"], 0)

    def test_interrupt_reaps_owned_command(self):
        commands = Commands(time.monotonic() + 2)
        started = time.monotonic()
        with (
            patch("fenrir_provider.os.killpg", wraps=os.killpg) as kill_group,
            patch(
                "fenrir_provider.selectors.EpollSelector.select"
                if sys.platform == "linux"
                else "fenrir_provider.selectors.KqueueSelector.select",
                side_effect=KeyboardInterrupt,
            ),
            self.assertRaises(KeyboardInterrupt),
        ):
            commands.run([sys.executable, "-c", "import time;time.sleep(30)"])
        self.assertLess(time.monotonic() - started, 3)
        kill_group.assert_called_once_with(commands.records[0]["pid"], signal.SIGKILL)
        with self.assertRaises(ProcessLookupError):
            os.kill(commands.records[0]["pid"], 0)

    def test_supervisor_baseline_gate_orders_real_collector(self):
        errors, results = [], []

        def provider():
            try:
                results.append(
                    self.run_provider(
                        identity=self.root / "supervised/observer-identity.json"
                    )
                )
            except (OSError, ValueError, KeyError, TypeError, AssertionError) as error:
                errors.append(repr(error))

        def runner():
            try:
                self.wait(self.root / "ready.json")
                self.assertTrue((self.root / "supervised/provider-ready.json").exists())
                self.launch()
                self.wait(self.root / "attached.json")
                # No actual device is opened: finish before publishing a feed.
                publish_envelope(
                    self.root / "observer-cancel.json",
                    encoded(
                        {
                            "schema_version": "1.0.0",
                            "state": "host_cancel_completed",
                            "binding": runner_binding(),
                            "request_succeeded": True,
                        }
                    ),
                )
            except (OSError, ValueError, KeyError, TypeError, AssertionError) as error:
                errors.append(repr(error))

        self.objects["devices"] = []
        self.save_objects()
        worker = threading.Thread(target=provider)
        worker.start()
        self.wait(self.output)
        run = threading.Thread(target=runner)
        run.start()
        with patch("supervisor.observer_identity", return_value=attachment_identity()):
            result = supervise(
                self.root / "runner.json",
                self.root / "binding.json",
                self.root / "scenario.json",
                self.root / "ready.json",
                self.root / "supervised",
                7,
                command=[
                    sys.executable,
                    str(Path(__file__).with_name("supervisor_fixture.py")),
                ],
                provenance="synthetic_fixture",
                runtime_attachment=self.output / "runtime.json",
                attachment_ready=self.root / "attached.json",
                provider_ready=self.output / "provider-ready.json",
            )
        worker.join()
        run.join()
        self.assertEqual(errors, [])
        self.assertEqual(result["coordination"], "published", result)
        self.assertEqual(results[0]["state"], "incomplete")
        if evidence := os.environ.get("PROVIDER_EVIDENCE"):
            shutil.copytree(
                self.root / "supervised", Path(evidence) / "supervisor-ordering"
            )


if __name__ == "__main__":
    unittest.main()
