"""Exercise the source-build gate without compiling LLVM."""

import hashlib
import json
import os
import types
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/build-verified-llvm18.sh"


@pytest.fixture
def builder():
    module = types.ModuleType("llvm_builder")
    module.__file__ = str(SCRIPT)
    body = SCRIPT.read_text().split("<<'PYTHON'\n", 1)[1].rsplit("\nPYTHON", 1)[0]
    exec(compile(body, str(SCRIPT), "exec"), module.__dict__)
    return module


def test_archive_missing_and_changed_refused(builder, tmp_path):
    archive = tmp_path / "source.tar.xz"
    expected = {"size_bytes": 3, "sha256": hashlib.sha256(b"abc").hexdigest()}
    with pytest.raises((ValueError, FileNotFoundError)):
        builder.verify_archive(archive, expected)
    archive.write_bytes(b"abd")
    with pytest.raises(ValueError, match="hash"):
        builder.verify_archive(archive, expected)
    archive.write_bytes(b"abc")
    builder.verify_archive(archive, expected)
    archive.write_bytes(b"abcd")
    with pytest.raises(ValueError, match="size"):
        builder.verify_archive(archive, expected)


def test_manifest_resume_requires_identical_inputs(builder, tmp_path):
    target = tmp_path / "inputs.json"
    inputs = {"compiler": "hash-a", "jobs": 4}
    builder.pin_inputs(target, inputs)
    original = target.read_bytes()
    builder.pin_inputs(target, inputs)
    assert target.read_bytes() == original
    with pytest.raises(ValueError, match="inputs changed"):
        builder.pin_inputs(target, {**inputs, "compiler": "hash-b"})
    assert json.loads(target.read_text()) == inputs


def test_configuration_is_bounded_and_explicit(builder, tmp_path):
    tools = {"cc": "/tool/clang", "cxx": "/tool/clang++", "ninja": "/tool/ninja"}
    flags = builder.configuration(tmp_path, tools, "/sdk")
    assert "-DLLVM_TARGETS_TO_BUILD=AArch64;X86" in flags
    assert "-DLLVM_ENABLE_PROJECTS=clang;lld;clang-tools-extra" in flags
    assert "-DLLVM_PARALLEL_LINK_JOBS=1" in flags
    assert "-DCMAKE_BUILD_TYPE=Release" in flags
    assert "-DCMAKE_C_COMPILER=/tool/clang" in flags
    assert "-DCMAKE_OSX_SYSROOT=/sdk" in flags
    assert "install-clang-tidy" in builder.INSTALL_TARGETS
    # LLVM installs these aliases as separate components.
    assert "install-llvm-ranlib" in builder.INSTALL_TARGETS
    assert "install-llvm-readelf" in builder.INSTALL_TARGETS


def test_inventory_rejects_missing_tool_and_binds_bytes(builder, tmp_path):
    binary = tmp_path / "bin"
    binary.mkdir()
    with pytest.raises(FileNotFoundError):
        builder.inventory(tmp_path)
    for name in builder.REQUIRED_TOOLS:
        (binary / name).write_bytes(name.encode())
    first = builder.inventory(tmp_path)
    (binary / "clang").write_bytes(b"changed")
    assert builder.inventory(tmp_path)["clang"]["sha256"] != first["clang"]["sha256"]


@pytest.mark.parametrize("jobs", ["0", "-1", "65", "nonsense"])
def test_invalid_parallelism_refused(builder, jobs):
    with pytest.raises(SystemExit):
        builder.arguments(["--archive", "/source", "--jobs", jobs])


def test_output_must_be_inside_ignored_build_tree(builder, tmp_path):
    with pytest.raises(ValueError, match="build directory"):
        builder.validate_output(tmp_path, Path("/"))
    valid = tmp_path / "build/verified-llvm18"
    assert builder.validate_output(tmp_path, valid) == valid


def test_source_tree_detects_modified_file_and_link(builder, tmp_path):
    (tmp_path / "file").write_bytes(b"source")
    first = builder.tree_digest(tmp_path)
    (tmp_path / "file").write_bytes(b"changed")
    second = builder.tree_digest(tmp_path)
    assert second != first
    (tmp_path / "alias").symlink_to("file")
    assert builder.tree_digest(tmp_path) != second


@pytest.mark.parametrize(
    "name", ["source", "objects", "install", "temporary", "extracting"]
)
def test_reserved_build_directory_cannot_escape_through_symlink(
    builder, tmp_path, name
):
    output = tmp_path / "build/verified-llvm18"
    output.mkdir(parents=True)
    external = tmp_path / "external"
    external.mkdir()
    (output / name).symlink_to(external, target_is_directory=True)
    with pytest.raises(ValueError, match="reserved build path"):
        builder.validate_output(tmp_path, output)


def test_inventory_rejects_symlinked_install_prefix(builder, tmp_path):
    external = tmp_path / "external"
    binary = external / "bin"
    binary.mkdir(parents=True)
    for name in builder.REQUIRED_TOOLS:
        (binary / name).write_bytes(name.encode())
    prefix = tmp_path / "prefix"
    prefix.symlink_to(external, target_is_directory=True)
    with pytest.raises(ValueError, match="install prefix"):
        builder.inventory(prefix)


def test_manifest_and_lock_reject_links(builder, tmp_path):
    external = tmp_path / "external"
    external.write_text('{"value": 1}\n')

    hardlink = tmp_path / "inputs.json"
    os.link(external, hardlink)
    with pytest.raises(ValueError, match="single-link regular file"):
        builder.pin_inputs(hardlink, {"value": 1})

    lock = tmp_path / "build.lock"
    lock.symlink_to(external)
    with pytest.raises(ValueError, match="build lock"):
        builder.open_build_lock(lock)
