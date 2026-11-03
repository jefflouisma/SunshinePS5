"""Retain reproducible synthetic test and consumer-contract evidence."""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    sys.dont_write_bytecode = True
    from observer import publish
    from test_observer import binding, population, scenario

    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--evidence-dir",
        type=Path,
        default=root / ".omo/evidence/t038-reference-observer-20261031",
        help="parent directory for a fresh synthetic verification attempt",
    )
    evidence = parser.parse_args().evidence_dir
    evidence.mkdir(parents=True, exist_ok=True)
    evidence = evidence.resolve()
    tool = Path(__file__).resolve().parent
    commands = [
        (
            "unit.log",
            [
                sys.executable,
                "-m",
                "unittest",
                "discover",
                "-s",
                str(tool),
                "-p",
                "test_*.py",
                "-v",
            ],
        ),
        ("ruff.log", [str(root / ".venv/bin/ruff"), "check", "--no-cache", str(tool)]),
        (
            "ruff-isolated.log",
            [
                str(root / ".venv/bin/ruff"),
                "check",
                "--no-cache",
                "--isolated",
                "--target-version",
                "py311",
                "--select",
                "E,F,I,UP",
                *map(str, sorted(tool.glob("*.py"))),
            ],
        ),
        (
            "format.log",
            [
                str(root / ".venv/bin/ruff"),
                "format",
                "--no-cache",
                "--check",
                str(tool),
            ],
        ),
    ]
    attempt = Path(tempfile.mkdtemp(prefix="attempt-", dir=evidence))
    (attempt / "source-hashes.json").write_text(
        json.dumps(
            {
                str(path.relative_to(root)): hashlib.sha256(
                    path.read_bytes()
                ).hexdigest()
                for path in [
                    *sorted(tool.glob("*.py")),
                    tool / "k3s-kubectl.sh",
                    tool / "README.md",
                ]
            },
            indent=2,
        )
        + "\n"
    )
    (attempt / "readiness-regression").mkdir()
    fixture = attempt / "fixture"
    publish(fixture, binding(), scenario(), population(), "synthetic_fixture")
    commands.append(
        (
            "consumer.log",
            [
                "cargo",
                "run",
                "--locked",
                "--offline",
                "--manifest-path",
                str(tool / "contract-check/Cargo.toml"),
                "--",
                str(fixture),
            ],
        )
    )
    commands.append(
        (
            "readiness-contract.log",
            [
                "cargo",
                "test",
                "--locked",
                "--offline",
                "--manifest-path",
                str(root / "tools/moonlight-mcp/Cargo.toml"),
                "--lib",
                "runner::tests::readiness_python_collector_publishes_accepted_envelope_without_promoting_criteria",
                "--",
                "--exact",
                "--nocapture",
            ],
        )
    )
    commands.append(
        (
            "readiness-regression.log",
            [
                "cargo",
                "test",
                "--locked",
                "--offline",
                "--manifest-path",
                str(root / "tools/moonlight-mcp/Cargo.toml"),
                "--lib",
                "runner::tests::readiness_",
                "--",
                "--nocapture",
            ],
        )
    )
    for name in (
        "valid-finish",
        "present-device",
        "acquisition-error-0",
        "acquisition-error-1",
        "cli/run",
        "cli-close-error/run",
    ):
        commands.append(
            (
                "finish-consumer-" + name.replace("/", "-") + ".log",
                [
                    "cargo",
                    "run",
                    "--locked",
                    "--offline",
                    "--manifest-path",
                    str(tool / "contract-check/Cargo.toml"),
                    "--",
                    str(attempt / "finish-protocol" / name),
                ],
            )
        )
    statuses = []
    for name in (
        "attachment",
        "supervised-attachment/collector",
        "late-device-feed",
        "remote-relay/cancel-true/local/remote/supervised/collector",
        "remote-relay/cancel-false/local/remote/supervised/collector",
        "remote-relay/large/local/remote/supervised/collector",
    ):
        commands.append(
            (
                "attachment-consumer-" + name.replace("/", "-") + ".log",
                [
                    "cargo",
                    "run",
                    "--locked",
                    "--offline",
                    "--manifest-path",
                    str(tool / "contract-check/Cargo.toml"),
                    "--",
                    str(attempt / name),
                ],
            )
        )
    target = os.environ.get("CARGO_TARGET_DIR") or tempfile.mkdtemp(
        prefix="t038-build-"
    )
    environment = os.environ | {
        "PYTHONDONTWRITEBYTECODE": "1",
        "CARGO_TARGET_DIR": target,
        "TMPDIR": str(Path(tempfile.gettempdir()).resolve()),
        "MOONLIGHT_READY_TEST_EVIDENCE": str(attempt),
        "OBSERVER_PUBLISH_TEST_EVIDENCE": str(attempt / "publication-interleavings"),
        "OBSERVER_FINISH_TEST_EVIDENCE": str(attempt / "finish-protocol"),
        "ATTACHMENT_EVIDENCE": str(attempt),
        "PROVIDER_EVIDENCE": str(attempt / "provider"),
        "RELAY_EVIDENCE": str(attempt / "remote-relay"),
    }
    for name, command in commands:
        result = subprocess.run(
            command,
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
            env=environment
            | (
                {"MOONLIGHT_READY_TEST_EVIDENCE": str(attempt / "readiness-regression")}
                if name == "readiness-regression.log"
                else {}
            ),
        )
        (attempt / name).write_text(
            "$ "
            + " ".join(command)
            + "\n"
            + result.stdout
            + result.stderr
            + f"\nexit={result.returncode}\n"
        )
        statuses.append({"log": name, "exit": result.returncode})
        print(name, result.returncode, flush=True)
        if result.returncode != 0:
            # The evidence directory is not uploaded; show why in the job log.
            print((result.stdout + result.stderr)[-6000:], flush=True)
    bridge = attempt / "python-collector"
    try:
        observable = json.loads((bridge / "observable.json").read_bytes())
        report = json.loads((bridge / "report.json").read_bytes())
        ready = (bridge / "ready.json").read_bytes()
        checks = {
            "launch_factory_called": observable["launch_factory_called"] is True,
            "overall_incomplete": report["overall_status"] == "Incomplete",
            "all_criteria_incomplete": bool(report["criteria"])
            and all(row["status"] == "Incomplete" for row in report["criteria"]),
            "exact_retained_envelope": ready
            == (bridge / "observer-ready.json").read_bytes(),
            "exact_runner_binding": json.loads(ready)["binding"]
            == json.loads((bridge / "observer-binding.json").read_bytes()),
        }
    except (OSError, ValueError, KeyError) as error:
        checks = {"artifact_error": str(error), "complete": False}
    (attempt / "readiness-artifacts.json").write_text(
        json.dumps(checks, indent=2) + "\n"
    )
    statuses.append(
        {
            "log": "readiness-artifacts.json",
            "exit": int(not all(value is True for value in checks.values())),
        }
    )
    interleavings = {}
    for point in ("directory_sync", "directory_close", "none"):
        try:
            artifact = attempt / "publication-interleavings" / point
            observable = json.loads((artifact / "observable.json").read_bytes())
            envelope = json.loads((artifact / "ready.json").read_bytes())
            interleavings[point] = (
                observable["reader_retained_before_housekeeping"] is True
                and observable["stable_identity"] is True
                and observable["identity_at_visibility"]
                == observable["identity_after_housekeeping"]
                and observable["single_link"] is True
                and observable["ready_still_present"] is True
                and observable["inventory_calls"] == 2
                and observable["records"][-1]["kind"] == "end"
                and observable["stdout"]["ready_directory_synced"]
                == (point != "directory_sync")
                and envelope["state"] == "ready"
            )
        except (OSError, ValueError, KeyError, IndexError):
            interleavings[point] = False
    (attempt / "publication-interleavings.json").write_text(
        json.dumps(interleavings, indent=2) + "\n"
    )
    statuses.append(
        {
            "log": "publication-interleavings.json",
            "exit": int(not all(interleavings.values())),
        }
    )
    finish_checks = {}
    for name, reason in (
        ("valid-finish", "finish_command"),
        ("present-device", "finish_command"),
        ("acquisition-error-0", "acquisition_error"),
        ("acquisition-error-1", "acquisition_error"),
        ("cli/run", "finish_command"),
        ("cli-close-error/run", "control_close_error"),
    ):
        try:
            artifact = attempt / "finish-protocol" / name
            raw = json.loads((artifact / "raw.json").read_bytes())
            manifest = json.loads((artifact / "manifest.json").read_bytes())
            last = raw["records"][-1]
            finish_checks[name] = (
                raw["provenance"] == "synthetic_fixture"
                and last["termination_reason"] == reason
                and all(row["satisfied"] is None for row in manifest["observations"])
                and (name != "present-device" or "event1" in last["devices"])
                and (reason != "acquisition_error" or last["kind"] == "error")
            )
        except (OSError, ValueError, KeyError, IndexError):
            finish_checks[name] = False
    for name, status in (("cli", "finished"), ("cli-close-error", "incomplete")):
        try:
            folder = attempt / "finish-protocol" / name
            cli = json.loads((folder / "observable.json").read_bytes())
            output = [
                json.loads(line)
                for line in (folder / "stdout.log").read_text().splitlines()
            ]
            finish_checks[name + "_bounded_exit"] = (
                cli["exit"] == 0
                and cli["elapsed_after_finish_seconds"] < 2
                and cli["synthetic_acquisition"] is True
                and cli["injected_control_close_failure"] == (name == "cli-close-error")
                and output[-1]["finish_status"] == status
            )
        except (OSError, ValueError, KeyError, IndexError):
            finish_checks[name + "_bounded_exit"] = False
    (attempt / "finish-artifacts.json").write_text(
        json.dumps(finish_checks, indent=2) + "\n"
    )
    statuses.append(
        {"log": "finish-artifacts.json", "exit": int(not all(finish_checks.values()))}
    )
    relay_checks = {}
    for name in (
        "cancel-true",
        "cancel-false",
        "large",
        "hash",
        "replace",
        "directory",
        "nonce",
        "timeout",
        "partial",
        "wrong-host",
        "wrong-hash",
        "stale",
    ):
        try:
            folder = attempt / "remote-relay" / name
            report = json.loads((folder / "local/relay.json").read_bytes())
            observable = json.loads((folder / "observable.json").read_bytes())
            published = name.startswith("cancel-") or name == "large"
            relay_checks[name] = (
                report["acceptance"] == "Incomplete"
                and report["transport"] == ("published" if published else "incomplete")
                and report["remote_cleanup"]
                == ("owners_reaped" if published else "unconfirmed")
                and not report["cleanup_errors"]
                and not observable["runner_errors"]
                and all(
                    hashlib.sha256((folder / "local" / path).read_bytes()).hexdigest()
                    == sha
                    for path, sha in report["artifacts"].items()
                )
            )
        except (OSError, ValueError, KeyError):
            relay_checks[name] = False
    (attempt / "relay-artifacts.json").write_text(
        json.dumps(relay_checks, indent=2) + "\n"
    )
    statuses.append(
        {"log": "relay-artifacts.json", "exit": int(not all(relay_checks.values()))}
    )
    (attempt / "results.json").write_text(json.dumps(statuses, indent=2) + "\n")
    print("evidence:", attempt, flush=True)
    return int(any(row["exit"] for row in statuses))


if __name__ == "__main__":
    raise SystemExit(main())
