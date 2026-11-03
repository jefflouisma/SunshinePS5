#!/usr/bin/env bash
# Audit the two authoritative locks; never updates them or executes archive contents.
# Requires Python >=3.11, git, rustup and the client's pinned Rust toolchain; network required.
# Exit 0: all scoped checks completed without findings; 1: findings; 2: incomplete
# (known findings remain reported). Licensing is excluded by owner instruction, not passed.
# Optional AUDIT_REPORT_PATH writes JSON; TMPDIR controls all downloaded/scratch data.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ -z ${TMPDIR:-} ]]; then
    if [[ -n ${RUNNER_TEMP:-} ]]; then
        export TMPDIR=$RUNNER_TEMP
    elif [[ -d /Volumes/4TB_Drive/tmp ]]; then
        export TMPDIR=/Volumes/4TB_Drive/tmp
    else
        export TMPDIR=${TMP:-${TEMP:-/tmp}}
    fi
fi
mkdir -p "$TMPDIR"
exec "${PYTHON:-python3}" -u - "$root" <<'PYTHON'
import concurrent.futures
import datetime
import hashlib
import http.client
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import tomllib
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(sys.argv[1])
OSV = "https://api.osv.dev/v1/"
REPORT = {
    "schema_version": 1,
    "started_at": datetime.datetime.now(datetime.UTC).isoformat(),
    "inputs": {},
    "licensing": {
        "status": "excluded",
        "reason": "Owner excluded licensing work; no license audit or license gate.",
    },
    "coverage": [
        "Host: all source/release archives in cmake/dependencies.lock.json; "
        "SHA-256/size against lock, git tag pins.",
        "Client: entire Cargo.lock graph, all platforms; crates.io archive "
        "SHA-256 against lock; locked Cargo resolution.",
        "Known vulnerabilities: live OSV GIT tags/commits and crates.io versions, "
        "including RustSec records imported by OSV.",
        "OSV coverage is not universal: no match does not establish that a "
        "project/version is vulnerability-free.",
        "Locked hashes identify bytes; absent publisher signatures/digests "
        "do not establish publisher authentication.",
        "Installed tool trees, PS5 builds/runtime, and external native client "
        "prerequisites are not authenticated by this audit.",
    ],
    "checks": [],
    "advisories": [],
}


class Finding(Exception):
    pass


def record(status, name, detail):
    REPORT["checks"].append({"status": status, "name": name, "detail": str(detail)})
    print(f"{status.upper()}: {name}: {detail}", flush=True)


def check(name, function, *args):
    try:
        detail = function(*args)
    except Finding as error:
        record("fail", name, error)
    except (
        OSError,
        ValueError,
        KeyError,
        TypeError,
        http.client.HTTPException,
        subprocess.SubprocessError,
    ) as error:
        record("incomplete", name, error)
    else:
        record("pass", name, detail)


def request(url, data=None):
    if urllib.parse.urlparse(url).scheme != "https":
        raise ValueError(f"HTTPS required: {url}")
    payload = None if data is None else json.dumps(data).encode()
    headers = {
        "User-Agent": "SunshinePS5-dependency-audit/1",
        "Content-Type": "application/json",
    }
    for attempt in range(3):
        try:
            response = urllib.request.urlopen(
                urllib.request.Request(url, data=payload, headers=headers), timeout=90
            )
            if urllib.parse.urlparse(response.url).scheme != "https":
                response.close()
                raise ValueError("HTTPS downgrade rejected")
            return response
        except urllib.error.HTTPError as error:
            if error.code not in (429, 500, 502, 503, 504) or attempt == 2:
                raise
        except (urllib.error.URLError, TimeoutError):
            if attempt == 2:
                raise
        time.sleep(2**attempt)
    raise ValueError("Request did not complete")


def json_request(url, data):
    with request(url, data) as response:
        return json.load(response)


def load(relative, parser):
    raw = (ROOT / relative).read_bytes()
    REPORT["inputs"][relative] = hashlib.sha256(raw).hexdigest()
    return parser(raw.decode())


def require_pin(value, pattern, description):
    if not isinstance(value, str) or re.fullmatch(pattern, value) is None:
        raise ValueError(f"Missing/invalid {description}: {value!r}")
    return value


