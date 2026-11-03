"""Bounded local parser campaign; only synthetic corpus files enter artifacts."""

import argparse
import json
import re
import shutil
import subprocess
from pathlib import Path

# Maximum libFuzzer input per target; each harness bounds its own INPUT_LIMIT and
# replay.c reads one byte more. Record-stream harnesses need multi-request inputs.
PARSERS = {"http": 32768, "rtsp": 32768, "control": 16384, "input": 16384}
ROOT = Path(__file__).resolve().parents[2]


def declared_targets(root=ROOT):
    """Every place that names a fuzz target; a new target must appear in all."""
    fuzz = root / "tests" / "fuzz"
    cmake = re.search(
        r"foreach\(parser IN ITEMS ([^)]*)\)", (fuzz / "CMakeLists.txt").read_text()
    )
    workflow = re.search(
        r"^\s*parser: \[([^\]]*)\]",
        (root / ".github" / "workflows" / "fuzz.yml").read_text(),
        re.MULTILINE,
    )
    if cmake is None or workflow is None:
        raise ValueError("fuzz target list not found in CMakeLists.txt or fuzz.yml")
    return {
        "cmake": sorted(cmake.group(1).split()),
        "run.py": sorted(PARSERS),
        "fuzz.yml": sorted(item.strip() for item in workflow.group(1).split(",")),
        "sources": sorted(
            path.stem for path in fuzz.glob("*.c") if path.name != "replay.c"
        ),
        "corpora": sorted(
            path.name
            for path in (fuzz / "corpus").iterdir()
            if path.is_dir() and any(item.is_file() for item in path.iterdir())
        ),
    }


def check_targets(root=ROOT):
    lists = declared_targets(root)
    expected = lists["cmake"]
    mismatched = {name: value for name, value in lists.items() if value != expected}
    print(json.dumps({"targets": expected, "mismatched": mismatched}))
    return 1 if mismatched or not expected else 0


def run(arguments):
    destination = arguments.artifacts.resolve()
    destination.mkdir(parents=True, exist_ok=False)
    corpus = destination / "corpus"
    shutil.copytree(arguments.corpus, corpus)
    seeds = [path for path in corpus.iterdir() if path.is_file()]
    if not seeds:
        raise ValueError("a nonempty reviewed seed corpus is required")
    executable = arguments.build / f"{arguments.parser}_fuzz"
    replay = arguments.build / f"{arguments.parser}_replay"
    if not executable.is_file() or not replay.is_file():
        raise ValueError("both fuzz and sanitizer-replay targets are required")
    command = [
        str(executable.resolve()),
        str(corpus),
        f"-max_total_time={arguments.seconds}",
        f"-max_len={PARSERS[arguments.parser]}",
        "-timeout=5",
        "-rss_limit_mb=1024",
        "-error_exitcode=77",
        f"-artifact_prefix={destination}/",
        "-print_final_stats=1",
    ]
    with (destination / "campaign.log").open("w") as log:
        result = subprocess.run(
            command,
            stdout=log,
            stderr=subprocess.STDOUT,
            timeout=arguments.seconds + 90,
            check=False,
        )
    artifacts = sorted(
        path
        for path in destination.iterdir()
        if path.name.startswith(("crash-", "timeout-", "oom-", "leak-"))
    )
    status = result.returncode
    reproductions = []
    for artifact in artifacts:
        minimized = destination / f"minimized-{artifact.name}"
        with (destination / f"minimize-{artifact.name}.log").open("w") as log:
            subprocess.run(
                [
                    str(executable.resolve()),
                    "-minimize_crash=1",
                    "-max_total_time=15",
                    f"-exact_artifact_path={minimized}",
                    str(artifact),
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=45,
                check=False,
            )
        reproduction = minimized if minimized.is_file() else artifact
        with (destination / f"replay-{artifact.name}.log").open("w") as log:
            observed = subprocess.run(
                [str(replay.resolve()), str(reproduction)],
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=30,
                check=False,
            )
        reproductions.append(
            {"artifact": reproduction.name, "exit": observed.returncode}
        )
    # Replay every retained mutation, not just the seeds, under the build's sanitizers.
    files = sorted(path for path in corpus.iterdir() if path.is_file())
    for offset in range(0, len(files), 64):
        with (destination / "corpus-replay.log").open("a") as log:
            observed = subprocess.run(
                [
                    str(replay.resolve()),
                    *(str(path) for path in files[offset : offset + 64]),
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=60,
                check=False,
            )
        if observed.returncode != 0:
            status = observed.returncode
            break
    summary = {
        "parser": arguments.parser,
        "synthetic": True,
        "hardware_evidence": False,
        "requested_seconds": arguments.seconds,
        "command": command,
        "campaign_exit": result.returncode,
        "retained_corpus_files": len(files),
        "reproductions": reproductions,
        "status": "Pass" if status == 0 and not artifacts else "Fail",
    }
    (destination / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))
    return 0 if summary["status"] == "Pass" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check-targets",
        action="store_true",
        help="fail unless CMake, run.py, fuzz.yml, sources and corpora agree",
    )
    parser.add_argument("--parser", choices=PARSERS)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--corpus", type=Path)
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--seconds", type=int)
    arguments = parser.parse_args()
    if arguments.check_targets:
        return check_targets()
    required = ("parser", "build", "corpus", "artifacts", "seconds")
    missing = [name for name in required if getattr(arguments, name) is None]
    if missing:
        parser.error("missing " + ", ".join(f"--{name}" for name in missing))
    if not 1 <= arguments.seconds <= 900:
        parser.error("--seconds must be between 1 and 900")
    return run(arguments)


if __name__ == "__main__":
    raise SystemExit(main())
