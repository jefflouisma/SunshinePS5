#!/usr/bin/env python3
"""Fail-closed release evidence index (exit 0 Pass, 1 Fail, 2 Incomplete).

Run from the repository root with a manifest and optional --evidence-root DIR.
Evidence references name local JSON attestations with schema_version, subject,
verdict, category, source and details. Details must identify the scenario,
invocation, observable and underlying captured artifacts for human review.
Hashes bind bytes, not measurement truth: human review remains a required gate.
This checker does not substitute for the acceptance report evaluators.
"""

import argparse
import hashlib
import json
import re
from pathlib import Path, PurePosixPath

LIMIT = 1024 * 1024
STATUSES = {"Pass", "Fail", "Incomplete"}
REQUIRED = {
    **{f"SC-{n:03}": ("acceptance", "hardware") for n in range(1, 15)},
    **{f"US{n}": ("acceptance", "hardware") for n in range(1, 6)},
    "CI-format-lint": ("ci",),
    "CI-build-linux-macos-ps5-client": ("ci",),
    "CI-unit-e2e": ("ci",),
    "CI-asan-ubsan": ("ci",),
    "CI-fuzz-pr-nightly": ("ci",),
    "CI-license-dependency-audit": ("ci",),
    "CONST-I-official-client-matrix": ("acceptance", "hardware"),
    "CONST-II-hotpaths-hardware-encode-input-isolation": ("hardware",),
    "CONST-II-pipeline-stage-timing-regression": ("hardware",),
    "CONST-III-cpu-memory-budgets-fixed-pools": ("hardware",),
    "CONST-III-termination-input-release-resource-cleanup": ("hardware",),
    "CONST-III-firmware-privilege-hooks-restored": ("hardware",),
    "CONST-IV-local-network-crypto-device-identity": ("ci", "hardware"),
    "CONST-V-portable-core-platform-verification": ("ci", "hardware"),
    "CONST-V-independent-reference-client": ("acceptance", "hardware"),
    "CONST-VI-source-audit-no-placeholders-truthful-docs": ("acceptance",),
    "RELEASE-open-toolchain-pinned-reproducible-build": ("ci",),
    "RELEASE-no-prohibited-content-functionality": ("acceptance",),
    "RELEASE-two-hour-on-device-soak": ("hardware",),
    "RELEASE-semver-changelog-tag-checksums-notes-matrices": ("acceptance",),
    "RELEASE-human-maintainer-review": ("acceptance",),
}
SOURCES = {
    "ci": {"ci", "fake-platform", "synthetic"},
    "acceptance": {"physical-console", "official-client", "operator"},
    "hardware": {"physical-console"},
}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path):
    with path.open("rb") as stream:
        raw = stream.read(LIMIT + 1)
    if not raw or len(raw) > LIMIT:
        raise ValueError("JSON artifact must contain 1..1048576 bytes")
    return json.loads(raw, object_pairs_hook=unique_object), raw


def fields(value, expected, context):
    if not isinstance(value, dict) or set(value) != set(expected):
        raise ValueError(f"{context}: expected exactly {', '.join(expected)}")


def nonempty(value):
    return isinstance(value, str) and bool(value.strip())


def evidence(ref, root, identifier, status):
    fields(ref, ("path", "sha256"), "evidence reference")
    name = ref["path"]
    if (
        not nonempty(name)
        or "\\" in name
        or ":" in name
        or any(part in {"", ".", ".."} for part in name.split("/"))
        or PurePosixPath(name).is_absolute()
    ):
        raise ValueError("unsafe evidence path")
    if not isinstance(ref["sha256"], str) or not re.fullmatch(
        r"[0-9a-f]{64}", ref["sha256"]
    ):
        raise ValueError("invalid SHA-256")
    path = root
    for component in name.split("/"):
        path = path / component
        if path.is_symlink():
            raise ValueError("symlink evidence path")
    if not path.resolve().is_relative_to(root) or not path.is_file():
        raise ValueError("missing or escaped evidence file")
    payload, raw = read_json(path)
    if hashlib.sha256(raw).hexdigest() != ref["sha256"]:
        raise ValueError("evidence checksum mismatch")
    fields(
        payload,
        ("schema_version", "subject", "verdict", "category", "source", "details"),
        "attestation",
    )
    if type(payload["schema_version"]) is not int or payload["schema_version"] != 1:
        raise ValueError("unsupported attestation version")
    if payload["subject"] != identifier or payload["verdict"] != status:
        raise ValueError("attestation subject/verdict mismatch")
    category = payload["category"]
    if not isinstance(category, str) or category not in REQUIRED[identifier]:
        raise ValueError("unexpected evidence category")
    if (
        not isinstance(payload["source"], str)
        or payload["source"] not in SOURCES[category]
    ):
        raise ValueError("source cannot satisfy evidence category")
    if not nonempty(payload["details"]):
        raise ValueError("missing attestation details")
    return category


