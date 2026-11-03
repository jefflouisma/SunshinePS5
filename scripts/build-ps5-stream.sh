#!/usr/bin/env bash
# Build the DIAGNOSTIC full-streaming PS5 console ELF.
#
# Compiles and links only; never deploys, runs or claims a console observation.
# Uses the same pinned SDK/LLVM/OpenSSL as scripts/build-ps5.sh and the audited
# ENet target archive. Output: dist/sunshineps5-stream.elf + .sha256.
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk=${PS5_PAYLOAD_SDK:-${HOME:-/nonexistent}/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk}
openssl_dir=${PS5_OPENSSL_DIR:-/Volumes/4TB_Drive/tmp/t043-t044/openssl-source/openssl-3.5.9}
opus_include=${PS5_OPUS_INCLUDE:-/Volumes/4TB_Drive/tmp/sunshine-t008-t010/opus-archive/opus-1.5.2/include}
opus_lib=${PS5_OPUS_LIB:-/Volumes/4TB_Drive/tmp/t079-opus-build/libopus.a}
enet_dir=${PS5_ENET_DIR:-/Volumes/4TB_Drive/tmp/t043-t044/enet-ps5-strict}
enet_include=${PS5_ENET_INCLUDE:-/Volumes/4TB_Drive/tmp/t043-t044/enet-source/enet-aca87840b57f045a1f7f9299e4b1b9b8e2a5e2f1/include}
output=${PS5_OUTPUT:-$repo/dist/sunshineps5-stream.elf}
llvm_config=${LLVM_CONFIG:-/opt/homebrew/opt/llvm@18/bin/llvm-config}
expected_sdk_commit=a7685ce8e97ce1f8a4f38253a41d54241c9124f1
expected_llvm=18.1.8
target_cc=$sdk/bin/prospero-clang
build_dir=$repo/build/ps5-stream
version=${SP5_VERSION:-$(tr -d '[:space:]' < "$repo/VERSION")}

case "$output" in
    /*) ;;
    *) output=$repo/$output ;;
esac
manifest=${output%.elf}.sha256
output_dir=$(dirname -- "$output")
output_name=$(basename -- "$output")
manifest_name=$(basename -- "$manifest")

fail() {
    printf 'PS5_STREAM_BUILD_FAILED %s\n' "$*" >&2
    exit 1
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1"
    else
        shasum -a 256 "$1"
    fi
}

[[ "$version" =~ ^[0-9A-Za-z.+-]{1,32}$ ]] || fail "invalid version: $version"
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
[[ -s "$opus_lib" ]] || fail "missing target libopus: $opus_lib"
[[ -s "$opus_include/opus.h" ]] || fail "missing target opus header: $opus_include/opus.h"
for library in libssl.a libcrypto.a; do
    [[ -s "$openssl_dir/$library" ]] || fail "missing target OpenSSL library: $openssl_dir/$library"
done
for header in ssl.h crypto.h evp.h pem.h x509.h; do
    [[ -s "$openssl_dir/include/openssl/$header" ]] ||
        fail "missing target OpenSSL header: $openssl_dir/include/openssl/$header"
done
[[ -s "$enet_dir/libenet.a" ]] || fail "missing ENet target archive: $enet_dir/libenet.a"
[[ -s "$enet_include/enet/enet.h" ]] ||
    fail "missing ENet headers (set PS5_ENET_INCLUDE): $enet_include/enet/enet.h"

command -v cmake >/dev/null 2>&1 || fail "cmake is required to embed the web app"
mkdir -p "$build_dir/generated" "$output_dir"
rm -rf "$build_dir/obj"
mkdir -p "$build_dir/obj"

printf 'SP5_VERSION=%s\n' "$version"
printf 'PS5_SDK=%s\n' "$sdk"
printf 'PS5_SDK_COMMIT=%s\n' "$sdk_marker"
printf 'PS5_OPENSSL_DIR=%s\n' "$openssl_dir"
printf 'PS5_ENET_DIR=%s\n' "$enet_dir"
printf 'PS5_TARGET_CC=%s (%s)\n' "$target_cc" "$("$target_cc" --version 2>/dev/null | head -1)"
printf 'PS5_LLVM_CONFIG=%s (%s)\n' "$llvm_config" "$expected_llvm"

assets='index.html|app.js|status.js|devices.js|settings.js|metrics.js|diagnostics.js|stop.js|style.css'
cmake -DSOURCE_DIR="$repo/webapp" -DASSETS="$assets" \
    -DOUTPUT="$build_dir/generated/web_assets.c" -DLIMIT=32768 \
    -P "$repo/cmake/embed-webapp.cmake" >/dev/null
[[ -s "$build_dir/generated/web_assets.c" ]] || fail "web asset generation produced no source"

# Full portable core (every translation unit used by the streaming composition)
# plus the real provisional console adapters and private-link web transport.
sources=()
while IFS= read -r source; do
    sources+=("$source")
done < <(cd "$repo" && find core -name '*.c' -type f | LC_ALL=C sort)
sources+=(
    "$build_dir/generated/web_assets.c"
    platform/main_stream.c
    platform/web_service.c
    platform/system/storage.c
    platform/ps5_media_global.c
    platform/capture/ps5.c
    platform/encode/ps5.c
    platform/audio/ps5.c
    platform/audio/opus_encoder.c
    platform/input/ps5.c
    platform/system/ps5.c
    platform/firmware/13.60.c
    platform/privileged/access.c
)

# The PS5 sysroot's arpa/inet.h needs the sys/socket.h pull-in and strict POSIX
# hides BSD declarations used by the POSIX storage adapter, as in build-ps5.sh.
# The pinned sysroot math.h defines isfinite via a _Generic that qualifies
# volatile cases; clang 18 flags that macro expansion. Suppress only this
# sysroot-macro diagnostic so the project's own -Werror policy stays intact.
cflags=(-std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
    -Wno-unreachable-code-generic-assoc
    -D_POSIX_C_SOURCE=200809L -D__BSD_VISIBLE=1 -include sys/socket.h
    -include "$repo/platform/stream_support.h"
    -I"$repo/core" -I"$repo/platform"
    -I"$repo/platform/capture" -I"$repo/platform/encode"
    -I"$repo/platform/audio" -I"$repo/platform/input"
    -I"$repo/platform/system" -I"$repo/platform/firmware"
    -I"$repo/platform/privileged"
    -I"$openssl_dir/include" -I"$enet_include" -I"$opus_include")
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
    if [[ $source == platform/main_stream.c ]]; then
        source_flags=(-DSP5_DIAGNOSTIC_PROTECTION_CLEAR=1 "-DSP5_VERSION=\"$version\"")
    fi
    "$target_cc" "${cflags[@]}" ${source_flags[@]+"${source_flags[@]}"} -c "$source_path" \
        -o "$object"
    objects+=("$object")
done

printf 'LINK %s\n' "$output"
"$target_cc" "${cflags[@]}" "${objects[@]}" \
    "$openssl_dir/libssl.a" "$openssl_dir/libcrypto.a" "$enet_dir/libenet.a" "$opus_lib" \
    -pthread -lSceUserService -lScePad -lSceRemoteplay -lSceSystemService -lSceNet -o "$output"

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
