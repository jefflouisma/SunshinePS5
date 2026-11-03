"""Opt-in read-only Fenrir provider. Publications remain unauthenticated evidence."""

import argparse
import hashlib
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
    DEPLOYED_REVISION,
    RuntimeAttachment,
    encoded,
    exact_fields,
    publish_envelope,
    read_runner_binding,
    strict_document,
)
from supervisor import absent, cancel_result, snapshot

SESSIONS = "sessions.direwolf.games-on-whales.github.io"


def require(value, reason):
    if not value:
        raise ValueError(reason)


def name(value):
    require(
        isinstance(value, str) and re.fullmatch(r"[a-z0-9][a-z0-9.-]{0,252}", value),
        "invalid Kubernetes name",
    )
    return value


def host_check(identity):
    require(sys.platform == "linux", "provider requires Linux host clock")
    require(
        Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        == identity["host_boot_id"],
        "observer host mismatch",
    )


def provider_ready(path, binding, identity):
    """Validate baseline barrier without trusting arbitrary source paths."""
    data = snapshot(Path(path), 16384)
    value = strict_document(data)
    exact_fields(
        value, "schema_version state binding observer_id baseline_sha256 sources"
    )
    require(
        value["schema_version"] == "1.0.0"
        and value["state"] == "baseline_ready"
        and value["binding"] == binding
        and value["observer_id"] == identity["observer_id"],
        "provider READY mismatch",
    )
    require(
        value["sources"]
        == [{"path": "baseline.json", "sha256": value["baseline_sha256"]}],
        "baseline source binding",
    )
    source = snapshot(Path(path).parent / "baseline.json", 16384)
    require(
        hashlib.sha256(source).hexdigest() == value["baseline_sha256"],
        "baseline digest",
    )
    baseline = strict_document(source)
    exact_fields(baseline, "session_uids observed_ns cluster_uid")
    require(
        type(baseline["observed_ns"]) is int
        and identity["created_ns"] <= baseline["observed_ns"] <= time.monotonic_ns(),
        "baseline clock",
    )
    require(
        isinstance(baseline["session_uids"], list)
        and len(baseline["session_uids"]) <= 128
        and len(set(baseline["session_uids"])) == len(baseline["session_uids"]),
        "baseline UIDs",
    )
    return data, source


class Commands:
    """No raw Kubernetes output/error is persisted: it can contain credentials."""

    def __init__(self, deadline):
        self.deadline = deadline
        self.records = []

    def check(self):
        require(time.monotonic() < self.deadline, "provider deadline")

    def run(self, argv):
        self.check()
        require(len(self.records) < 256, "command quota")
        record = {"argv": argv, "started_ns": time.monotonic_ns()}
        self.records.append(record)
        process = None
        selector = selectors.DefaultSelector()
        buffers = {"stdout": bytearray(), "stderr": bytearray()}
        try:
            process = subprocess.Popen(
                argv,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                start_new_session=True,
            )
            record["pid"] = process.pid
            until = min(self.deadline, time.monotonic() + 5)
            for key in buffers:
                stream = getattr(process, key)
                os.set_blocking(stream.fileno(), False)
                selector.register(stream, selectors.EVENT_READ, key)
            while selector.get_map() or process.poll() is None:
                require(time.monotonic() < until, "command deadline")
                for key, _ in selector.select(min(0.02, until - time.monotonic())):
                    data = os.read(key.fileobj.fileno(), 8192)
                    if not data:
                        selector.unregister(key.fileobj)
                    else:
                        target = buffers[key.data]
                        require(len(target) + len(data) <= 65536, "command quota")
                        target.extend(data)
            require(process.returncode == 0, "Kubernetes command failed")
            self.check()
            return strict_document(bytes(buffers["stdout"]))
        finally:
            # A faulty pipe/log close must never precede owned-process cleanup.
            if process is not None:
                # Once reaped, the numeric group ID can belong to another process.
                # Descendant cleanup after a known leader exit is unconfirmed.
                if process.poll() is None:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                try:
                    record["returncode"] = process.wait(timeout=2)
                finally:
                    for stream in (process.stdout, process.stderr):
                        try:
                            stream.close()
                        except OSError:
                            record["close_error"] = True
            selector.close()
            record["ended_ns"] = time.monotonic_ns()
            record["stdout_bytes"] = len(buffers["stdout"])
            record["stderr_bytes"] = len(buffers["stderr"])


