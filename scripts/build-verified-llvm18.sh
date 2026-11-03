#!/usr/bin/env bash
# Build a separate compiler from the locked source; never replace system tools.
set -euo pipefail
script_path=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/build-verified-llvm18.sh
exec python3 - "$script_path" "$@" <<'PYTHON'
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tarfile
import time


REQUIRED_TOOLS = (
    "clang", "clang++", "clang-format", "clang-tidy", "lld", "ld.lld",
    "llvm-config", "llvm-ar", "llvm-ranlib", "llvm-readelf", "llvm-objdump",
    "llvm-nm", "llvm-strings", "llvm-strip", "llvm-objcopy",
)
INSTALL_TARGETS = tuple("install-" + name for name in (
    "clang", "clang-resource-headers", "clang-format", "clang-tidy", "lld",
    "llvm-config", "llvm-ar", "llvm-ranlib", "llvm-readobj", "llvm-readelf", "llvm-objdump", "llvm-nm",
    "llvm-strings", "llvm-strip", "llvm-objcopy",
))
RESERVED_DIRECTORIES = ("source", "objects", "install", "temporary", "extracting")
RESERVED_FILES = ("build.lock", "inputs.json", "source-tree.json", "outputs.json")


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify_archive(path, expected):
    if path.is_symlink() or not path.is_file():
        raise ValueError("archive must be a regular file")
    if path.stat().st_size != expected["size_bytes"]:
        raise ValueError("archive size mismatch")
    if digest(path) != expected["sha256"]:
        raise ValueError("archive hash mismatch")


def pin_inputs(path, value):
    if os.path.lexists(path):
        metadata = os.lstat(path)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
            raise ValueError(f"manifest must be a single-link regular file: {path}")
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(descriptor) as stream:
            existing = json.load(stream)
        if existing != value:
            raise ValueError(f"inputs changed: use a new build directory ({path})")
    else:
        descriptor = os.open(
            path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600
        )
        with os.fdopen(descriptor, "w") as stream:
            json.dump(value, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())


def open_build_lock(path):
    try:
        descriptor = os.open(
            path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600
        )
    except OSError as error:
        raise ValueError(f"build lock must be a single-link regular file: {path}") from error
    metadata = os.fstat(descriptor)
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
        os.close(descriptor)
        raise ValueError(f"build lock must be a single-link regular file: {path}")
    return os.fdopen(descriptor, "a+")


def tree_digest(root):
    result = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        relative = str(path.relative_to(root))
        if path.is_symlink():
            record = [relative, "link", os.readlink(path)]
        elif path.is_file():
            record = [relative, "file", digest(path)]
        elif path.is_dir():
            record = [relative, "directory"]
        else:
            raise ValueError(f"unexpected tree object: {path}")
        result.update(json.dumps(record).encode() + b"\n")
    return result.hexdigest()


def configuration(output, tools, sdk):
    return [
        "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DLLVM_ENABLE_PROJECTS=clang;lld;clang-tools-extra",
        "-DLLVM_TARGETS_TO_BUILD=AArch64;X86", "-DLLVM_PARALLEL_LINK_JOBS=1",
        "-DLLVM_ENABLE_ASSERTIONS=OFF", "-DLLVM_INCLUDE_TESTS=OFF",
        "-DLLVM_INCLUDE_BENCHMARKS=OFF", "-DLLVM_INCLUDE_EXAMPLES=OFF",
        "-DLLVM_ENABLE_ZLIB=OFF", "-DLLVM_ENABLE_ZSTD=OFF", "-DLLVM_ENABLE_LIBXML2=OFF",
        "-DLLVM_ENABLE_TERMINFO=OFF", "-DLLVM_ENABLE_LIBEDIT=OFF",
        "-DLLVM_ENABLE_LIBPFM=OFF", "-DCLANG_ENABLE_ARCMT=OFF",
        "-DCLANG_ENABLE_STATIC_ANALYZER=ON",
        "-DCMAKE_C_COMPILER=" + tools["cc"], "-DCMAKE_CXX_COMPILER=" + tools["cxx"],
        "-DCMAKE_MAKE_PROGRAM=" + tools["ninja"], "-DCMAKE_OSX_SYSROOT=" + sdk,
        "-DCMAKE_INSTALL_PREFIX=" + str(output / "install"),
    ]


def inventory(prefix):
    if prefix.is_symlink() or not prefix.is_dir() or prefix.resolve() != prefix.absolute():
        raise ValueError(f"install prefix must be a direct directory: {prefix}")
    confined = prefix.resolve()
    result = {}
    for name in REQUIRED_TOOLS:
        path = prefix / "bin" / name
        resolved = path.resolve(strict=True)
        if not resolved.is_relative_to(confined) or not resolved.is_file():
            raise ValueError(f"tool escapes install directory: {path}")
        result[name] = {"path": str(path), "target": str(resolved), "sha256": digest(path)}
    return result


def arguments(argv):
    parser = argparse.ArgumentParser(description="Plan or build pinned LLVM 18.1.8 locally")
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--build", action="store_true", help="execute the retained plan")
    args = parser.parse_args(argv)
    if not 1 <= args.jobs <= 64:
        parser.error("jobs must be between 1 and 64")
    return args


