"""Synthetic long-session, overflow, concurrent-reader and no-browser integration.

The C fixture advances >600 seconds without wall-time sleeps and uses the real
metrics engine. The management fixture checks durable toggles and no-browser
collection separately; neither fixture establishes console acceptance.
"""

import argparse
import json
import os
import selectors
import shutil
import subprocess
import tempfile
import time
import urllib.request
from pathlib import Path


def require(condition, detail):
    if not condition:
        raise AssertionError(detail)


def fetch(url, path, body=None, token=None):
    headers = {}
    data = None
    if body is not None:
        headers = {
            "Content-Type": "application/json",
            "Origin": url,
            "X-SP5-Token": token,
        }
        data = json.dumps(body).encode()
    request = urllib.request.Request(
        url + "/api/v1" + path,
        headers=headers,
        data=data,
        method="PATCH" if body is not None else "GET",
    )
    with urllib.request.urlopen(request, timeout=5) as response:
        return json.load(response)


def start_host(binary, state, assets):
    process = subprocess.Popen(
        [str(binary), str(state), str(assets), "ok"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    try:
        require(selector.select(10), "management fixture startup timeout")
        url = process.stdout.readline().strip()
        require(url.startswith("http://127.0.0.1:"), "fixture startup failed")
        return process, url
    except BaseException:
        process.kill()
        process.wait(timeout=5)
        raise
    finally:
        selector.close()


def stop_host(process):
    if process.poll() is None:
        process.terminate()
    _, error = process.communicate(timeout=10)
    require(process.returncode == 0 and not error, error or "fixture exited with error")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--management-bin", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = subprocess.run(
        [str(args.fixture.resolve())],
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )
    require(result.returncode == 0 and not result.stderr, result.stderr)
    observations = [json.loads(line) for line in result.stdout.splitlines()]
    require(len(observations) == 4, "missing virtual-time cases")
    cases = {item["case"]: item["snapshot"] for item in observations}
    long = cases["long_gap_overflow"]
    require(long["published_us"] == 701000000, "long session was not simulated")
    require(long["retained"]["frames"]["records"] == 552, "raw expiry population")
    require(long["summaries"][0]["population"]["seen"] == 651, "whole totals lost")
    require(long["summaries"][0]["p99"]["value_us"] is None, "overflow became a value")
    require(long["coverage"]["gaps"], "disabled gap disappeared")
    scratch = Path(os.environ.get("TMPDIR", "/Volumes/4TB_Drive/tmp"))
    directory = Path(tempfile.mkdtemp(prefix="us4-retention-", dir=scratch))
    directory.chmod(0o700)
    process = None
    try:
        assets = Path(__file__).resolve().parents[2] / "webapp"
        process, url = start_host(args.management_bin.resolve(), directory, assets)
        initial = fetch(url, "/metrics")
        require(not initial["requested"] and not initial["effective"], "default was on")
        token = fetch(url, "/page")["token"]
        fetch(url, "/settings", {"metrics_enabled": True}, token)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            on = fetch(url, "/metrics")
            if on["effective"]:
                break
            time.sleep(0.02)
        require(on["effective"], "two-second application deadline")
        session = on["capture"]["session_id"]
        first = on["capture"]["continuous"]["frame_outcomes"][3]
        # There is no browser process. Worker/collection must keep progressing.
        time.sleep(0.25)
        later = fetch(url, "/metrics")
        require(
            later["capture"]["continuous"]["frame_outcomes"][3] > first,
            "no-browser worker",
        )
        require(later["capture"]["session_id"] == session, "toggle interrupted session")
        stop_host(process)
        process = None
        process, url = start_host(args.management_bin.resolve(), directory, assets)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            restored = fetch(url, "/metrics")
            if restored["effective"]:
                break
            time.sleep(0.02)
        require(
            restored["requested"] and restored["effective"],
            "persisted toggle not applied",
        )
        summary = {
            "synthetic": True,
            "hardware_evidence": False,
            "virtual_cases": observations,
            "no_browser_progress": True,
            "persistent_toggle": True,
            "status": "Pass",
        }
        args.output.write_text(json.dumps(summary, indent=2) + "\n")
    finally:
        if process is not None:
            stop_host(process)
        shutil.rmtree(directory)


if __name__ == "__main__":
    main()
