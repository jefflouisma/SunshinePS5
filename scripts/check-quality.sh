#!/usr/bin/env bash
# Bootstrap locally: npm ci; python3 -m venv .venv;
# .venv/bin/python -m pip install --require-hashes -r tests/requirements.lock
# LLVM_BIN may select LLVM 18.1.8; QUALITY_BUILD_DIR may select its CMake build.
# This checks source without modifying it and reports every check even after failures.
set -uo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 1
if [[ -z ${TMPDIR:-} ]]; then
    if [[ -n ${RUNNER_TEMP:-} ]]; then
        export TMPDIR=$RUNNER_TEMP
    elif [[ -d /Volumes/4TB_Drive/tmp ]]; then
        export TMPDIR=/Volumes/4TB_Drive/tmp
    else
        export TMPDIR=${TMP:-${TEMP:-/tmp}}
    fi
fi
mkdir -p "$TMPDIR" || exit 1
work=$(mktemp -d "$TMPDIR/sunshine-quality.XXXXXX") || exit 1
trap 'rm -rf -- "$work"' EXIT
export RUFF_CACHE_DIR="$work/ruff-cache"
export PYTHONDONTWRITEBYTECODE=1
export CARGO_TARGET_DIR=${CARGO_TARGET_DIR:-"$TMPDIR/sunshine-quality-cargo"}
python="$root/.venv/bin/python"
biome="$root/node_modules/.bin/biome"
playwright="$root/node_modules/.bin/playwright"
build=${QUALITY_BUILD_DIR:-"$work/build"}
status=0

run_check() {
    local name=$1
    shift
    printf '\n== %s ==\n' "$name"
    if "$@"; then
        printf 'PASS: %s\n' "$name"
    else
        local result=$?
        printf 'FAIL: %s (exit %s)\n' "$name" "$result" >&2
        status=1
    fi
}

# Locate the pinned tools, rather than silently using a different Homebrew LLVM.
if [[ -z ${LLVM_BIN:-} ]] && command -v brew >/dev/null 2>&1; then
    llvm_prefix=$(brew --prefix llvm@18 2>/dev/null) && LLVM_BIN="$llvm_prefix/bin"
fi
clang_format=${LLVM_BIN:+"$LLVM_BIN/"}clang-format
clang_tidy=${LLVM_BIN:+"$LLVM_BIN/"}clang-tidy
clang=${LLVM_BIN:+"$LLVM_BIN/"}clang

check_llvm() {
    local tool
    for tool in "$clang" "$clang_format" "$clang_tidy"; do
        "$tool" --version || return
        if ! "$tool" --version | grep -Eq 'version 18\.1\.8([[:space:]]|$)'; then
            printf 'Expected LLVM 18.1.8: %s\n' "$tool" >&2
            return 1
        fi
    done
}

check_python_tools() {
    "$python" -m pip check || return
    "$python" - <<'PY'
from importlib.metadata import version
from pathlib import Path

from packaging.requirements import Requirement

for line in Path("tests/requirements.lock").read_text().splitlines():
    if line and not line.startswith(("#", " ", "-")):
        requirement = Requirement(line.split("\\", 1)[0].strip())
        if requirement.marker and not requirement.marker.evaluate():
            continue
        actual = version(requirement.name)
        if actual not in requirement.specifier:
            raise SystemExit(f"{requirement.name}: expected {requirement.specifier}, installed {actual}")
        print(f"{requirement.name}=={actual}")
PY
}

check_node_tools() {
    npm ls --depth=0 || return
    "$biome" --version || return
    "$playwright" --version
}

check_rust_tools() {
    local cargo_path
    cargo_path=$(rustup which --toolchain 1.93.0 cargo) || return
    # Cargo subcommands otherwise may resolve newer global rustfmt/Clippy binaries.
    export PATH="$(dirname -- "$cargo_path"):$PATH"
    rustup run 1.93.0 cargo --version || return
    rustup run 1.93.0 rustc --version || return
    rustup run 1.93.0 cargo fmt -- --version || return
    rustup run 1.93.0 cargo clippy --version || return
    rustup run 1.93.0 cargo clippy --version | grep -Eq '^clippy 0\.1\.93([[:space:]]|$)'
}

# Discover only project source roots, never vendored dependencies or generated output.
source_files() {
    "$python" - "$1" <<'PY'
import os
import sys
from pathlib import Path

extensions = {
    "c": {".c", ".h", ".cc", ".cpp", ".hpp"},
    "python": {".py"},
    "web": {".js", ".mjs", ".cjs", ".ts", ".tsx", ".jsx", ".css", ".html"},
}[sys.argv[1]]
roots = ("core", "platform", "webapp", "tests", "scripts", "tools")
excluded = {".git", ".venv", "node_modules", "target", "build", "artifacts", "vendor"}
for root in roots:
    for directory, folders, files in os.walk(root):
        folders[:] = sorted(folder for folder in folders if folder not in excluded)
        for name in sorted(files):
            path = Path(directory, name)
            if path.suffix in extensions:
                sys.stdout.buffer.write(os.fsencode(path) + b"\0")
PY
}

