#!/usr/bin/env python3
"""Separate-process, always-active checks of the nonshipping T050 diagnostic."""

import os
import pathlib
import re
import shutil
import subprocess
import sys
import uuid


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


binary = sys.argv[1]
prefix = "/private/tmp/t050-" if sys.platform == "darwin" else "/tmp/t050-"
root = pathlib.Path(prefix + uuid.uuid4().hex)


def run(*args, success, env=None):
    result = subprocess.run(
        [binary, *map(str, args)],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
        env=env,
    )
    print(result.stdout, end="")
    print(result.stderr, end="")
    check(
        (result.returncode == 0) == success,
        f"unexpected status {result.returncode}: {args[0]}",
    )
    check(
        "PRIVATE KEY" not in result.stdout + result.stderr
        and "BEGIN CERTIFICATE" not in result.stdout + result.stderr,
        "secret-bearing log",
    )
    return result.stdout


try:
    partial = pathlib.Path(prefix + "20261031-codex4")
    check(
        not partial.exists() and not partial.is_symlink(),
        "partial test path already exists",
    )
    partial.mkdir(mode=0o700)
    try:
        partial_children = [
            "state",
            "corrupt-identity",
            "truncated-identity",
            "missing-identity",
            "corrupt-settings",
            "unreadable-registry",
            "identity-permissions",
            "identity-symlink",
        ]
        for child in partial_children:
            directory = partial / child
            directory.mkdir(mode=0o700)
            names = {"identity.bin", "settings.bin", "devices.bin"}
            if child == "missing-identity":
                names.remove("identity.bin")
            if child == "identity-symlink":
                names.remove("identity.bin")
                names.add("identity-decoy.bin")
            for name in names:
                item = directory / name
                item.write_bytes(b"fixture")
                item.chmod(0o600)
        (partial / "identity-permissions/identity.bin").chmod(0)
        run("cleanup-partial-codex4", root, success=False)
        run("cleanup", partial, success=False)

        def partial_refused(case):
            print(f"T050_TEST partial_cleanup_reject={case}")
            run("cleanup-partial-codex4", partial, success=False)
            check(
                (partial / "state/identity.bin").read_bytes() == b"fixture",
                "partial cleanup changed state before inventory refusal",
            )

        extra = partial / "unexpected"
        extra.touch()
        partial_refused("unexpected-root-entry")
        extra.unlink()
        extra = partial / "identity-symlink/identity.bin"
        extra.touch()
        partial_refused("unexpected-identity")
        extra.unlink()
        missing = partial / "identity-symlink/identity-decoy.bin"
        missing.unlink()
        partial_refused("missing-decoy")
        missing.symlink_to("settings.bin")
        partial_refused("unexpected-file-symlink")
        missing.unlink()
        missing.write_bytes(b"fixture")
        missing.chmod(0o600)
        run("cleanup-partial-codex4", partial, success=True)
        check(not partial.exists(), "partial cleanup left root")
        run("cleanup-partial-codex4", partial, success=False)
        print(
            "T050_TEST partial_cleanup_exact=1 mode0000=1 "
            "refusal_before_delete=1 pass=1"
        )
    finally:
        if partial.exists():
            shutil.rmtree(partial)
    partial5 = pathlib.Path(prefix + "20261031-codex5")
    check(
        not partial5.exists() and not partial5.is_symlink(),
        "codex5 partial path exists",
    )
    partial5.mkdir(mode=0o700)
    try:
        for child in partial_children + ["identity-multilink"]:
            directory = partial5 / child
            directory.mkdir(mode=0o700)
            for name in (
                ("settings.bin", "devices.bin")
                if child == "missing-identity"
                else ("identity.bin", "settings.bin", "devices.bin")
            ):
                item = directory / name
                item.write_bytes(b"fixture")
                item.chmod(0o600)
        (partial5 / "identity-permissions/identity.bin").chmod(0)
        run("cleanup-partial-codex5", root, success=False)
        run("cleanup-partial-codex4", partial5, success=False)
        run("cleanup", partial5, success=False)

        def partial5_refused(case):
            print(f"T050_TEST partial_codex5_reject={case}")
            run("cleanup-partial-codex5", partial5, success=False)
            check(
                (partial5 / "state/identity.bin").read_bytes() == b"fixture",
                "partial5 mutated before refusal",
            )

        extra = partial5 / "directory-permissions"
        extra.mkdir()
        partial5_refused("unexpected-tenth-directory")
        extra.rmdir()
        extra = partial5 / "identity-multilink/identity-decoy.bin"
        extra.touch()
        partial5_refused("unexpected-decoy")
        extra.unlink()
        missing = partial5 / "identity-multilink/identity.bin"
        missing.unlink()
        partial5_refused("missing-identity")
        missing.symlink_to("settings.bin")
        partial5_refused("symlink-identity")
        missing.unlink()
        missing.write_bytes(b"fixture")
        missing.chmod(0o600)
        held_link = pathlib.Path(str(partial5) + "-held-link")
        check(not held_link.exists(), "partial5 hard-link hold path exists")
        os.link(missing, held_link)
        try:
            partial5_refused("multiple-links")
        finally:
            held_link.unlink()
        run("cleanup-partial-codex5", partial5, success=True)
        check(not partial5.exists(), "partial5 cleanup left root")
        run("cleanup-partial-codex5", partial5, success=False)
        print(
            "T050_TEST partial_codex5_exact=1 nine_dirs=1 regular_single_link=1 "
            "mode0000=1 refusal_before_delete=1 pass=1"
        )
    finally:
        if partial5.exists():
            shutil.rmtree(partial5)
    for invalid in [
        "/not-t050/example",
        prefix,
        str(root) + "/child",
        str(root) + "/../escape",
        "/data/ps5-dev/t050-example",
        str(root) + "_bad",
    ]:
        run("init", invalid, success=False)
        run("cleanup", invalid, success=False)
    run("unknown", root, success=False)
    run("verify", root, "0" * 64, success=False)
    run("cleanup", root, success=False)
    check(not root.exists(), "rejected operation created root")
    initial = run("init", root, success=True)
    fingerprint = re.search(r"sha256=([0-9a-f]{64})", initial).group(1)
    check(initial.count("T050 recovery=") == 9, "missing recovery case")
    check("pass=1" in initial, "init incomplete")
    check(
        "recovery=identity-symlink identity=0" in initial
        and "skipped=1" not in initial,
        "ordinary host symlink protection was skipped",
    )
    check(
        "recovery=identity-multilink identity=0" in initial,
        "ordinary host multilink protection was skipped",
    )
    check(
        "T050 preflight verdict=pass paths_created=0 store_loaded=0" in initial,
        "missing read-only preflight verdict",
    )
    for operation in ("raw_fstatat", "raw_openat", "raw_openat_without_nonblock"):
        check(
            re.search(rf"preflight op={operation}(?: flags=\d+)? rax=2 cf=1", initial),
            "missing raw carry/error comparison",
        )
    for file in range(3):
        check(
            re.search(
                rf"stage=read.open event=end file={file} result=-1 reason=would_block",
                initial,
            ),
            "missing initial missing-file trace",
        )
    check(
        "T050 load stage=initial status=0 reason=none" in initial,
        "missing load outcome",
    )
    check(
        initial.count("T050 trace stage=") <= 256
        and initial.count("T050 trace_limit=256 truncated=1") == 1,
        "trace budget not enforced",
    )
    alias = pathlib.Path(str(root) + "-alias")
    alias.symlink_to(root, target_is_directory=True)
    try:
        run("init", alias, success=False)
        run("verify", alias, fingerprint, success=False)
        run("cleanup", alias, success=False)
    finally:
        alias.unlink()
    before = (root / "state/identity.bin").read_bytes()
    for stage, reason in [
        ("read.fstat", "ownership"),
        ("read.data", "platform"),
        ("read.tail", "platform"),
        ("read.close", "platform"),
    ]:
        output = run(
            "verify",
            root,
            fingerprint,
            success=False,
            env=dict(os.environ, T050_FAULT_STAGE=stage),
        )
        check(
            f"T050 fault stage={stage} injected=1" in output, "read fault not reached"
        )
        check(
            f"stage={stage} event=end file=0 result=-1 reason={reason}" in output,
            "missing raw read failure result",
        )
        check(
            f"load stage=initial status=1 reason={reason}" in output,
            "read failure not surfaced",
        )
        check(
            (root / "state/identity.bin").read_bytes() == before,
            "read fault mutated identity",
        )
        check(
            "stage=read.close event=end" in output,
            "read failure did not consume descriptor",
        )
    run("init", root, success=False)
    run("verify", root, "0" * 64, success=False)
    run("verify", root, "g" * 64, success=False)
    verified = run("verify", root, fingerprint, success=True)
    for stage in (
        "read.fstat",
        "read.type",
        "read.mode",
        "read.secure",
        "read.data",
        "read.tail",
        "read.close",
    ):
        check(f"stage={stage} event=end" in verified, "missing read-path diagnostic")
    check(
        re.search(
            r"stage=read.open event=end file=0 result=[1-9]\d* reason=none", verified
        ),
        "read.open did not retain actual descriptor",
    )
    check(
        "pass=1" in verified and f"sha256={fingerprint}" in verified,
        "verify incomplete",
    )
    check(
        re.search(r"pid=(\d+)", initial).group(1)
        != re.search(r"pid=(\d+)", verified).group(1),
        "not separate processes",
    )
    check((root / "state/identity.bin").read_bytes() == before, "identity replaced")
    # Verification is a one-shot state transition, not a false fresh-process pass.
    run("verify", root, fingerprint, success=False)
    for entry in (root / "state").iterdir():
        st = entry.lstat()
        check(
            not entry.is_symlink()
            and st.st_nlink == 1
            and st.st_uid == os.geteuid()
            and st.st_mode & 0o777 == 0o600,
            "insecure state file",
        )
    check(
        {p.name for p in (root / "state").iterdir()}
        == {"identity.bin", "settings.bin", "devices.bin"},
        "unexpected temp file",
    )

    def rejected_cleanup(case):
        print(f"T050_TEST cleanup_reject={case}")
        run("cleanup", root, success=False)
        check(
            (root / "state/identity.bin").read_bytes() == before,
            "cleanup mutated before refusal",
        )

    unexpected = root / "unexpected"
    unexpected.touch()
    rejected_cleanup("unexpected-root-entry")
    unexpected.unlink()
    unexpected = root / "directory-permissions/.new-unexpected"
    unexpected.touch()
    rejected_cleanup("unexpected-last-child-file")
    unexpected.unlink()
    missing = root / "directory-permissions/settings.bin"
    saved = missing.read_bytes()
    missing.unlink()
    rejected_cleanup("missing-last-child-file")
    missing.mkdir()
    rejected_cleanup("directory-in-file-slot")
    missing.rmdir()
    missing.write_bytes(saved)
    missing.chmod(0o600)
    decoy = root / "identity-symlink/identity-decoy.bin"
    held_decoy = pathlib.Path(str(root) + "-held-decoy")
    check(not held_decoy.exists(), "decoy hold path already exists")
    decoy.rename(held_decoy)
    try:
        rejected_cleanup("missing-decoy-with-dangling-identity-symlink")
    finally:
        held_decoy.rename(decoy)
    hard_identity = root / "identity-multilink/identity.bin"
    hard_decoy = root / "identity-multilink/identity-decoy.bin"
    check(
        hard_identity.stat().st_ino == hard_decoy.stat().st_ino
        and hard_identity.stat().st_nlink == hard_decoy.stat().st_nlink == 2,
        "ordinary host fixture is not a real hard link",
    )
    hard_decoy.rename(held_decoy)
    try:
        rejected_cleanup("hardlink-missing-decoy-still-two-links")
    finally:
        held_decoy.rename(hard_decoy)
    saved_hard = hard_decoy.read_bytes()
    hard_decoy.unlink()
    hard_decoy.write_bytes(saved_hard)
    hard_decoy.chmod(0o600)
    rejected_cleanup("hardlink-decoy-is-independent-file")
    held_identity = pathlib.Path(str(root) + "-held-identity")
    check(
        not held_identity.exists() and not held_decoy.exists(),
        "hard-link hold path exists",
    )
    os.link(hard_identity, held_identity)
    try:
        os.link(hard_decoy, held_decoy)
        try:
            rejected_cleanup("hardlink-pair-has-different-inodes")
        finally:
            held_decoy.unlink()
    finally:
        held_identity.unlink()
    hard_decoy.unlink()
    os.link(hard_identity, hard_decoy)
    hard_identity.unlink()
    hard_identity.symlink_to("identity-decoy.bin")
    rejected_cleanup("hardlink-identity-is-symlink")
    hard_identity.unlink()
    os.link(hard_decoy, hard_identity)
    held = pathlib.Path(str(root) + "-held")
    child = root / "directory-permissions"
    child.rename(held)
    try:
        rejected_cleanup("missing-child")
        child.symlink_to(held, target_is_directory=True)
        try:
            rejected_cleanup("symlink-child")
        finally:
            child.unlink()
    finally:
        held.rename(child)
    sentinel = pathlib.Path(str(root) + "-sentinel")
    sentinel.write_text("must survive symlink cleanup")
    try:
        decoy = root / "identity-symlink/identity-decoy.bin"
        decoy.unlink()
        decoy.symlink_to(sentinel)
        cleaned = run("cleanup", root, success=True)
        check("removed=1 pass=1" in cleaned and not root.exists(), "cleanup left root")
        check(
            sentinel.read_text() == "must survive symlink cleanup",
            "cleanup followed file symlink",
        )
        run("cleanup", root, success=False)
    finally:
        sentinel.unlink()
    print("T050_TEST cleanup_inventory=1 no_follow=1 removed=1 repeat_refused=1 pass=1")
    unsupported_root = pathlib.Path(str(root) + "-unsupported")
    try:
        unsupported = run(
            "init",
            unsupported_root,
            success=True,
            env=dict(os.environ, T050_FAULT_STAGE="recovery.symlink_unsupported"),
        )
        check(
            re.search(
                r"recovery=identity-symlink unsupported=1 errno=\d+ "
                r"restored=1 skipped=1",
                unsupported,
            ),
            "unsupported symlink was not explicitly restored and skipped",
        )
        unsupported_fp = re.search(r"sha256=([0-9a-f]{64})", unsupported).group(1)
        fixture = unsupported_root / "identity-symlink"
        check(
            {p.name for p in fixture.iterdir()}
            == {"identity.bin", "settings.bin", "devices.bin"},
            "unsupported fixture retained unexpected files",
        )
        identity = fixture / "identity.bin"
        st = identity.lstat()
        check(
            not identity.is_symlink()
            and st.st_nlink == 1
            and st.st_uid == os.geteuid()
            and st.st_mode & 0o777 == 0o600
            and identity.read_bytes()
            == (unsupported_root / "state/identity.bin").read_bytes(),
            "restored identity not secure/equal",
        )
        extra = fixture / "unexpected"
        extra.touch()
        run("cleanup", unsupported_root, success=False)
        extra.unlink()
        missing = fixture / "devices.bin"
        saved = missing.read_bytes()
        missing.unlink()
        run("cleanup", unsupported_root, success=False)
        missing.write_bytes(saved)
        missing.chmod(0o600)
        run("verify", unsupported_root, unsupported_fp, success=True)
        run("cleanup", unsupported_root, success=True)
        check(not unsupported_root.exists(), "restored-layout cleanup left root")
    finally:
        if unsupported_root.exists():
            shutil.rmtree(unsupported_root)
    other_error_root = pathlib.Path(str(root) + "-symlink-error")
    try:
        failed = run(
            "init",
            other_error_root,
            success=False,
            env=dict(os.environ, T050_FAULT_STAGE="recovery.symlink"),
        )
        check(
            "T050 fault stage=recovery.symlink injected=1" in failed,
            "non-unsupported symlink fault not reached",
        )
        check(
            "skipped=1" not in failed and "restored=1" not in failed,
            "non-unsupported symlink error was ignored",
        )
    finally:
        if other_error_root.exists():
            shutil.rmtree(other_error_root)
    print(
        "T050_TEST unsupported_symlink_restored=1 ordinary_host_symlink_required=1 "
        "both_cleanup_layouts=1 other_errors_fail=1 pass=1"
    )
    for suffix, fault in [
        ("hardlink", "recovery.hardlink_unsupported"),
        ("bothlinks", "recovery.links_unsupported"),
    ]:
        link_root = pathlib.Path(str(root) + "-" + suffix)
        try:
            output = run(
                "init",
                link_root,
                success=True,
                env=dict(os.environ, T050_FAULT_STAGE=fault),
            )
            check(
                re.search(
                    r"recovery=identity-multilink unsupported=1 errno=\d+ "
                    r"secure=1 skipped=1",
                    output,
                ),
                "hard-link unsupported outcome missing",
            )
            if suffix == "bothlinks":
                check(
                    "recovery=identity-symlink unsupported=1" in output,
                    "combined unsupported symlink branch not exercised",
                )
            else:
                check(
                    "recovery=identity-symlink identity=0" in output,
                    "hard-link fault weakened ordinary symlink check",
                )
            fixture = link_root / "identity-multilink"
            check(
                {p.name for p in fixture.iterdir()}
                == {"identity.bin", "settings.bin", "devices.bin"},
                "unsupported hard-link fixture inventory wrong",
            )
            identity = fixture / "identity.bin"
            info = identity.lstat()
            check(
                not identity.is_symlink()
                and info.st_nlink == 1
                and info.st_uid == os.geteuid()
                and info.st_mode & 0o777 == 0o600
                and identity.read_bytes()
                == (link_root / "state/identity.bin").read_bytes(),
                "unsupported hard-link identity changed",
            )
            extra = fixture / "identity-decoy.bin"
            extra.write_bytes(b"unexpected")
            extra.chmod(0o600)
            run("cleanup", link_root, success=False)
            extra.unlink()
            missing = fixture / "devices.bin"
            saved = missing.read_bytes()
            missing.unlink()
            run("cleanup", link_root, success=False)
            missing.write_bytes(saved)
            missing.chmod(0o600)
            fp = re.search(r"sha256=([0-9a-f]{64})", output).group(1)
            run("verify", link_root, fp, success=True)
            run("cleanup", link_root, success=True)
            check(not link_root.exists(), "unsupported hard-link cleanup left root")
        finally:
            if link_root.exists():
                shutil.rmtree(link_root)
    hard_error = pathlib.Path(str(root) + "-hard-error")
    try:
        output = run(
            "init",
            hard_error,
            success=False,
            env=dict(os.environ, T050_FAULT_STAGE="recovery.hardlink"),
        )
        check(
            "T050 fault stage=recovery.hardlink injected=1" in output
            and "recovery=identity-multilink unsupported=1" not in output,
            "non-ENOTSUP hard-link failure was skipped",
        )
    finally:
        if hard_error.exists():
            shutil.rmtree(hard_error)
    print(
        "T050_TEST unsupported_hardlink_secure=1 real_host_multilink_required=1 "
        "combined_unsupported=1 strict_link_cleanup=1 other_errors_fail=1 pass=1"
    )
    preflight_faults = [
        "preflight.fixed_fstatat",
        "fixed_openat",
        "fixed_openat_without_nonblock",
        "preflight.new_descriptor",
    ]
    for index, stage in enumerate(preflight_faults):
        failed_root = pathlib.Path(str(root) + f"-preflight-{index}")
        output = run(
            "init",
            failed_root,
            success=False,
            env=dict(os.environ, T050_FAULT_STAGE=stage),
        )
        check(
            f"T050 fault stage={stage} injected=1" in output,
            "preflight fault not reached",
        )
        check(
            "T050 preflight verdict=fail paths_created=0 store_loaded=0" in output,
            "preflight failed to refuse before state creation",
        )
        check(
            not failed_root.exists()
            and "T050 load" not in output
            and "identity.generate" not in output,
            "preflight ran mutating work",
        )
        if stage in ("fixed_openat", "fixed_openat_without_nonblock"):
            check(
                "existing=1" in output and "owned=0 close_called=0" in output,
                "preflight closed an existing descriptor alias",
            )
        if stage == "preflight.new_descriptor":
            check(
                "added=1 lost=0 existing=0 fstat=0" in output
                and "owned=1 close_called=1 close_result=0" in output,
                "preflight did not prove ownership and close new descriptor",
            )
    legacy_faults = [
        "preflight.fstatat",
        "openat",
        "openat_without_nonblock",
        "absolute_open",
        "preflight.raw_openat_cf",
    ]
    for index, stage in enumerate(legacy_faults):
        legacy_root = pathlib.Path(str(root) + f"-legacy-{index}")
        try:
            output = run(
                "init",
                legacy_root,
                success=True,
                env=dict(os.environ, T050_FAULT_STAGE=stage),
            )
            check(
                f"T050 fault stage={stage} injected=1" in output,
                "legacy fault not reached",
            )
            for operation in (
                "fixed_fstatat",
                "fixed_openat",
                "fixed_openat_without_nonblock",
            ):
                check(
                    re.search(
                        rf"preflight op={operation}(?: flags=\d+)? "
                        r"rax=-1 cf=-1 errno=2",
                        output,
                    ),
                    "fixed absence check did not authorize independently",
                )
            check(
                "preflight verdict=pass paths_created=0 store_loaded=0" in output,
                "legacy wrapper result wrongly controlled authorization",
            )
            if stage != "preflight.fstatat":
                check(
                    "existing=1" in output and "owned=0 close_called=0" in output,
                    "legacy alias was not preserved",
                )
            run("cleanup", legacy_root, success=True)
        finally:
            if legacy_root.exists():
                shutil.rmtree(legacy_root)
    print(
        "T050_TEST fixed_preflight_faults=4 legacy_observations=5 "
        "existing_fd_preserved=1 new_fd_closed=1 no_state_created=1 "
        "read_faults=4 pass=1"
    )
    fault_stages = [
        "read.open",
        "identity.keygen",
        "identity.x509_new",
        "identity.bio_new",
        "identity.serial_random",
        "identity.serial_bn",
        "identity.serial_asn1",
        "identity.version",
        "identity.not_before",
        "identity.not_after",
        "identity.subject",
        "identity.issuer",
        "identity.public_key",
        "identity.sign",
        "identity.pem_private",
        "identity.pem_certificate",
        "identity.pem_data",
        "identity.append",
        "identity.pack",
        "replace.random",
        "replace.temp_create",
        "replace.write",
        "replace.file_fsync",
        "replace.close",
        "replace.rename",
        "replace.directory_fsync",
    ]
    for index, stage in enumerate(fault_stages):
        failed_root = pathlib.Path(str(root) + f"-fault-{index}")
        try:
            output = run(
                "init",
                failed_root,
                success=False,
                env=dict(os.environ, T050_FAULT_STAGE=stage),
            )
            check(f"T050 fault stage={stage} injected=1" in output, "fault not reached")
            check(
                re.search(
                    rf"stage={re.escape(stage)} event=end file=0 "
                    r"result=-?\d+ reason=platform",
                    output,
                ),
                "missing failure-stage trace",
            )
            check(
                "T050 load stage=initial status=1 reason=platform" in output,
                "failure not surfaced",
            )
            state = failed_root / "state"
            names = {entry.name for entry in state.iterdir()}
            check(
                names
                == ({"identity.bin"} if stage == "replace.directory_fsync" else set()),
                "failed initial write has unexpected leftovers",
            )
            if stage == "replace.directory_fsync":
                check(
                    "stage=replace.rename event=end file=0 result=0 reason=none"
                    in output,
                    "post-rename uncertainty not observable",
                )
        finally:
            if failed_root.exists():
                shutil.rmtree(failed_root)
    print(f"T050_TEST fault_stages={len(fault_stages)} no_secret_logs=1 pass=1")
    print("T050_TEST separate_process=1 identity_unchanged=1 rejection_checks=1 pass=1")
finally:
    # Only this test's random, exclusively created path; repair unreadable fixtures
    # for deterministic cleanup, never follow the deliberately created symlink.
    if root.exists():
        for parent, dirs, files in os.walk(root, followlinks=False):
            for name in files:
                path = pathlib.Path(parent) / name
                if not path.is_symlink():
                    path.chmod(0o600)
        shutil.rmtree(root)
