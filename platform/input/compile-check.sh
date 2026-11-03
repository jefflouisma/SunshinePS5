#!/usr/bin/env bash
# Compile-check the provisional PS5 input adapter with the pinned target
# compiler and the pinned host clang. Prints source/header/object SHA-256s.
set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
sdk=${PS5_PAYLOAD_SDK:-${HOME}/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk}
llvm_config=${LLVM_CONFIG:-/opt/homebrew/opt/llvm@18/bin/llvm-config}
llvm_bin=$("$llvm_config" --bindir 2>/dev/null) || llvm_bin=
host_cc=${SP5_HOST_CC:-${llvm_bin:+$llvm_bin/clang}}
target_cc=$sdk/bin/prospero-clang
out=$(mktemp -d "${TMPDIR:-/tmp}/sp5-ps5-input.XXXXXX") || exit 1
trap 'rm -rf -- "$out"' EXIT
status=0

hash_files() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$@"
    else
        sha256sum "$@"
    fi
}

check_whitespace() {
    local file
    for file in "$@"; do
        if grep -nE '[[:blank:]]+$' "$file"; then
            printf 'trailing whitespace: %s\n' "$file" >&2
            status=1
        fi
        if LC_ALL=C grep -n $'\r' "$file"; then
            printf 'carriage return: %s\n' "$file" >&2
            status=1
        fi
    done
}

flags=(-std=c11 -O2 -Wall -Wextra -Wpedantic -Werror)
includes=("-I$root/core" "-I$root/platform" "-I$root/platform/capture" "-I$root/platform/encode" "-I$here")
check_whitespace "$here/ps5.c" "$here/ps5.h" "$here/compile-check.sh" "$here/CMakeLists.txt"

printf '== target: %s ==\n' "$target_cc"
if [[ -x $target_cc ]] && env LLVM_CONFIG="$llvm_config" "$target_cc" "${flags[@]}" \
    "${includes[@]}" -c "$here/ps5.c" -o "$out/ps5-target.o"; then
    hash_files "$here/ps5.c" "$here/ps5.h" "$root/platform/ps5_media_global.h" \
        "$root/platform/firmware/13.60.h" "$out/ps5-target.o"
else
    printf 'target compile failed\n' >&2
    status=1
fi

printf '== host: %s ==\n' "${host_cc:-missing}"
if [[ -n $host_cc && -x $host_cc ]] && "$host_cc" "${flags[@]}" "${includes[@]}" \
    -c "$here/ps5.c" -o "$out/ps5-host.o"; then
    hash_files "$here/ps5.c" "$here/ps5.h" "$root/platform/ps5_media_global.h" \
        "$root/platform/firmware/13.60.h" "$out/ps5-host.o"
else
    printf 'host compile failed\n' >&2
    status=1
fi

exit "$status"
