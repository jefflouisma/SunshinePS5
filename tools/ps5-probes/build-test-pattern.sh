#!/usr/bin/env bash
# Nonshipping R4 probe; keep outputs in caller-selected scratch space.
set -euo pipefail
if [[ -n ${TEST_PATTERN_TMPDIR:-} ]]; then
    export TMPDIR=$TEST_PATTERN_TMPDIR
elif [[ -z ${TMPDIR:-} && -d /Volumes/4TB_Drive/tmp ]]; then
    export TMPDIR=/Volumes/4TB_Drive/tmp
fi
if [[ -z ${TMPDIR:-} && -z ${TEST_PATTERN_BUILD_DIR:-} ]]; then
    printf 'Set TMPDIR, TEST_PATTERN_TMPDIR, or TEST_PATTERN_BUILD_DIR to a scratch directory.\n' >&2
    exit 2
fi
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
probe_sdk=${PS5_PAYLOAD_SDK:-/Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk}
export LLVM_CONFIG=${LLVM_CONFIG:-/opt/homebrew/opt/llvm@18/bin/llvm-config}
probe_output=${TEST_PATTERN_BUILD_DIR:-$TMPDIR/t033-t034/pattern-build}
mkdir -p "$probe_output"
export TMPDIR=${TMPDIR:-$probe_output}
/opt/homebrew/opt/llvm@18/bin/clang -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
    -DTEST_PATTERN_HOST "$script_dir/test_pattern.c" -o "$probe_output/pattern-host"
"$probe_output/pattern-host"
/opt/homebrew/opt/llvm@18/bin/clang -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
    -DTEST_PATTERN_HOST -DTEST_PATTERN_HDR "$script_dir/test_pattern.c" \
    -o "$probe_output/pattern-host-hdr"
"$probe_output/pattern-host-hdr"
"$probe_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    "$script_dir/test_pattern.c" -o "$probe_output/test-pattern.elf" \
    -lkernel -lSceVideoOut -lSceSystemService
"$probe_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -DTEST_PATTERN_NETWORK "$script_dir/test_pattern.c" -o "$probe_output/network-inventory.elf" \
    -lkernel -lSceSystemService
"$probe_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -DTEST_PATTERN_NETWORK -DTEST_PATTERN_ALLOCATION_PROBE "$script_dir/test_pattern.c" \
    -o "$probe_output/allocation-preflight.elf" -lkernel -lSceSystemService
shasum -a 256 "$probe_output/test-pattern.elf" "$probe_output/network-inventory.elf" "$probe_output/allocation-preflight.elf" "$script_dir/test_pattern.c"

if [[ ${1:-} == --sdl || ${1:-} == --foreground-native || ${1:-} == --foreground-hdr-switch ]]; then
    probe_sdl=${TEST_PATTERN_SDL_PREFIX:-/Volumes/4TB_Drive/tmp/t033-t034/sdl-local}
    test -f "$probe_sdl/lib/libSDL2main.a"
    test -f "$probe_sdl/lib/libSDL2.a"
    if [[ ${1:-} == --sdl ]]; then
        "$probe_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
        -DTEST_PATTERN_SDL -I"$probe_sdl/include/SDL2" "$script_dir/test_pattern.c" \
        -o "$probe_output/test-pattern-sdl.elf" \
        "$probe_sdl/lib/libSDL2main.a" "$probe_sdl/lib/libSDL2.a" \
        -lkernel -lSceSystemService -lSceUserService -lSceVideoOut -lSceKeyboard -lSceImeDialog -lScePad
        shasum -a 256 "$probe_output/test-pattern-sdl.elf" "$probe_sdl/lib/libSDL2.a"
    elif [[ ${1:-} == --foreground-hdr-switch ]]; then
        "$probe_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
            -DTEST_PATTERN_FOREGROUND_NATIVE -DTEST_PATTERN_HDR -DTEST_PATTERN_HDR_SWITCH \
            -I"$probe_sdl/include/SDL2" "$script_dir/test_pattern.c" \
            -o "$probe_output/test-pattern-native-hdr-switch.elf" \
            "$probe_sdl/lib/libSDL2main.a" -lkernel -lSceSystemService -lSceVideoOut
        shasum -a 256 "$probe_output/test-pattern-native-hdr-switch.elf"
    else
        for probe_mode in sdr hdr; do
            probe_flags=(-DTEST_PATTERN_FOREGROUND_NATIVE)
            if [[ $probe_mode == hdr ]]; then probe_flags+=(-DTEST_PATTERN_HDR); fi
            "$probe_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
                "${probe_flags[@]}" -I"$probe_sdl/include/SDL2" "$script_dir/test_pattern.c" \
                -o "$probe_output/test-pattern-native-$probe_mode.elf" \
                "$probe_sdl/lib/libSDL2main.a" -lkernel -lSceSystemService -lSceVideoOut
            shasum -a 256 "$probe_output/test-pattern-native-$probe_mode.elf"
        done
    fi
fi
