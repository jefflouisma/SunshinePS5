"""Offline SSH subsystem fixture. No network access and no production acceptance."""

import json
import os
import subprocess
import sys
import time
from pathlib import Path
from unittest.mock import patch

import remote_relay as relay
import supervisor
from observer import encoded, publish_envelope
from supervisor import document
from test_observer import attachment_identity, attachment_value


def wait(path, deadline):
    while time.monotonic() < deadline:
        try:
            return document(path, 1048576)
        except FileNotFoundError:
            time.sleep(0.005)
    raise TimeoutError("fixture deadline")


def provider(root, timeout):
    until = time.monotonic() + timeout
    out = root / "provider"
    out.mkdir(mode=0o700)
    identity = wait(root / "supervised/observer-identity.json", until)
    binding = document(root / "observer-binding.json")
    baseline = encoded(
        {
            "session_uids": [],
            "observed_ns": time.monotonic_ns(),
            "cluster_uid": "fixture",
        }
    )
    publish_envelope(out / "baseline.json", baseline)
    publish_envelope(
        out / "provider-ready.json",
        encoded(
            {
                "schema_version": "1.0.0",
                "state": "baseline_ready",
                "binding": binding,
                "observer_id": identity["observer_id"],
                "baseline_sha256": relay.digest(baseline),
                "sources": [
                    {"path": "baseline.json", "sha256": relay.digest(baseline)}
                ],
            }
        ),
    )
    launch = wait(root / "observer-launch.json", until)
    value = attachment_value()
    value.update(
        runner_binding=binding,
        observer_id=identity["observer_id"],
        launch_nonce=launch["launch_nonce"],
        devices=[],
        observed_ns=time.monotonic_ns(),
        sources=[{"path": "runtime-source.json", "sha256": relay.digest(b"{}\n")}],
    )
    publish_envelope(out / "runtime-source.json", b"{}\n")
    publish_envelope(out / "runtime.json", encoded(value))
    wait(root / "attached.json", until)
    wait(root / "observer-cancel.json", until)


def supervised(root, timeout):
    def identity(binding):
        value = attachment_identity()
        value.update(
            runner_binding=binding,
            pid=os.getpid(),
            collector_sha256=relay.code_hashes()["observer.py"],
            supervisor_sha256=relay.code_hashes()["supervisor.py"],
        )
        return value

    collector = [sys.executable, str(Path(__file__).with_name("supervisor_fixture.py"))]
    if os.environ.get("RELAY_FAULT") == "large":
        collector = [sys.executable, __file__, "collector"]
    with patch("supervisor.observer_identity", side_effect=identity):
        supervisor.supervise(
            root / "observer-binding.json",
            root / "binding.json",
            root / "scenario.json",
            root / "ready.json",
            root / "supervised",
            timeout,
            command=collector,
            provenance="synthetic_fixture",
            runtime_attachment=root / "provider/runtime.json",
            attachment_ready=root / "attached.json",
            device_feed=root / "provider/devices.json",
            provider_ready=root / "provider/provider-ready.json",
        )


class FixtureOwner(relay.Owner):
    def __init__(self, *args):
        with patch("remote_relay.sys.platform", "linux"):
            super().__init__(*args)

    def start(self):
        for mode in ("provider", "supervisor"):
            self.processes.append(
                subprocess.Popen(
                    [
                        sys.executable,
                        __file__,
                        mode,
                        str(self.root),
                        str(max(0.001, self.deadline - time.monotonic())),
                    ],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    start_new_session=True,
                )
            )
            if mode == "provider":
                self.await_provider_directory()

    def poll(self):
        result = super().poll()
        mode = os.environ.get("RELAY_FAULT")
        if "artifact" in result:
            item = result["artifact"]
            if mode == "hash":
                item["sha256"] = "0" * 64
            elif mode == "replace":
                path = self.root / item["name"]
                data = path.read_bytes()
                path.unlink()
                path.write_bytes(data)
            elif mode == "directory":
                self.root.rename(self.root.with_name(self.root.name + "-replaced"))
                self.root.mkdir(mode=0o700)
            elif mode == "nonce" and item["name"] == "attached.json":
                value = json.loads(relay.unpack(item, relay.REMOTE_FILES)[1])
                value["launch_nonce"] = "419887b2-499d-4c11-9ea2-4f093aaa087a"
                result["artifact"] = relay.packet(item["name"], encoded(value))
        return result


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "collector":
        import observer

        original = observer.publish

        def large(root, binding, scenario, records, provenance):
            records.append(
                {
                    "kind": "error",
                    "reason": "fixture" * 4000,
                    "session_id": binding["session_id"],
                }
            )
            return original(root, binding, scenario, records, "synthetic_fixture")

        sys.argv.pop(1)
        with (
            patch("observer.sys.platform", "linux"),
            patch("observer.inventory", return_value={}),
            patch("observer.publish", side_effect=large),
        ):
            observer.main()
        return
    if len(sys.argv) == 4 and sys.argv[1] in {"supervisor", "provider"}:
        root, timeout = Path(sys.argv[2]), float(sys.argv[3])
        (supervised if sys.argv[1] == "supervisor" else provider)(root, timeout)
        return
    # Verify the entire invocation against an independently enumerated policy.
    args = sys.argv[1:]
    relay.require(args[:4] == ["-F", "/dev/null", "-T", "-p"], "fixture flags")
    relay.require(
        args[4:7] == ["22", "-l", "observer"] and args[7] == "-i", "fixture identity"
    )
    relay.require(
        args[-3:] == ["-s", "observer.test", "sunshine-reference-observer-v1"],
        "fixture subsystem",
    )
    options = args[9:-3]
    relay.require(
        len(options) % 2 == 0 and options[::2] == ["-o"] * (len(options) // 2),
        "fixture options",
    )
    values = dict(item.split("=", 1) for item in options[1::2])
    expected = {
        "StrictHostKeyChecking": "yes",
        "BatchMode": "yes",
        "ForwardAgent": "no",
        "ClearAllForwardings": "yes",
        "PasswordAuthentication": "no",
        "KbdInteractiveAuthentication": "no",
        "IdentitiesOnly": "yes",
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
        "HostKeyAlias": "observer.test",
        "LogLevel": "ERROR",
        "IdentityAgent": "none",
    }
    known_hosts = values.pop("UserKnownHostsFile")
    relay.require(
        values == expected and Path(known_hosts).is_file(), "fixture SSH policy"
    )
    fault = os.environ.get("RELAY_FAULT")
    if fault == "timeout":
        time.sleep(30)
    elif fault == "partial":
        os.write(1, b'{"relay_id":')
        time.sleep(30)
    else:
        relay.serve(owner_factory=FixtureOwner)


if __name__ == "__main__":
    main()