def archive_integrity(url, digest, size=None):
    require_pin(digest, r"[0-9a-f]{64}", "SHA-256 pin")
    if size is not None and (type(size) is not int or size <= 0):
        raise ValueError("Archive size must be a positive integer")
    # Each untrusted download gets its own new empty directory; do not extract it.
    with tempfile.TemporaryDirectory(prefix="sunshine-t011-archive-") as directory:
        actual = hashlib.sha256()
        total = 0
        with (
            request(url) as response,
            (Path(directory) / "download").open("wb") as output,
        ):
            while block := response.read(1024 * 1024):
                total += len(block)
                if size is not None and total > size:
                    raise Finding(f"Archive exceeds locked size {size}: {url}")
                actual.update(block)
                output.write(block)
        if size is not None and total != size:
            raise Finding(f"Size mismatch: expected {size}, got {total}: {url}")
        if actual.hexdigest() != digest:
            raise Finding(
                f"SHA-256 mismatch: expected {digest}, got {actual.hexdigest()}: {url}"
            )
    return f"{total} bytes; SHA-256 {digest} matches lock"


def host_archive(archive):
    verification = archive["verification"]
    if verification["status"] not in (
        "publisher_digest_matched",
        "downloaded_and_hashed",
    ):
        raise ValueError(f"Unverified archive: {verification}")
    publisher = verification.get("publisher_digest")
    if publisher is not None and publisher != "sha256:" + archive["sha256"]:
        raise Finding("Recorded publisher digest disagrees with locked SHA-256")
    if verification["status"] == "publisher_digest_matched" and publisher is None:
        raise ValueError("Publisher verification claimed without publisher digest")
    result = archive_integrity(archive["url"], archive["sha256"], archive["size_bytes"])
    return (
        result
        + "; publisher signature verified in lock: "
        + str(verification.get("signature_verified", False))
    )


def git_pin(repository, tag, expected=None):
    if not repository.startswith("https://"):
        raise ValueError("Git repository must use HTTPS")
    require_pin(tag, r"[A-Za-z0-9][A-Za-z0-9._-]*", "tag")
    result = subprocess.run(
        [
            "git",
            "ls-remote",
            "--exit-code",
            repository,
            f"refs/tags/{tag}",
            f"refs/tags/{tag}^{{}}",
        ],
        check=True,
        capture_output=True,
        text=True,
        timeout=120,
    )
    refs = dict(line.split()[::-1] for line in result.stdout.splitlines())
    resolved = refs.get(f"refs/tags/{tag}^{{}}", refs.get(f"refs/tags/{tag}"))
    require_pin(resolved, r"[0-9a-f]{40}", "resolved commit")
    if expected is not None:
        require_pin(expected, r"[0-9a-f]{40}", "source commit")
        if expected != resolved:
            raise Finding(f"Tag {tag} now resolves to {resolved}, expected {expected}")
    return f"{repository} {tag} -> {resolved}"


