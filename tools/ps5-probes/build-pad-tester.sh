#!/usr/bin/env bash
# Builds the NONSHIPPING foreground pad tester (pad_tester.c) with the same
# foreground SDL_main glue as the test pattern's --foreground-native build.
# Output: $PAD_TESTER_BUILD_DIR (default $TMPDIR/pad-tester)/pad-tester.elf
set -euo pipefail
if [[ -z ${TMPDIR:-} && -d /Volumes/4TB_Drive/tmp ]]; then
    export TMPDIR=/Volumes/4TB_Drive/tmp
fi
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
probe_sdk=${PS5_PAYLOAD_SDK:-/Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk}
probe_sdl=${TEST_PATTERN_SDL_PREFIX:-/Volumes/4TB_Drive/tmp/t033-t034/sdl-local}
export LLVM_CONFIG=${LLVM_CONFIG:-/opt/homebrew/opt/llvm@18/bin/llvm-config}
output=${PAD_TESTER_BUILD_DIR:-$TMPDIR/pad-tester}
mkdir -p "$output"
test -f "$probe_sdl/lib/libSDL2main.a"
"$probe_sdk/bin/prospero-clang" -std=gnu11 -O2 -g -Wall -Wextra -Werror \
    -I"$probe_sdl/include/SDL2" "$script_dir/pad_tester.c" -o "$output/pad-tester.elf" \
    "$probe_sdl/lib/libSDL2main.a" -lkernel -lSceSystemService -lSceVideoOut \
    -lSceUserService -lScePad
shasum -a 256 "$output/pad-tester.elf"
