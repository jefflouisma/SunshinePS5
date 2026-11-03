# R3 media reverse-engineering research — no qualifying video path established

Recorded **2026-10-25**, target **PS5 firmware 13.60**, supporting T041/T045.
This research track has no tasks.md checkbox. It does not close either task or
change the specification. **Verdict: no evidenced capture + hardware H.264/HEVC
encode interface currently satisfies the requested runtime/homebrew/no-kernel-patch
conditions.** This is a bounded negative finding, not proof of impossibility.

There is a substantial **audio candidate**: the public Avcap2 callers below
specify seven NIDs, request fields and a lifecycle; SoloBridge reports 13.60 use.
The initial research pass did not call media functions. Later T041 follow-ups
record owner-approved audio and four one-off 30-second SDR video samples; none
establishes the full codec/HDR/protection requirements. No proprietary binary,
listing or implementation was added to this repository.

## Evidence grades and boundaries

| Grade | Meaning in this report |
|---|---|
| S | Public source inspected at the exact linked commit; proves what a caller declares/does, not that a firmware accepts it |
| U | Upstream author reports a hardware observation; firmware and limitations retained, not reproduced here |
| L | This worker's local observation, with command and scope; local source checks and FTP failures do not prove media capability |
| H | Hypothesis/search lead; no invocation permitted until its complete ABI and effects are sourced |

A matching name-derived NID establishes a consistent name/hash pair. It does
not establish a module exports it on 13.60. A resolved address would still not
establish argument types, authorization, buffer ownership or successful delivery.

## Sources and exact scope

