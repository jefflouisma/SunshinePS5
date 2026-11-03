#!/usr/bin/env bash
# Build only. Console deployment always needs a separate approved run/window.
set -euo pipefail
r3_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
r3_output=${R3_BUILD_DIR:-/Volumes/4TB_Drive/tmp/t040-t042/build}
case "$r3_output" in
    /Volumes/4TB_Drive/tmp/*) ;;
    *) echo 'R3_BUILD_DIR must be under /Volumes/4TB_Drive/tmp/' >&2; exit 64 ;;
esac
r3_sdk=${PS5_PAYLOAD_SDK:-/Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk}
export LLVM_CONFIG=${LLVM_CONFIG:-/opt/homebrew/opt/llvm@18/bin/llvm-config}
r3_llvm_bin=$("$LLVM_CONFIG" --bindir)
if [[ $("$LLVM_CONFIG" --version) != 18.1.8 ]]; then
    echo 'R3 requires pinned LLVM 18.1.8' >&2
    exit 1
fi
mkdir -p "$r3_output"
export TMPDIR="$r3_output"
for r3_probe in capture_encode input_system; do
    "$r3_llvm_bin/clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
        -DR3_HOST "$r3_root/$r3_probe.c" -o "$r3_output/$r3_probe-host"
    # shellcheck disable=SC2068 # bash 3.2 cannot expand an empty array under set -u
    r3_libraries=()
    if [[ $r3_probe == input_system ]]; then r3_libraries=(-lSceUserService); fi
    "$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
        "$r3_root/$r3_probe.c" -o "$r3_output/$r3_probe.elf" ${r3_libraries[@]+"${r3_libraries[@]}"}
    shasum -a 256 "$r3_root/$r3_probe.c" "$r3_output/$r3_probe.elf"
done
"$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -DR3_MEDIA_INVENTORY "$r3_root/capture_encode.c" -o "$r3_output/capture-inventory.elf"
"$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -DR3_INPUT_AUTORUN "$r3_root/input_system.c" -o "$r3_output/input-four-neutral.elf" \
    -lSceUserService
shasum -a 256 "$r3_output/capture-inventory.elf" "$r3_output/input-four-neutral.elf"
"$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -DR3_INPUT_NAVIGATION_AUTORUN "$r3_root/input_system.c" \
    -o "$r3_output/input-navigation-once.elf" -lSceUserService
shasum -a 256 "$r3_output/input-navigation-once.elf"
for r3_input_mode in controls assign-four; do
    if [[ $r3_input_mode == controls ]]; then
        r3_input_define=R3_INPUT_CONTROLS_AUTORUN
    else
        r3_input_define=R3_INPUT_ASSIGN_FOUR_AUTORUN
    fi
    "$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
        -D"$r3_input_define" "$r3_root/input_system.c" \
        -o "$r3_output/input-$r3_input_mode-once.elf" -lSceUserService
    shasum -a 256 "$r3_output/input-$r3_input_mode-once.elf"
done
"$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -DR3_INPUT_READBACK_AUTORUN "$r3_root/input_system.c" \
    -o "$r3_output/input-readback-once.elf" -lSceUserService -lScePad
shasum -a 256 "$r3_output/input-readback-once.elf"
"$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -DR3_INPUT_TYPED_AUTORUN "$r3_root/input_system.c" \
    -o "$r3_output/input-typed-virtual.elf" -lSceUserService -lScePad
shasum -a 256 "$r3_output/input-typed-virtual.elf"
"$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -DR3_INPUT_STOP_HELD_AUTORUN "$r3_root/input_system.c" \
    -o "$r3_output/input-stop-held.elf" -lSceUserService
"$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -DR3_INPUT_GAME_AUTORUN "$r3_root/input_system.c" \
    -o "$r3_output/input-game-coexistence.elf" -lSceUserService
shasum -a 256 "$r3_output/input-stop-held.elf" "$r3_output/input-game-coexistence.elf"
"$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -DR3_MEDIA_AUDIO "$r3_root/capture_encode.c" -o "$r3_output/capture-audio-five-seconds.elf"
shasum -a 256 "$r3_output/capture-audio-five-seconds.elf"
"$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -DR3_MEDIA_ADMISSION "$r3_root/capture_encode.c" -o "$r3_output/capture-video-admission.elf"
shasum -a 256 "$r3_output/capture-video-admission.elf"
for r3_mode in 720p30 720p60 1080p30 1080p60; do
    case "$r3_mode" in
        720p30) r3_tier=3; r3_fps=30 ;;
        720p60) r3_tier=3; r3_fps=60 ;;
        1080p30) r3_tier=4; r3_fps=30 ;;
        1080p60) r3_tier=4; r3_fps=60 ;;
    esac
    "$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
        -DR3_MEDIA_VIDEO -DR3_VIDEO_TIER="$r3_tier" -DR3_VIDEO_FPS="$r3_fps" \
        -DR3_VIDEO_DURATION_US=10000000 \
        "$r3_root/capture_encode.c" -o "$r3_output/capture-video-$r3_mode.elf"
    shasum -a 256 "$r3_output/capture-video-$r3_mode.elf"
done
for r3_mode in 720p30 720p60 1080p30 1080p60; do
    case "$r3_mode" in
        720p30) r3_tier=3; r3_fps=30 ;;
        720p60) r3_tier=3; r3_fps=60 ;;
        1080p30) r3_tier=4; r3_fps=30 ;;
        1080p60) r3_tier=4; r3_fps=60 ;;
    esac
    "$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
        -DR3_MEDIA_VIDEO -DR3_VIDEO_TIER="$r3_tier" -DR3_VIDEO_FPS="$r3_fps" \
        -DR3_VIDEO_DURATION_US=30000000 \
        "$r3_root/capture_encode.c" -o "$r3_output/capture-video-$r3_mode-30s.elf"
    shasum -a 256 "$r3_output/capture-video-$r3_mode-30s.elf"
done
# T041 completion candidates: corrected byte-rate units, bounded metadata
# collection, and explicit partial-start / rejected-mode cleanup. Build only.
for r3_mode in 720p30 720p60 1080p30 1080p60; do
    case "$r3_mode" in
        720p30) r3_tier=3; r3_fps=30 ;;
        720p60) r3_tier=3; r3_fps=60 ;;
        1080p30) r3_tier=4; r3_fps=30 ;;
        1080p60) r3_tier=4; r3_fps=60 ;;
    esac
    for r3_rate in 2000 6000 10000; do
        "$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
            -DR3_MEDIA_VIDEO -DR3_VIDEO_TIER="$r3_tier" -DR3_VIDEO_FPS="$r3_fps" \
            -DR3_VIDEO_BITRATE_KBPS="$r3_rate" -DR3_VIDEO_DURATION_US=10000000 \
            "$r3_root/capture_encode.c" -o "$r3_output/capture-rate-$r3_mode-$r3_rate.elf"
        shasum -a 256 "$r3_output/capture-rate-$r3_mode-$r3_rate.elf"
    done
done
for r3_depth in 8 10; do
    "$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
        -DR3_MEDIA_VIDEO -DR3_VIDEO_TIER=4 -DR3_VIDEO_FPS=60 -DR3_VIDEO_CODEC=1 \
        -DR3_VIDEO_DEPTH="$r3_depth" -DR3_VIDEO_DURATION_US=10000000 \
        "$r3_root/capture_encode.c" -o "$r3_output/capture-hevc-1080p60-$r3_depth.elf"
    shasum -a 256 "$r3_output/capture-hevc-1080p60-$r3_depth.elf"
done
for r3_checkpoint in OPEN_ONLY INVALID_MODE; do
    "$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
        -DR3_MEDIA_VIDEO -DR3_VIDEO_CHECKPOINT="R3_VIDEO_$r3_checkpoint" \
        "$r3_root/capture_encode.c" -o "$r3_output/capture-cleanup-$r3_checkpoint.elf"
    shasum -a 256 "$r3_output/capture-cleanup-$r3_checkpoint.elf"
done
# Configuration-header query is opt-in and requires a new exact-hash window.
for r3_header_mode in avc-720p30 hevc8-1080p60 hevc10-1080p60; do
    case "$r3_header_mode" in
        avc-720p30) r3_tier=3; r3_fps=30; r3_codec=0; r3_depth=8 ;;
        hevc8-1080p60) r3_tier=4; r3_fps=60; r3_codec=1; r3_depth=8 ;;
        hevc10-1080p60) r3_tier=4; r3_fps=60; r3_codec=1; r3_depth=10 ;;
    esac
    "$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
        -DR3_MEDIA_VIDEO -DR3_MEDIA_HEADERS -DR3_VIDEO_TIER="$r3_tier" \
        -DR3_VIDEO_FPS="$r3_fps" -DR3_VIDEO_CODEC="$r3_codec" -DR3_VIDEO_DEPTH="$r3_depth" \
        -DR3_VIDEO_DURATION_US=10000000 "$r3_root/capture_encode.c" \
        -o "$r3_output/capture-headers-$r3_header_mode.elf"
    shasum -a 256 "$r3_output/capture-headers-$r3_header_mode.elf"
done
# Explicit payload-retention candidates; output is private video data, emitted only
# after owned cleanup. Build only; the parent investigation chooses a run window.
for r3_save_mode in avc-720p30 avc-720p60 avc-1080p30 avc-1080p60 hevc8-1080p60 hevc10-1080p60; do
    case "$r3_save_mode" in
        avc-720p30) r3_tier=3; r3_fps=30; r3_codec=0; r3_depth=8 ;;
        avc-720p60) r3_tier=3; r3_fps=60; r3_codec=0; r3_depth=8 ;;
        avc-1080p30) r3_tier=4; r3_fps=30; r3_codec=0; r3_depth=8 ;;
        avc-1080p60) r3_tier=4; r3_fps=60; r3_codec=0; r3_depth=8 ;;
        hevc8-1080p60) r3_tier=4; r3_fps=60; r3_codec=1; r3_depth=8 ;;
        hevc10-1080p60) r3_tier=4; r3_fps=60; r3_codec=1; r3_depth=10 ;;
    esac
    "$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
        -DR3_MEDIA_VIDEO -DR3_MEDIA_HEADERS -DR3_MEDIA_SAVE_VIDEO \
        -DR3_VIDEO_TIER="$r3_tier" -DR3_VIDEO_FPS="$r3_fps" \
        -DR3_VIDEO_CODEC="$r3_codec" -DR3_VIDEO_DEPTH="$r3_depth" \
        -DR3_VIDEO_DURATION_US=10000000 "$r3_root/capture_encode.c" \
        -o "$r3_output/capture-save-$r3_save_mode.elf"
    shasum -a 256 "$r3_output/capture-save-$r3_save_mode.elf"
done
"$r3_llvm_bin/clang" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DR3_HOST -DR3_MEDIA_TEST "$r3_root/capture_encode.c" \
    "$r3_root/../../tests/unit/test_media_probe.c" -o "$r3_output/test-media-probe"
"$r3_output/test-media-probe"
"$r3_llvm_bin/clang" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DR3_HOST -DR3_INPUT_TEST "$r3_root/input_system.c" \
    "$r3_root/../../tests/unit/test_input_probe.c" \
    -o "$r3_output/test-input-probe"
"$r3_output/test-input-probe"
# Explicit-command interactive helper: build/test only, no deployment.
"$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    "$r3_root/input_interactive.c" -lSceUserService -o "$r3_output/input-interactive.elf"
"$r3_llvm_bin/clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    "$r3_root/../../tests/unit/test_input_interactive.c" -o "$r3_output/test-input-interactive"
"$r3_output/test-input-interactive"
shasum -a 256 "$r3_root/input_interactive.c" "$r3_output/input-interactive.elf"
"$r3_sdk/bin/prospero-clang" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    "$r3_root/re/remoteplay_export.c" -o "$r3_output/remoteplay-export.elf"
shasum -a 256 "$r3_root/re/remoteplay_export.c" "$r3_output/remoteplay-export.elf"
"$r3_llvm_bin/clang" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DR3_RE_TEST -D_DARWIN_C_SOURCE "$r3_root/re/remoteplay_export.c" \
    "$r3_root/../../tests/unit/test_remoteplay_export.c" -o "$r3_output/test-remoteplay-export"
"$r3_output/test-remoteplay-export"
"$r3_sdk/bin/prospero-clang" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DR3_RE_RESOURCE_EXPORT "$r3_root/re/remoteplay_export.c" \
    -o "$r3_output/resource-library-export.elf"
shasum -a 256 "$r3_output/resource-library-export.elf"
"$r3_llvm_bin/clang" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DR3_RE_TEST -DR3_RE_RESOURCE_EXPORT -D_DARWIN_C_SOURCE \
    "$r3_root/re/remoteplay_export.c" "$r3_root/../../tests/unit/test_remoteplay_export.c" \
    -o "$r3_output/test-resource-export"
"$r3_output/test-resource-export"
"$r3_sdk/bin/prospero-clang" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DR3_RE_LOAD_RECORDING "$r3_root/re/remoteplay_export.c" \
    -o "$r3_output/recording-library-export.elf"
shasum -a 256 "$r3_output/recording-library-export.elf"
"$r3_llvm_bin/clang" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DR3_RE_TEST -DR3_RE_LOAD_RECORDING -D_DARWIN_C_SOURCE \
    "$r3_root/re/remoteplay_export.c" "$r3_root/../../tests/unit/test_remoteplay_export.c" \
    -o "$r3_output/test-recording-export"
"$r3_output/test-recording-export"

# T044 clock characterization is opt-in; these commands build only.
for r5_mode in admission idle clock threads window; do
    r5_defines=()
    case "$r5_mode" in
        idle) r5_defines=(-DBUDGET_IDLE) ;;
        clock) r5_defines=(-DBUDGET_CLOCK_CHARACTERIZE) ;;
        threads) r5_defines=(-DBUDGET_THREADS) ;;
        window) r5_defines=(-DBUDGET_WINDOW) ;;
    esac
    "$r3_llvm_bin/clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
        ${r5_defines[@]+"${r5_defines[@]}"} "$r3_root/budget.c" \
        -o "$r3_output/budget-$r5_mode-host"
    "$r3_sdk/bin/prospero-clang" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
        ${r5_defines[@]+"${r5_defines[@]}"} "$r3_root/budget.c" \
        -o "$r3_output/budget-$r5_mode.elf"
    shasum -a 256 "$r3_output/budget-$r5_mode.elf"
done
"$r3_llvm_bin/clang" -std=c11 -O2 -g -DNDEBUG -Wall -Wextra -Wpedantic -Werror \
    -DBUDGET_IDLE -DBUDGET_TEST "$r3_root/budget.c" "$r3_root/test_budget.c" \
    -o "$r3_output/test-budget"
"$r3_output/test-budget"