class Kubernetes:
    def __init__(self, commands, executable, kubeconfig, context, namespace, port):
        executable = Path(executable)
        require(executable.is_absolute() and os.access(executable, os.X_OK), "kubectl")
        require(
            isinstance(context, str) and re.fullmatch(r"[\w.-]{1,128}", context),
            "context",
        )
        self.namespace, self.port = name(namespace), name(port)
        # Freeze the config inode/bytes and refuse TLS bypass in selected config.
        self.config_path = Path(kubeconfig)
        self.config = snapshot(self.config_path, 65536, with_identity=True)
        self.commands = commands
        self.base = [
            str(executable),
            "--kubeconfig",
            str(self.config_path),
            "--context",
            context,
            "--request-timeout=4s",
        ]
        selected = self.call(["config", "view", "--minify", "-o", "json"])
        clusters = selected.get("clusters", [])
        require(len(clusters) == 1, "selected cluster missing")
        cluster = clusters[0]["cluster"]
        require(
            cluster.get("insecure-skip-tls-verify", False) is False
            and str(cluster.get("server", "")).startswith("https://")
            and not cluster.get("proxy-url"),
            "verified Kubernetes TLS required",
        )
        # CA may be a file/data or the system trust store. kubectl verifies TLS.
        self.cluster_uid = self.get("namespaces", "kube-system", scoped=False)[
            "metadata"
        ]["uid"]

    def call(self, args):
        require(
            snapshot(self.config_path, 65536, with_identity=True) == self.config,
            "kubeconfig changed",
        )
        value = self.commands.run(self.base + args)
        require(
            snapshot(self.config_path, 65536, with_identity=True) == self.config,
            "kubeconfig changed",
        )
        return value

    def get(self, resource, item=None, *, scoped=True):
        args = ["get", resource]
        if item is not None:
            args.append(name(item))
        if scoped:
            args += ["--namespace", self.namespace]
        return self.call(args + ["-o", "json"])

    def devices(self, pod, agent):
        # Kubernetes ResourceLocation requires scheme:pod:numeric-port.
        endpoint = (
            f"/api/v1/namespaces/{self.namespace}/pods/"
            f"https:{name(pod)}:{agent['container_port']}/proxy/input-devices"
        )
        value = self.call(["get", "--raw", endpoint])
        exact_fields(value, "devices")
        require(isinstance(value["devices"], list), "device list")
        return value["devices"]


def metadata(value, namespace):
    item = value["metadata"]
    require(
        item.get("namespace") == namespace and not item.get("deletionTimestamp"),
        "object namespace/deletion",
    )
    return {
        "namespace": namespace,
        "name": name(item["name"]),
        "uid": item["uid"],
        "resource_version": item["resourceVersion"],
    }


def owner(value, parent, kind, namespace):
    result = metadata(value, namespace)
    refs = value["metadata"].get("ownerReferences", [])
    require(len(refs) == 1, "ambiguous owner")
    result["owner_references"] = [
        {key: refs[0][key] for key in ("kind", "name", "uid", "controller")}
    ]
    require(
        result["owner_references"]
        == [
            {
                "kind": kind,
                "name": parent["name"],
                "uid": parent["uid"],
                "controller": True,
            }
        ]
        and refs[0]["controller"] is True,
        "foreign owner",
    )
    return result


def children(items, parent, kind):
    require(isinstance(items, list) and len(items) <= 128, "object list quota")
    matches = [
        item
        for item in items
        if any(
            ref.get("uid") == parent["uid"] and ref.get("kind") == kind
            for ref in item.get("metadata", {}).get("ownerReferences", [])
        )
    ]
    require(len(matches) == 1, "runtime missing or ambiguous")
    return matches[0]