| ID | Immutable/public source | What was inspected |
|---|---|---|
| SDK | [sdk a7685ce8e97ce1f8a4f38253a41d54241c9124f1](https://github.com/ps5-payload-dev/sdk/tree/a7685ce8e97ce1f8a4f38253a41d54241c9124f1) | `sce_stubs/genstub.py`, Remoteplay/VideoOut/AudioOut stubs, `crt/{rtld_sprx,kernel,nid,crt,patch}.c`; matches [dependency lock](../../cmake/dependencies.lock.json) |
| A1 | [SoloBridge capture caller 71fc2af65e04b188ed364274f58351f8f413fc60](https://github.com/hebiii994/ps5-solobridge/blob/71fc2af65e04b188ed364274f58351f8f413fc60/solobridge/capture.c) | Complete audio caller, typedefs, request, NIDs, read/cleanup; header and main also inspected |
| A2 | [SoloBridge report at the same commit](https://github.com/hebiii994/ps5-solobridge/blob/71fc2af65e04b188ed364274f58351f8f413fc60/README.md) | Explicit 13.60 test claim; full console mix, float32 stereo, 1024-frame records; U evidence only |
| A3 | [FGG-PlayPods capture d726e1c32568048aae66f3218c0540adc357e408](https://github.com/FGGstore/FGG-PlayPods/blob/d726e1c32568048aae66f3218c0540adc357e408/src/capture.c) | Same originating audio interface; [engineering account](https://github.com/FGGstore/FGG-PlayPods/blob/d726e1c32568048aae66f3218c0540adc357e408/README.md) attributes recovery to library/Remote Play disassembly and recorded audio analysis |
| A4 | [FGG resident RFC](https://github.com/FGGstore/FGG-PlayPods/blob/d726e1c32568048aae66f3218c0540adc357e408/docs/RESIDENT-AUDIO-RFC.md) | Current branch is an RFC, with 12.40 owner observations and inherited 11.60 producer conditions; neither is local 13.60 proof |
| A5 | [HearBridge avcap2 caller 5da0744d28fe360d6cb236999800e8a4de042167](https://github.com/X-F1REBALL-X/HearBridge-PS5/blob/5da0744d28fe360d6cb236999800e8a4de042167/src/avcap2.c) | Same NIDs, signed 64-bit opaque handle representation, metadata initialization, explicit privilege fallbacks; [creds.c](https://github.com/X-F1REBALL-X/HearBridge-PS5/blob/5da0744d28fe360d6cb236999800e8a4de042167/src/creds.c) inspected |
| A6 | [SoundBridge caller 050fe4fd32389312aa4865bce4c9d7fbdc7dd7aa](https://github.com/Xnos666/SoundBridge-PS5/blob/050fe4fd32389312aa4865bce4c9d7fbdc7dd7aa/src/avcap2.c) | Derived from HearBridge; comments identify 0x2000-byte read requirement on 13.60; not an independent ABI derivation |
| V1 | [PSCapture RE d292d0fbf7434d1aeda50103911e4d2a18266b85](https://github.com/fstat13/PSCapture/blob/d292d0fbf7434d1aeda50103911e4d2a18266b85/docs/REVERSE_ENGINEERING.md) | Recording/IPMI dead end, scan-out mapping method, module names and upstream daemon offsets; [engineering log](https://github.com/fstat13/PSCapture/blob/d292d0fbf7434d1aeda50103911e4d2a18266b85/docs/ENGINEERING_LOG.md) and payload inspected through existing T040 material |
| W1 | [PSDevWiki Libraries](https://www.psdevwiki.com/ps5/Libraries) | Module identifiers, not signatures; secondary corroboration of the primary SDK table |
| W2 | [PSDevWiki Auth IDs](https://www.psdevwiki.com/ps5/Auth_IDs) | Paths/SELF identities for Avcap2 families; library auth ID is not evidence the caller must assume it |
| F1 | [Media Gallery 108d8119dc0aae40cef7685488baba6e322b26e3](https://github.com/Yahballon/PS5-Media-Gallery-Payload/blob/108d8119dc0aae40cef7685488baba6e322b26e3/README.md) | Browses existing captured files; no live encoder API demonstrated |

The ps5-payload-dev organization repository inventory, SDK source tree, these
public source trees, PSDevWiki and web/GitHub repository searches were checked.
Searches for `sceAvcap2OpenVideo`, `sceVenc`, `libSceVenc`, PS5 hardware video
encoder and PS5 libdoc did not yield a typed, verified video encoder caller in
this search. Search indexing and repository descriptions are incomplete; no
exhaustive absence claim is made. Mirrors and emulator placeholder implementations
were not accepted as firmware ABI evidence.

## Local module search and exclusive console window

**L:** `rg --files` searched `/Volumes/4TB_Drive` and the three PS5 skill trees,
filtering module/dump/ELF names. The inventory contained 29,199 matching paths;
37 matched the selected media-name filter. Results include PS4 11.00 system
modules and copies used by shadPS4, SDK import libraries, public source and
unrelated FFmpeg encoder-dump tools. No existing PS5 Avcap2/Venc/SceSysAvControl
decrypted module was identified. This filename search respects ripgrep's ignore
rules and is not an assertion about every byte or archive on the volume.

Two inspected examples were
`/Volumes/4TB_Drive/shadps4-macos-sdl-0.12.5/sys_modules/libSceAvcap.sprx`
and `/Volumes/4TB_Drive/Games/PS4/PS4-Firmware-sys_modules11.00/sys_modules/libSceVideoRecording.sprx`.
`file` identifies ELF64 x86-64/FreeBSD. Their paths/provenance identify PS4,
not target-firmware PS5 modules. They cannot establish the new Avcap2 ABI.
A bounded `rizin` metadata query on the first yielded no imports/symbols, with
unsupported-dynamic-entry/section warnings; no successful ABI analysis is claimed.
The SDK `.so` files are generated import stubs, not decrypted Sony implementations.

Coordinator explicitly granted the `CONSOLE WINDOW REQUEST` for anonymous,
read-only FTP on 192.168.2.2:1337, maximum five minutes. Grant confirmed firmware
13.60 and attributed current IPv4/IPv6 isolation to the T034 worker's checks:
eth0 192.168.2.2 plus link-local IPv6, deliberately dead IPv4 default
192.168.2.254, no IPv6 default/global address. This worker did not independently
rerun those on-console checks. LIST `/system/vsh` returned `app` and
`SceShellCore.elf`; LIST `/system_ex` returned its top-level directories.

| Exact fresh-connection RETR | Observed outcome |
|---|---|
| `/system/common/lib/libSceAvcap2.sprx` | Timeout; zero bytes |
| `/system/common/lib/libSceVenc.sprx` | Timeout; zero bytes |
| `/system/common/lib/libSceAvcap2mvr.sprx` | Timeout; zero bytes |
| `/system/common/lib/libSceVideoRecording.sprx` | Timeout; zero bytes |
| `/system/common/lib/libSceAudioSystem.sprx` | FTP 550; zero bytes |
| `/system/common/lib/libSceRemoteplay.sprx` | Timeout; zero bytes |
| `/system/priv/daemon/SceSysAvControl.elf` | FTP 550; zero bytes |

An initial attempt reused a connection after its first timeout; later operations
on that connection were invalid and are not independent file observations. The
fresh-connection pass above took 30.07 seconds and explicitly closed every
connection. Partial empty files were removed locally. A status message released
the window. No payload, console write, decryption mode, process attachment,
service start or setting change occurred. A timeout/550 does not prove absence,
encryption or a privilege restriction; no magic/hash of a complete PS5 module
can be reported. The manifest stays at
`/Volumes/4TB_Drive/tmp/r3-media-re/console-readonly/manifest.json`.

No new decrypted PS5 input was available for Ghidra. Headless Ghidra 11.4.2 was
located but not run; no GUI, speculative decompile or PS4-to-PS5 signature
transfer was used. Future dumps and projects must stay in this scratch tree.

## Candidate map

| Candidate | Sourced identifier and role | Interface/sequence evidence | Reachability and disposition |
|---|---|---|---|
| Avcap2 audio | SDK `libSceAvcap2.sprx`, module ID `0x80000085`; A1–A6 NIDs below | S complete public caller; U 13.60 report; L owner-approved five-second run with 239,616 non-silent stereo float32 frames | Partial local evidence; protection, timestamps, permissions and attributable budget remain unknown |
| Avcap2 video / Avcap2mvr | SDK Avcap2 ID above; firmware-13.60 Remote Play, Avcap2 and SceAvCapture images | Recovered `sceAvcap2OpenVideo` NID `4jwUgirLFrA`, mode-0 request and ReadVideo lifecycle; L one 720p30 run returned 301 Annex-B records | Partial path demonstrated; codec/profile, per-frame identity, bitrate bounds across modes, HDR and protection remain unverified |
| Hardware Venc | SDK `libSceVenc.sprx`, module ID `0x80000084`; SceAvCapture internal call path | SceAvCapture invokes Venc preset/configuration before Avcap2 ReadVideo; L one video run reached that path, but output codec/profile and hardware acceleration are not independently verified | No direct normal-process Venc client contract or Main10 evidence; one service-owned path is measurable |
| System AV daemon | V1 `SceSysAvControl.elf`; upstream `ScaninControl` and `getScaninBufferOffset` labels | Upstream geometry/control analysis, not a public capture/encode contract | Privileged daemon context; no valid local binary or cross-process service ABI |
| Recording/broadcast | W1 `libSceVideoRecording` public ID `0x85`, `libSceGameLiveStreaming` `0x81`; V1 recording investigation | Upstream file-backed session / IPC failure, no complete service-client invocation | Cannot read a low-latency hardware bitstream via a sourced homebrew API |
| Remote Play / Share Play | SDK Remoteplay control exports; `libSceRemotePlayClientIpc.sprx` `0x800000b9`, `libSceShareInternal.native.sprx` `0x8000008e` | Control/status names only, not frame/audio/encoder endpoints | Neither offline/PSN-free service startup nor bitstream access established |
| HDR output state | SDK `sceVideoOutGetOutputStatus`, R3/R4 public typed caller | Existing own-output status evidence; see below | Partial display-state lead; foreign-title source metadata still unknown |
| Screenshots / JPEG | W1 `libSceScreenShot` `0x9c`, `libSceJpegEnc` `0x8b`; V1 JPEG investigation | No qualifying continuous H.264/HEVC service contract | Still images/JPEG are not the required negotiated video codec |
| AudioSystem / AudioIn / AudioOut | SDK AudioSystem ID `0x80000083`, AudioIn ID `0x80000002`; AudioOut typed playback callers | AudioOut plays this process's samples; no game-loopback ABI from these names | Avcap2 is the sourced mix-capture lead; other names do not establish loopback |

Module IDs are not NIDs, process privileges, or authorization grants. In
particular public `0x85` and internal `0x80000085` identify different entries.
The Avcap2 mode-0 API and selector-registration path are recovered from Sony
callers and were exercised on the owner console. Direct Venc and VideoRecording
client signatures remain unknown; no guessed call is promoted here.

## Avcap2 audio: argument evidence and unresolved ABI details

**S:** A1's declarations describe the following ABI *shape*. These are research
observations, not a production header or approval to call them:

| Symbol | NID | Public caller's signature shape and evidence for arguments |
|---|---|---|
| `sceAvcap2Initialize` | `svzPXluOz8U` | A1 `int(void)`; called without arguments before OpenAudio |
| `sceAvcap2Terminate` | `gojqghDU+1Y` | A1 `int(void)`; used after closing owned handles or partial-start cleanup |
| `sceAvcap2OpenAudio` | `3nHpE7Dp5SE` | A1 `int(void **out, const void *request, uint8_t kind)`; argument 1 is address of initially null handle storage; argument 2 points to the 56-byte request; argument 3 is zero (`KIND_AUDIO`) |
| `sceAvcap2Start` | `8CGNCwBsItI` | A1 `int(void *handle)`; receives OpenAudio's opaque result, never dereferenced by caller |
| `sceAvcap2ReadAudio` | `WhsHggqdwcg` | A1 `int(void *handle, void *buffer, size_t capacity, void *info, uint32_t flags)`; live handle, writable sample buffer, byte capacity, 16-byte writable metadata storage, `flags=1` nonwaiting read |
| `sceAvcap2Stop` | `Z5dKK0xnQ8g` | A1 `int(void *handle)`; after successful Start, before Close |
| `sceAvcap2Close` | `tR7gPe1i8hw` | A1 `int(void *handle)`; closes an acquired handle even if Start failed |

A5 instead declares handle storage as `int64_t *`, handles as `int64_t`, and
OpenAudio's third argument as `int`. Those share the observed 64-bit handle
representation/zero argument on x86-64; they do **not** prove pointer semantics
or the full accepted width/domain of the third argument. Only zero is evidenced.
A5 initializes metadata's first u32 to `0x10`; A1 passes an uninitialized 16-byte
array. Input-versus-output semantics, other metadata fields, timestamp units and
source epochs therefore remain unestablished. A future call needs this resolved,
not a choice of whichever declaration happens to compile.

The request's widths/offsets come from A1's struct and size assertion, with
independent matching byte writes in A5:

| Offset | Width | Value used | Evidence/meaning limit |
|---|---|---|---|
| `0x00` | u64 | `0x38` | Total request size in both callers |
| `0x08` | u64 | `0x18` | Both callers; field meaning unknown, not labeled a version by inference |
| `0x10` | u32 | `0` | Cleared by callers; meaning unknown |
| `0x14` | u32 | `2` | A1/A3 identify Remote Play mix; A3 reports comparison with recording source 0 and inspection of Remote Play's caller |
| `0x18` | u32 | `0` | A1/A3 identify raw stereo float mode; other modes not admitted |
| `0x1c` | u32 | `0xffffffff` | Both callers; meaning unknown |
| `0x20` | u64 | `0` | Cleared; meaning unknown |
| `0x28` | u32 | `0` | A1 labels bitrate for encoded modes; units/range not verified |
| `0x2c` | u32 | `0` | Padding/reserved in A1; no alternate use established |
| `0x30` | u64 | `0` | Cleared; meaning unknown |

These sources are related implementations, not four independent experiments.
The offline audit recomputes all seven NIDs using the SDK's published name-hash
scheme and checks the request size/offsets. That validates transcription only.

**Publicly reconstructed service call sequence:** load
`/system/common/lib/libSceIpmi.sprx` first, then Avcap2; resolve all seven NIDs;
Initialize → OpenAudio(request above, kind zero) → Start → repeated ReadAudio →
Stop → Close → Terminate. A3 attributes the request to Remote Play's own caller
and the error interpretation to library disassembly. This is an **upstream
reconstruction**, not this worker tracing Sony's 13.60 process. The service's
internal dispatch, authentication, IPC transaction numbers and allocator
sequence have not been independently recovered.

A1 validates nonnegative read results as byte counts no larger than capacity and
multiples of eight (stereo float32). Its empty/overrun codes are `0x81950002` and
`0x81950004`; `0x81950008` indicates ended session. A5 additionally treats
`0x81950001` as invalid/sticky and has restarts/fallbacks. A6 flags 0x1000-byte
reads as invalid on 13.60 and uses 0x2000. These remain sourced semantics requiring
local confirmation. Overrun is a coverage gap, not successful uninterrupted audio.

## Process privileges and budgets

A1's capture path/main contains no explicit credential-elevation operation.
A2 reports use as a resident payload on 13.60; A1 resolves addresses using
`kernel_dynlib_resolve` in the current process. The pinned SDK implementation
walks kernel module metadata with `kernel_copyout` and then returns a mapped
user address. Thus the evidence concerns an existing jailbroken payload context,
not an ordinary unprivileged title or an arbitrary foreground game's process.
There is no evidence that merely loading the module confers video access.

A5 tries unchanged auth ID first, but on failure tries `0x4900000000000002` and
`0x4801000000000013`. Its `creds.c` writes auth ID and fills capabilities with
ones. **Do not adopt those fallbacks.** An Avcap2 library SELF auth ID in W2
does not justify impersonating it. Bluetooth/USB handling in these full programs
is outside the media probe and must not be executed as a capture shortcut.

There is a second, significant qualification: pinned SDK
[`crt/crt.c`](https://github.com/ps5-payload-dev/sdk/blob/a7685ce8e97ce1f8a4f38253a41d54241c9124f1/crt/crt.c)
invokes `__patch_init` before main, and
[`crt/patch.c`](https://github.com/ps5-payload-dev/sdk/blob/a7685ce8e97ce1f8a4f38253a41d54241c9124f1/crt/patch.c)
writes the current process's kernel credential capabilities/attributes and
syscall-origin bounds. Consequently:

- No added media hook/page-table patch is visible in A1's audio caller.
- A stock SDK-built payload is **not** evidence of zero kernel writes.
- Under a strict prohibition on all additional kernel data changes, this startup
  is disqualifying until a separately reviewed launch/runtime meets that rule.
- If the existing authorized homebrew runtime is the baseline, audio remains a
  plausible candidate requiring no demonstrated *additional media* patch; that
  narrower statement cannot waive the requested no-patching requirement.

No new runtime bypass or credential modification was implemented here.

**Calculated, not measured:** 1024 stereo float frames occupy 8192 bytes and
span 21.333 ms at 48 kHz; throughput is 384,000 bytes/s. A3 describes a roughly
one-third-second ring; driver/service allocation accounting remains unknown.
A future probe can reserve one 8192-byte sample buffer and 16-byte metadata plus
bounded counters, but those bytes do not bound attributable service memory.
For scale only, tightly packed 1080p NV12 is 3,110,400 bytes/surface and P010 is
6,220,800; three such surfaces use 9,331,200 or 18,662,400 bytes before alignment,
encoder/reference/output pools. The actual surface format/count is unknown.

No CPU affinity/priority, foreground-game impact, encoder allocation, sustained
cadence or cleanup bound was measured. The plan's 256 MiB total / 128 MiB media
ceilings remain proposed ceilings; an upstream program's latency claim cannot
satisfy R5. A record size is not capture latency or an audio/video synchronization
measurement.

## Refuted shortcuts and HDR limits

**Venc / recording / AV daemon:** V1 reports a file-backed recording session and
an IPC barrier; that is a failed experiment under its conditions, not a proof
all legitimate clients are rejected. Its daemon offsets (`ScaninControl`
`0x4f1958`, `getScaninBufferOffset` `0x435a30`) have no local binary hash/load
base or complete calling convention. They are upstream references only. A
cross-process function address cannot be directly called in a homebrew process.
No surface submission, encoder completion or bitstream API emerges from it.

**PSCapture:** its page-table modification and raw eight-bit output fail the
requested no-patch/hardware-codec combination. Its reported sub-60fps transport
also cannot satisfy the baseline. Neither original P010 memory nor dropping its
low bits establishes ten-bit encoded output. No offsets or mapping code were
adopted. JPEG experimentation does not supply the H.264/HEVC contract.

**Remote Play / Share Play:** `sceRemoteplayInitialize`, connection/status names,
server lock and prohibition setters do not expose encoded media ownership.
No complete Sony server call graph was available. Authenticating a Remote Play
client is not a demonstrated homebrew service API, and protocol bridging,
offline registration or live-file tailing is not a verified fallback. Existing
Gallery MP4 browsing likewise proves neither low-latency streaming nor source
buffer access. No Remote Play, Share Play or broadcast session was started.

**HDR:** the existing [R3 findings](research-r3-findings.md) and
[R4 calibration](research-r4-calibration.md) distinguish a typed
`int sceVideoOutGetOutputStatus(int32_t own_handle, status_pointer)` caller from
foreign-title capture. The two arguments are a handle acquired by that renderer
and its 48-byte output structure, sourced there from ProsperoLight; R4's observed
own-output dynamic-range value is not repeated as this worker's observation.
Neither that query nor `sceVideoOutSysSetHdrMetadata` supplies readable source
mastering primaries, white point, luminance, MaxCLL or MaxFALL. A setter, EDID or
sink capability is not the source record. No new metadata-getter signature was
found, and no foreign output handle was guessed.

Main10 support, PQ/BT.2020 pixel semantics, HDR-to-SDR conversion and metadata
changes must each be established separately. [Moonlight protocol notes](../../tools/moonlight-mcp/docs/protocol-notes.md)
require real HDR state/metadata signaling; a ten-bit codec advertisement or
buffer label cannot manufacture that source evidence. Protection is similarly
unresolved: source 2 hearing menus does not authorize protected content capture.
Until an observable protection boundary is sourced, no captured content should
be forwarded by a production host. The [settled D2 policy](contracts/implementation-decisions.md)
continues to apply.

## Remaining validation proposals — T041 is still incomplete

The source-image recovery and a bounded Avcap2 video call are now completed. The
five-second audio run is recorded in the T041 findings ledger, and the new
video run is summarized below. Any additional device execution still needs a
separate exact-hash approval, command, startup effects, resource/timeout plan and
exclusive console window. The existing validation proposals that follow describe
the research state before those runs; the later T041 additions supersede them
where stated.

1. **Resolve admission before code execution.** Obtain lawful, separately
   approved decrypted 13.60 library and real-client images, with complete hashes
   and process/firmware provenance. Ordinary FTP failed here. Do not silently
   substitute a dumper payload or enable FTP decryption. Analyze locally with
   Ghidra 11.4.2 headless, projects below `/Volumes/4TB_Drive/tmp/r3-media-re/`.
   Recover module-specific exports/imports, caller/callee register uses and
   bounds checks. Every argument needs evidence from both relevant caller and
   callee, including metadata input/output direction and third-argument width.
   Resolve strict no-kernel-write launch context; inspect startup, not only main.
2. **Proposed small audio probe.** Use only the sourced unchanged-auth path,
   Ipmi-first load order and seven resolved NIDs after admission. Preallocate
   buffers and bounded sample/error counters. Read for a short declared window
   over known unprotected menu/game audio, with no Bluetooth/USB/installer paths,
   credentials fallback, online services or forwarding to clients. Check finite
   float samples, stereo ordering, byte counts/rate, non-silence when expected,
   source timestamps/coverage and session-ended behavior. Record all cleanup
   returns in Stop → Close → Terminate order; unavailable driver accounting is
   absent evidence, never zero. A watchdog must not require killing Sony services
   or rebooting to declare success. Cleanup and ordinary-game coexistence require
   independent corroboration.
3. **Only after recovering a video contract, propose a synthetic-surface encoder
   probe.** Source create/configure and surface registration/stride/alignment,
   exact H.264 and HEVC profile/bit-depth values, synchronization/fence and
   output-buffer ownership, IDR/rate-control controls, drain/release/destroy and
   process budget requirements. First encode one known, owned surface; parse and
   independently decode its access units off-console. Then propose 1920×1080 at
   60 fps in bounded fixed pools, H.264 SDR and HEVC SDR separately, and HEVC Main10
   only with a sourced ten-bit input contract. If only service-owned capture is
   supported, document that limitation instead of pretending synthetic input is
   accepted. No guessed call is an allowable first probe.
4. **Measure before selecting.** Keep monotonic capture/source/submit/completion
   times, unique frame counts, full populations and p50/p95/p99/max, hardware
   encode provenance, output codec/profile/VUI/SEI, actual rate, queue and pool
   high-water marks, CPU/game impact and service allocations. Sustained 1080p60
   requires frame completion without growing queues; later full-pipeline p99 must
   meet the constitution's 16.67 ms interval constraint. Reject unsupported modes.
   Capture of a foreground game's output, source metadata/protection epochs,
   HDR-to-SDR, audio sync, conflicts and partial-start teardown need subsequent
   authorized probes; encoding an owned surface proves none of them.

**Stop conditions:** missing export, unresolved ABI, need for auth ID/capability
writes or daemon injection, unknown buffer ownership, unbounded service wait,
unsupported format, unknown protection, or unsafe cleanup keeps the gate open.
A crash/reboot is not a successful recovery test. This plan does not authorize
stress/fault injection or changes to the shared console.

## Verification and reproducibility

Scratch sources/manifests: `/Volumes/4TB_Drive/tmp/r3-media-re/`. Public files were
fetched from GitHub commit URLs and inspected as data; no downloaded build/deploy
script was executed. The original locked SDK archive was rehashed with:

```sh
shasum -a 256 /Volumes/4TB_Drive/tmp/sunshine-t001-t002/sdk-source-fvm8q5f0/sdk-a7685ce.tar.gz
```

Result: `2b3aa72a1f832923d175b7f469a5d19c51b39fba08b4757c917be5f9032a5fbf`,
matching the lock. The new original offline-only audit does not load modules,
contact the console, copy binaries or generate a deployable probe:

```sh
export TMPDIR=/Volumes/4TB_Drive/tmp
export RUFF_CACHE_DIR=/Volumes/4TB_Drive/tmp/r3-media-re/ruff-cache
python3 tools/ps5-probes/re/audit_media_sources.py \
  --sdk-source /Volumes/4TB_Drive/tmp/t040-t042/source-sdk \
  --audio-source /Volumes/4TB_Drive/tmp/r3-media-re/public/hebiii994-ps5-solobridge/solobridge/capture.c \
  --audio-source /Volumes/4TB_Drive/tmp/r3-media-re/public/X-F1REBALL-X-HearBridge-PS5/src/avcap2.c
.venv/bin/python -m ruff format --isolated --check --target-version py311 tools/ps5-probes/re/audit_media_sources.py
.venv/bin/python -m ruff check --isolated --target-version py311 --select E,F,I,UP tools/ps5-probes/re/audit_media_sources.py
```

The audit exits 0: five SDK module-table entries match, all seven public NIDs
recompute correctly in both sources, and the independent local layout is 56
bytes with source at offset 20. Formatting/lint pass. Initial audit revisions
caught the second caller's macro naming convention and formatting issues; these
were corrected before the final run. Negative-input verification and report
local-link validation are recorded with the final checks below. These are
source/document checks, not hardware tests or a passing T041 result.

Final local checks:

```sh
python3 /Volumes/4TB_Drive/tmp/r3-media-re/verify-report.py
git diff --check -- specs/001-moonlight-streaming-host/research-r3-media-re.md tools/ps5-probes/re/audit_media_sources.py
```

Both exit 0. The scratch verifier runs the two-source audit, verifies rejection
of an unrelated Remoteplay source and a missing file (both exit 1), checks all
five relative Markdown targets, checks whitespace in both new files, and confirms
the FTP manifest has seven failures with zero received bytes. Ruff format exits
0 with one file already formatted; Ruff lint exits 0 with all checks passed.
Because this workspace's feature files are untracked, `git diff --check` alone
does not check their contents; the verifier explicitly checks the new files.
The audit script SHA-256 is
`61cdedf8677f0acaf6edb1efa83573bb0d6f842f625ffb103096b314be7cacc9`.
Its two audited public source hashes are respectively
`e612b71cb91659f8c8f0c7a1131681a137f6172d33db7f3ed9cd30cd785bd6e9`
and `da8a86452220118e04fd99cffc11c1b3b835f210c97354c4654f564bfd9431ac`.
Machine-readable local results remain in `source-audit.json` and
`verification.json` below the scratch directory, outside version control.

## Readiness disposition

| Requirement | Evidence now | Still required |
|---|---|---|
| Foreground-game continuous capture | Names/upstream investigation only | Typed permitted API, sustained baseline modes, ownership/protection/cleanup |
| Console hardware H.264/HEVC encode | Venc module identifier only | Complete ABI, legitimate process context and actual hardware bitstream/budgets |
| Game/system stereo capture | Sourced Avcap2 caller + U 13.60 report | Local admitted probe; protection, timestamp and cleanup semantics |
| Ten-bit/HDR and source metadata | Existing own-output status lead | Capture-to-Main10, independent source record, transitions and conversion |
| No kernel patching | No new operations executed here | A runtime satisfying the exact condition; stock SDK startup cannot prove it |

The research deliverable is complete with this evidence-graded negative verdict
and a concrete partial audio lead. T041/T045 stay pending: neither public audio
success nor a source-only audit supplies the missing capture/hardware-encoder
path, and none of the gaps is a spec waiver.

## Follow-up: selector registration and corrected video candidate — 2026-10-25

This addendum supersedes the earlier statement that no caller-accessible
selector setup had been recovered. Firmware-13.60 Remote Play exports and its
normal caller identify a user-process ResourceArbitrator path; it requires no
ioctl or guessed kernel call. The recovered NIDs are:

| NID | Recovered interface / effect |
|---|---|
| `fBX+DBOwNis` | Query the shared 32-entry selector array |
| `kJvjeet+f9I` | Register resource ID, callback and context; sends IPC command 0 |
| `CYjfezkWcaE` | Request a bounded state name and token/PID; sends IPC command 2 |
| `LqmYZGB-UEQ` | Unregister resource ID; sends IPC command 1 |

The endpoint is `SceArbitratorIpc`. SceRemotePlay sites `0x41c231a0`,
`0x41c23500`/`0x41c23590` and `0x41c23440` register resource `0xb4`, request
`RUNNING`/`STOP`, and unregister. Its video request uses Avcap source 2 and
selector `0xb4` at request offset `0x28`. The ResourceArbitrator client map
receives the callback context through registration; its state worker sends the
event to the service, which updates the selector list. This establishes a
caller-accessible population trigger from recovered client code and from the
bounded console observation below. The service's internal policy implementation
is not recovered.

One exact-hash, owner-observed console run loaded Ipmi and ResourceArbitrator,
queried the selector list, registered `0xb4`, and requested `RUNNING`. The
selector was absent before the request, appeared after it, and disappeared
after `STOP` plus unregister. Register, RUNNING, STOP, unregister, selector
removal, both owned-module unloads and ordinary process exit all completed with
zero return codes. The run made no Avcap2/media call and its result remains
`incomplete`; it proves selector admission only. The raw log and fresh pre/post
network-route, service and process observations are under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/admission-window-20261025-1/`.

The new 720p30 candidate carries the recovered request envelope (`0x2e8`
total bytes, embedded `0x28` header, source 2, selector `0xb4`) and translates
the SceRemotePlay tier writer's context-relative offsets from the request base
at context `+0x40`. It holds the registration while it calls Avcap2 mode-0
OpenVideo/ReadVideo and cleans up video before `STOP`, unregister and selector
removal. ReadVideo uses the recovered blocking flag 0; an individual call
deadline is still unknown. The candidate subsequently ran once under separate
exact-hash approval; the outcome and alternative-path assessment follow.

The list-producer blocker is resolved at the caller/API level, but no successful
video frame, sustained 720p/1080p mode, hardware bitrate, ten-bit HDR,
HDR-to-SDR conversion, source HDR metadata, protection boundary or successful
media-session cleanup is established. T041 remains unchecked.

## Corrected 720p30 candidate run and alternatives — 2026-10-25

The owner-approved corrected candidate used selector `0xb4` while holding a
normal-process ResourceArbitrator registration. The probe's shared-list query
showed `0xb4` present immediately before media initialization, alongside
`0x64` and `0xbe`. It then made 64 bounded Avcap2 OpenVideo calls; every call
returned `0x81950008`, and no handle was returned. Start and ReadVideo were not
called; frames, bytes and bitrate are absent. Terminate, ResourceArbitrator
`STOP`, unregister, selector removal, all three owned-module unloads and
ordinary process exit returned success. Postflight IPv4/IPv6 routes, service
listeners and resident Sony processes matched preflight. Raw records are in
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/corrected-video-window-20261025-1/`.
Owner QuickTime observation had not arrived when this record was written.

The recovered Avcap2 mode-1 and mode-2 request branches require caller memory
descriptors at offsets `+0x1c0/+0x1c8/+0x1e0/+0x1e8`; the mode-1 Remote Play path
also uses IPC `0xa70013` preparation returning two 32-byte descriptors, whose
full allocation and cleanup ownership was not recovered. All modes converge on
the same `0xa70001` submission and the SceAvCapture selector predicate executes
before request-specific Venc setup, so changing mode does not avoid this gate.
The mode-1/2 paths are not sufficiently specified for a corrected probe.

`libSceVideoRecording.sprx` was present by console stat, but it was not resident
in the selected exporter targets and no image, exported NID or ordinary caller
contract was recovered. SceAvCapture's `AvcapVideoRecordingData` strings name an
internal daemon feature and do not establish a caller API. The other recovered
Remote Play media entry points are Avcap2 audio calls; Venc is owned by the
SceAvCapture daemon, and no direct Venc client contract or other supported
game/system capture caller was recovered. The offline alternative-path
assessment is preserved at
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/analysis/alternative-path-disposition.md`.

The corrected registration passed its admission test but did not produce video.
T041 remains unchecked: sustained 720p/1080p at 30/60 fps SDR, measured hardware
bitrate bounds, ten-bit HDR, bounded HDR-to-SDR conversion, source HDR metadata,
capture-protection behavior and successful media-session cleanup remain
unevidenced.

## Recovered Remote Play queries and second video run — 2026-10-26

The first corrected probe had matched the 0x2e8 OpenVideo envelope and selector
but omitted two calls that the complete SceRemotePlay caller makes before every
OpenVideo. `DHFDHdKAr9U` returns four bytes through IPC `0xa7000e`; the caller
copies that value to request `+0x2a8`. `RW88AiwmL+Y` fills a 0x20-byte state
record through IPC `0xa7000c`; the caller reads bytes `+0x14` and `+0x17`, waits
16 ms and retries while `+0x14` is nonzero, and writes `state[0x17] XOR 1` to
request `+0x2bc`. For the recovered default constructor mode class 2 and
context `+0x330 = 2`, request `+0x2ac` becomes 6 when flags bit 0 is set, 8
when bit 1 is set, otherwise 2. The source is
`/Volumes/4TB_Drive/tmp/t041-hermes/analysis-final/remoteplay-openvideo-caller.txt`;
the Avcap2 IPC wrappers are in
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/analysis/avcap2-query-helpers.txt`.
The corrected candidate now performs these calls and repeats them on the
source-matched 16 ms OpenVideo retry path, bounded to 64 cycles.

The owner directly approved an exclusive window for one corrected 720p30 run,
bracketed by exact-hash network-inventory preflight and postflight. The owner
was present with the Settings tile focused and reported `active_bigapp=-1`
before launch. Fresh IPv4/IPv6 route dumps decoded to eth0 `192.168.2.2`,
link-local `fe80::2e4:21ff:fefd:ec6b`, loopback, the sole IPv4 default gateway
`192.168.2.254`, and no IPv6 default route or global IPv6 address; preflight
and postflight both returned result 0. The corrected payload on firmware
13.600.007 (raw `0x13600007`) loaded its owned Ipmi, ResourceArbitrator and
Avcap2 modules, found selector `0xb4` after requesting `RUNNING`, and got
success from Avcap2 Initialize, mode-flags query (`0x13c`), capture-state query
(`ready=0`, mode byte 0), OpenVideo and Start.

During the 10.012160-second observation, ReadVideo returned 301 positive
Annex-B records (probe frame counter 301), totaling 94,856 bytes / 92,430
nonzero bytes, with no empty reads or overruns. The measured output rate was
75,792 bits/s against a requested 10,000 kb/s, and p99 read interval was
33,285 microseconds. This is one bounded 720p30 observation, not evidence for
other resolutions/cadences or a sustained production stream. It demonstrates
that the corrected Avcap2 → SceAvCapture → Venc path can return encoded-looking
Annex-B records; codec/profile and hardware acceleration were not independently
verified, and no bitstream was stored for offline inspection.

Avcap2 Stop, Close and Terminate returned 0. ResourceArbitrator STOP and
unregister returned 0, the follow-up selector query showed `0xb4` absent, and
owned Avcap2, ResourceArbitrator and Ipmi module unloads all returned 0. The
payload exited with its intended `incomplete` result. Protection, source HDR
metadata, bounded HDR-to-SDR conversion, ten-bit output, bitrate bounds and the
other three SDR mode combinations remain unverified. The owner QuickTime
before/during/after observation was requested and remains pending at this
entry's write time.

Raw preflight, candidate and postflight logs plus decoded route summaries are
under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/corrected-video-window-20261025-2/`.
The exact source/header/ELF hashes, commands and build/test results are recorded
in the T041 findings ledger.

The owner subsequently reported the QuickTime observation for this run directly:
the Settings dashboard stayed stable before, during and after, with no visible
display interruption or abnormal audio. This supersedes the pending-observation
wording above. The current build script also emits a separate 30-second ELF for
each required SDR mode combination. Those builds are not console evidence; the
720p30 30-second and remaining mode results are pending exact-hash approval and
fresh isolation preflight.

## 30-second 720p30 dynamic-game run — 2026-10-25

The owner approved the exact source/header/ELF hashes and an exclusive window,
with an unprotected dynamic game visible. Fresh route inventory and host checks
matched the owner-attested isolated IPv4/IPv6 link before and after the run. The
console reported firmware `0x13600007`, `active_bigapp=24`, mode flags `0x144`,
capture state ready 0 / mode 0, and successful Avcap2 OpenVideo/Start.

ReadVideo returned 900 positive Annex-B records / 23,405,408 bytes over
30.033111 seconds, 23,310,644 bytes nonzero, no empty reads or overruns, p99
read interval 33,303 microseconds, and 6,234,561 measured bits/s against the
10,000 kb/s request. Avcap2 Stop/Close/Terminate, ResourceArbitrator STOP and
unregister, 0xb4 removal, and all three owned-module unloads returned zero.
This adds one 30-second 720p30 sample only; codec/profile and hardware
acceleration remain unverified, no raw media was saved or forwarded, and HDR,
metadata, protection, and the other mode combinations remain open. Owner
QuickTime display/audio observation was requested separately after the run.

The preflight/candidate/postflight output and decoded route snapshots are under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/console-720p30-30s-20261025/`.

The owner subsequently reported clean audio during the 30-second 720p30 run,
but gameplay playback and the UI were a little choppy. The game remained
visible/running but did not return to normal smoothness after probe termination,
despite zero Avcap2, ResourceArbitrator and module-unload return codes. Record
this as an observed incomplete reversal co-occurring with the run, not proven
causality; it is a potential output-mode persistence item for T045/R5 cleanup
design. The owner confirmed the game was running normally before capture.

## 30-second 720p60 run — 2026-10-25

The owner approved a separate 1280x720@60 run with the game visible. Firmware
was `0x13600007`, `active_bigapp=24`, mode flags were `0x144`, and capture state
was ready 0 / mode 0; Avcap2 OpenVideo and Start succeeded. ReadVideo returned
1,799 positive Annex-B records / 14,348,885 bytes (14,288,129 nonzero) over
30.016582 seconds, with no empty reads or overruns, p99 read interval 16,667 us,
and 3,824,255 measured bits/s against the 10,000 kb/s request. Avcap2
Stop/Close/Terminate, ResourceArbitrator STOP/unregister, selector removal and
all three owned-module unloads returned zero. Codec/profile, hardware
acceleration, HDR, metadata and protection remain unverified; no media was
saved or forwarded.

The owner reports QuickTime laggy throughout the run (gameplay/UI choppy, with
no abnormal audio reported). After probe termination and then closing the game,
the home UI remained choppy. Record this as a persistent output-state change
co-occurring with the capture session, not proven causality; all API cleanup
return codes were zero, so the visible output did not fully recover by that
observation. This is a potential T045/R5 and production-host cleanup item.
The owner chose option B: pause capture payloads for a separately approved,
read-only diagnostic window. That snapshot found the expected Sony capture,
Remote Play and encoder-adjacent processes sleeping, the four development
service ports listening, and the accepted console route topology intact. The
read-only process query returned no thread/CPU-use measurements, so the results
do not explain the persistent UI lag. The window was released after collection;
the owner then directly chose to resume the remaining 1080p runs despite the
lag. Each mode needs its own exact-hash window approval; the first 1080p30
request is pending and 1080p60 is not yet approved. Raw
preflight, candidate and postflight evidence is under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/console-720p60-30s-20261025/`.
The separate post-lag diagnostics are under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/console-postlag-diagnostics-20261025/`.

## 30-second 1080p30 run — 2026-10-25

The owner directly chose to resume the remaining SDR modes despite the
persistent lag. After a separate exact-hash approval, the owner relaunched a
game with dynamic unprotected gameplay visible in QuickTime. The 1080p30
candidate ran once on firmware `0x13600007` with `active_bigapp=8216`, mode flags
`0x144`, capture state ready 0/mode 0, and successful OpenVideo/Start.
ReadVideo returned 899 positive Annex-B records / 9,892,303 bytes (9,848,799
nonzero) over 30.001605 seconds, with no empty reads or overruns, p99 read
interval 33,290 microseconds and 2,637,806 measured bits/s against the declared
10,000 kb/s request. The candidate path is Avcap2 → SceAvCapture → Venc, but
codec/profile and hardware-acceleration provenance were not independently
verified and no media was stored or forwarded.

Avcap2 Stop/Close/Terminate, ResourceArbitrator STOP/unregister, selector
removal and all three owned-module unloads returned zero. The postflight route
summary matched preflight, the game remained active, all development-service
ports listened and no probe remained in the process list. The owner later
reported normal audio, persistent lag in the game menu and home UI, and a
possible-but-unconfirmed improvement during gameplay. This continues the
observed output lag across the 720p30, 720p60 and 1080p30 sessions despite
successful API/resource cleanup; it is co-occurrence, not proof that capture
caused the state. Source/header/ELF hashes are
`7707921b2f3467f004e91bdf83dc7cce58e4825167b18c5feae9e57ec056f6c0`,
`f6a6fac7213d91417dec8c6a8d67eb737dacf260e9b92aea586c18a466d8b531` and
`50cae47680a23de615e776c27f432456a4ef7a0726ecc9ccdb83c1682e745440`.
Raw logs and route summaries are under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/console-1080p30-30s-20261025/`.

## 30-second 1080p60 run — 2026-10-25

The owner approved a separate exact-hash window with the dynamic game visible.
The first inventory invocation used a stale, nonexistent path and the deployer
returned `Unknown payload format`; no capture candidate ran on that attempt.
The approved network inventory ELF was then reverified at its task-provided
path with SHA-256 `30154bcf13d0ee3d870426f7d3cb328f3aadcaa3c8fa72344d6666d5fd556248`
and ran successfully. Preflight matched the isolated IPv4/IPv6 route
disposition; postflight matched preflight, retained the active
game, showed all four development services listening, and found no probe process.
The candidate ran on firmware `0x13600007`, `active_bigapp=8216`, mode flags
`0x144`, capture state ready 0 / mode 0, and successful OpenVideo/Start.

ReadVideo returned 1,798 positive Annex-B records / 565,407 bytes (526,097
nonzero) over 30.002574 seconds, with no empty reads or overruns, p99 read
interval 16,582 us and measured 150,762 bits/s against the 10,000 kb/s request.
Avcap2 Stop/Close/Terminate, ResourceArbitrator STOP/unregister, selector
removal and all three owned-module unloads returned zero. Selector `0xb4` was
present while requested and absent after cleanup. This is one sample only:
codec/profile, actual encoded dimensions, hardware acceleration and supported
bitrate bounds remain unverified; the measured bitrate is far below the request.
No media was saved or forwarded.

The owner later reported that gameplay, the game menu and home UI were all still
laggy, with no audio change reported. This completes the four-session persistent
output-degradation observation despite zero-return normal cleanup; it is
co-occurrence, not proof that capture caused the state. A console output-mode
change that teardown does not exit is the owner's current hypothesis, not an
independently established cause. Any reboot remains a separate owner decision
because jailbreak recovery would be required. Exact source/header/ELF hashes
were `7707921b2f3467f004e91bdf83dc7cce58e4825167b18c5feae9e57ec056f6c0`,
`f6a6fac7213d91417dec8c6a8d67eb737dacf260e9b92aea586c18a466d8b531` and
`bf1af7f003951081835b211a97993bca2cbd386e012bb5883b0d6a242e9ed142`.
Raw logs and route/service checks are under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/console-1080p60-30s-20261025/`;
post-lag process-list observations are under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/console-postlag-diagnostics-20261025/`.

## Remaining T041 rows and next-run disposition — 2026-10-25

The four required SDR mode combinations now each have one 30-second console
sample, and every normal Avcap2, ResourceArbitrator and owned-module cleanup
return was zero. That does not identify the codec/profile or independently
prove hardware acceleration, encoded dimensions, or supported bitrate bounds.
The single requested 10,000 kb/s setting per mode produced measured rates from
150,762 to 6,234,561 bits/s, so no supported rate bound is established.
All four runs used the same setting. Although the Remote Play request fields
are reproduced from its caller, the accepted operating range and independent
Venc completion/codec semantics are not recovered; another setpoint run could
add empirical samples but would not establish a supported hardware-encoder
bound. Given the persistent output lag observed after all four sessions, no
further rate-sweep payload is proposed without a newly identified row that it
could close.

No additional owner-observed console run is proposed for the remaining rows
because no recovered caller contract makes one conclusive or safely bounded:

- **Ten-bit HDR output:** Avcap2/SceRemotePlay request fields do not expose a
  sourced Main10/output-depth choice, and the internal Venc calls have no
  recovered external configuration ABI. Confirming that a game supports HDR
  would not supply that missing control or verify the returned bitstream.
- **Bounded HDR-to-SDR conversion:** no capture conversion entry point,
  source-HDR epoch, queue/memory bound, or timing contract was recovered.
- **Source HDR metadata:** `sceVideoOutGetOutputStatus` describes the process's
  display state and `sceVideoOutSysSetHdrMetadata` is a setter; neither reads
  metadata associated with the captured game source.
- **Capture protection:** the daemon's internal
  `sceVencSetNormalPrivacyProtectionConfig` setter sites do not define
  protected-content behavior, a state getter, or a safe silence/flush boundary.
  No protected content was accessed.
- **Error/partial-start media cleanup:** normal successful-start cleanup is
  evidenced. The recovered caller contract supplies no safe console fault
  injection or supported way to request a controlled partial-start failure;
  host lifecycle simulations do not replace that hardware observation.

The owner identified a possible console output-mode change that teardown does
not exit as a hypothesis; a reboot remains a separate owner decision because
jailbreak recovery would be required. No reboot or further console payload was
run.

## Completion follow-up: recovered Venc rate and depth controls (2026-10-25)

This follow-up supersedes the preceding bounded negative conclusions about
missing output-depth controls and safe partial-start experiments. It is local
RE and software verification, **not new console evidence**. The complete
SceAvCapture image remains 360,448 bytes at `0x5badc000`, SHA-256
`c297ed6dd2350260e0894673c1b58f58aab580779795304f30d42b4db569e029`.
All new disassembly, Ghidra projects and manifests are private under
`/Volumes/4TB_Drive/tmp/t041-codex2/analysis/`; predecessor artifacts are
unchanged. The Venc reconstruction follows the recorded four readable runs
at offsets `0/0x20000/0x60000/0xa0000`, retaining a validity mask for holes.
Ghidra uses the x86-64 GCC calling convention; raw bytes and Capstone remain
the authority for the register/field conclusions below.

### Request-to-encoder configuration

`sceVencCreateEncoder` at SceAvCapture `0x5badfeb2` receives a `0x68`-byte
aggregate and a `0x20`-byte memory aggregate. Helper `0x5bae0f70` initializes
the aggregate's nested configuration sizes (`0x38`, `0x10`, `0x20`, `0x18`
and `0x10`), then calls `sceVencQueryPreset`. Helper `0x5bae1170` applies
the request's codec, picture and optional configuration. Helper `0x5bae6bf0`
asks `sceVencQueryMemorySize` for three descriptors and rounds their sizes to
16 KiB before the daemon supplies their allocations. The payload continues
through Avcap2; it does not call these internal helpers or create a second
Venc client.

| Original OpenVideo request | Recovered effect and code evidence |
|---|---|
| `+0x20`, u32 | `0` selects preset depth 8; `1` selects depth 10. Sony validates `0..1` at `0x41c279d1`; SceAvCapture `0x5baf3e7e..0x5baf3e89` emits the actual preset depth. |
| `+0x24`, u32 | `0` selects AVC, `1` HEVC. Sony maps caller codec `2/3` to `0/1` at `0x41c279e2..0x41c279f9`. SceAvCapture `0x5bae19b0` selects classes independently named `AvcapVideoCodecAvc` and `AvcapVideoCodecHevc`; `0x5baf3e3c..0x5baf3e50` emits Venc codec `1/2`. |
| Depth/codec combination | Venc `0x811407a97..0x811407b40` accepts depths 8/10 and explicitly rejects depth 10 with codec 1 (AVC). A ten-bit HEVC request is therefore sourced, but Main10/PQ/source-HDR acceptance still needs bitstream and source observations. |
| `+0xc8/+0xcc/+0xd0`, u32 | SceAvCapture `0x5baf0c53` and `0x5baf4320` copy these into Venc's rate-control structure at `+8/+0xc/+0x10`. Venc preset generation at `0x811408920` takes codec-level bit limits and shifts right by three before supplying those fields; the request uses **byte-rate/byte-budget units**. |

The wrapper constructor at `0x5baf0270..0x5baf029f` copies exactly `0x2e8`
wire bytes to object `+8`. Thus the rate helper
`0x5baf0c53..0x5baf0c63` passing object `+0xc8` to a copier reading
`+8/+0xc/+0x10` maps precisely to wire request `+0xc8/+0xcc/+0xd0`, rather
than introducing a hidden eight-byte offset. Evidence is
`analysis/daemon-request-wrapper.txt` and `analysis/daemon-rate-copy.txt`.

The preceding payloads multiplied a quantity labelled `bitrate_kbps` by 1000
and wrote that result directly into the byte-rate fields. Thus their declared
10,000 kb/s setting was **not a verified 10 Mbit/s ceiling**: the peak field
actually contained 10,000,000 bytes/s (80,000,000 bits/s). Historical frame,
byte, interval and measured-output observations remain unchanged. The corrected
probe multiplies kbit/s by 125, uses Sony's AVC/HEVC divisor `1.12/1.10` for
the first field, and derives the per-frame byte budget using the same
`1001/(fps*1000)` expression. This corrects request transcription; it does
not prove constant-rate behavior or causally explain the reported UI lag.

The earlier description of double-precision bitrate fields also needs precision:
at `0x41c28283..0x41c282d9`, Sony computes two values with double-precision
coefficients `0.262/1129.9` and `0.2103/554.98`, converts them to integers,
clamps them to context `+0x8c`, and stores **u32** values at context
`+0x12c/+0x130` (request `+0xec/+0xf0`). They are not double values stored in
the IPC envelope, and their complete optional-policy semantics remain unknown.
The new sweep uses the directly traced rate-control fields rather than enabling
this optional policy by guessing.

### Sourced ceilings and bounded experiments

Venc helper `0x811400510` selects an AVC level from the actual table at
`0x8114359a0`, constrained by macroblocks/frame, macroblocks/second and reference
capacity. With one reference, the four requested geometries/cadences select
level codes **31, 32, 40, 42**. The corresponding bit ceilings from helper
`0x811400400` are **14, 20, 20, 50 Mbit/s**. The independently decoded branch
addresses and each constraint are in `venc-avc-sweep-bounds.json`; source
disassembly/decompilation is in `venc-levels.c` and `venc-rate-units.txt`.
These are static codec/preset bounds, not measured device-wide limits.

`build-r3.sh` prepares ten-second AVC candidates at **2, 6 and 10 Mbit/s**
for each mode, below all four sourced ceilings, plus ten-second 1080p60 HEVC
eight-/ten-bit candidates. No minimum-device-rate claim follows from the
chosen 2 Mbit/s point. Each stream retains one 2 MiB read buffer, 4,096 timing
entries and at most eight NALs / 8 KiB: the first parameter NAL per type
and distinct valid mastering-display/content-light SEI NALs. It unescapes SEI
payloads and excludes NALs containing only unrelated UUID/timing SEIs; a retained
mixed-message NAL can include other messages and consumes the same bounded
quota. Overflow makes a complete metadata observation unavailable. Picture
payloads are discarded.
The NAL type map is sourced to FFmpeg n8.0
[H.264 definitions](https://github.com/FFmpeg/FFmpeg/blob/n8.0/libavcodec/h264.h)
and [HEVC definitions](https://github.com/FFmpeg/FFmpeg/blob/n8.0/libavcodec/hevc/hevc.h).
The SEI payload types 137/144 and lengths 24/4 bytes are sourced to FFmpeg
n8.0 [SEI definitions](https://github.com/FFmpeg/FFmpeg/blob/n8.0/libavcodec/sei.h).
FFmpeg `trace_headers` can independently inspect SPS geometry/profile/depth/color
signaling in actual collected bytes. Local **synthetic** AVC and HEVC Main10
fixtures show that parameter-only `ffprobe` may report unknown dimensions and
that `trace_headers` may parse the fields while the overall header-only remux
exits 234 because no picture establishes muxer dimensions; that is not a
successful picture decode. Encoded signaling does not by itself prove source
mastering metadata, protected-content behavior or conversion quality.

Two additional candidates exercise owned-resource cleanup: an intentional
exit after successful OpenVideo and before Start, and request mode 3. The latter
is rejected locally at Avcap2 `0x814e4848a..0x814e484a1`, before IPC, with
`0x81950001`. This replaces the earlier claim that no safe error trigger exists;
it tests a client-rejection path, not a daemon/encoder fault. The partial-open
case closes only its acquired handle, terminates its own client, requests STOP,
unregisters and verifies selector removal. Neither candidate injects a service
failure, kills another process or changes credentials beyond the accepted SDK
startup baseline. Exact-hash approval and fresh IPv4/IPv6 checks are still
required before any execution.

The owner reports that the UI remains slow **after an owner-performed restart
with no probes running**. Capture causality is unresolved, and boot-time slowness
or the Elgato/QuickTime observation chain may contribute. The initial service
checks found loader 9021 and all four dev ports refused; the coordinator is
arranging owner recovery and has authorized only local work until its explicit
go. No new console result is implied by the recovered contracts or local tests.

### Additional bounded status/HDR investigation

The `0x70`-byte ReadVideo info remains opaque in the payload. A complete
SceAvCapture trace now ties byte `+0x59` to Venc read return `0x80c70200` and byte
`+0x64` to `0x80c70201`: `0x5bae059f..0x5bae0633` chooses the booleans,
`0x5bae06a0..0x5bae06bb` forwards them through `0x5bae07b0`, and
`0x5bae1260` stores them at `0x5bae12e5/0x5bae12f1`. Their meanings are not
established as HDR/protection. The independent Venc read wrapper at
`0x8114065e0..0x811406790` preserves those special returns while copying nested
read info; naming a flag as capture protection from these bytes alone would be
unsupported. Raw evidence is `analysis/venc-read-wrapper.txt`, the predecessor
`daemon-disasm.txt` at those exact addresses, and the corrected GCC-ABI local
`analysis/daemon-gcc-decompiled.c`; Capstone and raw bytes remain authoritative.

The two-source capture-state singleton `0x5bae4740` and getters
`0x5bae4820/0x5bae4890/0x5bae4900/0x5bae4a20/0x5bae4a80/0x5bae4ae0` expose
mode cadence/support/enumeration data, not a recovered mastering-display/light
record. Additional Avcap2 exports at offsets `0x1b90/0x1c60` are used around
callback/state registration and teardown (`0x41c273ab..0x41c273c3`), so their
pointer-bearing structures have not been treated as source HDR getters or
invoked. No tone-map conversion contract or protected-content state getter has
yet been demonstrated by these additional traces.

### Recovered configuration-header query after ReadVideo returned no parameter NALs

The two new 720p30 rate points returned only unselected NAL types, with zero
collection errors; an independent encoded-format observation therefore needs
Sony's separate header path. The earlier local artifact name
`avcap-query-video-preset.txt` was provisional: tracing farther establishes
**configuration-header query**, not merely preset data. NID `VAFgNc0Pfp8` at
Avcap2 `0x814e494f0` has the fully recovered signature
`int(const void *request40, void *output424)`. Its helper `0x814e4c890` sends
IPC `0xa70008` with exactly `0x28` input / `0x1a8` output bytes. Sony's caller
`0x41c28ec0..0x41c290b3` constructs the same 40-byte header present at OpenVideo
request `+8`, zeroes the output and sets its first qword to `0x1a8`.

SceAvCapture `0x5bae8550` maps that header through `0x5baf3e30`, selects the
codec class and calls its query method. The AVC path calls
`sceVencQueryHeader` at `0x5bae1cfe`; the HEVC counterpart is `0x5bae284e`.
Venc `0x811405690..0x811405847` generates the configuration header through
`0x811409220`; its admitted sizes are `0x118` for AVC and `0x1a0` for HEVC.
The daemon packet packers `0x5bae1f68..0x5bae1fcf` and
`0x5bae2b3d..0x5bae2bc9` publish a flat `0x1a8` packet with codec tag 0/1 at
`+8`, SPS length/data at `+0x10/+0x18`, PPS at `+0x98/+0xa0`, and HEVC VPS at
`+0x120/+0x128`. Each raw NAL is bounded to 128 bytes; its original four-byte
start code was removed. This completes the input/output layout without guessing
an API type. No source HDR record or capture-protection state follows from it.

The opt-in `R3_MEDIA_HEADERS` build resolves only that additional sourced NID,
queries once after owned Initialize, validates size/tag/lengths/NAL types and
reconstructs bounded Annex-B parameter NALs before ordinary Open/Start/Read.
AVC and HEVC parsing is transactional, so an invalid packet adds no partial
headers. Configuration-query signaling is explicitly labelled
`association_with_read_frames=unverified`; it is not independent VCL decoding
or proof of ten-bit HDR. The three build-only candidates are
`capture-headers-avc-720p30.elf`, `capture-headers-hevc8-1080p60.elf` and
`capture-headers-hevc10-1080p60.elf`. They require new exact-hash approval and
have not run during the current service HOLD. Red-first synthetic layout/bounds
and query-error cleanup tests are separate from all hardware evidence.

### Dispatch 5: Remote Play bitrate-envelope caller trace (2026-10-26)

This local-only trace uses the partial SceRemotePlay image at
`/Volumes/4TB_Drive/tmp/t041-hermes/analysis/sceremoteplay-image.bin`
(5,783,552 bytes, SHA-256
`2f36ccef67dd2eb3979266b6c3f3427a0b3b627faaa21a0fd9eac1107a4d6e8f`).
The newly saved Capstone output is under
`/Volumes/4TB_Drive/tmp/t041-codex2/analysis-dispatch5/`; the image and those
private RE artifacts were not modified by this analysis.

The source writer distinguishes AVC from HEVC before filling the optional
rate-envelope fields. `0x41c27840` validates the depth selector at input
`+0x0c` as 0/1, maps input codec 2 to request `+0x24 = 0` and codec 3 to
request `+0x24 = 1` (`0x41c279d1..0x41c279f9`), and stores input depth at
request `+0x20`. The same input codec selector sets request byte `+0xe8` to
one for AVC and zero for HEVC (`0x41c27b6a..0x41c27b79`). This flag is the
branch condition used by the subsequent envelope writer at `0x41c28273`.

The initializer stores a 64-bit value at context `+0x84` and a 32-bit clamp
at context `+0x8c` (`0x41c27a2b..0x41c27a82`): the latter is
`input+0x1c << 3`; the former is `1` for codec selector 3, or
`min(((0x3c000 / fps) * 0x4444445) >> 32, input+0x18)` otherwise. For the
AVC branch (selector 2), it uses the latter formula. The original OpenVideo
settings adapter at `0x41c24320..0x41c2442b` sources context `+0x84`'s
clamp input from its live context field `+0x1ec`, and context `+0x8c`'s
source from field `+0x2005f8`.
Their application-level names and units are not established by the available
partial image. This prevents reproducing Sony's dynamic clamp inputs as fixed
payload constants without inventing meanings or values.

The bitrate input `x` is the 32-bit descriptor word at `+0x0c`, passed by the
tier builder `0x41c27fa0..0x41c2810e` to `0x41c28190`; the builder uses the
same descriptor rate in its frame/peak calculations. The source applies
double-precision arithmetic, truncates to u32, and clamps each result to
context `+0x8c`:

| Wire request field | Source write when `+0xe8` is set |
|---|---|
| `+0xec` | `min(trunc(0.262 * x + 1129.9), context+0x8c)` |
| `+0xf0` | `min(trunc(0.2103 * x + 554.98), context+0x8c)` |
| `+0xf4` | 64-bit copy from context `+0x84` |
| `+0xfc` | 32-bit copy from context `+0x8c` |

When `+0xe8` is clear (the HEVC caller branch), the writer zeros
`+0xec..+0xff` (`0x41c282e5..0x41c282f8`). The offsets in the trace are
relative to the `0x2e8` OpenVideo request; the arithmetic in `0x41c28190` is
relative to its context base at request `+0x40`. The code has recovered the
writes and their source context, but the partial image gives no stable public
semantic names for these four fields and no runtime values for the active
`+0x1ec` and `+0x2005f8` inputs. Existing AVC candidates leave this optional
policy zeroed; their measurements do not test the recovered AVC range branch.
This limits treating those requested points as a faithful Sony rate-policy
sweep.

The evidence files are `remoteplay-openvideo-caller-head.txt`,
`remoteplay-openvideo-caller-tail.txt`, `remoteplay-initializer.txt`,
`remoteplay-tier-and-rate-fill.txt`, `remoteplay-rate-envelope.txt`,
`remoteplay-rate-caller.txt`, `remoteplay-rate-caller2.txt`, and
`remoteplay-rate-source.txt` in `analysis-dispatch5/`. The image hash and
offsets make the inference reproducible; they do not establish runtime values
that were not observed.

### Dispatch 5: privacy and prohibition paths do not expose capture protection

The SceAvCapture image's import table names
`sceVencSetNormalPrivacyProtectionConfig` (Venc NID
`3-o2Ds4E87g#E#A`; daemon import `3-o2Ds4E87g#E#F`). Call sites
`0x5baf14d3..0x5baf1517` and `0x5baf3a22..0x5baf3a69` build a 24-byte
configuration and pass it with the daemon's encoder handle at object `+0x418`.
The configuration byte is computed by testing whether any of sixteen 16-byte
regions at offsets `+0x600..+0x6f0` is nonzero in a `0xb000`-byte image block
(`0x5baf2840..0x5baf28e6`); the accompanying pointer identifies the current
block (`0x5baf2830`). Venc's matching export starts at image offset `0x7040`
and validates the handle/config pointers before copying a 0x60-byte internal
representation and applying it through an encoder object. This is a
normal-privacy-region mask/update path, not a getter for DRM/content protection
state. Its `Protection` name alone is not evidence of protected-title behavior.
The new call-site and Venc disassembly is saved as
`analysis-dispatch5/daemon-privacy-call1.txt`,
`daemon-privacy-call3.txt`, `daemon-privacy-config-build.txt`, and
`venc-privacy-config.txt`.

The SceRemotePlay policy path is distinct. Its JSON writer at
`0x41bd6550..0x41bd67d3` reads `appId`, `appType`, `status`, `protectAudio`,
`protectVideo`, `internetRestrict`, `compatTitle`, `disableInputDev`, and
`prohibitType` into a Remote Play prohibition record. The WtRp server path at
`0x41ecb780` constructs `reqRpNotifyProhibitStatus`, and the connection
formatter at `0x41ed9820..0x41ed9b6a` serializes an `RP-Prohibit:` field.
These are Remote Play policy/status notification and protocol fields; the
available code does not connect them to an Avcap2 capture-state getter,
encoded-frame blacking, audio silence, flush/epoch transition, or protected
content test. Evidence is in the existing private artifacts
`analysis/remoteplay-prohibition-type-json.txt`,
`remoteplay-prohibition-notification.txt`, and
`remoteplay-prohibition-log.txt`.

Avcap2 exports at module offsets `0x1b90` and `0x1c60` (NIDs
`yN0w4tWivqs` and `96UQhNnBlxc`) are called by SceRemotePlay around state and
callback registration/teardown. Their pointer-bearing arguments are not a
recovered source-HDR record or capture-protection result. The recovered
Avcap2 status/control surface is tied to a caller-owned capture handle; the
available status fields do not map the foreign application's mastering
display/content-light metadata. Across the recovered Avcap2, SceAvCapture,
Venc and SceRemotePlay exports/imports, no callable source-HDR getter or
protected-content capture-state contract was recovered. The prior negative
results are bounded to these images and paths; they do not prove no such
private interface exists elsewhere in the firmware.