def validate_output(root, output):
    output = output.absolute()
    base = (root / "build").resolve()
    if not output.resolve().is_relative_to(base) or output.resolve() == base:
        raise ValueError("output must be a child of the repository build directory")
    for path in (output, *output.parents):
        if path.is_symlink():
            raise ValueError("output ancestors must not be symlinks")
    if os.path.lexists(output) and not output.is_dir():
        raise ValueError("output must be a directory")
    for name in RESERVED_DIRECTORIES:
        path = output / name
        if os.path.lexists(path) and (
            path.is_symlink()
            or not path.is_dir()
            or not path.resolve().is_relative_to(output.resolve())
        ):
            raise ValueError(f"reserved build path must be a confined directory: {path}")
    for name in RESERVED_FILES:
        path = output / name
        if os.path.lexists(path):
            metadata = os.lstat(path)
            if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
                raise ValueError(
                    f"reserved build path must be a single-link regular file: {path}"
                )
    return output


def capture(command):
    return subprocess.check_output(command, text=True).strip()


def main(script, argv):
    args = arguments(argv)
    root = script.resolve().parent.parent
    lock = json.loads((root / "cmake/dependencies.lock.json").read_text())["dependencies"]["llvm"]
    if lock["version"] != "18.1.8":
        raise ValueError("this builder requires locked LLVM 18.1.8")
    archive = args.archive.absolute()
    verify_archive(archive, lock["source_archive"])
    output = validate_output(root, args.output or root / "build/verified-llvm18")
    tools = {
        "cc": capture(["/usr/bin/xcrun", "--find", "clang"]),
        "cxx": capture(["/usr/bin/xcrun", "--find", "clang++"]),
        "cmake": shutil.which("cmake"), "ninja": shutil.which("ninja"),
    }
    if not all(tools.values()):
        raise ValueError("CMake and Ninja are required")
    # Preserve clang++'s invocation name; resolving that symlink changes driver mode.
    tools = {name: str(Path(path).absolute()) for name, path in tools.items()}
    sdk = str(Path(capture(["/usr/bin/xcrun", "--show-sdk-path"])).resolve(strict=True))
    identities = {name: {"path": path, "sha256": digest(path),
                         "version": capture([path, "--version"])} for name, path in tools.items()}
    if not identities["cmake"]["version"].startswith("cmake version 4.3.4"):
        raise ValueError("CMake 4.3.4 is required")
    if identities["ninja"]["version"] != "1.13.2":
        raise ValueError("Ninja 1.13.2 is required")
    if "Apple clang version 21.0.0" not in identities["cc"]["version"]:
        raise ValueError("AppleClang 21.0.0 bootstrap is required")
    output.mkdir(parents=True, exist_ok=True)
    with open_build_lock(output / "build.lock") as lease:
        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        config = configuration(output, tools, sdk)
        inputs = {"schema_version": 1, "source": lock["source_archive"],
                  "source_commit": lock["source_commit"], "archive": str(archive),
                  "script_sha256": digest(script), "bootstrap": identities,
                  "sdk": {"path": sdk, "tree_sha256": tree_digest(Path(sdk))},
                  "configuration": config, "jobs": args.jobs,
                  "install_targets": INSTALL_TARGETS, "python": sys.version}
        # Normalize tuples before comparing a resumed JSON manifest.
        inputs = json.loads(json.dumps(inputs))
        pin_inputs(output / "inputs.json", inputs)
        if not args.build:
            print(f"PLAN_RECORDED={output / 'inputs.json'}")
            return
        if shutil.disk_usage(output).free < 30 * 1024**3:
            raise ValueError("at least 30 GiB free is required")
        memory = int(capture(["/usr/sbin/sysctl", "-n", "hw.memsize"]))
        if args.jobs * 2 * 1024**3 > memory:
            raise ValueError("allow at least 2 GiB RAM per compilation job")
        source = output / "source"
        tree_marker = output / "source-tree.json"
        if not source.exists():
            staging = output / "extracting"
            if staging.exists():
                raise ValueError("interrupted extraction: select a fresh output directory")
            staging.mkdir()
            with tarfile.open(archive, "r:xz") as package:
                package.extractall(staging, filter="data")
            children = list(staging.iterdir())
            if len(children) != 1 or not (children[0] / "llvm/CMakeLists.txt").is_file():
                raise ValueError("unexpected LLVM source layout")
            children[0].rename(source)
            staging.rmdir()
            pin_inputs(tree_marker, {"sha256": tree_digest(source)})
        elif not tree_marker.exists():
            raise ValueError("unattested source directory")
        else:
            pin_inputs(tree_marker, {"sha256": tree_digest(source)})
        environment = {"PATH": "/usr/bin:/bin:/usr/sbin:/sbin:" + str(Path(tools["cmake"]).parent),
                       "TMPDIR": str(output / "temporary"), "LC_ALL": "C", "LANG": "C"}
        Path(environment["TMPDIR"]).mkdir(exist_ok=True)
        commands = [
            [tools["cmake"], "-S", str(source / "llvm"), "-B", str(output / "objects"), *config],
            [tools["cmake"], "--build", str(output / "objects"), "--parallel", str(args.jobs),
             "--target", *INSTALL_TARGETS],
        ]
        for label, command in zip(("configure", "build-install"), commands, strict=True):
            logfile = output / f"{label}-{time.time_ns()}.log"
            print(f"RUNNING={label} LOG={logfile}", flush=True)
            with logfile.open("x") as log:
                log.write(json.dumps({"argv": command, "environment": environment}) + "\n")
                log.flush()
                result = subprocess.run(command, env=environment, stdout=log, stderr=subprocess.STDOUT)
                log.write(f"\nEXIT={result.returncode}\n")
            if result.returncode:
                raise ValueError(f"{label} failed: {logfile}")
        outputs = inventory(output / "install")
        pin_inputs(output / "outputs.json", outputs)
        print(f"BUILD_COMPLETE={output / 'outputs.json'}")


if __name__ == "__main__":
    try:
        main(Path(sys.argv[1]), sys.argv[2:])
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
PYTHON