def host_inventory():
    lock = load("cmake/dependencies.lock.json", json.loads)
    if lock["schema_version"] != 1:
        raise ValueError("Unsupported host lock schema")
    dependencies = lock["dependencies"]
    if not {"ps5_payload_sdk", "llvm", "openssl"} <= dependencies.keys():
        raise ValueError("Required host dependency missing from lock")
    system = lock["build_system"]
    if system["name"] != "CMake":
        raise ValueError("Unsupported build system")
    entries = {
        "cmake": {
            **system,
            "repository": "https://github.com/Kitware/CMake",
            "tag": "v" + system["version"],
        },
        **dependencies,
    }
    queries = []
    for name, entry in entries.items():
        if name == "enet":
            # This reviewed fork is commit-pinned, with no upstream release tag.
            # Keep its local version verbatim; never query OSV for a fictional tag.
            version = require_pin(
                entry["version"], r"1\.3\.17-moonlight-[0-9a-f]{7}", "enet exact version"
            )
            commit = require_pin(entry.get("source_commit"), r"[0-9a-f]{40}", "enet commit")
            if (
                entry["repository"] != "https://github.com/cgutman/enet"
                or version != "1.3.17-moonlight-" + commit[:7]
                or "tag" in entry
                or entry["source_archive"]["url"]
                != "https://codeload.github.com/cgutman/enet/tar.gz/" + commit
            ):
                raise ValueError("enet: local version, repository and commit archive disagree")
            tag = None
            record(
                "pass",
                f"host enet@{version} source pin",
                f"{entry['repository']} commit {commit}; local version {version}; "
                "commit-only pin, no release tag asserted; archive integrity checked separately",
            )
        else:
            version = require_pin(
                entry["version"], r"\d+\.\d+(?:\.\d+)?", f"{name} exact version"
            )
            tag = entry["tag"]
            if version not in tag:
                raise ValueError(f"{name}: version and tag disagree")
            check(
                f"host {name}@{version} git pin",
                git_pin,
                entry["repository"],
                tag,
                entry.get("source_commit"),
            )
        if name != "cmake" and not entry.get("source_commit"):
            record("incomplete", f"host {name} pin", "Source commit is missing")
        archives = [
            (key, value) for key, value in entry.items() if key.endswith("_archive")
        ]
        if not archives or "source_archive" not in entry:
            record("incomplete", f"host {name} integrity", "Source archive pin missing")
        for key, archive in archives:
            check(f"host {name}@{version} {key}", host_archive, archive)
        provenance = entry.get("local_observation", {}).get("archive_provenance")
        if provenance:
            print(
                f"SCOPE: host {name} installed-tool provenance remains "
                f"{provenance['status']}: {provenance.get('reason', '')}"
            )
        label = f"host {name}@{version}"
        if tag is not None:
            queries.append(
                (
                    label + " tag",
                    {
                        "package": {"name": entry["repository"], "ecosystem": "GIT"},
                        "version": tag,
                    },
                )
            )
        if entry.get("source_commit"):
            queries.append((label + " commit", {"commit": entry["source_commit"]}))
    return queries


def cargo_resolution(toolchain):
    with tempfile.TemporaryDirectory(prefix="sunshine-t011-cargo-") as directory:
        result = subprocess.run(
            [
                "rustup",
                "run",
                toolchain,
                "cargo",
                "metadata",
                "--locked",
                "--format-version",
                "1",
                "--manifest-path",
                str(ROOT / "tools/moonlight-mcp/Cargo.toml"),
            ],
            capture_output=True,
            text=True,
            timeout=600,
            env={**os.environ, "CARGO_TARGET_DIR": directory},
        )
    if result.returncode:
        raise ValueError(
            f"Locked Cargo metadata failed ({result.returncode}): {result.stderr}"
        )
    packages = json.loads(result.stdout)["packages"]
    if not packages:
        raise ValueError("Cargo resolved an empty dependency graph")
    return (
        f"{toolchain}; {len(packages)} resolved packages; "
        "--locked succeeded (no native build)"
    )


def client_inventory():
    load("tools/moonlight-mcp/Cargo.toml", tomllib.loads)
    lock = load("tools/moonlight-mcp/Cargo.lock", tomllib.loads)
    if lock["version"] != 4:
        raise ValueError("Unsupported Cargo lock version")
    toolchain = load("tools/moonlight-mcp/rust-toolchain.toml", tomllib.loads)[
        "toolchain"
    ]["channel"]
    require_pin(toolchain, r"\d+\.\d+\.\d+", "Rust toolchain")
    check("client locked manifest/graph", cargo_resolution, toolchain)
    crates = []
    queries = []
    local = []
    for package in lock["package"]:
        name = require_pin(package["name"], r"[A-Za-z0-9_-]+", "crate name")
        version = require_pin(
            package["version"],
            r"\d+\.\d+\.\d+(?:[-+][A-Za-z0-9.+-]+)?",
            "crate version",
        )
        label = f"client {name}@{version}"
        source = package.get("source")
        if source is None:
            local.append(name)
            continue
        if source != "registry+https://github.com/rust-lang/crates.io-index":
            record(
                "incomplete",
                label,
                f"Unsupported source needs integrity/vulnerability support: {source}",
            )
            continue
        crates.append((label, name, version, package.get("checksum")))
        queries.append(
            (
                label,
                {
                    "package": {"name": name, "ecosystem": "crates.io"},
                    "version": version,
                },
            )
        )
    if local != ["moonlight-mcp"] or not crates:
        raise ValueError(
            f"Unexpected local packages or empty dependency graph: {local}"
        )
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as executor:
        futures = [
            (
                label,
                executor.submit(
                    archive_integrity,
                    f"https://static.crates.io/crates/{name}/{name}-{version}.crate",
                    checksum,
                ),
            )
            for label, name, version, checksum in crates
        ]
        for label, future in futures:
            check(label + " integrity", future.result)
    return queries


