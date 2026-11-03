"""Host fixture verification; binary is built with NDEBUG."""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

binary = str(Path(sys.argv[1]).resolve())
results = []


def run(root, expected):
    result = subprocess.run(
        [binary, str(root)], capture_output=True, text=True, timeout=5, check=False
    )
    value = json.loads(result.stdout)
    assert result.returncode == expected, (result.returncode, value, result.stderr)
    assert value["complete"] == (expected == 0)
    if expected:
        assert "entries" not in value
    return value


with tempfile.TemporaryDirectory(prefix="inspector-", dir="/private/tmp") as tmp:
    root = Path(tmp) / "fixture"
    root.mkdir(mode=0o700)
    child = root / "state"
    child.mkdir(mode=0o700)
    data = child / "identity.bin"
    content = b"fixture-only-not-a-secret\x00" * 300
    data.write_bytes(content)
    data.chmod(0o600)
    value = run(root, 0)
    file_entry = next(e for e in value["entries"] if e["path"] == "state/identity.bin")
    st = data.stat()
    assert file_entry["sha256"] == hashlib.sha256(content).hexdigest()
    assert (
        file_entry["device"],
        file_entry["inode"],
        file_entry["uid"],
        file_entry["mode"],
        file_entry["links"],
        file_entry["size"],
    ) == (st.st_dev, st.st_ino, st.st_uid, 0o600, st.st_nlink, st.st_size)
    assert value["no_writer"] is None and value["process_start_identities"] is None
    assert value["atomic_snapshot"] is False and value["firmware_validated"] is False
    assert data.read_bytes() == content
    results.append(
        "valid inventory, metadata, SHA256, bytes unchanged, unavailable claims: PASS"
    )
    faults = subprocess.run(
        [
            str(Path(binary).with_name("recovery-inspector-faults")),
            str(root),
            str(data),
        ],
        capture_output=True,
        text=True,
        timeout=5,
        check=False,
    )
    assert faults.returncode == 0, (faults.stdout, faults.stderr)
    results.append(faults.stdout.strip())
    data.write_bytes(content)
    for name, create in [
        ("symlink", lambda p: p.symlink_to(data)),
        ("hardlink", lambda p: os.link(data, p)),
        ("fifo", lambda p: os.mkfifo(p)),
        ("invalid-name", lambda p: p.write_bytes(b"")),
    ]:
        bad = root / ('bad"name' if name == "invalid-name" else name)
        create(bad)
        run(root, 1)
        bad.unlink()
        results.append(name + ": REFUSED")
    alias = Path(tmp) / "alias"
    alias.symlink_to(root, target_is_directory=True)
    run(alias, 1)
    results.append("symlink ancestor: REFUSED")
    data.write_bytes(b"x" * (1024 * 1024 + 1))
    run(root, 1)
    data.write_bytes(content)
    results.append("oversize: REFUSED")
    for n in range(5):
        (root / str(n)).write_bytes(b"x" * (1024 * 1024))
    run(root, 1)
    results.append("aggregate byte quota: REFUSED")
    for n in range(5):
        (root / str(n)).unlink()
    for n in range(128):
        (root / str(n)).touch()
    run(root, 1)
    results.append("entry quota: REFUSED")
    for n in range(128):
        (root / str(n)).unlink()
    deep = child / "a" / "b" / "c"
    deep.mkdir(parents=True)
    run(root, 1)
    results.append("depth quota: REFUSED")
print(json.dumps({"result": "PASS", "scenarios": results}, indent=2))
