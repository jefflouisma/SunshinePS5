"""Command adapters use local synthetic children only; no PS5/device access."""

import asyncio
import hashlib
import importlib.util
import json
import os
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
PROBES = ROOT / "tools/ps5-probes"
sys.path.insert(0, str(PROBES))
SPEC = importlib.util.spec_from_file_location(
    "input_adapters", PROBES / "input_interactive_adapters.py"
)
A = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = A
SPEC.loader.exec_module(A)


def command(code):
    return (sys.executable, "-u", "-c", code)


def producer(fault="", at=3):
    return command(
        "import sys,time,select\n"
        "frame=b'P6\\n2 2\\n255\\n'+bytes(range(12))\n"
        f"fault={fault!r};at={at}\n"
        "for i in range(1000):\n"
        " if fault=='gap' and i==at: time.sleep(2)\n"
        " if fault=='truncated' and i==at:\n"
        "  sys.stdout.buffer.write(frame[:15]);sys.stdout.flush();sys.exit(0)\n"
        " sys.stdout.buffer.write(frame);sys.stdout.flush()\n"
        " if select.select([sys.stdin],[],[],0.02)[0]: sys.exit(0)\n"
    )


def decoder(fail=False):
    return command(
        "import sys\n"
        "data=sys.stdin.buffer.read()\n"
        "assert len(data)%23==0\n"
        "print('frame='+str(len(data)//23))\n"
        "print('progress=end')\n"
        f"sys.exit({int(fail)})\n"
    )


def version():
    return command("print('synthetic command version 1')")


def specs(**changes):
    values = {
        "discovery": {"schema_version": 1, "ready": True, "firmware": A.D.FIRMWARE},
        "services": {"schema_version": 1, "healthy": True},
        "process": {"schema_version": 1, "pid": 123, "absent": True},
        "hdmi": {"schema_version": 1, "signal_present": True},
    }
    values.update(changes)
    return {
        key: command(f"print({json.dumps(value)!r})") for key, value in values.items()
    }


def preflight():
    return {
        "schema_version": 1,
        "observed_ns": time.monotonic_ns(),
        "firmware": A.D.FIRMWARE,
        "t050_cleanup_complete": True,
        "no_active_game_or_probe": True,
        "ipv4_ipv6_isolated": True,
        "exclusive_capture": True,
        "sources": {"preflight": b"synthetic independent preflight"},
    }


def helper(fail=False):
    return command(
        "import sys\n"
        "print('READY version=1 pid=123 firmware=0x13600007 "
        "neutral=accepted next=1 delivery=unverified',flush=True)\n"
        "assert sys.stdin.readline()=='1 neutral\\n'\n"
        "print('ACK seq=1 mask=0x0000 neutral=accepted begin_ms=1 "
        "release_ms=2 end_ms=3',flush=True)\n"
        "assert sys.stdin.readline()=='2 stop\\n'\n"
        "print('DONE code=0 owned=1 neutral_rc=0 remove_rc=0 close_rc=0 "
        "terminate_rc=0 hardware_acceptance=incomplete',flush=True)\n"
        "assert sys.stdin.read()==''\n"
        f"sys.exit({int(fail)})\n"
    ) + ("{elf}",)


def execute(tmp_path, monkeypatch, **options):
    binary = b"synthetic ELF never deploy"
    elf = tmp_path / "fixture.elf"
    elf.write_bytes(binary)
    monkeypatch.setattr(A.D, "ELF_SHA256", hashlib.sha256(binary).hexdigest())
    values = {
        "capture_argv": producer(),
        "decode_argv": decoder(),
        "version_argv": version(),
        "launch_argv": helper(),
        "launch_version_argv": version(),
        "postflight_commands": specs(),
        "device": "synthetic PPM source",
        "width": 2,
        "height": 2,
        "frame_gap_seconds": 0.5,
        "synthetic": True,
        "total_seconds": 5,
    }
    values.update(options)
    return asyncio.run(
        A.run_with_commands(tmp_path / "run", elf, preflight(), **values)
    )