check_c_format() {
    local files=() file
    source_files c > "$work/c-files" || return
    while IFS= read -r -d '' file; do files+=("$file"); done < "$work/c-files"
    if [[ ${#files[@]} -eq 0 ]]; then
        printf 'No checked-in C source yet; checking the real CMake C11 setup translation unit.\n'
        files=("$build/tests/c11_build_contract.c")
    fi
    "$clang_format" --dry-run --Werror --style="file:$root/.clang-format" "${files[@]}"
}

check_c_tidy() {
    "$python" - "$build" > "$work/translation-units" <<'PY'
import json
import os
import sys
from pathlib import Path

database = Path(sys.argv[1], "compile_commands.json")
entries = json.loads(database.read_text())
if not entries:
    raise SystemExit("No C translation units in compile_commands.json")
for source in sorted({entry["file"] for entry in entries}):
    sys.stdout.buffer.write(os.fsencode(source) + b"\0")
PY
    local result=$?
    [[ $result -eq 0 ]] || return "$result"
    local file failed=0
    while IFS= read -r -d '' file; do
        "$clang_tidy" -p "$build" --config-file="$root/.clang-tidy" "$file" || failed=1
    done < "$work/translation-units"
    return "$failed"
}

# Fuzz harnesses are C sources too: lint them from a replay-only fuzz
# configuration (no libFuzzer runtime needed) instead of skipping them.
check_fuzz_tidy() {
    local fuzz_build="$work/fuzz-tidy" file failed=0
    cmake -S "$root" -B "$fuzz_build" -DCMAKE_C_COMPILER="$clang" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DSP5_BUILD_FUZZERS=ON -DSP5_FUZZ_REPLAY_ONLY=ON \
        -DSP5_FUZZ_SANITIZERS=undefined >/dev/null || return
    # Configure only: reuse the pinned dependency headers the quality build already
    # verified and built, instead of downloading and rebuilding them for linting.
    "$python" - "$fuzz_build" "$build" <<'PY' || return
import json
import sys
from pathlib import Path

fuzz, build = Path(sys.argv[1]), Path(sys.argv[2])
database = fuzz / "compile_commands.json"
text = database.read_text().replace(str(fuzz / "dependencies"), str(build / "dependencies"))
if not any(entry["file"].endswith("/tests/fuzz/http.c") for entry in json.loads(text)):
    raise SystemExit("fuzz harnesses are missing from the fuzz compile database")
database.write_text(text)
PY
    "$python" -I tests/fuzz/run.py --check-targets || return
    for file in "$root"/tests/fuzz/*.c; do
        "$clang_tidy" -p "$fuzz_build" --config-file="$root/.clang-tidy" "$file" || failed=1
    done
    return "$failed"
}

check_web_format() {
    "$biome" format --indent-style=space --indent-width=2 --line-width=100 "${web_files[@]}"
}

check_web_lint() {
    "$biome" lint --error-on-warnings "${web_files[@]}"
}

check_python_format() {
    "$python" -m ruff format --isolated --check --target-version py311 "${python_files[@]}"
}

check_python_lint() {
    "$python" -m ruff check --isolated --target-version py311 --select E,F,I,UP "${python_files[@]}"
}

run_check 'LLVM versions' check_llvm
run_check 'Python locked tools' check_python_tools
run_check 'Node locked tools' check_node_tools
run_check 'Rust pinned tools' check_rust_tools
run_check 'CMake configure' cmake -S "$root" -B "$build" -DCMAKE_C_COMPILER="$clang" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
run_check 'C warning-free build' cmake --build "$build"
run_check 'clang-format' check_c_format
run_check 'clang-tidy' check_c_tidy
run_check 'clang-tidy (fuzz harnesses)' check_fuzz_tidy
run_check 'rustfmt' rustup run 1.93.0 cargo fmt --manifest-path tools/moonlight-mcp/Cargo.toml --all -- --check
run_check 'Clippy' rustup run 1.93.0 cargo clippy --manifest-path tools/moonlight-mcp/Cargo.toml --locked --all-targets --all-features -- -D warnings

web_files=(package.json)
if source_files web > "$work/web-files"; then
    while IFS= read -r -d '' file; do web_files+=("$file"); done < "$work/web-files"
else
    status=1
fi
run_check 'Web format (Biome)' check_web_format
run_check 'Web lint (Biome)' check_web_lint

# Until T012 supplies project schema tests, exercise the real installed schema tooling.
# This is tooling verification, not a passing project schema or browser behavior suite.
cat > "$work/test_schema_tooling.py" <<'PY'
import jsonschema
import pytest


def test_schema_tooling():
    schema = {"type": "integer", "minimum": 1}
    jsonschema.Draft202012Validator.check_schema(schema)
    jsonschema.validate(1, schema)
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(0, schema)
PY
python_files=("$work/test_schema_tooling.py")
if source_files python > "$work/python-files"; then
    while IFS= read -r -d '' file; do python_files+=("$file"); done < "$work/python-files"
else
    status=1
fi
run_check 'Python format (Ruff)' check_python_format
run_check 'Python lint (Ruff)' check_python_lint
run_check 'Schema tooling smoke (not project acceptance)' "$python" -m pytest -q -p no:cacheprovider "$work/test_schema_tooling.py"

printf '\nQuality exit status: %s (all checks attempted; no source files changed)\n' "$status"
exit "$status"
