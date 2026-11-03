#!/usr/bin/env bash
# Console-only compile check for all provisional PS5 adapters.
# Compiles with the pinned prospero-clang and the pinned LLVM 18 host clang.
# It never links these objects into portable core targets and never deploys.
set -uo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk=${PS5_PAYLOAD_SDK:-${SP5_PS5_SDK:-}}
if [[ -z "$sdk" ]]; then
    for candidate in \
        "${HOME:-/nonexistent}/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk" \
        "/opt/ps5-payload-sdk"; do
        if [[ -d "$candidate" ]]; then
            sdk=$candidate
            break
        fi
    done
fi
llvm_config=${LLVM_CONFIG:-/opt/homebrew/opt/llvm@18/bin/llvm-config}
if [[ ! -x "$llvm_config" ]]; then
    candidate=$(command -v "$llvm_config" 2>/dev/null) || candidate=
    if [[ -n "$candidate" ]]; then
        llvm_config=$candidate
    fi
fi
if [[ -x "$llvm_config" ]]; then
    resolved=$(readlink -f "$llvm_config" 2>/dev/null) || resolved=$llvm_config
    llvm_config=$resolved
fi
build_dir=${SP5_ADAPTER_BUILD_DIR:-${TMPDIR:-/tmp}/sp5-adapters}
target_cc="$sdk/bin/prospero-clang"
host_cc=$("$llvm_config" --bindir 2>/dev/null)/clang

status=0
for tool in "$target_cc" "$host_cc"; do
    if [[ ! -x "$tool" ]]; then
        printf 'MISSING_COMPILER %s\n' "$tool" >&2
        status=1
    fi
done
if [[ ! -x "$llvm_config" ]] || [[ "$("$llvm_config" --version 2>/dev/null)" != 18.1.8 ]]; then
    printf 'MISSING_LLVM_CONFIG 18.1.8\n' >&2
    status=1
fi
if [[ ! -d "$sdk" ]]; then
    printf 'MISSING_PS5_SDK %s\n' "$sdk" >&2
    status=1
fi
mkdir -p "$build_dir" || exit 1
export LLVM_CONFIG="$llvm_config"
export LLVM_BINDIR=$("$llvm_config" --bindir 2>/dev/null)

common=(-std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I"$repo/core" -I"$repo/platform")
host_extra=(-D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE=1)
sources=(
    firmware/13.60.c
    privileged/access.c
    ps5_media_global.c
    capture/ps5.c
    encode/ps5.c
    system/ps5.c
    ps5-adapters.smoke.c
)
for optional in audio/ps5.c input/ps5.c; do
    if [[ -f "$repo/platform/$optional" ]]; then
        sources+=("$optional")
    fi
done
headers=(
    firmware/13.60.h
    privileged/access.h
    capture/ps5.h
    encode/ps5.h
    audio/ps5.h
    input/ps5.h
    system/ps5.h
    ps5_media.h
    ps5_clock.h
    ps5_annexb.h
    ps5_media_global.h
)
objects=()

hash_files() {
    local file
    for file in "$@"; do
        if [[ -f "$file" ]]; then
            if command -v shasum >/dev/null 2>&1; then
                shasum -a 256 "$file"
            else
                sha256sum "$file"
            fi
        fi
    done
}

printf 'PS5_ADAPTER_COMPILER=%s\n' "$("$target_cc" --version 2>/dev/null | head -1)"
printf 'HOST_COMPILER=%s\n' "$("$host_cc" --version 2>/dev/null | head -1)"
for source in "${sources[@]}"; do
    source_dir=$(dirname -- "$source")
    includes=("${common[@]}" "-I$repo/platform/$source_dir")
    name=${source//\//-}
    target_object="$build_dir/target-$name.o"
    host_object="$build_dir/host-$name.o"
    if [[ -x "$target_cc" ]]; then
        printf 'TARGET_COMPILE %s\n' "$repo/platform/$source"
        if "$target_cc" "${includes[@]}" -c "$repo/platform/$source" -o "$target_object"; then
            objects+=("$target_object")
        else
            printf 'TARGET_COMPILE_FAILED %s\n' "$source" >&2
            status=1
        fi
    fi
    if [[ -x "$host_cc" ]]; then
        printf 'HOST_COMPILE %s\n' "$repo/platform/$source"
        if "$host_cc" "${includes[@]}" "${host_extra[@]}" -c "$repo/platform/$source" \
            -o "$host_object"; then
            objects+=("$host_object")
        else
            printf 'HOST_COMPILE_FAILED %s\n' "$source" >&2
            status=1
        fi
    fi
done

printf 'SOURCE_AND_OBJECT_HASHES\n'
hash_files "${headers[@]/#/$repo/platform/}"
hash_files "${sources[@]/#/$repo/platform/}"
if ((${#objects[@]} != 0)); then
    hash_files "${objects[@]}"
fi
exit "$status"