def evaluate(manifest, root):
    root = Path(root).resolve()
    errors, failures, incomplete, seen = [], set(), set(), set()
    try:
        fields(manifest, ("schema_version", "overall", "entries"), "manifest")
        if (
            type(manifest["schema_version"]) is not int
            or manifest["schema_version"] != 1
        ):
            raise ValueError("unsupported manifest version")
        if (
            not isinstance(manifest["overall"], str)
            or manifest["overall"] not in STATUSES
        ):
            raise ValueError("invalid overall status")
        if not isinstance(manifest["entries"], list) or len(manifest["entries"]) > 128:
            raise ValueError("entries must be a list of at most 128 entries")
        for entry in manifest["entries"]:
            identifier = "invalid entry"
            try:
                fields(entry, ("id", "status", "reason", "evidence"), "entry")
                identifier = entry["id"]
                if not isinstance(identifier, str) or identifier not in REQUIRED:
                    identifier = "unknown entry"
                    raise ValueError("unknown requirement")
                if identifier in seen:
                    raise ValueError("duplicate requirement")
                seen.add(identifier)
                status = entry["status"]
                if not isinstance(status, str) or status not in STATUSES:
                    raise ValueError("invalid status")
                if status == "Incomplete":
                    incomplete.add(identifier)
                if not nonempty(entry["reason"]):
                    raise ValueError("missing reason")
                refs = entry["evidence"]
                if not isinstance(refs, list) or len(refs) > 16:
                    raise ValueError("evidence must be a list of at most 16 references")
                categories, paths = set(), set()
                for ref in refs:
                    category = evidence(ref, root, identifier, status)
                    if ref["path"] in paths:
                        raise ValueError("duplicate evidence reference")
                    paths.add(ref["path"])
                    categories.add(category)
                if status != "Incomplete" and categories != set(REQUIRED[identifier]):
                    raise ValueError("missing required evidence categories")
                if status == "Fail":
                    failures.add(identifier)
            except (ValueError, OSError, UnicodeError) as exc:
                errors.append(f"{identifier}: {exc}")
                incomplete.add(identifier)
        missing = set(REQUIRED) - seen
        incomplete.update(missing)
        errors.extend(
            f"{identifier}: missing requirement" for identifier in sorted(missing)
        )
        overall = (
            "Incomplete" if incomplete or errors else "Fail" if failures else "Pass"
        )
        if manifest["overall"] != overall:
            errors.append(
                f"declared overall {manifest['overall']} "
                f"does not match computed {overall}"
            )
    except (ValueError, OSError, UnicodeError) as exc:
        errors.append(str(exc))
        incomplete.update(REQUIRED)
    return {
        "overall": "Incomplete"
        if incomplete or errors
        else "Fail"
        if failures
        else "Pass",
        "failures": sorted(failures),
        "incomplete": sorted(incomplete),
        "errors": errors,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--evidence-root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    try:
        manifest, _ = read_json(args.manifest)
        result = evaluate(manifest, args.evidence_root)
    except (ValueError, OSError, UnicodeError) as exc:
        result = {
            "overall": "Incomplete",
            "failures": [],
            "incomplete": sorted(REQUIRED),
            "errors": [str(exc)],
        }
    print(json.dumps(result, indent=2))
    return {"Pass": 0, "Fail": 1, "Incomplete": 2}[result["overall"]]


if __name__ == "__main__":
    raise SystemExit(main())
