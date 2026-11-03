# R4 native SDR/HDR calibration

**T033 and T034 are complete as implementation/calibration research tasks.**
Native source error, repeatable still/motion evidence and conservative paired delay
bounds are recorded; the owner approved both exact spec/profile refinements and
the final software gates pass. This does not establish release acceptance or an
exact HDMI exposure timestamp.

Known-source native SDR and ten-bit HDR still/motion pictures were acquired on the
physical PS5. HDR required sourced title-capability metadata; simply setting the
console to Always On did not permit a default SDR title to register HDR buffers.
The owner approved the additive SDR median SSIM >=0.95 refinement on 2026-10-25;
the exact HDR objective refinement was also approved and applied. Existing criteria
are not relaxed.

The console and capture device are released: all payload PIDs are absent, every
acquired resource's observed cleanup status passed, temporary files are removed,
normal home UI and macOS UVC/audio ownership were independently verified, and the
installed Launcher identity is unchanged. The owner restored HDR Always On after
SDR work. No firmware, jailbreak, network, package or installed-title change occurred.
These calibration results do not establish stream, gameplay, client-platform or
release acceptance; required missing/unmatched evidence remains Incomplete.

## Environment and evidence authority

Observed 2026-10-25: macOS 27.0.1 arm64, public payload SDK v0.45 commit
`a7685ce8e97ce1f8a4f38253a41d54241c9124f1`, LLVM 18.1.8, Rust 1.93.0,
FFmpeg 8.0.1, client library 0.2.0. The console runbook records firmware 13.60 and the
live shsrv banner reports 13.600.007. No different firmware is assumed supported.

Logical source and capture size is1920x1080; capture mode is nominal 60 fps. The
Elgato4KX raw helper supplies P010 with interval 166666x100 ns. The earlier owner
observation in [T032](research-r4-capture.md) was3840x2160@60 RGB(HDR), Always On,
HDCP Off. For the new SDR/HDR epochs the owner confirmed HDR Off/Always On but did
not transcribe Current Video Output Signal again: the earlier exact HDMI values
are not promoted into new measurements. Native HDR VideoOut status returned0,
dynamic_range2, refresh_code 3; the pinned Kodi source maps code 3 to 59.94Hz.
Mastering/light metadata and exact physical HDMI EOTF were not independently measured.

The source-to-reference chain includes PS5 output composition/scaling, Elgato CSC/
scaling and either AVFoundation SDR capture or raw P010 USB acquisition. The declared
RGB-to-YCbCr conversion oracle below is a software interpretation, not an independently
measured HDMI raster. Native source codes, response, frame IDs and source/API timing
are actual hardware observations; host self-tests are explicitly synthetic.

Direct link remained Mac en9 192.168.2.1 <-> PS5 192.168.2.2 with deliberately dead
gateway 192.168.2.254. Owner attested the sole attachment. Fresh getifaddrs plus
complete IPv4/IPv6 route dumps before/after every guarded run show only eth0's
private IPv4/link-local IPv6 and loopback, that sole IPv4 default and no global
IPv6/default route. Decoder uses pinned SDK rt_msghdr v5/size 152, offsets 8/12,
checked aligned sockaddr/message boundaries, independently target-compiled.
Mac forwarding flags1 were retained; PF/NAT sudo reads were not approved or run.

## Repeatable known-source tool and sourced launch

The nonshipping [source](../../tools/ps5-probes/test_pattern.c) and
[build script](../../tools/ps5-probes/build-test-pattern.sh) are owner-authorized.
Current C source SHA-256 is
`f27d18053f26a07e676c8fe04047416bfe2009781f5df476b3d5850b8530fe0a`.

