# Nonshipping PS5 capability research

These tools support R3/T040–T042 and the separate R4 calibration work. They are
not host adapters or a claim that console streaming is possible. Findings and
unavailable evidence belong in
[research-r3-findings.md](../../specs/001-moonlight-streaming-host/research-r3-findings.md).
Keep generated binaries, downloaded references and raw observations under
`/Volumes/4TB_Drive/tmp/t040-t042/`, never in this directory.

## Source and ABI admission rules

The pinned public SDK is **v0.45**, commit
`a7685ce8e97ce1f8a4f38253a41d54241c9124f1`; LLVM is **18.1.8**. See
[the dependency lock](../../cmake/dependencies.lock.json). An SDK `.so` is an
import/link artifact, not a console implementation. Its generated symbol labels
do not describe parameters, structure sizes, calling permissions, synchronization,
ownership, success semantics or rollback. In particular, several labels at the
same address are not working implementations of those operations. Inspect
[the stub generator](https://github.com/ps5-payload-dev/sdk/blob/a7685ce8e97ce1f8a4f38253a41d54241c9124f1/sce_stubs/genstub.py)
and the relevant `sce_stubs/*.c` before interpreting an import list.

A callable probe needs a cited implementation/declaration, layout and firmware
scope, a known invocation context, permission requirements, bounded execution,
resource ownership and a cleanup observation. Do not cast a discovered symbol to
a guessed function type. A module, symbol or successful ioctl alone is not proof
of the intended capability. Unknown permission must remain unknown; do not escalate
credentials or patch checks to turn an experiment into a success.

**Stock-runtime qualification:** pinned SDK `crt/crt.c` invokes `__patch_init`
before `main`; `crt/patch.c` changes current-process credential capabilities,
attributes and syscall-origin bounds through kernel writes. Thus even a
stat-only SDK-built payload is **not zero-kernel-write evidence**. This probe
adds no such operations, but SDK startup reversal and possible shared-credential
effects are unverified. Exact-hash execution approval must explicitly resolve
this runtime baseline or require an audited runtime/reversal first. Process
exit alone is not proof that every shared effect reverted. See the
[separate media audit](../../specs/001-moonlight-streaming-host/research-r3-media-re.md#process-privileges-and-budgets).

## Candidate map (source review on 2026-10-25)

| Requirement | Sourced candidate | What the source establishes / remaining boundary |
|---|---|---|
| Capture the foreground output | Firmware-13.60 Remote Play / Avcap2 / SceAvCapture live callers; upstream `libSceVideoRecording`, `SceSysAvControl` investigations | Remote Play registers selector `0xb4` through ResourceArbitrator and passes it to Avcap2 source 2; the daemon owns Venc internally. Four owner-approved 30-second SDR samples at 720p30/60 and 1080p30/60 returned Annex-B records through Avcap2 ReadVideo and completed normal teardown. Independent codec/profile, hardware acceleration, encoded dimensions and supported bitrate bounds remain unverified. PSCapture's scan-out ring uses privileged memory operations. |
| Hardware H.264 / HEVC Main10 encoding | Upstream names `libSceVenc`; built-in recording/Remote Play services | No sourced encoder configuration, input/output ownership, completion, permission or cleanup ABI was found in the inspected SDK/references. A library name is not a callable encoder. |
| Stereo game-audio capture | FGG-PlayPods `src/capture.c`; SDL AudioOut backend | The newer reference declares Avcap2 lifecycle/NIDs and raw 48 kHz stereo float reads, tested upstream on 11.60; Ipmi must load first. Firmware-13.60 permissions/protection/cleanup remain unverified. SDL AudioOut only plays this app's audio. |
| Ten-bit presentation | SDL VideoOut v2; ProsperoLight native presentation; existing R4 pattern | Typed presentation and direct-memory lifecycle, plus a ten-bit PQ format reference. Rendering our own buffers does not capture a game or encode Main10. R4 launch/title-attribute permissions are separate from media capture permissions. |
| HDR state and independent mastering/light metadata | ProsperoLight VideoOut output status; `sceSystemServiceGetHdrToneMapLuminance`, `sceVideoOutSysSetHdrMetadata` imports | ProsperoLight declares a 48-byte output-status query on its own VideoOut handle; R4 observed dynamic_range=2. This is display state, not source mastering/light metadata or permission to inspect another title. The luminance name has no admitted ABI here; the HDR metadata function is a setter. |
| Bounded HDR-to-SDR | VideoOut tone/color-conversion imports | No sourced capture-to-encoder conversion ABI, queue/memory bound or measured timing. SDR-to-HDR display conversion is a different operation. |
| Protected video/audio boundary | Remote Play prohibition imports; system capture family | Prohibition setters do not prove an observable current protection state. No safe epoch/flush signal established. Unknown protection cannot authorize reading or forwarding content. |
| Controller injection/removal | SDK `scePadVirtualDevice*` imports; ps5-mcp `/dev/hid` reference | The SDK supplies names only. The newer reference specifies request/report layouts and reports testing on 13.60, but requires assignment and independent functional/cleanup verification on this console. |
| Rumble, motion, touch | SDL PadData/read/vibration; ps5-mcp report | SDL reads local controllers and drives local vibration; this does not establish reverse rumble notifications for a virtual pad. The ioctl reference leaves touch coordinates unmapped; its identity quaternion is not a motion-delivery demonstration. |
| Remote Play / Share Play conflict | `sceRemoteplayGetConnectionStatus`, `sceRemoteplayIsRemotePlaying`, `sceShellCoreUtilGetSharePlayStatus` | Import candidates only; no verified query ABI/permissions or active-transition observation. Do not call a no-argument guess. |
| Rest/stop and foreground-game scheduling | SDK POSIX threads/clocks; R4 normal-exit cleanup | Available thread primitives do not select safe priorities/cores. Rest/wake, game impact, immediate input release and all partial-start cleanup need their own observations. |

## Maintained reference pins

1. [SDK source](https://github.com/ps5-payload-dev/sdk/tree/a7685ce8e97ce1f8a4f38253a41d54241c9124f1):
   `sce_stubs/{genstub.py,libSceVideoOut.c,libSceRemoteplay.c,libSceAudioOut.c,libScePad.c,libSceSystemService.c}`;
   `include/ps5/kernel.h`, `crt/kernel.c`, `crt/rtld_sprx.c` and the installed
   `target/include` POSIX declarations. The public firmware helper reads
   libSceLibcInternal's process parameter; it does not independently attest HEN.
2. [SDL PS5 backend](https://github.com/ps5-payload-dev/SDL/tree/ee4c47dc0d617b3bc8f35108f9956baf228a1322/src):
   `video/ps5/SDL_ps5video.{h,c}`, `SDL_ps5tilemap.c`;
   `audio/ps5/SDL_ps5audio.{h,c}`; `joystick/ps5/SDL_ps5joystick.{h,c}`.
   These define presentation, audio playback and local-pad reading, respectively.
3. [ProsperoLight](https://github.com/LedyBacer/ProsperoLight/tree/a5301b75f62d31a4d590aec9cad473bb26e76a9f):
   `include/ps5_videoout_formats.h`, `src/native_agc_present.cpp` for packed
   R10/G10/B10/A2 PQ/BT.2020 presentation. See
   [R4 calibration](../../specs/001-moonlight-streaming-host/research-r4-calibration.md)
   for the separately approved title-context experiments and their actual results.
4. [PSCapture](https://github.com/fstat13/PSCapture/tree/d292d0fbf7434d1aeda50103911e4d2a18266b85):
   `payload/main.c`, `docs/REVERSE_ENGINEERING.md`, `docs/ENGINEERING_LOG.md`.
   This is a research lead, **not an approved probe**: it changes page-table
   mappings, reads firmware-dependent structures, converts to eight-bit NV12,
   and documents geometry/tearing workarounds and upstream crashes. It supplies
   neither a safe protected-content boundary nor the required hardware encoder.
5. [ps5-mcp](https://github.com/leolobato/ps5-mcp/tree/84c0f7b316772b13cd62e0121c154afc3ed2482c):
   `docs/vpad-abi.md`, `docs/findings.md`, `payload/system_ps5.c`.
   The `/dev/hid` interface is explicitly distinct from ScePadData; accepted
   request headers can hide an incorrect report. Assignment can disconnect the
   physical controller; force-killing can leave a phantom controller until reboot.
   No PS-button or touch-coordinate support is established by this report path.
6. [YetAnotherControllerEnabler](https://github.com/MegaCadeDev/YetAnotherControllerEnabler/tree/3fd0665c045a5631161d63150d7e8b29451222ca):
   `include/pad_data.h`, `src/pad_assign.c`, `src/main.c`.
   This independently documents typed `scePadVirtualDevice*` calls and a 120-byte
   application sample. Its assignment strategy also includes credential changes
   and code injection into another process; those are not approved for these probes.
7. [Kodi PS5](https://github.com/VivaLaVent/kodi-ps5/tree/83a7be649914535d0cb7837eea65f6f7e6eaaa21):
   `overlay/xbmc/platform/ps5/VideoOutInfo.cpp` supplies the 48-byte output-status
   layout/dynamic-range interpretation; `scripts/30-deploy.sh` supplies the
   `attribute |= 0x62000000` title declaration used by the separately approved R4
   renderer. Neither supplies an independent source-mastering metadata getter.
8. [FGG-PlayPods](https://github.com/FGGstore/FGG-PlayPods/tree/d726e1c32568048aae66f3218c0540adc357e408):
   `src/capture.{c,h}`, `README.md`, `TESTING.md`. The branch identifies itself
   as a resident-audio RFC; upstream hardware scope is 11.60. Its Avcap2 audio
   ABI is a concrete lead, with mandatory Ipmi-first loading and explicit
   Stop/Close/Terminate cleanup. No 13.60 permission or protection result is inferred.

## R3 admission diagnostic

`bash tools/ps5-probes/build-r3.sh` builds host and PS5 versions with strict C11
warnings as errors; it does **not** deploy. `R3_BUILD_DIR` must remain under the
external scratch root. The default is `/Volumes/4TB_Drive/tmp/t040-t042/build`.

`capture_encode` defaults to `--describe`: print all mandatory media paths as
unavailable and return **2 / Incomplete**. It implements no guessed media API.
On the console, `--inventory` first checks the SDK firmware helper for 13.60 and
then uses `stat` on seven candidate library paths. It does not load libraries,
resolve/call candidate symbols, open display/capture/audio, allocate media pools,
or add credential/mapping writes beyond the stock SDK startup described above.
A missing file may reflect namespace/permission;
a present file establishes neither an ABI nor permission to call it. It still
returns 2. Unknown/unsupported firmware returns 1; invalid arguments and
host-side `--inventory` return 64. Output failures also return 1.

The raw ELF loader does not transport CLI arguments. The separately named
`capture-inventory.elf` selects the inventory mode at build time. The ordinary
`capture_encode.elf` remains describe-only on an argument-free launch.

This is **partial T041 preparation**. No sustained media, protection, source-HDR
metadata or media-resource cleanup evidence is produced. Console execution of
even this limited diagnostic requires the approved window and exact payload hash.

### T041 audio candidate and Remote Play investigation follow-up

The separate explicit `--audio-five-seconds` mode uses the seven public Avcap2
audio declarations cited in the media audit. The request is fixed to raw stereo
source 2, kind 0, with one 8192-byte sample buffer and 16 opaque metadata bytes.
It checks finite floats and record sizes, counts samples/nonzero values/overruns,
and records Stop/Close/Terminate plus owned-module cleanup results. It never
saves or forwards samples, retries a failed service, changes credentials in main,
or claims a source clock/protection/stereo-order result from those counters.
Its loop is limited to five seconds or 5000 reads with NOWAIT reads and 1 ms
waits; individual service-call bounds and driver allocations remain unknown.
SIGINT/SIGTERM request ordinary cleanup. A cleanup failure stays failed on a
second cleanup call, without reusing a released handle or repeatedly unloading.

`capture-audio-five-seconds.elf` selects that experiment on an argument-free
loader launch; ordinary `capture_encode.elf` still defaults to describe-only.
Host builds refuse the audio mode. The approved 2026-10-25 run on 13.60 returned
239,616 non-silent stereo float32 frames in 5.0009 seconds with clean teardown;
source clock, channel ordering, protection and driver accounting remain unknown.
Its raw results and exact hashes are in the R3 findings.

Five approved exporter generations recovered the Remote Play, Avcap2, Venc and
SceAvCapture live images and linker tables. [The Remote Play research
procedure](re/README.md) documents their bounds and evidence. The recovered
OpenVideo mode-0 contract needs no surface pointers; SceAvCapture owns Venc
internally and returns encoded output through Avcap2 ReadVideo. The separate
`--video-ten-seconds` experiment reproduces that request and validates bounded
Annex-B records without saving or forwarding them. Dashboard, active-game and
audio-first 720p30 attempts returned `0x81950008`. A subsequent audit recovered
the selector registration trigger, request envelope, context-to-request offset
correction and two omitted calls from Sony's OpenVideo caller. Before each
OpenVideo, Remote Play calls `DHFDHdKAr9U` for a four-byte mode-flags result and
`RW88AiwmL+Y` for a 0x20-byte state record. With constructor mode class 2 and
context+0x330 equal to 2, it copies flags to request+0x2a8, selects request+0x2ac
as 6 for flag bit 0, 8 for flag bit 1, otherwise 2, and sets request+0x2bc to
state[0x17] XOR 1. A nonzero state byte at +0x14 or OpenVideo status
`0x81950008` returns to those queries after a 16 ms wait. The corrected source-2 /
selector-`0xb4` mode-0 720p30 candidate was first run under exact-hash owner
approval with the selector present, but all 64 OpenVideo attempts still returned
`0x81950008`; no handle or read was produced. The later corrected candidate now
executes the recovered query sequence. The four
`capture-video-{720p30,720p60,1080p30,1080p60}.elf` autorun variants do not
establish sustained hardware behavior across the required matrix, HDR,
protection, source metadata or conversion. No Remote Play account/login, online
session, daemon injection or guessed video signature is introduced. Two portable
simulated probe suites are registered in CMake and also run by `build-r3.sh`;
those tests never supply hardware evidence. The approved corrected run and its
network/service/process preflight and postflight records are under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/corrected-video-window-20261025-1/`.

A second exact-hash owner-approved run on firmware 13.60 returned success from
OpenVideo and Start, then collected 301 Annex-B records over 10.012160 seconds:
94,856 bytes, 92,430 nonzero bytes, p99 read interval 33,285 microseconds, and
75,792 measured bits per second against a requested 10,000 kb/s. The reported
encoder route is Avcap2 → SceAvCapture → Venc; codec/profile, per-frame identity,
dynamic range, protection, source metadata and driver allocations remain
unverified. Avcap2 Stop/Close/Terminate, ResourceArbitrator STOP/unregister,
selector removal and all three owned-module unloads returned zero. No access
units were stored or forwarded. The payload correctly returned incomplete
because the remaining T041 requirements are open; raw logs and decoded network
snapshots are under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/corrected-video-window-20261025-2/`.

The owner later directly reported that the Settings dashboard remained stable
before, during and after this run, with no visible interruption or abnormal
audio. `build-r3.sh` now also emits distinct 30-second video candidates for the
four SDR combinations; these artifacts are preparation only until each exact
binary is approved and run. No 30-second or additional mode result is implied
by the 10-second 720p30 observation.

An exact-hash-approved 30-second 720p30 run with owner-confirmed unprotected
dynamic gameplay in the foreground returned 900 Annex-B records / 23,405,408
bytes over 30.033111 seconds (6,234,561 measured bits/s at a 10,000 kb/s
request), with p99 read interval 33,303 microseconds, no empty reads and no
overruns. The query flags were `0x144`; capture state reported ready 0 / mode 0.
Avcap2 Stop/Close/Terminate, ResourceArbitrator STOP/unregister, selector
removal, and all three owned-module unloads returned zero. Codec/profile,
hardware acceleration, HDR, source metadata and capture protection remain
unverified; the probe did not save or forward media. Fresh pre/post IPv4/IPv6
route inventories matched the owner-attested isolated link. Raw logs are under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/console-720p30-30s-20261025/`;
the owner observed clean audio, while gameplay playback and UI were a little
choppy during the run. They remained visible/running but did not return to
normal smoothness after the probe terminated despite zero cleanup return codes.
This is an observed incomplete output-state reversal co-occurring with the run,
not proof of causality; it is a potential T045/R5 cleanup item.

The owner then approved a separate 30-second 720p60 run. It returned 1,799
Annex-B records / 14,348,885 bytes over 30.016582 seconds (3,824,255 measured
bits/s at the same 10,000 kb/s request), p99 read interval 16,667 microseconds,
with no empty reads or overruns. Cleanup calls and owned-module unloads all
returned zero. QuickTime remained laggy throughout; after probe termination and
closing the game, the home UI was still choppy. This second observation is also
co-occurring, not proof that capture caused the persistent state change. No
further capture payload ran. The owner chose option B and approved a separate
read-only diagnostics window; the post-lag snapshot found the expected Sony
capture/Remote Play/encoder-adjacent processes sleeping, all four dev-service
ports listening, and the prior isolated console route disposition intact, but
did not expose CPU-use/thread data or identify a cause for the persistent lag.
The window was released after collection. Raw video logs and pre/post route
summaries are under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/console-720p60-30s-20261025/`.
The diagnostics are under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/console-postlag-diagnostics-20261025/`;
the owner then directly chose to resume the remaining 1080p runs despite the
persistent lag. The owner approved each mode in a separate exact-hash window.
The separately approved 30-second 1080p30 run then returned 899 positive
Annex-B records / 9,892,303 bytes (9,848,799 nonzero) in 30.001605 seconds,
with no empty reads or overruns, p99 interval 33,290 microseconds, and
2,637,806 measured bits/s against the declared 10,000 kb/s request. OpenVideo,
Start and ordinary cleanup succeeded; codec/profile and hardware-encoder
provenance remain unverified. The owner later reported normal audio, persistent
lag in the game menu and home UI, and a possible-but-unconfirmed improvement
during gameplay. This continues the observed output lag across the 720p30,
720p60 and 1080p30 sessions despite successful API/resource cleanup; it is
co-occurrence, not proof that capture caused the state. The run’s exact hashes
and pre/post route/service evidence are under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/console-1080p30-30s-20261025/`.

The separately approved 30-second 1080p60 run then returned 1,798 positive
Annex-B records / 565,407 bytes (526,097 nonzero) in 30.002574 seconds, with
no empty reads or overruns, p99 interval 16,582 microseconds, and a measured
150,762 bits/s against the declared 10,000 kb/s request. OpenVideo and Start
succeeded, query flags were `0x144`, capture state was ready 0 / mode 0, and
Avcap2 Stop/Close/Terminate, ResourceArbitrator STOP/unregister, selector
removal and all three owned-module unloads returned zero. Codec/profile,
hardware acceleration, actual encoded dimensions, supported bitrate bounds,
HDR, metadata and protection remain unverified; the much lower measured bitrate
does not establish the requested rate. The owner directly reported that
gameplay, the game menu and home UI were all still laggy, with no audio change
reported. This completes the four-session persistent output-degradation
observation despite zero-return cleanup; it is co-occurrence, not proof that
capture caused the state. A console output-mode change that teardown does not
exit is the owner's current hypothesis, not an independently established
cause; any reboot is a separate owner decision because jailbreak recovery
would be required. Raw evidence and pre/post route/service records are under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/console-1080p60-30s-20261025/`.

The separate `--video-admission-five-seconds` / `capture-video-admission.elf`
tests only the recovered ResourceArbitrator registration lifecycle. It queries
the 32-selector list and refuses registration if Sony's sourced resource ID
`0xb4` is already available; otherwise it registers its callback, requests
`RUNNING`, polls for at most five seconds / 320 queries, then requests `STOP`
and unregisters. A further 64 polls check selector removal. One approved,
owner-observed 2026-10-25 run confirmed the list changed from absent to containing
`0xb4`, then absent again; registration, RUNNING, STOP, unregister, owned-module
unload and process cleanup returned success. It made no Avcap2 media call and
cannot establish video, encoding or HDR capability. The corrected video probe
now holds this registration during its OpenVideo/ReadVideo lifecycle and releases
it after video cleanup. See the dated T041 findings and local raw logs for
provenance. In the first corrected video run, selector admission succeeded but
OpenVideo still returned `0x81950008`. The later state-query correction passed
OpenVideo and produced bounded video records; one 720p30 run does not establish
the complete required capture/encode/HDR/protection behavior.

## T041 completion candidates — rate units, depth and cleanup

The completion follow-up in
[the media RE record](../../specs/001-moonlight-streaming-host/research-r3-media-re.md#completion-follow-up-recovered-venc-rate-and-depth-controls-2026-10-25)
supersedes the earlier missing-depth and bitrate-unit conclusions. Sony's
request `+0x20` selects eight-/ten-bit preset depth and `+0x24` selects AVC/HEVC;
Venc rejects ten-bit AVC. The rate-control fields carry byte counts, so the
current probe converts kbit/s using **125**, correcting the earlier factor
1000. Historical runs retain their actual byte/rate observations and their
original request bytes; they were not verified 10 Mbit/s-ceiling tests.

`build-r3.sh` additionally builds ten-second
`capture-rate-{720p30,720p60,1080p30,1080p60}-{2000,6000,10000}.elf`, and
`capture-hevc-1080p60-{8,10}.elf`. These require separate exact-hash approval; they are not
measured supported ranges or HDR proof. The 2/6/10 Mbit/s test subset is
below the recovered AVC level ceilings of 14/20/20/50 Mbit/s for the four
modes. A chosen test minimum is not a device minimum.

The video probe collects at most eight NALs / 8 KiB: the first parameter NAL
per type and distinct valid mastering-display/content-light SEI NALs (payload
types 137/144), excluding NALs containing only unrelated UUID/timing SEIs.
Mixed-message NALs consume the same quota; overflow prevents a complete metadata
observation. It unescapes SEI payloads
and reports overflow, parameter changes and malformed/truncated collection
separately.
By default it logs those bytes plus one opaque 112-byte metadata record and discards VCL
picture payloads. The opt-in `R3_MEDIA_SAVE_VIDEO` path below retains a bounded population.
FFmpeg's `trace_headers` can independently inspect SPS/VUI
fields from the collected bytes. Parameter-only `ffprobe` can report unknown
dimensions, and a header-only remux may fail because it contains no picture;
neither outcome should be disguised as a successful picture decode. Ten-bit
signaling, source HDR, source mastering metadata and conversion remain separate
observations.

The first two corrected rate trials returned no selected parameter NALs in
ReadVideo, so `R3_MEDIA_HEADERS` is a separate opt-in configuration-header query.
It uses Sony's sourced `VAFgNc0Pfp8` interface (40-byte input / 424-byte output),
validates the codec-tagged SPS/PPS/VPS slots and reconstructs bounded Annex-B
headers. `capture-headers-{avc-720p30,hevc8-1080p60,hevc10-1080p60}.elf` require
new exact-hash approval. Their configured signaling is explicitly unassociated
with the subsequent VCL records; it cannot prove decoded depth or source HDR.
That candidate sequence was held after a mandatory preflight shsrv banner timeout.
The owner's 2026-10-31 authorization subsequently enabled dispatches 6 and 7 below;
these documents themselves grant no console-operation authority.

`capture-cleanup-OPEN_ONLY.elf` intentionally closes a successfully opened video
handle before Start/Read. `capture-cleanup-INVALID_MODE.elf` sets request mode 3,
which the recovered client rejects before IPC with `0x81950001`, then tears
down owned initialization and ResourceArbitrator registration. This is a
controlled client error, not service fault injection. Both use the existing
bounded admission/query/selector-removal lifecycle and require fresh pre/post
IPv4/IPv6 inventories. Individual service-call deadlines remain unknown.

Current code uses capacity-checked byte copies and rejects null/incompatible
function-pointer representations when binding the SDK resolver's flat 64-bit
addresses. No guessed function type, warning suppression, new privilege
fallback or new console API is introduced by those binding checks.

The owner reports slow UI after an owner-performed restart with no probes
running; capture causality is unresolved and the Elgato/QuickTime chain may
contribute. Initial post-restart loader/dev-port checks refused connections.
The coordinator approved the two exact cleanup payloads under the owner's
standing console-testing directive, **queued until explicit recovery go**.
Preparation, approval and local tests are not console results.

### Cleanup hardening and trace (2026-10-31)

Dispatch 6 later reached an intermittent cleanup-completion failure after Avcap2 unload.
The shared module cleanup helper now records attempts before calls, unloads owned modules in
reverse order, refuses uncertain retries and stops dependency unloads after failure. Target
paths flush `R3_<kind>_CLEANUP_BEGIN` and completion records with handles/monotonic timestamps.
Unverified selector removal also blocks unloading. Host tests remain active under `-DNDEBUG`;
the [implementation evidence](../../.omo/evidence/cleanup-lifecycle-20261031/REPORT.md) and
[independent review](../../.omo/evidence/cleanup-lifecycle-code-review.md) record passing checks.
These changes bound orchestration, while individual platform-call completion remains unbounded.

A separately built one-second 720p60/6,000-kbit/s `-O2` trace completed as PID 234 with
60 records / 21,311 bytes over 1,006,144 us. All cleanup returned zero; observed unload spans
were 152 us for Avcap2, 193 us for ResourceArbitrator and 85 us for Ipmi. The first `-O0`
attempt returned no stdout and left no probe process, so it is invalid evidence. Two bounded
ten-second repetitions then completed as PIDs 238/239, each with 600 records, respectively
78,400/81,903 bytes and ResourceArbitrator spans of 204/208 us. Both completed zero-return
cleanup, removed the selector and exited with value 2. The maximum two consecutive repetitions
was reached without recurrence; `service_call_deadline=unknown`. The prior
intermittent hang remains unexplained; this short trace does not establish full restoration,
a supported bitrate bound or T041 completion. See
[dispatch 7](../../specs/001-moonlight-streaming-host/research-r3-findings.md#t041-dispatch-7--cleanup-hardening-and-one-second-trace-2026-10-31)
for exact artifacts, kernel-side observations and postflight limits.

### HEVC10 configuration-header observation (2026-10-31)

The separately reviewed HEVC10 header artifact ran once as PID 254 and returned
600 records / 528,559 bytes over 10,014,875 us, p99 read 16,642 us, and 103 bytes of
VPS/SPS/PPS. FFmpeg 8.0.1 parses Main10, ten-bit luma/chroma, 1920x1088 cropped to
1920x1080, and BT.2020/PQ configuration signaling. Header-to-read-record linkage is
unverified and no pictures were retained. `ffprobe` reports missing picture and
unknown decoded format; header-only remux exits 234. Cleanup returned zero and
PID 254 exited with value 2; the prior intermittent hang remains unexplained.
See [dispatch 8](../../specs/001-moonlight-streaming-host/research-r3-findings.md#t041-dispatch-8--hevc10-configuration-header-observation-2026-10-31)
and its retained parser outputs. T041/T045 remain open; configuration signaling
does not establish decoded Main10, source HDR, protection or supported bitrate bounds.

### Bounded HEVC10 picture export and decode (2026-10-31)

`R3_MEDIA_SAVE_VIDEO` requires `R3_MEDIA_VIDEO` and `R3_MEDIA_HEADERS`. It retains exact
read bytes and record boundaries in a fixed 2 MiB / 1024-record store; invalid boundaries,
read mismatches, overruns and capacity exhaustion invalidate the recording. The
`capture-save-hevc10-1080p60.elf` build emits indexed chunks only after video/resource/module
cleanup and signal restoration. Consumers must validate every record/offset/length, totals
and `R3_VIDEO_EXPORT_END ... complete=1`; truncated output cannot establish a complete capture.
See the [implementation checks](../../.omo/evidence/hevc-vcl-export-20261031/REPORT.md).

The retained-picture build matrix also includes AVC 720p30, 720p60, 1080p30 and
1080p60, plus HEVC eight-bit 1080p60 (`capture-save-<mode>.elf`). All use the same
ten-second window and fixed retention limits. These are locally verified build
configurations; their decoded console populations still require separate runs.
The [matrix verifier](../../.omo/evidence/t041-export-matrix-20261031/verify_export.py)
accepts expected codec, depth, geometry and nominal fps, checks ordered export
totals and cleanup, and strictly decodes into a new evidence directory. Its fps
check compares the request and bitstream timing metadata (including 1000/1001
rates); it does not establish capture cadence. See its
[local verification report](../../.omo/evidence/t041-export-matrix-20261031/REPORT.md).

The reviewed artifact ran once as PID 261. It retained 600 records / 542,607 bytes over
10,014,179 us (433,470 bit/s), p99 read 16,618 us, and 103 bytes of configuration headers.
FFmpeg/ffprobe 8.0.1 independently decode all 600 frames as HEVC Main10, `yuv420p10le`,
1920x1080 (coded 1920x1088), level 150, limited-range BT.2020/PQ signaling, with strict
error handling and zero decode errors. This establishes configuration-header compatibility
with the retained VCL population. Cleanup returned zero, the selector disappeared, PID 261
was absent afterward and fresh HDMI frames showed the home screen. The coordinator observed
all four development services listening. Private raw video is local evidence.

See [dispatch 9](../../specs/001-moonlight-streaming-host/research-r3-findings.md#t041-dispatch-9--retained-hevc10-pictures-and-independent-decode-2026-10-31)
and its [decode record](../../.omo/evidence/t041-hevc10-vcl-20261031/parse-summary.md).
Independent source HDR metadata, protection, HDR-to-SDR conversion, hardware provenance,
supported bitrate/audio bounds, driver accounting and platform-call deadlines remain unverified.
T041/T045 remain open and readiness remains ERROR.

## Controller lifecycle and delivery probes

`input_system` defaults to `--describe` and returns 2 / Incomplete without device
access. `--self-test` checks the documented descriptor/report bytes and native
request sizes, explicitly as **source-layout evidence only**, and returns 0.
Host builds refuse `--four-neutral` with exit 64.

The target's explicit `--four-neutral [--seconds 1..30]` mode uses only the
ps5-mcp `/dev/hid` ABI cited above, plus its sourced UserService declarations.
It initializes its own UserService client, reads the foreground user, opens
`/dev/hid` O_RDWR, creates at most four distinct handles and sends only neutral
reports. The default observation loop is ten seconds with a ten-millisecond
wait between iterations; individual driver calls have **unknown completion
bounds**. SIGINT/SIGTERM request ordinary cleanup. Partial failure neutralizes
every acquired controller before removing the acquired handles in reverse order,
then closes the descriptor, terminates only its own initialized UserService
client and restores signal handlers. Cleanup failures are logged, remaining safe
cleanup proceeds, and operations are not retried indefinitely.

The separately named `input-four-neutral.elf` selects that fixed ten-second
operation at build time for the argument-free ELF loader. Ordinary
`input_system.elf` does not create controllers on a default launch. Inspect the
filename, source and binary hashes before approving either run.

The probe never sends Cross/PS or automates assignment. Creating virtual devices
can show assignment UI or affect physical-controller ownership. Owner readiness
and before/after observation are required. It prints raw ioctl results and
monotonic observation times, but always labels effective delivery and removal
unverified. A normal experiment returns 2 / Incomplete; native/setup/cleanup
failure returns 1. A ten-second neutral loop does not prove held-button release,
four-player game input, rumble, motion/touch delivery, conflict transitions,
rest cleanup or game-safe priorities/affinity. Those remain explicit R3 gaps.

A separate explicit `--navigation-once` mode, and separately named
`input-navigation-once.elf`, reserve **one** owned controller for six seconds.
At approximately two seconds they send sourced D-pad Right (`0x20`) for120ms,
then neutral; at four seconds D-pad Left (`0x80`) for120ms, then neutral. This
mode needs a separate exact-hash/window approval and an owner-observed known
safe home-screen focus with no active game. It never presses Cross/PS/Create or
automates assignment. Cleanup first rebuilds the complete neutral report even
if a navigation submission failed while a direction was held. A visible move
and restoration would corroborate delivery only for that event/controller;
generic ioctl success remains insufficient, and all missing input features
remain unavailable. Host builds refuse this mode. No run is authorized merely
because the additional artifact was built.

`STOP_HELD` is a separately compiled one-controller, six-second diagnostic. It
emits Cross near two seconds, neutral, then holds Right from four seconds until
ordinary cleanup. The shared cleanup path always rebuilds and submits a full
neutral report before reverse-order handle removal; host tests cover every
ownership subset and injected cleanup failure position under ordinary and
`NDEBUG` builds. These tests establish orchestration, not console delivery.

One reviewed 2026-10-31 run completed as PID 275 with 594 accepted reports and
zero-return neutral/remove/HID-close/UserService-terminate records. HDMI showed
the assignment dialog, navigation to Game Library and a controller-disconnected
toast. Because Game Library is the rightmost boundary and exact CamShot exposure
times are unavailable, stable later frames cannot prove held-Right release.
Postflight found PID 275 absent and all four development services available.
See the [device evidence](../../.omo/evidence/t042-stop-held-device-20261031/REPORT.md).
Signal/transport stop, rest/wake, partial failure, feedback/readback, conflicts,
four-player function and game-safe scheduling remain unverified. T042/T045 stay
open and readiness stays ERROR.

`NEUTRAL_OBSERVE` is a separately compiled one-controller diagnostic selected by
`-DR3_INPUT_NEUTRAL_OBSERVE_AUTORUN` (artifact `input-NEUTRAL_OBSERVE.elf`). It
presses assignment Cross for 120 ms at two seconds, stays neutral until twenty
seconds, then holds sourced D-pad Down (`0x40`) until the twenty-one-second
deadline. Ordinary cleanup submits the full neutral report, retains that handle
for a three-second observation, then removes it. This delay is exclusive to this
diagnostic; other modes retain immediate neutral-before-remove cleanup. A setup
or report failure, unsuccessful neutral, failed log flush, or stop request skips
the observation. A stop during observation interrupts it. Clock reversal/error,
wait error or 600 iterations without completion records failure and proceeds to
removal. Each platform call still has an unknown completion bound.

Use continuous HDMI recording with advancing frames, a verified no-active-game
preflight, and a safe long Settings list with focus near its upper interior.
Confirm assignment and menu setup before the Down phase; the twenty-second setup
window requires a supervising operator and does not navigate into Settings.
The initial Cross is the only confirm pulse. Retain footage showing repeated
Down movement, cessation at an interior row while the handle is still registered,
and the later removal. A boundary, missing transition, unresolved assignment,
unexpected UI action or recording loss makes the observation Incomplete. Stop
after one reviewed execution or any cleanup/service failure and retain postflight
process/service/HDMI evidence before another diagnostic.

Flushed `cleanup_neutral_begin`/`cleanup_neutral`,
`neutral_observation_begin`/`neutral_observation_end`, and `remove_begin`/`remove`
records bracket the phases. Observation-end result 1 means its wait completed;
2 means interrupted by stop; -1 means an observation error. They do not establish
console delivery or synchronized camera exposure. Host tests verify every
neutral byte, timeline boundaries, handle ownership throughout the wait, normal
ordering and stop/error bypass under ordinary and `NDEBUG` builds. No device run
or T042 completion is implied by compilation.

## T044 concurrent thread-clock diagnostic

The opt-in `BUDGET_THREADS` build produces `budget-threads.elf`. It creates and
joins two owned workers per round, swaps active/sleep-like roles in the second
round, and brackets each thread-CPU sample with monotonic readings. Work and
start waits have explicit iteration caps; platform calls retain unknown
deadlines. The [implementation evidence](../../.omo/evidence/t044-two-thread-20261031/REPORT.md)
and [independent review](../../.omo/evidence/t044-two-thread-code-review.md)
cover partial creation, failed joins, missing completion, clock reversal,
interval mismatch and sum overflow.

One reviewed [PS5 run](../../.omo/evidence/t044-two-thread-device-20261031/result.md)
on 2026-10-31 completed all four workers. Round 0 measured sleep/active CPU of
1,080,000 / 248,383,000 ns; round 1 measured active/sleep CPU of 248,660,000 /
1,085,000 ns over comparable 250-ms intervals. This establishes concurrent
thread-local distinction for those workers, with the role following workload.
The 249,463,000 ns and 249,745,000 ns sums cover independently sampled intervals.
They do not establish exact common-window aggregation, production thread census,
ten-second host budgets, daemon/driver CPU, media/input populations or game impact.
Postflight showed the normal home screen, four service processes and no diagnostic;
the coordinator separately checked all four service ports. Sender success and
the final Incomplete marker do not provide a separately retained payload PID or
OS exit record. **T044/T045 remain open and readiness stays ERROR.**

## T044 coordinated ten-second diagnostic

The separate opt-in `BUDGET_WINDOW` build produces `budget-window.elf`. After
both owned workers report readiness, the coordinator publishes one monotonic
ten-second interval with a 50-ms preparation lead. Each worker brackets its
thread-CPU reads with monotonic reads, sampling just before the start and after
the end. Admission requires both brackets to enclose the entire target interval,
with at most 5 ms of excess at each edge. Missing, late, reversed or incomplete
samples fail the observation and emit no sum. Successful output retains each
raw bracket, the sum over enclosing CPU intervals, guaranteed common coverage,
edge-skew bounds and total outside-window wall bound. The CPU sum includes the
edge margins; it is not an exact simultaneous ten-second CPU total.

Readiness and completion polling have deadlines and iteration caps. Failure
publishes cancellation and allows a bounded grace; only workers that announce
completion are joined. Platform calls, including joins, retain unknown deadlines.
Worker storage has static lifetime if completion or joining is uncertain. The
diagnostic includes two workers (one active, one sleeping); coordinator CPU,
runtime/driver threads and the production host census remain outside its sum.
It returns 2 / Incomplete on a complete observation, 1 on failure. Compilation
and host tests provide no PS5 clock, workload, resource or readiness acceptance.

## T050 disposable storage and identity diagnostic

`storage.c` is a nonshipping probe of the **exact existing** `core/runtime.c`,
`core/settings/{host,store,devices}.c` and `platform/system/storage.c` implementation.
It adds no production storage adapter. Target compilation uses SDK `a7685ce8`
POSIX headers (`fcntl.h`, `unistd.h`, `sys/stat.h`, `dirent.h`) and pinned OpenSSL
3.5.9 public PEM/X509/EVP interfaces. No target API substitutions were needed.
The SDK's stock privileged CRT bootstrap still applies; the probe adds no hooks,
credential changes, console service calls or network access.

The three modes are intentionally one-shot:

1. `init /data/ps5-dev/t050-<run-id>` creates an exclusive mode-0700 run directory
   and `state` child, writes a new identity, enables metrics, changes the name,
   and inserts one explicitly synthetic registry entry. It runs nine isolated
   recovery fixtures, then reopens the healthy state and emits its public
   certificate SHA-256 fingerprint.
2. A **new process** runs `verify <same-directory> <fingerprint>`, rejects missing
   files before loading, checks the expected identity/settings/registry, disables
   metrics and changes the name again, then closes/reopens and checks persistence.
   A second verification fails because the expected initial settings are gone.
3. `cleanup <same-directory>` inventories the completed run and removes its fixed
   files and directories through held directory descriptors. It requires no
   fingerprint, rejects missing/unexpected entries and refuses repeated cleanup.

`<run-id>` must contain 1–64 ASCII letters, digits or hyphens. All other paths
are refused, including nested children, traversal and empty suffixes. `init`
refuses any existing root. Every ancestor is opened with `O_NOFOLLOW`; the
already-existing `/data/ps5-dev` parent is never created or reconfigured. The
caller must serialize runs. The probe retains its uniquely owned files for
inspection until the explicit cleanup mode runs; it never recursively traverses
unknown entries.

For payload launches without argv, build with `T050_DIRECTORY` and `T050_MODE`
string macros; `verify` additionally requires `T050_FINGERPRINT`, obtained from
the completed init run. A successful target build is **not** authorization to
deploy. The build-validation `target-init.elf`, `target-verify.elf` and
`target-cleanup.elf` use a placeholder directory and all-zero verification
fingerprint; rebuild with the reviewed unique path and actual fingerprint before
a device run. `target-next-init.elf` is the separate diagnostic candidate for
`/data/ps5-dev/t050-20261031-codex6`; it has not been run on the device. The matching
`target-next-cleanup.elf` uses that root. `target-next-verify.elf` is a template
until rebuilt with init's actual public digest using `T050_VERIFY_FINGERPRINT`;
the default all-zero digest intentionally fails verification. Exact compiler/source/library arguments
and SHA-256 hashes are in the [evidence](../../.omo/evidence/t050-storage-probe-20261031/REPORT.md).

The first device attempt failed at store load before any file was visible; see
its [separate record](../../.omo/evidence/t050-storage-device-20261031/REPORT.md).
The optional `SP5_STORAGE_DIAGNOSTICS` build now reports begin/end stages for
initial file opens (including the actual returned descriptor), fstat, safe
type/mode/security checks, read/tail byte counts, close, RSA key generation, certificate construction/signing, PEM
encoding, record packing and each atomic-write boundary. It emits at most 256
trace records plus one truncation marker. Records contain a fixed stage name,
file kind (`0` identity, `1` settings, `2` registry), operation result, reason,
errno and numeric OpenSSL error snapshot. They never contain file paths, record
contents, random values, certificates, keys or pointers. OpenSSL codes and errno
are contextual snapshots and may be stale on success; use the failing stage's
result/reason, not a nonzero errno alone. `T050 load stage=initial status=...`
reports the actual store result before the always-active check.

The second attempt localized the failure to reading an apparently successfully
opened missing identity. Codex3 confirmed the SDK wrappers returned a positive
kernel errno as though it were an FD. Codex4 retains a read-only preflight **before
any mkdir, store load, settings/registry access or key generation**. It confirms
the unique root is absent by inspecting at most 4096 parent entries, then compares
fstatat, openat with the current flags, openat without O_NONBLOCK and absolute open
on the same absent `<root>/state/identity.bin`. It also makes diagnostic-only raw
openat/fstatat comparisons that capture signed RAX and CF in one assembly block.
Optimized target disassembly verifies `setb` (the `setc` alias) immediately follows
`syscall`.

The target-only project wrappers in `platform/system/storage_syscalls.h` now
translate carry-set positive RAX to -1 and `errno` through the SDK's thread-local
`__error()` accessor. Carry-clear values, including a valid descriptor 2, are
preserved. Host calls remain ordinary libc; SDK/global symbols are not overridden.
The storage adapter uses these wrappers for openat/fstatat/renameat/unlinkat.
Probe setup/fixtures additionally use translated mkdirat/link/symlink. Reviewed
SDK SYS constants are statically asserted; the fourth argument uses r10, and the
assembly declares rcx/r11/cc/memory clobbers. Optimized disassembly verifies each
helper's immediate carry capture and the errno relocation.

The preflight emits at most twelve records, separately from the trace cap.
`cf=-1` marks a library wrapper whose carry flag is not captured; the `raw_*`
records contain the actual raw pair on target. Host tests simulate that raw pair
using POSIX return/errno, never execute PS5 syscalls. Descriptor snapshots cover
0–255 before/after each open. A nonnegative apparent FD is closed only if the
inventories are valid, exactly that descriptor is new, no prior descriptor was
lost, and its exact fstat succeeds. Existing aliases and unproven returns are
never closed. Legacy SDK/raw results are observations only. The fixed project
fstatat/openat comparisons must return -1/ENOENT; those checks, root absence and
descriptor safety alone authorize store load. Fixed-wrapper or inventory failure
stops before creating the root. Calls still have unknown target
deadlines, and callers must serialize runs.

Without the macro, production builds evaluate no diagnostic arguments and emit
no trace. `SP5_STORAGE_DIAGNOSTIC_TEST` is restricted to host probe builds and
injects one named failure using `T050_FAULT_STAGE`; target builds reject that
macro. The host driver verifies 26 original failure stages, four read-path faults,
four fixed-preflight/ownership faults and five legacy-result observations. A
separate normal/NDEBUG C test checks carry translation, malformed errors,
successful positive descriptors and thread-local errno isolation. Tests check zero temp leftovers before
rename, the visible identity after a directory-sync failure, and bounded output.
The ordinary production settings/lifecycle suites run without the diagnostic
macro. No identity-generation behavior was changed to speculate about the target
failure: the working T043 path uses explicit entropy polling and EVP context
keygen, while this adapter uses `EVP_PKEY_Q_keygen`, a random serial, a longer
certificate lifetime and private-key PEM encoding. The pinned OpenSSL source
confirms its RSA size argument is `size_t`, matching this adapter.

Healthy state checks include effective UID, directory 0700, files 0600, regular
files with link count one, and exactly three files with no temporary leftovers.
Identity validation parses the private key and certificate in memory, checks
their match and self-signature, and prints **only the public certificate digest**.
Recovery fixtures cover corrupt/truncated identity envelopes, identity absent
while other state exists, corrupt settings, unreadable registry, identity mode
0000, identity symlink/multiple links and directory mode 0755. Damaged identity
must refuse pairing and expose no identity; unreadable registry must authorize
no devices. Corrupt settings retain the identity and registry while returning
defaults and an explicit recovery diagnostic. Fixtures are intentionally damaged
and are not included in the healthy-state permission/file-count audit.

Codex4 reached identity/settings/registry writes and six recovery cases before
the target refused symlink creation with ENOTSUP (45). Codex5 treats **only that
target error** as unsupported: it verifies the expected three-file partial
fixture, atomically renames the decoy back to identity, syncs the directory,
rechecks ownership/modes/link count, and reloads the same identity and registry.
It emits `recovery=identity-symlink unsupported=1 errno=45 restored=1 skipped=1`.
That case is explicitly unmeasured on the target; it is not a symlink-protection
pass. Other errors still fail. Ordinary host runs must perform the real symlink
rejection test; only the host fault seam can simulate the unsupported branch.
Complete cleanup accepts four names with a symlink identity or three names with
a secure regular restored identity. A dangling symlink with a missing decoy is
refused, as are missing/unexpected entries in the restored layout.

The one-off `cleanup-partial-codex4` artifact is separately archived in
[its immutable review package](../../.omo/evidence/t050-partial-codex4-20261031/REPORT.md).
It accepts only `/data/ps5-dev/t050-20261031-codex4` and the precisely reviewed
eight-directory partial layout, with identity absent and the decoy present in
the last fixture. It tolerates mode0000 by reading metadata only, requires
regular owned single-link files, inventories everything before deletion and
uses held FDs/safe wrappers/fsync. It has no recursive or general partial fallback.

Codex5 then reached hard-link creation, which also returned target ENOTSUP/45.
Codex6 accepts only that error for the hard-link fixture, audits the unchanged
secure three-file layout, fsyncs its directory, reloads the same identity and
paired authorization, and emits `recovery=identity-multilink unsupported=1
errno=45 secure=1 skipped=1`. This is an explicitly unsupported case, not a
target multi-link-protection pass. Any other error fails; ordinary host tests
must still create real hard links and observe the adapter's multi-link refusal.
The host seam also tests both unsupported link cases together.

Complete hard-link-fixture cleanup accepts exactly the four-file layout with
regular mode0600 identity and decoy sharing the same inode/device and link count
two, or the three-file layout with secure single-link regular identity. Wrong
types/counts/inodes and missing/unexpected required entries are refused before
deletion. The separately archived
[exact partial codex5 cleanup](../../.omo/evidence/t050-partial-codex5-20261031/REPORT.md)
is bound only to the nine-directory codex5 layout; it is not a generalized
partial-cleanup facility. The codex4 and codex5 archives remain unchanged.

`test_storage_probe` runs init and verify in separate host processes, checks
unchanged identity bytes, and exercises invalid paths, existing and symlinked
roots, missing state, invalid/mismatched fingerprints and repeated verification.
Cleanup scenarios reject unexpected or missing entries and symlinked directories
before mutation, remove the complete fixed tree, and confirm an external file
symlink target survives unchanged.
The registered CMake binary defines `NDEBUG`; checks never use `assert`.
Host runs use `/private/tmp/t050-*` on macOS or `/tmp/t050-*` elsewhere and delete
only their own UUID directory. Run `ctest --test-dir <build> -R '^test_storage_probe$'
--output-on-failure` after building `test_storage_probe_binary`.

This evidence establishes host behavior and target compilation only. It does not
establish PS5 execution, console restart/power-loss durability, confidentiality
against privileged readers, or production key protection. The store's identity
flag checks the record envelope; this probe separately checks the generated PEM
key and certificate. The synthetic registry entry bypasses TLS/pairing proof by
design and cannot establish a real client pairing. T050 remains open pending
the separately reviewed device and protection evidence.

After retaining the results, build the same probe with `T050_MODE="cleanup"`
and the exact same `T050_DIRECTORY`; omit `T050_FINGERPRINT`. Launch it through
the same reviewed payload runner, in a separate process with no concurrent
probe. A successful run emits `T050 mode=cleanup ... removed=1 pass=1` and returns
zero. The retained `target-cleanup.elf` is a build-validation artifact with the
placeholder path, so rebuild it for the actual run ID before use.

Cleanup opens every ancestor and fixed child directory with `O_NOFOLLOW`. It
inventories all ten expected child directories and each expected file before
mutation, requiring the effective owner. It rejects unknown names, missing
entries, directory entries in file slots and symlinked directories. Through
held directory descriptors it unlinks only `identity.bin`, `settings.bin`,
`devices.bin` and the two fixtures' `identity-decoy.bin`; a file symlink is
removed itself without opening its target. It then fsyncs the child directories,
removes only the now-empty fixed children, fsyncs the run directory, removes the
exact run root and fsyncs its parent. It checks that each directory still matches
the opened inode before removal. Host tests confirm that rejected inventories
leave the healthy identity untouched and a symlink target outside the run tree
survives successful cleanup.

Cleanup requires the completed init layout; incomplete runs and unexpected
leftovers fail closed for review. A failure during removal may leave a partial
tree, which subsequent cleanup also refuses. Preserve the failure evidence and
inspect the exact remaining entries. Calls have no established console deadline;
this mode is not a concurrent hostile-writer defense or a secure-erasure claim.
Only the reviewed init/verify/cleanup commands are supported; no shsrv shell
semantics are assumed.

## Interactive input diagnostic (T042)

`input_interactive.c` builds as `input-interactive.elf` through `build-r3.sh`.
It uses the existing loader's interactive standard streams; after the no-active-game
preflight and exact-artifact review, the sender form is
`prospero-deploy -h 192.168.2.2 -p 9021 -i input-interactive.elf`.
The initial device check must be `READY` followed by `1 neutral` and `2 stop`, with
continuous HDMI capture and independent process/removal inspection. Interactive
loader transport is locally prepared but still requires that first device check.

After `READY version=1 ... neutral=accepted next=1`, send one newline-terminated
ASCII command at a time. Sequence numbers start at 1 and increase exactly once:
`1 pulse down`, `2 neutral`, `3 stop`, for example. The pulse allowlist is
`up`, `right`, `down`, `left`, `cross`, `circle`. Each pulse requests 120 ms,
then explicitly submits neutral before emitting its `ACK`; `neutral` always
resubmits neutral. There is no automatic assignment Cross. Inspect HDMI before
each requested action. Raw masks, combinations, PS/Home, extra tokens, CR/NUL,
oversized lines and stale/skipped sequence numbers terminate the session.

The helper limits sessions to 32 commands, 30 seconds per input line and five
minutes total. Nonblocking output has a 250 ms limit per response. Stop, EOF,
malformed input, deadline, SIGINT/SIGTERM, and I/O/API failure all attempt owned
neutral → remove → HID close → UserService terminate, with cleanup performed
before final output. The `DONE` line contains cleanup return codes. A failed
cleanup is reported and never treated as independent proof of removal.
The clock fields are sampled console monotonic milliseconds; capture correlation
requires an independently retained bridge. Individual firmware calls have no
proven hard deadline, so these transport bounds do not bound a hung platform call.

`tests/unit/test_input_interactive.c` exercises literal descriptor/report bytes,
ACK ordering, command refusal, ownership and failure cleanup, real fragmented
socket input, peer disconnect and output backpressure. Its platform callbacks
are synthetic. Local normal/NDEBUG/sanitizer and target compilation evidence is
in `.omo/evidence/t042-interactive-helper-20261031/`. Visual delivery, neutral
release, physical removal, target transport and privileged restoration remain
unverified; T042 remains incomplete.

## Console and capture procedure

Read the local `ps5-jailbreak`, `ps5-dev-services` and `ps5-homebrew-dev` skills.
The user dispatch additionally requires an exclusive **CONSOLE WINDOW REQUEST**
through Orca before *any* console/capture access, including status/log reads.
New payload execution needs separate approval identifying source/binary SHA-256,
exact command, cited interfaces, bounds and cleanup. Owner-assisted actions,
crash/reboot risk, jailbreak recovery, hooks/patches and Mac sudo need an ask.
Do not send a payload merely because it builds.

Verify live IPv4 **and** IPv6 isolation before a run: en9 Mac 192.168.2.1,
PS5 192.168.2.2, console gateway 192.168.2.254 deliberately unreachable, no other
console attachment, no global IPv6/default route. A Mac forwarding flag alone
cannot prove the console's routes. Reuse a separately approved read-only route
inventory if needed; do not change routes, NAT, PF or Internet Sharing.
Historical R4 isolation evidence is not a fresh R3 preflight.

Record firmware, environment, launch context, commands, hashes, raw return/error
values and timestamps. On unresponsiveness, stop and ask; do not retry or re-jailbreak.
QuickTime losing the Elgato preview can be a capture ownership/mode issue and
does not establish a console hang. Do not seize the device from its current owner.
Clean up only resources this probe acquired and verify reversal independently.

An unavailable mandatory media/input path leaves T045 and production-host work
gated. Never substitute HDMI recordings, local presentation, packet ACKs,
source-only declarations, a fixture, or a successful loader transport for the
missing capability or console-delivery observation.

## T050 offline recovery coordinator

`recovery.py` validates supplied recovery observations and exposes an injected
one-shot state machine. Its CLI performs no network, capture, shell or payload
operation:

```sh
.venv/bin/python -B tools/ps5-probes/recovery.py FRESH_OBSERVATIONS.json
```

The successful CLI result is `inspection complete; verification not armed`.
It cannot arm or dispatch either payload. The module's injected transport API is
for reviewed integration and offline tests; no live transport adapter ships here.
Adapters must enforce the supplied 30-second/64-KiB bounds, retain complete
stdout/stderr and exit evidence, and transmit the exact immutable bytes passed
by the coordinator. A host timeout does not prove target termination or cleanup.

The observation bundle binds console `192.168.2.2`, host ID `00E421FDEC6B`, firmware
`13600007`, and preserved root `/data/ps5-dev/t050-20261031-codex6`. It requires
ready discovery, fresh explicit resume assessment, protocol identity for all five
services, no-game/no-writer census, IPv4/IPv6 isolation and ownership, healthy HDMI,
the expected public identity fingerprint, and all ten child directories plus every
file's no-follow metadata (type, owner, permissions, device/inode, link count,
size and on-target digest). Deliberately unreadable test files have mode 0000;
all other files are mode 0600 and directories 0700. Restored unsupported link
fixtures must be ordinary single-link files. The historical web listing lacks
this metadata and is rejected. No identity bytes should be downloaded.

`validate_bundle` and `expected_layout` define the bounded JSON contract; the
unit fixture is a synthetic example, not a usable device observation. Source
SHA-256 values bind supplied operator/adapter assertions, not remote attestation.
Actual resume determination, complete on-target inventory and public fingerprint
acquisition still need reviewed observation tooling. The 120-second freshness
window requires reacquisition when an operator checkpoint takes longer.

The module pins the reviewed verifier and the original cleanup artifact from
their different evidence directories. Each phase requires separate explicit
arming. Journal creation walks directory descriptors without following symlinks,
validates newly created directories and the final journal, and fsyncs every
directory plus its parent before returning. Existing ancestor links are synced
too, covering an earlier interrupted creation. Any failure prevents dispatch.
An exclusive, fsynced intent record then precedes transport; any crash,
timeout, malformed result or missing exit leaves that phase non-retryable.
All future real integrations must use the fixed `recovery.JOURNAL` for this
population. Never remove, replace or relocate it to bypass an ambiguous attempt.
Local tests use separate temporary journals and synthetic payloads.

Cleanup additionally requires retained successful verification and a fresh
postverify inventory that matches the newly supplied cleanup inventory. Final
confirmation requires a later independent exact-root HTTP 404 plus fresh service,
process, isolation and HDMI observations. The coordinator validates these supplied
assertions; its synthetic tests do not establish target durability or physical
cleanup. Failures preserve the journal and target state. T050 remains incomplete.

Offline verification:

```sh
.venv/bin/python -B -m pytest tests/unit/test_ps5_recovery.py
```

## T050 recovery-inspector launcher

The [recovery inspector](recovery-inspector.md) now builds a complete target ELF
using the pinned SDK v0.45 CRT and target OpenSSL SHA-256 implementation. Its launcher
checks exact firmware `0x13600007` before inventory observation. Normal/NDEBUG host
regressions, target ELF inspection and independent code review pass; exact hashes
and verification are in the [launcher report](../../.omo/evidence/t050-recovery-launcher-20261101/REPORT.md).
The CRT performs process-local credential/syscall bootstrap before the gate.
The [exact-loader audit](../../.omo/evidence/t050-relapse-loader-compat-20261101/REPORT.md)
establishes distribution provenance and a byte-identical SDK v0.45 full-CRT verifier
that historically executed through this loader on the same 13.60 console. Five CRT
functions match the inspector after masking only pinned object relocation fields,
supporting the historically exercised full-CRT ABI class. Inspector-specific behavior
for 23 GLOB_DAT imports and one R_X86_64_64 relocation, current-boot loader continuity,
inspector execution/output and independent exit remain unverified. Exact loader build
provenance remains unresolved. The build supplies no inspector deployment evidence.

All seven recovery callbacks (services, boot, process, isolation, HDMI, inventory
and exit observer), the reviewed public-certificate fingerprint path and genuine
pre-rest boot identity remain unbound or unevidenced. Process/game/writer exclusion,
atomic-snapshot guarantees and console behavior remain unverified. No inspector,
verifier or cleanup payload was dispatched. [Discovery at `2026-11-01T01:15:03Z`](../../.omo/evidence/t050-rest-device-20261031/discovery-20261101T011503Z.log)
returned `HTTP/1.1 620 Server Standby` for host `00E421FDEC6B`, firmware `13600007`;
the [rest record](../../.omo/evidence/t050-rest-device-20261031/rest-attempt.md)
also records absent exploit/development-service ports. Preserve
`/data/ps5-dev/t050-20261031-codex6` without reinitializing it before inspection.
T050 remains Incomplete.

## T042 neutral/stop coordination driver

`input_interactive_driver.py` supplies an async, injected coordinator for the
initial transport diagnostic. Executing the file directly refuses device access.
The `run()` API defaults to disabled live execution; an explicitly enabled live
caller must provide fresh preflight, capture, deploy and postflight adapters.
The opt-in command adapters described below can supply capture and postflight;
their live bindings still require an awake console and the same fresh preflight.
The test suite uses synthetic adapters and never accesses the PS5 or HDMI device:

```sh
.venv/bin/python -B -m pytest tests/unit/test_input_interactive_driver.py
```

The preflight requires the exact firmware `0x13600007`, completed T050 cleanup,
no active game/probe, verified IPv4/IPv6 isolation, exclusive capture ownership,
host-monotonic freshness within 60 seconds, and retained source observations.
These are operator/provider assertions, not authenticated remote attestations.
The coordinator pins the reviewed interactive ELF SHA-256
`3f0fa25d8bf4d4e6aee59b0459e507f6ee9c71b51b1027210971ec0607f94f2b`,
retains a read-only byte snapshot, and gives only that snapshot to the deploy
factory. A new output directory is mandatory. Never reuse an interrupted run or
automatically repeat a deployment after uncertain transmission.

The output path must have existing real directory ancestors: parent traversal
and symlinks are refused. Ancestors must be owned by root or the current user;
world-writable ancestors require sticky protection. The immediate parent must be
owned by the current user and not group/world writable. The new run directory
must be mode 0700. Held directory descriptors anchor exclusive creation and
artifact writes. Before capture or deploy can begin, the coordinator fsyncs every
containing directory entry, including the new run directory's parent. Any
creation, ownership, mode or fsync failure aborts before dispatch and preserves
the directory for inspection; retrying that same output path remains refused.

Capture must start first and return a fresh completed segment with successful
decoder output. The coordinator then fsyncs dispatch intent, launches once,
validates exact READY fields including PID, sends only `1 neutral`, checks the
neutral ACK and ordered console timestamps, sends only `2 stop`, checks the
complete successful DONE line, closes stdin and requires EOF plus exit zero.
Every command intent is retained before sending. Extra, reordered or malformed
responses, nonzero cleanup, missing output and uncertain writes stop the sequence.
The default total watchdog is 60 seconds, with a maximum of five minutes; output,
artifact size/count and cleanup waits are bounded. The capture health monitor
continues while transport or postflight operations wait.

The capture provider implements async `start`, `check`, `segment(after_ns)` and
`close`; each `Segment` contains original media bytes, decoder JSON with positive
`frames` and zero `decode_exit`, and host-monotonic start/end timestamps. Segment
intervals must be contiguous from before dispatch through the independently
observed postflight. The provider must establish those timestamps and decode
results from the actual media. The coordinator validates and binds its supplied
bytes; it does not itself decode video or establish a console/HDMI clock bridge.
Postflight must identify the READY PID, observe it absent after DONE, observe
healthy services and retain sources. Capture continues through a completed
segment ending after postflight, then all owned local children must be reaped.

The deploy factory returns an async `read/send/finish/wait/close` transport.
`ProcessTransport.spawn(reviewed_argv)` offers bounded pipe I/O through the
coordinator. TERM/KILL escalation applies only while its direct child remains
live; cleanup stops escalation after observed exit, reaps the direct child and
reports `descendant_cleanup_confirmed: false`. Future live
integration may supply the already documented interactive `prospero-deploy`
invocation. All adapters must cooperate with asyncio cancellation, avoid blocking
the event loop, and clean up acquisitions if their start/factory call is cancelled.
Host-side child cleanup does not prove the payload stopped on the console.

Successful protocol completion publishes `manifest.json` last, binding the ELF,
preflight/postflight sources, command/event records, exact transport bytes and
capture/decode files. Failures preserve partial artifacts and `incomplete.json`
when the quota permits; no success manifest is published. API cleanup returns,
process absence, physical removal, delivery and clock correlation remain separate
fields. Physical removal/delivery/correlation remain unverified even on protocol
success; `hardware_acceptance` is always `Incomplete`. T042 remains unchecked.

### Opt-in capture and postflight command adapters

`input_interactive_adapters.py` implements `run_with_commands()` around this
coordinator. Importing it or executing the file does not access a device. A live
caller must set `enable_device=True`, supply the reviewed ELF, a fresh preflight
with its original source bytes, and every command binding. The wrapper creates a
new private output directory containing `coordinator/` and command artifacts;
existing or symlinked output directories are refused. No retry or recovery is
performed. Complete the preserved T050 recovery/cleanup before creating a live
T042 preflight.

The capture adapter owns one continuous process producing concatenated binary PPM
frames. `avfoundation_argv(ffmpeg, "Elgato 4K X")` builds a literal FFmpeg argument
vector for 1920x1080 at 30 fps, spatially reduced to 320x180 RGB without a frame-rate
conversion filter. Device availability, exclusive ownership and camera permission
must already be established. This helper does not start Terminal.app, claim the
existing camshot service, or terminate a competing capture process. Custom reviewed
capture commands may be supplied if they produce the exact configured P6 geometry.

Each complete frame receives a host monotonic receipt timestamp. After the first
frame, a default 250 ms receipt gap or stall aborts the sequence. Before dispatch,
at least two complete frames must be retained and decoded. Acquisition continues
while the helper and postflight commands run; the final retained segment must end
after postflight. Each segment is limited to 32 MiB and 1,024 frames. At the default
320x180 RGB geometry this is roughly six seconds at 30 fps, so this adapter is for
the short neutral/stop diagnostic. A slower sequence fails its quota instead of
evicting early frames. No pixel or clock calibration is implied by spatial scaling.

`decoder_argv(ffmpeg)` decodes each retained PPM segment through a separate FFmpeg
process with `-xerror`. The adapter requires exit zero, the exact retained frame
count and an end-of-progress marker. Decode metadata includes both command vectors,
device name, frame receipt timestamps, byte hash and capture-version-output hash.
The actual version command output, stdout/stderr, executable hashes and command
timestamps are retained in the outer directory. Use the same FFmpeg build for the
capture, decode and version bindings. This independently checks media decodability;
receipt-time continuity cannot establish source timestamp continuity, an HDMI clock
bridge, absence of repeated source frames, or visible input release. Those limits
remain explicit in every result.

Raw segment candidates and their receipt timestamps are also committed before
decoding under `capture-retained/`. If decoding, helper execution, postflight or
the coordinator fails, these bytes survive independently of coordinator success.
After the capture process has been stopped and reaped, any remaining complete
buffered frames are retained as a final partial batch. A separate diagnostic
decode has a one-second limit and bounded process cleanup; decoder failure never
deletes the raw batch. This finalization follows the coordinator's run watchdog
and cannot restore protocol completion. The capture evidence has a separate
97 MiB quota (three 32 MiB batches plus metadata), while command logs retain their
4 MiB quota. Storage/publication faults are explicit in the outer result; they
cannot skip already-required process cleanup.

`capture-retained/manifest.json` always labels its batches `partial_unverified`
and hardware acceptance `Incomplete`, including successful segment candidates.
It hashes every retained raw, timestamp and decode-result artifact. The outer
adapter manifest hashes this nested manifest, its sources and the coordinator's
outcome file. Both manifests publish last with atomic no-replace visibility;
failed publication preserves available sources. Read the outer retention errors
even when a nested coordinator manifest records a completed protocol: a later
retention failure makes the adapter result incomplete. No capture is silently
promoted to a verified timing, visible-release or hardware observation.

The launch vector must contain exactly one whole argument `{elf}`. The wrapper
replaces it with the coordinator's retained, hash-checked ELF snapshot; no shell
expansion occurs. For the already documented SDK tool, bind the reviewed equivalent
of `[prospero_deploy, "-h", "192.168.2.2", "-p", "9021", "-i", "{elf}"]`.
Supply a separate local launch-tool version/help command. Executable identity,
version output and the final deployment argv are retained before launch.

`postflight_commands` must contain four reviewed read-only command vectors. Each
must exit zero and produce one strict UTF-8 JSON object with integer
`"schema_version": 1`. Duplicate fields and malformed/unsupported output fail the
run. These commands are operator-supplied observation providers, so review their
inspection method and include the raw discovery, service, process and HDMI evidence
in their JSON fields; the adapter does not authenticate assertions or turn them
into hardware acceptance.

| Binding | Required result fields |
|---|---|
| `discovery` | `"ready": true`, `"firmware": "0x13600007"`, based on a fresh console observation |
| `services` | `"healthy": true`, based on fresh checks of the required service ports |
| `process` | `"pid": <exact READY PID>`, `"absent": true`, based on a fresh complete process inspection |
| `hdmi` | `"signal_present": true`, based on fresh HDMI inspection; a no-signal card cannot satisfy it |

A whole `{pid}` argument in a postflight vector is replaced with the READY PID.
All four commands start after DONE/EOF/zero helper exit, and capture continues until
a new completed frame follows the final observation. A command's stdout/stderr is
bounded to 64 KiB each, with a five-second timeout. TERM/KILL escalation applies
only while the direct child remains live, stops after observed exit, reaps that
child and reports descendant cleanup as unconfirmed. Failure, cancellation, quota exhaustion, truncated frames,
decoder disagreement or missing postflight leaves protocol completion false. The
outer `adapter-manifest.json` binds retained command/capture evidence and reports
the coordinator outcome plus any final retention failure; its existence alone is not success. Inspect
`coordinator/manifest.json` or `coordinator/incomplete.json` and all cleanup fields.

Device-free verification uses synthetic subprocesses for command ordering, capture
loss/gaps/truncation, decoding disagreement, malformed postflight, watchdogs,
cancellation and child reaping:

```sh
.venv/bin/python -B -m pytest -q -p no:cacheprovider \
  tests/unit/test_input_interactive_driver.py \
  tests/unit/test_input_interactive_adapters.py
```

The retained local FFmpeg verification generates a synthetic `lavfi` stream and
decodes its actual PPM bytes. It opens no HDMI device and contacts no PS5. Live
camera/TCC behavior, interactive loader transport, concrete console inspection
commands and visible release still need on-device evidence before T042 can pass.

### T050 fixed-endpoint recovery adapter (live observation bindings required)

`recovery_adapter.py` adds opt-in unicast discovery and exact-byte TCP transfer
around `recovery.Coordinator`. Importing or executing the module does no I/O.
There is deliberately no standalone dispatch CLI while the live observers remain
unbound. `Adapter.dispatch("verify", armed=True)` acquires fresh observations
before creating the coordinator journal. `record_postverify()` acquires another
inventory after verified completion; `dispatch("cleanup", armed=True)` is a
separate operation and requires that inventory. The existing coordinator's final
independent root-404/service/HDMI postflight remains required after cleanup.

Discovery sends one request from `192.168.2.1` to `192.168.2.2:9302`, checks the
reply endpoint and the exact host ID/firmware fields, and refuses Standby. The
request is the same as the retained `discover-ps5.pl` (chiaki-ng source commit
`8179a2d1964c75dc32614fd27e51c2381093f539`). Transfer sends only a hash-approved
verifier or cleanup ELF to `192.168.2.2:9021`, with the isolated local address,
one 30-second deadline and a 64-KiB output limit. This follows the installed SDK
v0.45 `bin/prospero-deploy` raw-ELF/stdout protocol. A socket EOF does not prove
process exit. A mandatory independent observer must establish successful exit
for the matching PID, process-start identity, boot identity, nonce, payload hash
and stdout hash. An uncertain send/exit consumes the durable journal intent;
it must never be retried or cleared.

Every acquisition uses a fresh nonce and a private exclusive attempt directory.
The module preserves each raw JSON source, discovery bytes, HDMI bytes, hashes,
an index and the validated bundle. Transfer records include exact raw output or
partial output on failure. Records use exclusive creation, held no-follow
directory descriptors, private permissions and fsync. No keys or file contents
from the console inventory are copied: the inventory consists of metadata and
digests. Source providers must avoid secrets in their returned transcripts.

All of these async bindings are still required and have **no built-in live
implementation**:

- `services`: protocol checks and nonempty transcripts for loader, websrv,
  gdbsrv, klogsrv and shsrv. Open TCP ports alone do not qualify.
- `boot`: fresh resume assessment and the same boot identity as independently
  retained before the controlled rest at `2026-10-31T21:49:25Z`. A missing
  pre-rest identity cannot be reconstructed by copying the current boot ID.
- `process`: fresh evidence that no game and no writer own the target population.
- `isolation`: current IPv4, IPv6 and exclusive-population checks.
- `hdmi`: freshly captured PNG bytes and their digest, plus a reviewed source
  health classification. A PNG signature/hash alone cannot classify NO SIGNAL.
- `inventory`: a complete no-follow inventory for the fixed codex6 root with
  device/inode/owner/mode/link-count/size/digest and the public fingerprint.
  Websrv directory listings cannot establish those security properties.
- `exit_observer`: independent process-exit evidence correlated to the dispatched
  invocation; loader stdout and EOF cannot supply this observation themselves.

Callbacks receive `(nonce, absolute_asyncio_deadline)` and return UTF-8 JSON with
exact fields `nonce`, `console`, `host_id`, `firmware`, `root`, `observed_at` and
`value`. They must acquire new observations and honor cancellation. The exit
callback also receives `(mode, payload_sha256, stdout_sha256)`. Source timestamps
must fall within this acquisition. Field contracts are checked in `collect()`
and `_transfer()`. These trusted callbacks are code bindings, not arbitrary
operator JSON files, and hashes/nonces do not authenticate their assertions.
There is no claim of an autonomous live recovery adapter until these bindings
are implemented, reviewed and validated on the awake console.

Device-free tests:

```sh
.venv/bin/python -B -m pytest -q -p no:cacheprovider \
  tests/unit/test_ps5_recovery.py tests/unit/test_ps5_recovery_adapter.py
```

The retained adapter report is
`../../.omo/evidence/t050-live-adapter-20261031/REPORT.md`. Synthetic callbacks and
sockets demonstrate the gates and mechanics only. T050 remains unchecked.

### Conservative live acquisition components

`recovery_live_callbacks.py` provides opt-in building blocks, with no import-time
I/O. `LiveComponents(evidence, enabled=True)` accepts the adapter's private
`Evidence` recorder. `service_observation(name, deadline)` records bounded HTTP,
RSP, klog and interactive shell diagnostic transcripts; its result always has
`verified: false`. It never supplies a successful aggregate services callback.
Loader observations refuse without opening a socket. RSP uses only `qSupported`
and packet acknowledgement, not attach, execution or breakpoint requests.
These protocol observations do not authenticate the running binaries.

`host_observation(evidence, deadline, enabled=True)` runs literal host commands
for en9, the fixed console route, both forwarding flags and PF NAT rules. It
rejects ambiguous addresses/routes, IPv6, forwarding and nonempty NAT output.
Even admitted host conditions leave isolation/exclusivity false: they cannot
prove the console's configuration or absence of another writer.

`callbacks()` returns refusing bindings for all six observation callbacks unless
a reviewed async HDMI classifier is explicitly provided; `exit_observer` also
refuses. The classifier receives `(png_bytes, exact_device_name, deadline)` and
must return literal `True`. `classifier_review` must equal the SHA-256 of its
actual source file, checked at construction and acquisition. This pins reviewed
source provenance, not its dependencies or mutable runtime environment; review
must cover those too. CamShot `/shot` must replace the prior file inode, produce
a new timestamp and exact supported-device metadata; pre/post hashes and stable
reads are retained. Identical image bytes after genuine replacement are allowed,
but neither PNG syntax nor a digest establishes healthy HDMI.

No production classifier is shipped, so **zero of seven required live callback
proofs become live-ready**. Boot/resume identity, no-game/no-writer, target
IPv4/IPv6/exclusive-writer isolation, no-follow complete inventory, authoritative
per-invocation exit and aggregate service identity remain unavailable. Raw local
transcripts can contain console diagnostics: use the private `Evidence` recorder
and do not publish them. No credentials or shell command interpolation are used. Subprocess cleanup
signals a group only while asyncio reports its leader still running. A known
exit, including nonzero exit, never triggers a group signal; cleanup of surviving
descendants is then unproven. Signal/reap errors annotate the original exception,
and cleanup attempts have a one-second reap budget. This is bounded best-effort
cleanup, not a supervisor that guarantees descendant ownership after leader exit.

Sources: local PS5 development-service operations runbook and CamShot helper;
[GDB query packet specification](https://sourceware.org/gdb/current/onlinedocs/gdb.html/General-Query-Packets.html).
The RSP implementation is a conservative generic protocol diagnostic, not a
claim that the installed PS5 gdbsrv has been verified. Local fixture evidence:
`.omo/evidence/t050-live-callbacks-20261101/REPORT.md`.