def agent_endpoint(pod, configured):
    specs = [c for c in pod["spec"]["containers"] if c["name"] == "wolf-agent"]
    statuses = [
        c for c in pod["status"]["containerStatuses"] if c["name"] == "wolf-agent"
    ]
    require(len(specs) == len(statuses) == 1, "agent container absent/ambiguous")
    ports = specs[0].get("ports", [])
    require(isinstance(ports, list) and len(ports) <= 32, "agent port quota")
    for port in ports:
        require(
            type(port.get("containerPort")) is int
            and 1 <= port["containerPort"] <= 65535,
            "invalid container port",
        )
    if configured.isdecimal():
        require(str(int(configured)) == configured, "noncanonical numeric port")
        matches = [p for p in ports if p["containerPort"] == int(configured)]
    else:
        matches = [p for p in ports if p.get("name") == configured]
    require(len(matches) == 1, "agent port absent/ambiguous")
    port = matches[0]
    require(
        port.get("protocol", "TCP") == "TCP"
        and sum(p["containerPort"] == port["containerPort"] for p in ports) == 1,
        "agent port protocol/ambiguity",
    )
    return {
        "container_name": "wolf-agent",
        "container_port": port["containerPort"],
        "port_name": port.get("name"),
        "scheme": "https",
    }


def runtime(kube, session, selection):
    ns = kube.namespace
    summary = metadata(session, ns)
    for field, reference in (
        ("user_name", "userReference"),
        ("app_name", "gameReference"),
        ("pairing_name", "pairingReference"),
    ):
        summary[field] = session["spec"][reference]["name"]
        require(summary[field] == selection[field], "foreign session")
    deployment = children(kube.get("deployments")["items"], summary, "Session")
    dep = owner(deployment, summary, "Session", ns)
    replica = children(kube.get("replicasets")["items"], dep, "Deployment")
    rs = owner(replica, dep, "Deployment", ns)
    pod = children(kube.get("pods")["items"], rs, "ReplicaSet")
    p = owner(pod, rs, "ReplicaSet", ns)
    node_name = name(pod["spec"]["nodeName"])
    node = kube.get("nodes", node_name, scoped=False)
    p.update(node_name=node_name, node_uid=node["metadata"]["uid"])
    containers = []
    for item in pod["status"]["containerStatuses"]:
        require(
            item.get("ready") is True and "running" in item["state"],
            "container not ready",
        )
        containers.append(
            {
                "name": item["name"],
                "container_id": item["containerID"],
                "image_id": item["imageID"],
                "restart_count": item["restartCount"],
                "started_at": item["state"]["running"]["startedAt"],
            }
        )
    return {
        "session": summary,
        "deployment": dep,
        "replicaset": rs,
        "pod": p,
        "containers": sorted(containers, key=lambda c: c["name"]),
        "wolf_session_id": session["status"]["wolfSessionID"],
    }, agent_endpoint(pod, kube.port)


def incarnation(value):
    """Resource versions may advance; object/container incarnations must not."""
    return {
        key: {k: v for k, v in item.items() if k != "resource_version"}
        if isinstance(item, dict)
        else item
        for key, item in value.items()
    }


