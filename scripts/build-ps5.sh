#!/usr/bin/env bash
# Build the provisional PAIR/WEB/STATUS-only PS5 console ELF.
#
# This script compiles and links only; it never deploys, runs or claims a
# console observation. Capture/encode/audio/input/RTSP/control/media adapters
# and ENet are deliberately absent. The pinned target OpenSSL is linked
# statically from PS5_OPENSSL_DIR.
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk=${PS5_PAYLOAD_SDK:-${HOME:-/nonexistent}/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk}
openssl_dir=${PS5_OPENSSL_DIR:-/Volumes/4TB_Drive/tmp/t043-t044/openssl-source/openssl-3.5.9}
output=${PS5_OUTPUT:-$repo/dist/sunshineps5.elf}
llvm_config=${LLVM_CONFIG:-/opt/homebrew/opt/llvm@18/bin/llvm-config}
expected_sdk_commit=a7685ce8e97ce1f8a4f38253a41d54241c9124f1
expected_llvm=18.1.8
target_cc=$sdk/bin/prospero-clang
build_dir=$repo/build/ps5-console

case "$output" in
    /*) ;;
    *) output=$repo/$output ;;
esac
manifest=${output%.elf}.sha256
output_dir=$(dirname -- "$output")
output_name=$(basename -- "$output")
manifest_name=$(basename -- "$manifest")

fail() {
    printf 'PS5_BUILD_FAILED %s\n' "$*" >&2
    exit 1
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1"
    else
        shasum -a 256 "$1"
    fi
}

[[ -d "$sdk" ]] || fail "missing PS5_PAYLOAD_SDK: $sdk"
[[ -x "$target_cc" ]] || fail "missing target compiler: $target_cc"
[[ -x "$llvm_config" ]] || fail "missing LLVM_CONFIG: $llvm_config"
[[ "$("$llvm_config" --version 2>/dev/null)" == "$expected_llvm" ]] ||
    fail "LLVM_CONFIG is not pinned $expected_llvm: $("$llvm_config" --version 2>/dev/null)"
export LLVM_CONFIG="$llvm_config"
[[ "$("$target_cc" --version 2>/dev/null | head -1)" == *"$expected_llvm"* ]] ||
    fail "target compiler is not LLVM $expected_llvm: $("$target_cc" --version 2>/dev/null | head -1)"

sdk_marker=absent
sdk_actual=
if [[ -d "$sdk/.git" ]] && command -v git >/dev/null 2>&1; then
    sdk_marker=git
    sdk_actual=$(git -C "$sdk" rev-parse HEAD 2>/dev/null) || sdk_actual=
elif [[ -f "$sdk/SDK_COMMIT" ]]; then
    sdk_marker=$sdk/SDK_COMMIT
    sdk_actual=$(head -n 1 "$sdk/SDK_COMMIT" | tr -d '[:space:]')
elif [[ -f "$sdk/COMMIT" ]]; then
    sdk_marker=$sdk/COMMIT
    sdk_actual=$(head -n 1 "$sdk/COMMIT" | tr -d '[:space:]')
elif [[ -f "$sdk/.sdk-commit" ]]; then
    sdk_marker=$sdk/.sdk-commit
    sdk_actual=$(head -n 1 "$sdk/.sdk-commit" | tr -d '[:space:]')
fi
if [[ "$sdk_marker" != absent ]]; then
    [[ "$sdk_actual" == "$expected_sdk_commit" ]] ||
        fail "SDK commit $sdk_actual != pinned $expected_sdk_commit ($sdk_marker)"
fi

[[ -d "$openssl_dir" ]] || fail "missing PS5_OPENSSL_DIR: $openssl_dir"
for library in libssl.a libcrypto.a; do
    [[ -s "$openssl_dir/$library" ]] || fail "missing target OpenSSL library: $openssl_dir/$library"
done
for header in ssl.h crypto.h evp.h pem.h x509.h; do
    [[ -s "$openssl_dir/include/openssl/$header" ]] ||
        fail "missing target OpenSSL header: $openssl_dir/include/openssl/$header"
done

command -v cmake >/dev/null 2>&1 || fail "cmake is required to embed the web app"
mkdir -p "$build_dir/generated" "$output_dir"
rm -rf "$build_dir/obj"
mkdir -p "$build_dir/obj"

printf 'PS5_SDK=%s\n' "$sdk"
printf 'PS5_SDK_COMMIT=%s\n' "$sdk_marker"
printf 'PS5_OPENSSL_DIR=%s\n' "$openssl_dir"
printf 'PS5_TARGET_CC=%s (%s)\n' "$target_cc" "$("$target_cc" --version 2>/dev/null | head -1)"
printf 'PS5_LLVM_CONFIG=%s (%s)\n' "$llvm_config" "$expected_llvm"

# The console serves immutable embedded app bytes, exactly like the root CMake
# target. Generate into the build directory; webapp/ is never modified.
assets='index.html|app.js|status.js|devices.js|settings.js|metrics.js|diagnostics.js|stop.js|style.css'
cmake -DSOURCE_DIR="$repo/webapp" -DASSETS="$assets" \
    -DOUTPUT="$build_dir/generated/web_assets.c" -DLIMIT=32768 \
    -P "$repo/cmake/embed-webapp.cmake" >/dev/null
[[ -s "$build_dir/generated/web_assets.c" ]] || fail "web asset generation produced no source"

# Exact core closure for settings/pairing/http/session/web/address plus the
# provisional console platform sources. core/session/revoke.c and the web
# settings/stop/devices routes are real core code; the entry point supplies
# the optional lifecycle hooks as fail-closed stubs because no lifecycle is
# linked (and ENet/media adapters are intentionally absent).
sources=(
    core/runtime.c
    core/settings/host.c
    core/settings/store.c
    core/settings/devices.c
    core/protocol/address.c
    core/protocol/http.c
    core/protocol/http_parser.c
    core/protocol/pairing.c
    core/protocol/pairing_state.c
    core/session/session.c
    core/session/revoke.c
    core/web/assets.c
    core/web/json.c
    core/web/server.c
    core/web/status.c
    core/web/devices.c
    core/web/settings.c
    core/web/stop.c
    "$build_dir/generated/web_assets.c"
    platform/system/storage.c
    platform/main.c
    platform/web_service.c
    platform/system/ps5.c
    platform/firmware/13.60.c
    platform/privileged/access.c
)

# The PS5 sysroot's arpa/inet.h does not pull in AF_INET/AF_INET6 from
# sys/socket.h the way the portable hosts do, and strict POSIX hides the BSD
# u_int/flock declarations used by the POSIX storage adapter. Force both.
cflags=(-std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
    -D_POSIX_C_SOURCE=200809L -D__BSD_VISIBLE=1 -include sys/socket.h
    -I"$repo/core" -I"$repo/platform" -I"$openssl_dir/include")
objects=()
for source in "${sources[@]}"; do
    case "$source" in
        /*) source_path=$source ;;
        *) source_path=$repo/$source ;;
    esac
    [[ -s "$source_path" ]] || fail "missing source: $source_path"
    object=$build_dir/obj/${source//\//-}.o
    printf 'CC %s\n' "$source_path"
    source_flags=()
    if [[ $source == platform/main.c ]]; then
        source_flags=(-DSP5_CONSOLE_DEFAULT_RUN_SECONDS=120)
    fi
    "$target_cc" "${cflags[@]}" ${source_flags[@]+"${source_flags[@]}"} -c "$source_path" -o "$object"
    objects+=("$object")
done

printf 'LINK %s\n' "$output"
"$target_cc" "${cflags[@]}" "${objects[@]}" \
    "$openssl_dir/libssl.a" "$openssl_dir/libcrypto.a" -pthread \
    -lSceRemoteplay -lSceSystemService -o "$output"

[[ -s "$output" ]] || fail "link produced no output"
magic=$(od -An -tx1 -N4 "$output" | tr -d ' \n')
[[ "$magic" == 7f454c46 ]] || fail "output is not an ELF file: $output"

(
    cd "$output_dir"
    sha256_file "$output_name" > "$manifest_name"
)
printf 'PS5_ELF=%s\n' "$output"
printf 'PS5_SHA256=%s\n' "$manifest"
cat "$manifest"