def test_real_children_complete_and_hashes(tmp_path, monkeypatch):
    result = execute(tmp_path, monkeypatch)
    assert result["protocol_complete"], result
    assert result["hardware_acceptance"] == "Incomplete"
    root = tmp_path / "run"
    assert (root / "coordinator/manifest.json").is_file()
    manifest = json.loads((root / "adapter-manifest.json").read_bytes())
    assert manifest["provenance"] == "synthetic_fixture"
    assert manifest["clock_boundary"] == "host_frame_receipt_only"
    for item in manifest["artifacts"]:
        data = (root / item["path"]).read_bytes()
        assert hashlib.sha256(data).hexdigest() == item["sha256"]
    capture = json.loads((root / "coordinator/capture-1-decode.json").read_bytes())
    assert capture["frames"] > 0 and capture["decode_exit"] == 0
    assert capture["frame_receipt_ns"] == sorted(capture["frame_receipt_ns"])
    assert len(capture["capture_argv"]) > 1
    assert result["capture"][0]["end_ns"] == result["capture"][1]["start_ns"]
    assert result["capture"][-1]["end_ns"] > result["postflight_ns"]


@pytest.mark.parametrize("fault", ["gap", "truncated"])
def test_capture_fault_blocks_completion(tmp_path, monkeypatch, fault):
    observation = specs()
    observation["discovery"] = command("import time;time.sleep(2)")
    result = execute(
        tmp_path,
        monkeypatch,
        capture_argv=producer(fault),
        postflight_commands=observation,
    )
    assert not result["protocol_complete"]
    assert not (tmp_path / "run/coordinator/manifest.json").exists()
    assert (tmp_path / "run/adapter-manifest.json").is_file()


@pytest.mark.parametrize("fault", ["decode", "helper", "postflight", "pid"])
def test_faults_fail_closed(tmp_path, monkeypatch, fault):
    options = {
        "decode": {"decode_argv": decoder(True)},
        "helper": {"launch_argv": helper(True)},
        "postflight": {"postflight_commands": specs(services={"healthy": False})},
        "pid": {"postflight_commands": specs(process={"pid": 124, "absent": True})},
    }[fault]
    result = execute(tmp_path, monkeypatch, **options)
    assert not result["protocol_complete"]
    assert not (tmp_path / "run/coordinator/manifest.json").exists()
    if fault == "decode":
        assert not (tmp_path / "run/launch-intent.json").exists()
        evidence = assert_partial_frames(tmp_path / "run", 0)
        first = evidence["batches"][0]
        decoded = json.loads(
            (tmp_path / "run/capture-retained" / first["decode"]).read_bytes()
        )
        assert decoded["status"] == "failed"


@pytest.mark.parametrize(
    "code",
    [
        "import sys;sys.exit(1)",
        "import sys;sys.stdout.buffer.write(b'P6\\n9 9\\n255\\n');sys.stdout.flush()",
        "import time;time.sleep(10)",
    ],
)
def test_capture_unavailable_never_dispatches(tmp_path, monkeypatch, code):
    result = execute(
        tmp_path, monkeypatch, capture_argv=command(code), total_seconds=0.3
    )
    assert not result["protocol_complete"]
    assert not (tmp_path / "run/launch-intent.json").exists()


def test_decoder_count_must_match_every_frame(tmp_path, monkeypatch):
    result = execute(
        tmp_path,
        monkeypatch,
        decode_argv=command(
            "import sys;sys.stdin.buffer.read();print('frame=0\\nprogress=end')"
        ),
    )
    assert not result["protocol_complete"]
    assert not (tmp_path / "run/launch-intent.json").exists()