def vulnerabilities(queries):
    if not queries:
        raise ValueError("No vulnerability queries were generated")
    # OSV API contract: https://google.github.io/osv.dev/post-v1-querybatch/
    # Follow per-query pagination; a partial response must never count as clean.
    pending = [(label, query, set()) for label, query in queries]
    matches = {label: set() for label, _ in queries}
    while pending:
        batch, pending = pending[:100], pending[100:]
        response = json_request(
            OSV + "querybatch", {"queries": [query for _, query, _ in batch]}
        )
        results = response["results"]
        if len(results) != len(batch):
            raise ValueError("OSV returned the wrong number of results")
        for (label, query, seen), result in zip(batch, results, strict=True):
            if not isinstance(result, dict) or set(result) - {
                "vulns",
                "next_page_token",
            }:
                raise ValueError(f"Invalid OSV response for {label}: {result}")
            for advisory in result.get("vulns", []):
                identifier = require_pin(
                    advisory["id"], r"[A-Za-z0-9_.-]+", "OSV advisory ID"
                )
                if identifier not in matches[label]:
                    matches[label].add(identifier)
                    REPORT["advisories"].append({"dependency": label, **advisory})
                    record(
                        "fail",
                        label + " advisory",
                        f"{identifier} https://osv.dev/vulnerability/{identifier}",
                    )
            token = result.get("next_page_token")
            if token:
                if token in seen:
                    raise ValueError(f"Repeated OSV page token for {label}")
                pending.append((label, {**query, "page_token": token}, seen | {token}))
            else:
                record(
                    "pass",
                    label + " OSV query",
                    f"Completed; {len(matches[label])} matching advisory IDs "
                    "(database coverage only)",
                )
    return (
        f"{len(queries)} complete live queries; "
        f"{len(REPORT['advisories'])} dependency/advisory matches"
    )


def unchanged_input(relative, digest):
    if hashlib.sha256((ROOT / relative).read_bytes()).hexdigest() != digest:
        raise ValueError("Input changed during audit; rerun required")
    return "Input SHA-256 unchanged during audit"


def main():
    print(REPORT["licensing"]["reason"])
    for scope in REPORT["coverage"]:
        print("SCOPE:", scope)
    # Audit both scopes even when one fails; do not silently drop missing inputs.
    for name, inventory in (("host", host_inventory), ("client", client_inventory)):
        try:
            queries = inventory()
        except (
            OSError,
            ValueError,
            KeyError,
            TypeError,
            subprocess.SubprocessError,
        ) as error:
            record("incomplete", name + " inventory", error)
        else:
            check(name + " known vulnerabilities", vulnerabilities, queries)
    # Detect concurrent lock changes rather than mix evidence from different inputs.
    for relative, digest in REPORT["inputs"].items():
        check(relative + " snapshot", unchanged_input, relative, digest)
    statuses = {entry["status"] for entry in REPORT["checks"]}
    code = 2 if "incomplete" in statuses else 1 if "fail" in statuses else 0
    REPORT["status"] = {0: "pass", 1: "fail", 2: "incomplete"}[code]
    REPORT["exit_code"] = code
    REPORT["finished_at"] = datetime.datetime.now(datetime.UTC).isoformat()
    path = os.environ.get("AUDIT_REPORT_PATH")
    if path:
        Path(path).write_text(json.dumps(REPORT, indent=2) + "\n")
        print("JSON report:", path)
    print(
        f"Audit status: {REPORT['status']}; exit {code}; "
        f"{len(REPORT['advisories'])} advisory matches; "
        "licensing EXCLUDED, never passed"
    )
    return code


if __name__ == "__main__":
    sys.exit(main())
PYTHON