The independently generated frozen scene contains a 16-bit binary frame ID with
low-four-bit complement in twenty96 px top cells; a neutral ramp, eight solid
neutral/primary/secondary patches, RGB ramps and8 px checker; a32 px moving bottom
bar encodes the same frame ID. Still uses ID0. SDR covers every full-range8-bit
code; HDR covers all 1024 native PQ-coded values packed R10/G10/B10/A2, red low10 bits.
No PQ decode/tone mapping or8-bit intermediate occurs in HDR. The bottom bar's
center and width independently agree with every valid SDR/HDR motion ID (error0 px,
width32), guarding against a hidden top/bottom frame mismatch.

Public sources, pinned before adoption:

- [SDL PS5 backend](https://github.com/ps5-payload-dev/SDL/tree/ee4c47dc0d617b3bc8f35108f9956baf228a1322/src/video/ps5): VideoOut v2 signatures, opaque sizes, direct-memory and tiled addresses.
- [ProsperoLight format contract](https://github.com/LedyBacer/ProsperoLight/blob/a5301b75f62d31a4d590aec9cad473bb26e76a9f/include/ps5_videoout_formats.h): native packed ten-bit PQ/BT.2020 word 0x8100070422000000.
- [Kodi in-place switch](https://github.com/VivaLaVent/kodi-ps5/blob/83a7be649914535d0cb7837eea65f6f7e6eaaa21/patches/ps5-opengl/kodi-additions.py): register SDR then SubmitChangeBufferAttribute2 on owned set 0, with no unregister/re-register fallback.
- [Kodi title capability](https://github.com/VivaLaVent/kodi-ps5/blob/83a7be649914535d0cb7837eea65f6f7e6eaaa21/scripts/30-deploy.sh#L235): HDR format is refused without attribute|=0x62000000. Individual bit meanings and effectiveness on untested firmware are not guessed.
- [Kodi output status](https://github.com/VivaLaVent/kodi-ps5/blob/83a7be649914535d0cb7837eea65f6f7e6eaaa21/overlay/xbmc/platform/ps5/VideoOutInfo.cpp):48-byte status ABI and dynamic-range interpretation.
- [websrv v0.34 loader](https://github.com/ps5-payload-dev/websrv/blob/4d35610914273f8f4fc37af2b1d5bfb069dfba5b/src/ps5/hbldr.c): sourced foreground launch and placeholder files. Its active-app-kill/remount branches were prevented by preconditions; they were not exercised.

The host self-test checks all 2,073,600 tiled addresses for injectivity/bounds and
all native ramp codes, including a literal ten-bit packed-word check. Two16 MiB
buffers retain the measured 8,830,976-byte maximum written extent. Native renderer
uses32 MiB total,500 ms flip wait,900s lifetime and only192.168.2.2:9040 for
still/motion/status/clock/stop. Source-submit timestamps follow drawing and the
maintained store fence. They are not called HDMI exposure timestamps.

The optional SDL surface experiment uses SDL 2.30.12 from the same pinned SDL
commit. Archive SHA-256869cf9d26ae7ff9f73c05762f0a5cc988655d90bd7ce9167c4d4e0d76634dd31
(7,857,142bytes), checked internal symlinks/extraction containment, project-local
build/install only, SDL_WERROR=ON. Local patches bound waits500 ms and require one
returned event; size allocation32 MiB, track successful allocation independently of
its numeric offset, trace cleanup, and guard disabled-HIDAPI preprocessor flags.
No warnings were suppressed. Private source/patch/CMakeCache/build provenance is
retained. Native foreground variants link only maintained SDL2main's exit wrapper,
which invokes sceSystemServiceLoadExec("exit",NULL) after native cleanup.

Every run was separately approved or within the explicitly extended standing
transaction envelope, with final source/payload/script hashes status-reported.
The guarded transaction requires fresh complete isolation/no-BigApp/mount4160
observations, absent temporary paths and an unchanged installed Launcher. It creates
only `/system_ex/app/FAKE00000`, its sce_sys child, and
`/data/homebrew/SunshineR4Pattern-T034`; writes param.json/our eboot.elf and copies the
existing5,215,045-byte signed `/system_ex/app/NPXS40106/eboot.bin` only inside the
console, using the sourced cp/chmod0755. Sony file bytes are not retained in Git.
The exact GET/hbldr request uses path=<our eboot.elf>,pipe=1,daemon=0,cwd=<our app>.
After stream EOF/PID absence/normal UI and postflight, cleanup removes only unchanged
owned files with exact expected directory contents. Unknown state stops and escalates.
No existing file is overwritten, no package installed, no app killed and no remount run.

HDR's separately approved **new temporary** param changes only attribute 1 to
1644167169 (0x62000001), preserving old bits. All other fields, including attribute2=0
and attribute3=4, remain source-identical. Param SHA-256:
`808ac54b58fd7168bed6d8907cdd75063856294aad162d6a57430aab1825ec2e`.
Installed `/user/app/FAKE00000` stays separate, listing/app.pkg unchanged; package
SHA-256604e5b56033bd48dd72b68b6844d72c4d9a730e7ed8ce2577fe4b0d6538a667b.

## Actual hardware progression and cleanup

| Run | Result |
|---|---|
| Initial strict splash guard, PID 136 |0x80940004; exited before video/memory, result1/frames0.|
| Advisory splash, PID 138 |VideoOut opened;64 MiB allocation0x80020023; no map/register/flip; close called, result1.|
| Allocation preflight, PID 143 |128 KiB/1/8/16/32 MiB type3 allocations succeeded/released0 one at a time;64 MiB alone failed; no map/video.|
| Raw32 MiB renderer, PID 146 |9,482 flips completed but remained behind Settings UI; stopped/result0, all acquired cleanup0; no visible oracle claimed.|
| Foreground SDL, PID 154 |Visible8-bit pattern,3,138 frames/result0; released32 MiB and queue0; not a native PQ oracle under Always On.|
| Native HDR initial registration, PID 164 |RegisterBuffers2(HDR)0x80290003; result1/frames0, all acquired cleanup0.|
| Native SDR foreground, PID 175 |Visible native SDR,6,618 frames/result0, all acquired cleanup0.|
| Native HDR in-place/default title, PID 185 |SDR registration0, HDR switch0x80290003; result1/frames0, all acquired cleanup0.|
| Native HDR approved title flags, PID 195 |SDR registration0/HDR switch0/status dynamic_range2;4,462 frames/result0, all acquired cleanup0.|
| Same native HDR repeat, PID 205 |Same success,2,294 frames/result0;211 continuous clock queries/zero errors; all acquired cleanup0.|

SDK errno identifies 0x23 as EAGAIN 35, not ENOMEM 12. The pinned payload SDK has no
matching VideoOut error-name header. The [OpenOrbis error header](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/0a1aaf9dd4a92695538bdeb09fb056d06dd11725/include/orbis/_types/errors.h#L139)
names the same PS4-family0x80290003 as invalid
pixel format; the exact observed PS5 result remains numeric. A loader's successful
transport was never substituted for a failing payload result. Void VideoOutClose
is recorded as called, without an invented return status.

Each normal/error path was checked for process absence and normal UI; each raw
helper exit reported restored=1 and independent IORegistry proved UVC0/UVC1/audio
returned. The only elevated child was approved sudo -n on the installed
`/usr/local/libexec/sunshineps5/uvc_capture` helper. No other sudo ran.

During the requested HDR-Off navigation the owner reported black/unresponsive
preview; all hardware work immediately halted. Saved prior cleanup/normal-UI
observations were reported without denying the new symptom. The owner identified
QuickTime's preview as the issue, restarted it successfully, confirmed HDR Off;
the coordinator verified network/services and lifted the halt before SDR work.
QuickTime can black out after capture takeover or HDMI mode changes; that preview
alone is not a console-hang diagnosis. Owner later confirmed Always On and usable
preview. No reboot, force recovery or jailbreak retry occurred.

## Full-population native source-error measurements

SDR: production `record-reference --seconds 2 --out <private directory>` uses
unprivileged AVFoundation/FFmpeg yuv420p8 BT.709. TMPDIR must be the task scratch
root: an initial missing environment failed symlink-ancestor validation before
capture, then the correctly configured invocations passed. Actual captured counts
are114/119/121 against 120 requested (still/motion/post-stop): deficits6/1/0,
excess0/0/1. Missing requested frames are not blamed on hardware without evidence.
All 233 acquired still/motion frames are scored, without an acceptance cap.

HDR: native helper captures120 frames per population, all complete P010 records,
zero invalid padding in 373,248,000signal samples per capture. Every frame receives
a score or explicit invalid-marker exclusion. Across two still/motion acquisitions,
466/480 frames are source-matched; the 14 startup exclusions stay in the declared
population. These populations cannot certify a 99% whole-run release criterion.

Oracle: exact emitted full-range RGB source, FFmpeg 8.0.1 conversion to declared
limited BT.709/BT.2020 with accurate_rnd+full_chroma_int, native yuv420p8/10 output.
HDR x2bgr10le packing preserves all ten signal bits. The declared HDR transform
predicts white943 rather than an assumed nominal940; dynamic neutral values are
taken from that same oracle. A separately emitted moving source's3,110,400 native
samples exactly match dynamic reconstruction. The preliminary hard-coded motion
white was corrected before the final results below. This is a software conversion
oracle, not independent HDMI metadata/EOTF measurement.

Production compare::score_plane evaluates native Y/U/V, peak 255/1023, SSIM uniform 8x8
stride4, perfect PSNR separately represented as infinity. Neither Gaussian 11x11
SSIM equivalence nor tone-mapped HDR fidelity is claimed. Each scene/repeat is
summarized separately; unmatched frames are never silently discarded from coverage.

| Population | Acquired/scored/excluded | Y SSIM min / median | Median Y / U / V PSNR(dB) |
|---|---:|---|---|
|SDR still|114/114/0|0.986018638 / 0.986024190|40.615957648 / 42.552796942 / 41.639457841|
|SDR motion|119/119/0|0.983504671 / 0.984230476|40.171343331 / 42.551178515 / 41.612559032|
|HDR run195 still|120/116/4|0.990505438 / 0.990508623|43.910056477 / 51.327282929 / 50.650608808|
|HDR run195 motion|120/118/2|0.987742889 / 0.988515247|43.132983653 / 51.320059347 / 50.643659766|
|HDR run205 still|120/116/4|0.990505438 / 0.990514306|43.914973608 / 51.327282929 / 50.650608808|
|HDR run205 motion|120/116/4|0.987767343 / 0.988681761|43.194623057 / 51.321906172 / 50.643728468|

All matched HDR frames contain at least 869 distinct **integer** column medians in
the 1024-code neutral ramp, using an odd141-row median at y 180..320. Across 466matched
frames, maximum neutral error2 codes, maximum backward step1, max solid-patch Y/U/V
median error0/1/1 codes, and0 clipped interior columns in 877,478 guarded observations.
Guards exclude8reference codes at each measured oracle endpoint; patch centers
exclude64horizontal edge pixels. The early frame12 even-row preview's883 distinct
medians included half-code medians and is not substituted for this integer metric.
These necessary native-response observations supplement source-format provenance.

## Delay uncertainty: paired causal bounds, no exact exposure correction

Reference timestamps are complete USB-frame or complete raw FFmpeg-frame receipt,
not independently measured HDMI exposure. Source/Mac before/after phase estimates
are conditional on unmeasured drift and remain diagnostic. Their narrow interval
is not advertised as verified accuracy.

A sound conservative bound instead uses strict event ordering: a Mac request send
precedes its source clock read; if that reading is strictly before a source-submit
timestamp, the send is a lower bound on submit in Mac time. Submit precedes receipt
of its own source-log line, giving an upper bound. No source-clock frequency/phase
conversion or interpolation is needed. Mac microsecond boundaries round outward.
Frame IDs do not repeat; the bottom bar independently confirms matching contents.

| Actual motion population | Bounded/matched | Median lower / upper submit-to-reference bound | Maximum upper bound / interval width |
|---|---:|---|---|
|SDR, before/after queries |119/119|73.840 /1453.456 ms|2452.637 /2383.736 ms|
|HDR repeat,211 continuous queries|116/116|48.033 /80.800 ms|81.991 /33.173 ms|

HDR repeat per-frame lower bounds span32.391..49.102 ms and upper bounds65.257..
81.991 ms. These are paired intervals, not a sum of independent percentiles or a
point delay correction. The wide SDR causal bounds remain honest; a future SDR
repeat can use the continuous-query protocol. No zero uncertainty is manufactured.
HDMI exposure itself is only causally after source submit/before reference receipt;
its exact timestamp remains unknown. Bounds apply to the measured run/mode/clock
epoch only; future/other modes require calibration. They are not an absolute
physical-display-latency pass, and threshold-crossing uncertainty stays Incomplete.

## Acceptance refinements and practical limits

**Owner approved2026-10-25, applied:** SC-014 median luma SSIM>=0.95 over the full
declared compared-frame population, in addition to99%>=0.70 and applicable prior
release regression<=0.01. The exact spec/profile diff was approved before editing,
also recorded in contracts/implementation-decisions.md. It is an engineering target
informed by native source-error results, not a confidence guarantee or additive
SSIM error budget. Per-run calibration and complete coverage remain required.

**Owner approved 2026-10-25, applied:** median luma SSIM>=0.97; at least99% of
declared frames jointly SSIM>=0.95 and Y/Cb/Cr PSNR>=35/40/40dB; perfect planes
satisfy PSNR floors. Frozen known-source ramp/patch comparisons with matching native
reference require>=512 distinct integer column-median levels, backward step<=4 native
codes, neutral/solid-patch median error<=8 codes and zero interior clipping after
an 8-code reference-endpoint guard. The literal SC-013/Assumptions/profile diff
was approved before editing and recorded in contracts/implementation-decisions.md.

All existing actual-HDR, native 10-bit PQ/BT.2020, independent metadata, supported
platform/display, game-set and side-by-side requirements remain. Missing metadata,
invalid format, unmatched required frames or uncertain crossings remain Incomplete.
The current acquired Recording sidecars retain absent calibration identities/explicit
reasons; research bounds are not silently promoted into a universal automatic
capture-correction authority. FR-057/D5's separate owner wording disposition remains
pending for acceptance. T034 is calibration research, not T045 readiness or release
acceptance; no stream/reference host, official-client matrix or gameplay budget is
claimed by these controlled source scenes.

T034 is checked because its requested native source-error and delay-uncertainty
research, repeatable scenes/temporal evidence and approved numerical refinements
are complete. The conservative bounds are the uncertainty result, not an invented
exact exposure correction. Unmeasured exact HDMI exposure, wide SDR timing bounds,
startup exclusions, independent mastering metadata and future mode/platform
coverage remain explicit acceptance limitations. T032's separate owner-deferred
physical-disconnect test is not borrowed as a waiver for T034; this task did not
claim to perform that test. A future console/capture operation now requires a
coordinator-granted window because other workers share the devices.

## T033 implementation and software verification

T033 removes the300-frame sample cap and all-frame RAM retention. Every declared
stream frame is streamed to private JSONL as a score or reason-coded exclusion;
native 8-bit SDR/P010/planar 10-bit HDR retain their signal precision and PQ/BT.2020.
On-disk thumbnails align the same interior population for every 1 ms delay candidate;
static/periodic/boundary/ambiguous optima and unavailable total uncertainty are
explicit. Cadence resolution is separate from calibrated accuracy. Exact nearest-rank
summaries use bounded 4096-value sorted runs, at most 256runs; perfect PSNR stays
infinite through separate counts, never clipped 100dB. Best/median/worst source pairs
remain native. Binary64 SSIM identity bits prevent JSON-roundtrip sample loss.
Scratch/output directories/files use0700/0600, including SDR PNG reservations.

Two previously pending T033 capture-contract cases were activated, failed first on
missing ten-bit API and152/304 scored frames, then passed without weakened assertions.
Regression review also exposed permissions and zero HDR sample pairs before fixes;
now all three native pairs and exact bytes/permissions are verified. T034 had no
pending contract cases to activate. Synthetic fixtures remain labeled as software.

Pinned verification commands (task TMPDIR, Rust 1.93.0 bin first on PATH):

```sh
cargo build --manifest-path tools/moonlight-mcp/Cargo.toml --locked
cargo test --manifest-path tools/moonlight-mcp/Cargo.toml --locked
cargo clippy --manifest-path tools/moonlight-mcp/Cargo.toml --locked --all-targets --all-features -- -D warnings
cargo fmt --manifest-path tools/moonlight-mcp/Cargo.toml --all -- --check
PYTHONDONTWRITEBYTECODE=1 .venv/bin/python -m pytest -q -o cache_dir=/Volumes/4TB_Drive/tmp/t033-t034/pytest-cache tests/unit/test_schemas.py
scripts/check-quality.sh
tools/ps5-probes/build-test-pattern.sh --foreground-native
tools/ps5-probes/build-test-pattern.sh --foreground-hdr-switch
```

All completed gates pass. After the approved SDR amendment:99 schema tests and
166 Rust tests(122 library/1 binary/13 capture/17 evidence/9 protocol/4 runner), zero failures
or ignored cases; the shared worktree includes other workers' completed tests.
Quality entry point exit0. Earlier T033-only gate was136 passes/six unrelated future
cases ignored; those historical counts are not represented as current counts.
Standalone LLVM18 target clang-tidy and warning-as-error builds pass native SDR,
native HDR/switch, network inventory, optional SDL and host SDR/HDR self-tests.
The current source has no warning suppression. After both approved amendments,
build/test/Clippy(-D warnings)/fmt/schema/quality all exit0 again:166 Rust tests,
99 schema tests, no failures or ignored tests. Hardware/visual evidence is not
inferred from software gates.

The build script now honors existing TMPDIR, gives TEST_PATTERN_TMPDIR explicit
override precedence, and uses the external scratch default only when that directory
exists and no caller TMPDIR is supplied. TEST_PATTERN_BUILD_DIR controls output;
if neither scratch nor output is available it refuses rather than creating a
machine-specific mount path. Three actual warning-free target builds verify an
existing TMPDIR, explicit override and external-default selection, all exit0.

## Private evidence, provenance and retention

All raw captures, source/console logs, identifying device information, exact commands,
received source-frame timestamps, per-frame scores/IDs/bounds, native first/last
frames, scripts/builds and checksums remain under
`/Volumes/4TB_Drive/tmp/t033-t034`, private0700directories/0600data, executables0700.
No generated binary/raw image/device serial enters Git. Installed helper is the
previous owner-approved T030/T031 acquisition path; acquisition harness changes
only its private output root. The full accepted native SDR/HDR populations remain
available for reanalysis, with no300-frame cap or silent warm-up trimming.

The original T032 P010 checksum remains
810ed46ddd9ec29d83e56b3491c97a88c4d1c1cd12ecdd5cd9c627d7a12e6932; its population was
not pruned. Initial passive settings/legal-notices captures measured adjacent-frame
variation only(Y medianSSIM0.988064300/0.988273199/0.988510305), not source error.
This task's first two passive UI populations were checksummed/analyzed then bulk
pruned per docs/evidence-layout.md; first/last frames, timestamps, summaries and
retention-manifest.json remain. Those deleted populations cannot support a new
full-frame comparison, and their identity hashes are not replacement calibration.
The full historical narrative is retained privately alongside the consolidated report.