@pytest.mark.parametrize(
    "raw",
    [
        b'{"schema_version":1,"healthy":true,"healthy":false}',
        b'{"schema_version":true,"healthy":true}',
        b"not-json",
        '{"schema_version":1}'.encode("utf-16"),
    ],
)
def test_postflight_invalid_json_refuses(tmp_path, monkeypatch, raw):
    observation = specs()
    observation["services"] = command(f"import sys;sys.stdout.buffer.write({raw!r})")
    result = execute(tmp_path, monkeypatch, postflight_commands=observation)
    assert not result["protocol_complete"]


def test_cancellation_stops_owned_capture(tmp_path, monkeypatch):
    binary = tmp_path / "fixture.elf"
    binary.write_bytes(b"synthetic")
    monkeypatch.setattr(A.D, "ELF_SHA256", hashlib.sha256(b"synthetic").hexdigest())

    async def scenario():
        task = asyncio.create_task(
            A.run_with_commands(
                tmp_path / "run",
                binary,
                preflight(),
                capture_argv=producer(),
                decode_argv=decoder(),
                version_argv=version(),
                launch_argv=command("import time;time.sleep(10)") + ("{elf}",),
                launch_version_argv=version(),
                postflight_commands=specs(),
                device="synthetic",
                width=2,
                height=2,
                frame_gap_seconds=0.5,
                synthetic=True,
            )
        )
        async with asyncio.timeout(3):
            while not (tmp_path / "run/launch-intent.json").exists():
                await asyncio.sleep(0.01)
        await asyncio.sleep(0.2)
        task.cancel()
        result = await task
        assert not result["protocol_complete"]
        assert "CancelledError" in result["error"]
        closed = json.loads((tmp_path / "run/capture-close.json").read_bytes())
        assert closed["reaped"] is True
        with pytest.raises(ProcessLookupError):
            os.kill(closed["pid"], 0)
        assert_partial_frames(tmp_path / "run", result["dispatch_ns"])

    asyncio.run(scenario())


def assert_partial_frames(root, after_ns):
    outer = json.loads((root / "adapter-manifest.json").read_bytes())
    assert outer["protocol_complete"] is False
    assert outer["hardware_acceptance"] == "Incomplete"
    for item in outer["artifacts"]:
        assert (
            hashlib.sha256((root / item["path"]).read_bytes()).hexdigest()
            == item["sha256"]
        )
    evidence = json.loads((root / "capture-retained/manifest.json").read_bytes())
    assert evidence["status"] == "partial_unverified"
    assert evidence["hardware_acceptance"] == "Incomplete"
    refs = {item["path"]: item["sha256"] for item in evidence["artifacts"]}
    receipts = []
    for item in evidence["batches"]:
        meta = json.loads((root / "capture-retained" / item["metadata"]).read_bytes())
        data = (root / "capture-retained" / item["raw"]).read_bytes()
        assert meta["status"] == "partial_unverified"
        assert meta["sha256"] == refs[item["raw"]] == hashlib.sha256(data).hexdigest()
        assert len(data) == len(meta["frame_receipt_ns"]) * 23
        receipts.extend(meta["frame_receipt_ns"])
    for name, digest in refs.items():
        assert (
            hashlib.sha256((root / "capture-retained" / name).read_bytes()).hexdigest()
            == digest
        )
    assert any(value > after_ns for value in receipts)
    return evidence