def provide(
    output,
    binding_path,
    identity_path,
    launch_path,
    ack_path,
    *,
    kubectl,
    kubeconfig,
    context,
    namespace,
    agent_port,
    user,
    app,
    pairing,
    timeout=60,
    enable=False,
):
    require(enable, "provider disabled")
    require(math.isfinite(timeout) and 0 < timeout <= 300, "provider timeout")
    output = Path(output).absolute()
    absent(output)
    output.mkdir(mode=0o700)
    deadline = time.monotonic() + timeout
    commands = Commands(deadline)
    result = {
        "acceptance": "Incomplete",
        "state": "incomplete",
        "commands": commands.records,
        "artifacts": {},
    }
    frozen = {}
    cancel_path = Path(binding_path).parent / "observer-cancel.json"

    def retain(filename, value):
        data = encoded(value)
        require(len(data) <= 16384, "provider publication quota")
        require(len(result["artifacts"]) < 16, "provider file quota")
        synced = publish_envelope(output / filename, data)
        result["artifacts"][filename] = hashlib.sha256(data).hexdigest()
        if not synced:
            result["directory_durability"] = "unconfirmed"
        return {"path": filename, "sha256": hashlib.sha256(data).hexdigest()}

    def read(path, *, wait=False):
        path = Path(path)
        while True:
            commands.check()
            if cancel_path.exists():
                cancel_result(
                    cancel_path, strict_document(frozen[Path(binding_path)][0])
                )
                raise ValueError("runner cancellation ended provider acquisition")
            try:
                current = snapshot(path, 65536, with_identity=True)
                break
            except FileNotFoundError:
                if not wait:
                    raise
                time.sleep(0.02)
        if path in frozen:
            require(frozen[path] == current, "coordination snapshot changed")
        frozen[path] = current
        return strict_document(current[0])

    def stable():
        for path in tuple(frozen):
            read(path)
        commands.check()

    try:
        for value in (user, app, pairing):
            name(value)
        absent(Path(launch_path))
        absent(Path(ack_path))
        binding = read(binding_path, wait=True)
        require(read_runner_binding(binding_path, binding) == binding, "binding")
        identity = read(identity_path, wait=True)
        host_check(identity)
        state = RuntimeAttachment(
            identity, binding, output / "runtime.json", ack_path, launch_path
        )
        kube = Kubernetes(commands, kubectl, kubeconfig, context, namespace, agent_port)
        baseline = kube.get(SESSIONS)["items"]
        require(isinstance(baseline, list) and len(baseline) <= 128, "baseline quota")
        baseline_uids = {item["metadata"]["uid"] for item in baseline}
        absent(Path(launch_path))  # No launch may race the baseline query.
        baseline_ref = retain(
            "baseline.json",
            {
                "session_uids": sorted(baseline_uids),
                "observed_ns": time.monotonic_ns(),
                "cluster_uid": kube.cluster_uid,
            },
        )
        absent(Path(launch_path))
        retain(
            "provider-ready.json",
            {
                "schema_version": "1.0.0",
                "state": "baseline_ready",
                "binding": binding,
                "observer_id": identity["observer_id"],
                "baseline_sha256": baseline_ref["sha256"],
                "sources": [baseline_ref],
            },
        )
        launch = read(launch_path, wait=True)
        exact_fields(launch, "schema_version state binding launch_nonce")
        nonce = uuid.UUID(launch["launch_nonce"])
        require(
            launch["schema_version"] == "1.0.0"
            and launch["state"] == "launched"
            and launch["binding"] == binding
            and nonce.version == 4
            and str(nonce) == launch["launch_nonce"],
            "launch marker",
        )
        state.launch = launch
        selection = {"user_name": user, "app_name": app, "pairing_name": pairing}
        while True:
            stable()
            sessions = kube.get(SESSIONS)["items"]
            require(
                isinstance(sessions, list) and len(sessions) <= 128, "session quota"
            )
            matches = [
                item
                for item in sessions
                if item["metadata"]["uid"] not in baseline_uids
                and all(
                    item["spec"][ref]["name"] == expected
                    for ref, expected in (
                        ("userReference", user),
                        ("gameReference", app),
                        ("pairingReference", pairing),
                    )
                )
            ]
            require(len(matches) <= 1, "ambiguous new session")
            if matches:
                break
            time.sleep(0.02)
        first, agent = runtime(kube, matches[0], selection)

        def verify_runtime():
            stable()
            now, current_agent = runtime(
                kube, kube.get(SESSIONS, first["session"]["name"]), selection
            )
            require(
                incarnation(now) == incarnation(first) and current_agent == agent,
                "runtime incarnation changed",
            )
            return now

        verify_runtime()
        source_data = {
            "cluster_uid": kube.cluster_uid,
            "agent_endpoint": agent,
            **first,
        }
        source = {
            "path": "runtime-source.json",
            "sha256": hashlib.sha256(encoded(source_data)).hexdigest(),
        }
        value = {
            "schema_version": "1.0.0",
            "state": "attached",
            "runner_binding": binding,
            "launch_nonce": launch["launch_nonce"],
            "observer_id": identity["observer_id"],
            "observed_ns": time.monotonic_ns(),
            "association": "exclusive_launch_candidate",
            "authenticated": False,
            "source_revision": DEPLOYED_REVISION,
            "cluster_identity": kube.cluster_uid,
            **first,
            "devices": [],
            "sources": [source],
        }
        state.validate(value)
        stable()
        retain("runtime-source.json", source_data)
        retain("runtime.json", value)
        state.value, state.data = value, encoded(value)
        expected = {
            "schema_version": "1.0.0",
            "state": "attached",
            "binding": binding,
            "launch_nonce": launch["launch_nonce"],
            "observer_id": identity["observer_id"],
            "association": "exclusive_launch_candidate",
            "authenticated": False,
            "runtime_attachment_sha256": hashlib.sha256(state.data).hexdigest(),
        }
        ack = read(ack_path, wait=True)
        require(ack == expected, "attachment acknowledgement")
        state.acknowledged, state.acknowledged_ns = True, time.monotonic_ns()
        while True:
            verify_runtime()
            devices = kube.devices(first["pod"]["name"], agent)
            verify_runtime()
            if devices:
                break
            time.sleep(0.02)
        source_data = {
            "pod_uid": first["pod"]["uid"],
            "agent_endpoint": agent,
            "devices": devices,
        }
        source = {
            "path": "devices-source.json",
            "sha256": hashlib.sha256(encoded(source_data)).hexdigest(),
        }
        feed = {
            "schema_version": "1.0.0",
            "state": "devices_attached",
            "runner_binding": binding,
            "launch_nonce": launch["launch_nonce"],
            "observer_id": identity["observer_id"],
            "runtime_attachment_sha256": hashlib.sha256(state.data).hexdigest(),
            "observed_ns": time.monotonic_ns(),
            "continuity": "unverified",
            "devices": devices,
            "sources": [source],
        }
        state.validate_feed(feed)
        stable()
        retain("devices-source.json", source_data)
        retain("devices.json", feed)
        result["state"] = "published"
    except (OSError, ValueError, KeyError, TypeError, KeyboardInterrupt) as error:
        # Never persist server exception messages, raw replies or credentials.
        result["error"] = "provider failed; partial publications remain Incomplete"
        result["error_type"] = type(error).__name__
    finally:
        # Reports carry references to bounded command batches, never raw replies.
        records = result.pop("commands")
        batches, batch = [], []
        for record in records:
            if len(encoded(batch + [record])) > 12000:
                batches.append(retain(f"commands-{len(batches)}.json", batch))
                batch = []
            batch.append(record)
        if batch:
            batches.append(retain(f"commands-{len(batches)}.json", batch))
        result["command_sources"] = batches
        retain("provider-report.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in (
        "output",
        "runner-binding",
        "observer-identity",
        "launch-marker",
        "attachment-ready",
        "kubectl",
        "kubeconfig",
        "context",
        "namespace",
        "agent-port",
        "user",
        "app",
        "pairing",
    ):
        parser.add_argument("--" + arg, required=True)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--enable-provider", action="store_true")
    args = parser.parse_args()

    def terminated(_number, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, terminated)
    provide(
        args.output,
        args.runner_binding,
        args.observer_identity,
        args.launch_marker,
        args.attachment_ready,
        kubectl=args.kubectl,
        kubeconfig=args.kubeconfig,
        context=args.context,
        namespace=args.namespace,
        agent_port=args.agent_port,
        user=args.user,
        app=args.app,
        pairing=args.pairing,
        timeout=args.timeout,
        enable=args.enable_provider,
    )
    return 2  # Coordination alone never passes acceptance.


if __name__ == "__main__":
    raise SystemExit(main())