@pytest.mark.parametrize(
    "fault", ["helper", "postflight", "gap", "truncated", "timeout", "decoder"]
)
def test_postdispatch_failure_retains_complete_frames(tmp_path, monkeypatch, fault):
    options = {}
    observation = specs()
    if fault == "helper":
        options["launch_argv"] = command(
            "import time,sys;time.sleep(.2);sys.exit(1)"
        ) + ("{elf}",)
    elif fault in ("gap", "truncated", "timeout"):
        options["capture_argv"] = producer(fault, at=10)
        options["launch_argv"] = command("import time;time.sleep(10)") + ("{elf}",)
        if fault == "timeout":
            options["total_seconds"] = 0.4
    else:
        # This is the reviewer's failed-services reproduction, retaining frames
        # collected during the failing command rather than just pre-dispatch.
        observation["services"] = command(
            "import time,sys;time.sleep(.2);"
            + (
                "sys.exit(1)"
                if fault == "postflight"
                else 'print(\'{"schema_version":1,"healthy":true}\')'
            )
        )
        if fault == "decoder":
            marker = tmp_path / "decode-marker"
            options["decode_argv"] = command(
                "import pathlib,sys\n"
                f"marker=pathlib.Path({str(marker)!r})\n"
                "data=sys.stdin.buffer.read()\n"
                "failed=marker.exists();marker.touch()\n"
                "print('frame='+str(len(data)//23));print('progress=end')\n"
                "sys.exit(int(failed))\n"
            )
    result = execute(tmp_path, monkeypatch, postflight_commands=observation, **options)
    assert not result["protocol_complete"]
    assert not (tmp_path / "run/coordinator/manifest.json").exists()
    evidence = assert_partial_frames(tmp_path / "run", result["dispatch_ns"])
    assert evidence["batches"]
    if fault == "decoder":
        reports = [
            json.loads(
                (tmp_path / "run/capture-retained" / item["decode"]).read_bytes()
            )
            for item in evidence["batches"]
        ]
        assert any(report["status"] == "failed" for report in reports)


@pytest.mark.parametrize("fault", ["raw", "metadata", "quota", "publication"])
def test_retention_faults_cannot_skip_capture_reaping(tmp_path, monkeypatch, fault):
    original_write = A.D.Artifacts.write
    original_publish = A.publish_json
    original_retain = A.PpmCapture.retain_batch

    def write(self, name, data):
        if (
            self.root.name == "capture-retained"
            and name == ("batch-1.ppm" if fault == "raw" else "batch-1.json")
            and fault in ("raw", "metadata")
        ):
            raise OSError("injected retention storage failure")
        return original_write(self, name, data)

    def publish(artifacts, name, value):
        if fault == "publication" and artifacts.root.name == "capture-retained":
            raise OSError("injected capture manifest publication failure")
        return original_publish(artifacts, name, value)

    def retain(self, frames, origin):
        if fault == "quota" and origin == "final_buffer":
            self.retained.remaining = 1
        return original_retain(self, frames, origin)

    monkeypatch.setattr(A.D.Artifacts, "write", write)
    monkeypatch.setattr(A, "publish_json", publish)
    monkeypatch.setattr(A.PpmCapture, "retain_batch", retain)
    result = execute(
        tmp_path,
        monkeypatch,
        launch_argv=command("import time,sys;time.sleep(.2);sys.exit(1)") + ("{elf}",),
    )
    assert not result["protocol_complete"]
    assert result["capture_retention_errors"]
    root = tmp_path / "run"
    outer = json.loads((root / "adapter-manifest.json").read_bytes())
    assert outer["capture_retention_complete"] is False
    assert outer["capture_retention_errors"]
    for item in outer["artifacts"]:
        assert (
            hashlib.sha256((root / item["path"]).read_bytes()).hexdigest()
            == item["sha256"]
        )
    closed = json.loads((root / "capture-close.json").read_bytes())
    assert closed["reaped"] is True
    with pytest.raises(ProcessLookupError):
        os.kill(closed["pid"], 0)
    if fault == "metadata":
        assert (root / "capture-retained/batch-1.ppm").stat().st_size > 0
        assert any(
            item["path"] == "capture-retained/batch-1.ppm"
            for item in outer["artifacts"]
        )


def test_partial_decode_timeout_keeps_bytes_and_reaps(tmp_path, monkeypatch):
    marker = tmp_path / "decode-marker"
    arguments = command(
        "import pathlib,sys,time\n"
        f"marker=pathlib.Path({str(marker)!r})\n"
        "data=sys.stdin.buffer.read()\n"
        "if marker.exists(): time.sleep(10)\n"
        "marker.touch();print('frame='+str(len(data)//23));print('progress=end')\n"
    )
    started = time.monotonic()
    result = execute(
        tmp_path,
        monkeypatch,
        decode_argv=arguments,
        launch_argv=command("import time,sys;time.sleep(.2);sys.exit(1)") + ("{elf}",),
    )
    assert time.monotonic() - started < 4
    assert not result["protocol_complete"]
    evidence = assert_partial_frames(tmp_path / "run", result["dispatch_ns"])
    final = json.loads(
        (
            tmp_path / "run/capture-retained" / evidence["batches"][-1]["decode"]
        ).read_bytes()
    )
    assert final["status"] == "failed" and final["error"] == "TimeoutError"
    command_record = json.loads(
        (tmp_path / "run" / final["command_record"]).read_bytes()
    )
    assert command_record["cleanup"]["reaped"] is True
    with pytest.raises(ProcessLookupError):
        os.kill(command_record["pid"], 0)


def test_top_level_publication_failure_follows_cleanup(tmp_path, monkeypatch):
    original = A.publish_json

    def fail(artifacts, name, value):
        if name == "adapter-manifest.json":
            raise OSError("injected outer publication failure")
        return original(artifacts, name, value)

    monkeypatch.setattr(A, "publish_json", fail)
    with pytest.raises(OSError, match="outer publication"):
        execute(tmp_path, monkeypatch)
    closed = json.loads((tmp_path / "run/capture-close.json").read_bytes())
    assert closed["reaped"] is True
    assert (tmp_path / "run/capture-retained/manifest.json").is_file()
    with pytest.raises(ProcessLookupError):
        os.kill(closed["pid"], 0)


def test_disabled_and_reused_directory(tmp_path, monkeypatch):
    with pytest.raises(ValueError, match="disabled"):
        execute(tmp_path, monkeypatch, synthetic=False)
    assert not (tmp_path / "run").exists()
    assert execute(tmp_path, monkeypatch)["protocol_complete"]
    with pytest.raises(FileExistsError):
        execute(tmp_path, monkeypatch)


@pytest.mark.parametrize("mode", ["timeout", "output"])
def test_command_bounds_and_reaping(tmp_path, mode):
    async def scenario():
        artifacts = A.D.Artifacts(tmp_path / "commands", 1024 * 1024)
        runner = A.Commands(artifacts, timeout=0.2)
        code = (
            "import time;print('started',flush=True);time.sleep(10)"
            if mode == "timeout"
            else "import sys;sys.stdout.write('x'*100000);sys.stdout.flush()"
        )
        with pytest.raises((ValueError, TimeoutError)):
            await runner.run(mode, command(code))
        record = json.loads((artifacts.root / f"{mode}.json").read_bytes())
        assert record["cleanup"]["reaped"] is True
        with pytest.raises(ProcessLookupError):
            os.kill(record["pid"], 0)
        artifacts.close()

    asyncio.run(scenario())


def test_avfoundation_argv_is_literal():
    args = A.avfoundation_argv("/usr/bin/ffmpeg", "Elgato 4K X;$(echo bad)")
    assert "Elgato 4K X;$(echo bad):none" in args
    assert args[-1] == "pipe:1"
    assert "-nostdin" not in args
    assert A.decoder_argv("/usr/bin/ffmpeg")[-1] == "-"


def test_no_input_command_accepts_already_exited_child(tmp_path, monkeypatch):
    original = A.spawn
    signals = []
    monkeypatch.setattr(A.D.os, "killpg", lambda pid, sig: signals.append((pid, sig)))

    async def already_exited(arguments):
        process = await original(arguments)
        assert await process.wait() == 0
        return process

    monkeypatch.setattr(A, "spawn", already_exited)

    async def scenario():
        artifacts = A.D.Artifacts(tmp_path / "commands", 1024 * 1024)
        try:
            output = await A.Commands(artifacts).run("version", version())
            assert output == b"synthetic command version 1\n"
            assert signals == []
        finally:
            artifacts.close()

    asyncio.run(scenario())
