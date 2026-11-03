# R3 console capability findings — Incomplete

Recorded **2026-10-25** for T040–T042. Firmware target: **13.60** with the existing
homebrew environment, isolated en9 Mac 192.168.2.1 / console 192.168.2.2.
**Task status:** T040 done; T041 Incomplete (no qualifying media path); T042
partial (four neutral driver handles, one owner-observed D-pad delivery sequence
and clean observed teardown; remaining controller/coexistence evidence unresolved). Neither T041
nor T042 is checked; T045 remains open.
This first section is **source research**, not a console run. No console, shell,
log, FTP, capture device, reference host, firmware setting or privilege was
accessed or changed during the initial source audit. At that stage, current
console readiness and live IPv4/IPv6 isolation had **not yet been reverified**;
subsequent explicitly approved console observations are recorded separately below.
The separate [R4 record](research-r4-calibration.md) contains its own dated hardware
observations; they are not reattributed to R3.

## T040 source audit

The [probe README](../../tools/ps5-probes/README.md) contains the complete
candidate/permission map and immutable source links. The SDK v0.45 source pin
is `a7685ce8e97ce1f8a4f38253a41d54241c9124f1`, compiler LLVM 18.1.8;
the [dependency lock](../../cmake/dependencies.lock.json) remains authoritative,
including its explicitly unresolved installed-LLVM archive provenance.

The full previously downloaded SDK source archive was read locally after
SHA-256 verification against the lock. Only regular source members were copied
to the task-local scratch directory, without executing downloaded code. Maintained
reference source was read at these commits:

| Reference | Pin | Inspected paths / relevance |
|---|---|---|
| ps5-payload-dev/sdk | `a7685ce8e97ce1f8a4f38253a41d54241c9124f1` | `sce_stubs/genstub.py`, VideoOut/Remoteplay/AudioOut/Pad/SystemService stubs; `include/ps5/kernel.h`, `crt/kernel.c`, `crt/rtld_sprx.c`; ABI versus symbol distinction |
| ps5-payload-dev/SDL | `ee4c47dc0d617b3bc8f35108f9956baf228a1322` | PS5 video, audio and joystick `.h/.c`; typed local presentation/playback/read interfaces |
| LedyBacer/ProsperoLight | `a5301b75f62d31a4d590aec9cad473bb26e76a9f` | Ten-bit VideoOut presentation reference documented in R4 |
| fstat13/PSCapture | `d292d0fbf7434d1aeda50103911e4d2a18266b85` | `payload/main.c`, `docs/REVERSE_ENGINEERING.md`, `docs/ENGINEERING_LOG.md`; privileged scan-out research, no approved run |
| leolobato/ps5-mcp | `84c0f7b316772b13cd62e0121c154afc3ed2482c` | `docs/vpad-abi.md`, `docs/findings.md`, `payload/system_ps5.c`, `payload/padd.c`; `/dev/hid` reports and assignment/cleanup limitations |
| MegaCadeDev/YetAnotherControllerEnabler | `3fd0665c045a5631161d63150d7e8b29451222ca` | `include/pad_data.h`, `src/pad_assign.c`, `src/main.c`; typed libScePad candidate, privileged assignment not admitted |
| VivaLaVent/kodi-ps5 | `83a7be649914535d0cb7837eea65f6f7e6eaaa21` | `overlay/xbmc/platform/ps5/VideoOutInfo.cpp`, `scripts/30-deploy.sh`; output-status layout, HDR title attributes |
| FGGstore/FGG-PlayPods | `d726e1c32568048aae66f3218c0540adc357e408` | `src/capture.{c,h}`, `README.md`, `TESTING.md`; Avcap2 audio ABI, upstream 11.60 scope and RFC limits |

The SDK stub generator emits assembly labels from a symbol/NID list. These are
not C prototypes or permission contracts. The inspected SDK has no sourced
foreground-game capture or H.264/HEVC encoder configuration/completion interface.
This is a bounded search finding, **not proof that those console facilities do
not exist**. The SDK loader's internal module-name table also does not supply a
service API. No guessed signature or firmware offset was called.

### Verified source declarations and permissions

SDL `src/video/ps5/SDL_ps5video.h` declares VideoOut v2 buffer registration,
presentation, flip events and direct-memory ownership. The audio header declares
`int32_t sceAudioOutInit(void)`,
`int32_t sceAudioOutOpen(int32_t,int32_t,int32_t,uint32_t,uint32_t,uint32_t)`,
`int32_t sceAudioOutOutput(int32_t,const void *)`, and
`int32_t sceAudioOutClose(int32_t)`.
These establish output to the display/speakers, not capture of another title.
R4 establishes a foreground launch-context requirement for its own renderer and
the title-attribute requirement for its native HDR experiment. Neither conveys
capture/encoder privileges to a background host.

SDL's joystick header declares local pad initialization/open/read/close and local
vibration. It does not declare VirtualDeviceAdd/Insert/Delete or reverse virtual
rumble. Those SDK exports alone cannot authorize calls. The YACE reference adds
`int scePadVirtualDeviceAddDevice(void *,int)`,
`int scePadVirtualDeviceInsertData(int,const PadData *)` and
`int scePadVirtualDeviceDeleteDevice(int)`, with a 120-byte application report.
Its privilege/remote-process assignment implementation is not adopted: sourced
code is not permission to inject it, and exact rollback on this console is unverified.

The ps5-mcp reference documents a separate `/dev/hid` **O_RDWR** ioctl interface:

| Operation | Request / argument layout |
|---|---|
| Add | `0xC018482A`, 24 bytes: u32 op=3, u32 reserved, input pointer, output handle pointer |
| Report | `0x8018482C`, same 24-byte header with handle in first word, report input, null output |
| Remove | `0x80104850`, 16 bytes: u8 op=5, seven reserved bytes, u32 handle, u32 reserved |
| Descriptor | 168 bytes: protocol=3 at 0x00, signed user ID at 0x1c, class=1 at 0x20 |
| Report | 0xa0 bytes: buttons at 0x0c, four sticks at 0x10, triggers at 0x14/0x15, active=1 at 0x17, motion block at 0x78; identity quaternion w at 0x84 |

This reference reports testing on 13.60; its original ABI source was tested on
11.60. That is **upstream evidence**, not verification on this console. Its
report is not ScePadData. Returning success validates the request header and can
silently accept incorrectly placed fields. Neutral requires sticks **0x80**,
buttons/triggers zero, identity orientation; a zero-filled report is not neutral.
Assignment requires a correctly observed system dialog and can turn the owner's
physical controller off. User assignment, effective delivery and handle removal
remain separate observations. PS-button input and touch-surface coordinates are
not established by this path; rumble return and motion units are unresolved.

### Capture / encode / HDR candidate disposition

Built-in Remote Play reuse remains a candidate, with status/control names in
`sce_stubs/libSceRemoteplay.c`, but no admitted media interface or independently
verified offline/PSN-free execution. `sceRemoteplayProhibit*` are not a capture
protection observation. `sceShellCoreUtilGetSharePlayStatus` likewise supplies
only a name without a verified query structure or transition semantics.

PSCapture's 13.60 engineering log reports recording-service IPC/session problems
and upstream console crashes. Its replacement maps a display-engine ring using
kernel page-table writes and firmware-specific structure assumptions, then emits
eight-bit NV12. Its reported throughput is below the mandatory sustained 60 fps
baseline; geometry offsets, chroma-tail replication and timing heuristics also
need independent validation. No protected-content/audio boundary, source HDR
mastering/light metadata or hardware H.264/HEVC output is established there.
**Do not run this upstream payload or copy its offsets as a fallback.** An
approved future investigation would first need bounded validated reads, explicit
firmware provenance, reversible mapping ownership and verified restoration.

`sceVideoOutSysSetHdrMetadata` sets metadata. It cannot independently verify the
source metadata later serialized by a host. ProsperoLight declares
`int sceVideoOutGetOutputStatus(int32_t,video_output_status_t *)`, with a 48-byte
status (u32 resolution/output class, u64 refresh/flags, u32 mode, five reserved
u32 words). R4 independently observed return 0 / dynamic_range 2 on its own
VideoOut handle after the approved temporary title attribute `0x62000001`.
That establishes a display-state candidate, not a foreign-title frame or source
metadata getter. EDID, tone-map-luminance and tone-conversion symbols remain
search leads without admitted metadata/conversion ABI or permissions. Ten-bit pixels
and HDR composition do not establish mastering-display/MaxCLL/MaxFALL values.
No such numbers or a conversion throughput budget are invented in this report.

Additional primary-source research found a concrete **audio** candidate in
FGG-PlayPods `src/capture.c`. It resolves Avcap2 lifecycle by NID: Initialize
`svzPXluOz8U`, Terminate `gojqghDU+1Y`, OpenAudio `3nHpE7Dp5SE`,
Close `tR7gPe1i8hw`, Start `8CGNCwBsItI`, Stop `Z5dKK0xnQ8g`,
ReadAudio `WhsHggqdwcg`. The source declares `int(void)` for init/terminate,
`int(void **,const void *,uint8_t)` for open, `int(void *)` for lifecycle,
and `int(void *,void *,size_t,void *,uint32_t)` for read. The 56-byte open
block uses size=0x38, unk08=0x18, source=2, mode=0, unk1c=UINT32_MAX and
other fields zero. Reported records are 8192 bytes / 1024 stereo float32
frames at 48 kHz. It loads `/system/common/lib/libSceIpmi.sprx` before Avcap2;
its source documents process termination if the dependency is unresolved.
NOWAIT=1, empty=0x81950002, overrun=0x81950004 are source-defined results.
Cleanup is Stop→Close→Terminate, preserving uncertainty on failure, without
infinite retry. The README's hardware scope is **11.60**, and this branch
labels itself an RFC; tests with mocked APIs do not establish 13.60 behavior.
Thus this is a sourced ABI candidate, improving on the SDK's names alone,
while target permissions, content protection, service-call bounds, returned
record semantics and independent cleanup remain unverified. No audio function
was called by this worker. The coordinator received the lead for the separate
media reverse-engineering track; this worker will not duplicate that track.

## T041 required evidence ledger

| Required observation | Current status / missing prerequisite |
|---|---|
| Sustained 1280x720 at 30 and 60 fps, SDR | Unavailable: no admitted console capture + hardware encoder; no run, frame population or timing |
| Sustained 1920x1080 at 30 and 60 fps, SDR | Unavailable for the same reason; external HDMI/R4 presentation is not this path |
| 1920x1080 at 60 fps, ten-bit HDR | Unavailable: no capture-to-Main10 path; no produced bitstream/decoded precision |
| Hardware encoding and bitrate bounds | Unavailable: no admitted encoder ABI, permissions, output provenance or resource accounting |
| Stereo game audio | **Observed with data and clean teardown** (2026-10-25 run below): Avcap2 7-NID lifecycle all-0, 239,616 non-silent stereo float32 frames in 5.0009 s, Stop/Close/Terminate/module-unload all 0. Protection, source clock, stereo ordering and driver accounting still unverified |
| Bounded HDR-to-SDR | Unavailable: no admitted conversion path, source epoch or measured duration/memory bound |
| Source HDR metadata and transitions | Unavailable: no independent readable mastering/light metadata tied to source epochs |
| Capture protection, audio silence, stale-queue invalidation | Unavailable: no observable protection boundary; no protected content was accessed |
| Normal/error/partial-start media cleanup | **Normal-path cleanup observed** (2026-10-25 audio run: Stop/Close/Terminate and owned-module unload all 0 after a successful session); error and partial-start paths remain untested |

These missing capabilities leave the hardware gate open. A future report must
retain raw return codes, elapsed-time populations, bitstream format/provenance,
resource high-water marks and independent cleanup evidence for every admitted
path. A source-only diagnostic must return Incomplete, never measured zeros.

## T042 required evidence ledger

| Required observation | Current status / missing prerequisite |
|---|---|
| Successful delivery meaning / SC-005 boundary | Owner corroborated Right/Left navigation, a Cross-triggered SELECT USER notice, and later Right navigation; exact application-time/SC-005 boundary remains unverified |
| Four distinct controllers and ordered assignment | Four handles, four prompts and four Right navigations were owner-observed with clean reverse removal; multi-controller support is owner-deferred and no further multi-controller runs are authorized |
| Neutral release and individual removal on disconnect/stop/partial failure | Ordinary one/four-handle exits and one bounded held-Right normal stop submitted neutral before removal with returned cleanup calls. The held run showed navigation and a disconnect toast, but the rightmost menu boundary masks continued Right; visible release, transport disconnect, partial failure and rest remain unverified |
| Required buttons including Create/PS, proportional sticks/triggers | Cross and D-pad delivery corroborated; Create and proportional delivery untested; PS failed at typed AddDevice and is not established by HID; analog/motion report acceptance alone is not delivery |
| Rumble back to client | Blocked: no sourced virtual-controller feedback API or output-report layout found; no guessed HID read attempted |
| Motion / touch coordinates | Blocked: HID pad-service Open/GetHandle returned `0x809b0081`/`0x80920008`; typed AddDevice returned `0x803b0001`; no ReadState or effective motion/touch-coordinate evidence |
| Remote Play / Share Play conflicts at start and during session | Blocked: no admitted typed query/transition ABI or recovered call sites; no Sony online services used |
| Rest/wake and normal stop cleanup | Ordinary stops and one bounded held-Right normal cleanup returned without reported error; exact exposure timing and the rightmost menu boundary prevent a visible release claim. Signal/transport stop, rest/wake and partial-platform failure remain unresolved |
| Foreground-game coexistence / safe scheduling | Prepared only: min-priority/restore and full control sequence owner-declined; no game run or game-impact evidence |

The required delivery boundary is acceptance by the console input subsystem with
documented semantics and independent functional corroboration, **not** network
send/ACK or generic ioctl success. Release runs first on detected termination,
independent of media/exports, then removes only the probe's own handles. There
is no authority to remove arbitrary virtual devices or kill other payloads.

## Execution and verification record

Local-only evidence directory: `/Volumes/4TB_Drive/tmp/t040-t042/`.
The coordinator conveyed the owner's decision in reply to
`msg_a54b762d1102`: finish T040 and the fail-closed T041 diagnostic, authorize
T042 preparation across the incomplete T041 dependency, then request a console
window and separately approved owner-assisted actions. A different worker owns
the funded media reverse-engineering track (`research-r3-media-re.md` when it
lands). No window or payload execution is implied by that preparation approval.
T041/T042 remain unchecked until their required evidence is complete.

T040 verification on Darwin arm64 / macOS **27.0.1**, LLVM **18.1.8**:

- `shasum -a 256 /Volumes/4TB_Drive/tmp/sunshine-t001-t002/sdk-source-fvm8q5f0/sdk-a7685ce.tar.gz`
  returned `2b3aa72a1f832923d175b7f469a5d19c51b39fba08b4757c917be5f9032a5fbf`,
  matching `cmake/dependencies.lock.json`.
- Python `pathlib`/link validation over the README and this report: every local
  Markdown target exists. Source links preserve the exact commits/paths inspected.
- The local source audit found each explicitly listed SDK import in its named
  `sce_stubs` source. No candidate symbol was invoked and no hardware capability
  was inferred from it.

### T041 local diagnostic preparation (not device evidence)

`tools/ps5-probes/capture_encode.c` implements an admission diagnostic rather than
inventing the missing media APIs. Default/`--describe` emits the five required
SDR/HDR mode entries with unavailable reasons, absent frame/duration observations,
and explicit open readiness gate, returning **2 / Incomplete**. Target-only
`--inventory` reads SDK firmware context and stats seven candidate module paths;
it loads/calls none of the candidate services. Unknown firmware is refused.
It acquires no media resources, so its lack of acquired resources is **not**
normal/error/partial-start media cleanup evidence. The approved subsequent
read-only inventory run is recorded below; no media API was invoked.

Exact local checks performed successfully:

```sh
export TMPDIR=/Volumes/4TB_Drive/tmp
bash tools/ps5-probes/build-r3.sh
bash -n tools/ps5-probes/build-r3.sh
/opt/homebrew/opt/llvm@18/bin/clang-format --dry-run --Werror tools/ps5-probes/capture_encode.c
/opt/homebrew/opt/llvm@18/bin/clang-tidy tools/ps5-probes/capture_encode.c -- -std=c11 -DR3_HOST -Wall -Wextra -Wpedantic -Werror
/opt/homebrew/opt/llvm@18/bin/clang-tidy tools/ps5-probes/capture_encode.c -- -std=c11 -target x86_64-sie-ps5 -isysroot /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk -isystem /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/target/include -Wall -Wextra -Wpedantic -Werror
python3 /Volumes/4TB_Drive/tmp/t040-t042/verify-local.py
```

Both host/target builds are warning-free. Clang-tidy exits 0 with no project
diagnostics; its default output separately counts diagnostics in system headers.
Six invocation cases pass: default/describe return 2, host inventory/unknown
option/conflicting options return 64, unwritable stdout returns 1. The last case
first reproduced exit 2 before the buffered output was explicitly flushed;
it passes after the fix. The local script also validates document
links, the locked SDK source hash, and absence of candidate media/privilege
dynamic imports. Static SDK runtime remains subject to source review; an import
listing alone cannot prove all runtime effects.

Initial approved source SHA-256 (before the runtime qualification below):
`a13ac2282551837928208fc8972f2bb487f34b84f130651bc9f2f7fddc40a963`;
PS5 diagnostic ELF:
`8b3cedfb3db05be63c6123569142238945855b75530477bd0f164ada4a182416`.
These hashes identify locally built bytes, **not execution approval or hardware
results**. T041 remains unchecked because the required media observations above
are unavailable. T042 preparation is owner-authorized across this dependency.

### T042 local preparation and software checks

`input_system.c` implements the sourced neutral/add/report/remove lifecycle only.
It does not invoke SDK VirtualDevice imports or injected assignment code. It
checks the 13.60 firmware context, initializes/query/terminates its own UserService
client, creates at most four unique owned `/dev/hid` handles, sends only centered
neutral state, and attempts neutralization of **all** owned handles before
removing any. Setup/error paths remove only handles acquired successfully;
removal errors remain failures and do not prevent remaining safe cleanup. It
does not reinterpret an ioctl return as effective delivery/removal evidence.

`--describe` is the safe default. `--self-test` verifies request sizes/offsets,
the complete 168-byte descriptor and 160-byte neutral report against literal
source-layout fixtures. The target's explicit `--four-neutral` mode defaults to
ten seconds (CLI range 1..30); it makes no assignment-button presses and requests
no scheduling/privilege change. Because raw loader 9021 has no argument transport,
`input-four-neutral.elf` selects this fixed ten-second mode at compile time,
while the ordinary `input_system.elf` remains describe-only by default.
Likewise `capture-inventory.elf` selects read-only inventory at compile time.
Every new binary still needs exact-hash approval and an exclusive window.

Local checks, all passing:

- `bash tools/ps5-probes/build-r3.sh`: host and PS5 C11 builds, warnings as
  errors; four PS5 artifacts built, no deployment.
- `python3 /Volumes/4TB_Drive/tmp/t040-t042/verify-local.py`: previous media
  checks plus twelve input invocation/layout/refusal cases and an unwritable
  output regression. Host input hardware operations are always refused.
- `clang-format --dry-run --Werror` on both new C sources; host and PS5-target
  `clang-tidy` with the exact flags recorded above, substituting `input_system.c`:
  exit 0, no project diagnostics and no lint suppression added.
- Native lifecycle simulation under
  `/Volumes/4TB_Drive/tmp/t040-t042/lifecycle-test/lifecycle.c`: six cases
  cover four-handle completion, failed third add, failed initial neutral,
  failed removal with continued cleanup, failed HID open, failed UserService
  initialization; exact acquired-handle removal, all-neutrals-before-removal,
  reverse removal order, no duplicate deletion and signal-handler restoration.
  All substituted APIs are host fakes, **not console evidence**.

The simulation passes both a plain LLVM 18.1.8 build and an Apple Clang
AddressSanitizer/UndefinedBehaviorSanitizer build:

```sh
/usr/bin/clang -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I/Volumes/4TB_Drive/tmp/t040-t042/lifecycle-test/include /Volumes/4TB_Drive/tmp/t040-t042/lifecycle-test/lifecycle.c -o /Volumes/4TB_Drive/tmp/t040-t042/lifecycle-test/lifecycle-apple
/Volumes/4TB_Drive/tmp/t040-t042/lifecycle-test/lifecycle-apple
```

The first LLVM 18 sanitizer-built simulation consumed CPU without output and
was stopped with SIGTERM after a bounded manual wait; its plain build and
the Apple sanitizer build both exited 0 with six cases passing. The cause of
that host-only sanitizer/runtime failure is **unknown**, not an established
console/compiler defect or a passing sanitizer result. No console was involved.

Initial approved input source SHA-256 (not executed):
`001fba7be54995b5b47f867606917e5ae0d9204b23c7ae6dd67539d2c7548f14`;
ordinary diagnostic ELF:
`817b17a5466ab914d60cdf5adf6615dc1c81e97901d984d1824852cb90f05d3a`;
ten-second input ELF:
`dc35ed4d3d860721f892346cd141f32600357aecbeaa294e8a09450a0910a091`;
read-only media-inventory ELF:
`3e5df2ba68b7ced5050f9140f1e728b4636a56da7d2e2eae35274f24d7460922`.
These are preparation hashes only. A combined window/payload/owner-observation
request was sent through Orca; execution approval and observations follow.

## Approved console window: read-only R3 inventory

The coordinator granted this worker exclusive console ownership, approved the
exact network/media/input binary hashes below, and required separate
`OWNER OBSERVE BEFORE` / `OWNER OBSERVE AFTER` asks around input. The owner
was ready to observe via their QuickTime preview. This did not authorize capture
ownership or mode changes, additional probe-defined privilege changes, hooks,
rest/reboot or new media reverse engineering. No such probe-defined operation
occurred. The stock SDK's before-main writes were subsequently identified;
the qualification and hold below supersede earlier blanket zero-change wording.

Live shsrv banner: **13.600.007**, PID120; SDK media diagnostic firmware context
**0x13600007**. Existing processes include etaHEN services, kstuff and development
services; this is an observation of the current environment, not independent
hypervisor attestation. Mac remains Darwin arm64 / macOS27.0.1.

Readiness commands (raw observations under `preflight/` in the local evidence root):

```sh
ifconfig en9
sysctl net.inet.ip.forwarding net.inet6.ip6.forwarding
netstat -rn -f inet6
ping -c 2 192.168.2.2
nc -z -G 2 192.168.2.2 9021
ping -c 1 -W 1000 -S 192.168.2.1 192.168.2.254
arp -n 192.168.2.254
bash /Users/jefflouisma/.agents/skills/ps5-dev-services/scripts/services.sh status
python3 /Volumes/4TB_Drive/tmp/t040-t042/console-readonly.py ps --out /Volumes/4TB_Drive/tmp/t040-t042/preflight/ps.log
```

Observed en9 active with 192.168.2.1/24 and link-local IPv6 only. Both Mac
forwarding flags are **1**, retained unchanged; these flags alone do not prove
the console is Internet-connected or isolated. Console ping: two replies, no
loss; loader9021 and all four preexisting development-service ports respond.
The deliberately dead gateway192.168.2.254 returned no ping response and has
only an **incomplete** ARP entry on en9.

The reused source-only-network build uses `test_pattern.c` SHA-256
`f27d18053f26a07e676c8fe04047416bfe2009781f5df476b3d5850b8530fe0a`,
compiled with `-DTEST_PATTERN_NETWORK` only, SDK v0.45 / LLVM18.1.8,
`-std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror -lkernel -lSceSystemService`.
Approved ELF SHA-256:
`30154bcf13d0ee3d870426f7d3cb328f3aadcaa3c8fa72344d6666d5fd556248`.
Its exact loader command was:

```sh
timeout 20 /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy -h 192.168.2.2 -p 9021 /Volumes/4TB_Drive/tmp/t040-t042/build/network-inventory.elf
```

PID213 emitted `R4_NETWORK_END result=0`; the transport exited0. The bounded
read-only route decoder, copied to this task's scratch directory from R4 and
using the independently target-verified SDK route layout, consumed both complete
route dumps. Console interfaces contain only eth0 192.168.2.2/link-local IPv6
and loopback. The sole IPv4 default gateway is192.168.2.254; **no IPv6 default
route or global IPv6 address exists**. Together with the retained owner
attestation of the sole direct connection and the unresolved gateway neighbor,
this verifies the accepted IPv4/IPv6 isolation disposition without changing Mac
or console network state. The console query reports **active_bigapp=-1**,
temporary FAKE00000 paths absent, and `/system_ex` flags4160, retained unchanged.

The exact approved media diagnostic invocation was:

```sh
timeout 20 /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy -h 192.168.2.2 -p 9021 /Volumes/4TB_Drive/tmp/t040-t042/build/capture-inventory.elf
```

PID214 observed regular files for Remoteplay, Avcap2, VideoRecording, Venc,
AudioOut and VideoOut at the seven declared candidate paths; AudioSystem's
candidate path returned **stat=-1/errno2**. A missing candidate path does not
prove the service is absent in all namespaces. Every printed callability and
permission field remains **unknown**. It acquired no media resources and printed
`R3_MEDIA_RESULT status=incomplete readiness_gate=open` with all five required
modes unavailable/observations absent. The loader transport exited0; this is
**not** a media-capability pass or independently observed native exit status2.
A subsequent independent shsrv process inventory confirms both PID213 and
PID214 are absent; shsrv remains responsive with the same firmware banner.

### Stock SDK startup qualification and input hold

The subsequently read [separate media audit](research-r3-media-re.md) identifies
a missed runtime qualification. Independent inspection of pinned SDK
`crt/crt.c` confirms `__patch_init` runs before `main`. `crt/patch.c` writes
current-process capability bytes5/7/15, attribute byte3 and syscall-origin
bounds. Therefore the initial media diagnostic's printed
`privileged_changes=none` was **too broad**: it describes no operations added
by the probe body, not the stock runtime. Source/output wording has been
corrected. The earlier network/media inventory hashes identify what actually
ran; corrected binaries require a fresh approval before execution.

PID213/214 absence establishes their exit, **not independently verified reversal
of all possibly shared credential effects**. No before-main snapshot was taken;
no unconditional restoration claim is justified. The worker immediately held
the unexecuted controller run and sent an escalation asking whether the owner
explicitly accepts the existing stock SDK bootstrap as the baseline or requires
an audited runtime/reversal. No custom kernel hook, credential fallback or
speculative restoration was attempted. At this checkpoint **input has not run**.

The media audit also independently finds no qualifying video/hardware-encoder
path; its source-pinned SoloBridge caller reports 13.60 audio use, improving the
audio candidate's upstream firmware evidence. It preserves unresolved metadata
input/output semantics, permission and cleanup limits; it calls no media API
locally. That report is a separate research result, not evidence produced by
this worker or a substitute for the mandatory media observations above.

### Explicit stock-runtime decision and neutral-only console run

The coordinator first accepted the pinned SDK bootstrap as the baseline of the
owner's existing homebrew environment, then conveyed an **explicit owner
decision** accepting those per-process before-main writes for this nonshipping
run while retaining the reversal gap. The owner approved corrected input source
`82d6744ac2773fc5246401c691d8105afca2186eba93f005dffb97bec423598a`
and ten-second ELF
`45175658e2ea7a528a57ece91baacd3bf5e57785f4caa9c9860e56aacb511bca`.
The exact executed source/binary copies are retained under
`corrected-payloads/` in the local evidence root. **Those** bytes produced the
following observations; subsequent navigation-preparation hashes are different.

The coordinator relayed the owner's fresh before-state: **normal home UI,
physical controller navigates, owner watching the QuickTime preview**. One run:

```sh
timeout 25 /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy -h 192.168.2.2 -p 9021 /Volumes/4TB_Drive/tmp/t040-t042/build/input-four-neutral.elf
```

The transport exited0. PID220 read firmware0x13600007, initialized UserService,
queried the foreground user and opened HID. Four AddDevice calls returned0 with
distinct handles **1442573, 1508108, 1573643, 1639178**. Every initial and periodic
report was centered neutral, with no buttons; **994 initial/periodic reports per
slot** were accepted, followed by one additional neutral per slot in cleanup.
All four cleanup-neutral calls returned0 **before any removal**; reverse-order
DeleteDevice calls, HID close and UserService termination all returned0.
`stop_requested=0`; the program explicitly printed effective delivery/removal
unverified and hardware acceptance Incomplete. No active game or nonneutral
input was part of this run.

Raw operation timestamps use console CLOCK_MONOTONIC truncated to microseconds
and mark observation after the operation. Clock resolution/accuracy was not
measured, and no authenticated network input-arrival boundary exists here.
These logs are not an SC-005 latency population or proof of a sub-2ms boundary.

The coordinator relayed the owner after-state: **normal home UI as before; no
assignment dialog or ownership change observed during or after; no visible
phantom controllers/pending assignment; physical controller still navigates**.
The owner observed **no visible effect** from the neutral virtual devices.
This corroborates no visible persistent side effect for this run and the
successful cleanup returns; it does not prove effective input delivery or an
independent census of all console input devices. No held button was exercised.

Postflight read-only shsrv inventory independently confirms PID220 absent.
All existing websrv/gdbsrv/klogsrv/shsrv ports remain listening. Repeated approved
network inventory PID226 completed with result0 and is independently absent;
the fully decoded interface/route summaries match preflight exactly, still no
active BigApp and mount flags4160. No capture ownership/mode, console files,
system settings, hooks, priority/affinity or extra credential fallback changed.

Read-only `procstat -s 82 98 120` was attempted through shsrv before and after.
It emitted **no credential rows**, only a prompt; this is an unavailable
credential observation, not an empty/unchanged capability set. Process listings
show loader PID82, websrv PID98 and shsrv PID120 with UID0 before and after,
which verifies only UID, **not caps/attrs/syscall bounds or shared-ucred
reversal**. That limitation remains explicit under the accepted baseline.
The **shipping host T070 must handle this as an explicit `platform/privileged`
design/reversal item**, rather than silently inherit a claim of zero writes.
Pinned source:
[SDK crt/patch.c](https://github.com/ps5-payload-dev/sdk/blob/a7685ce8e97ce1f8a4f38253a41d54241c9124f1/crt/patch.c).

The worker sent **CONSOLE WINDOW RELEASED** after these checks and the owner
after-state. No controller or other probe remains running.

### Separate positive-delivery probe and approved execution

At the coordinator's suggestion, a separate **one-controller** delivery probe
is prepared for an explicitly observed harmless home-screen navigation test:
six-second lifetime, Right (`0x20`) for120ms near t=2s, neutral, Left (`0x80`)
for120ms near t=4s, neutral and owned-handle cleanup. Both button bits and report
positions are sourced from ps5-mcp84c0f7b. It never sends Cross/PS/Create, launches
a title or automates assignment. The default and four-neutral modes preserve
their previous behavior. All cleanup paths rebuild the complete neutral report
before neutralization/removal, including a failed report while Right is held.

Two additional host simulations verify the right/left sequence and failure
while Right is active, with neutral-before-remove; the Apple ASan/UBSan suite
now passes **eight** lifecycle cases. Host argument checks additionally refuse
`--navigation-once`. Both host/PS5 builds and both clang-tidy modes pass without
project diagnostics. These remain software evidence only.

Current input source:
`7bf7f4f6ea3b360a6c447fdbf751df8e459385b1bbb9f423d8fc0c400aa6b082`;
navigation ELF:
`e79dbf667ea687be0499995b08b5572030ec7941f4da9427b193d6b42a270039`.
Current media source:
`6481c8bf4eb36a941dbef51c0c29261c2e294e74ed0822b9fe3664fcd2e86ae3`;
inventory ELF:
`2abb87d16f59d5be6c243a9a893417aea6538c408068ffd654bb54ac34eb4c36`.
Both current runtime outputs explicitly identify stock SDK per-process writes
before main, with unverified reversal and no added probe-defined privilege
change. A **new** CONSOLE WINDOW REQUEST, exact-hash command and owner-observed
before/after proposal was sent. Only the navigation artifact was subsequently
approved and executed below; the newer media diagnostic was not rerun.

SDK `crt/crt.c` also permits argc0 when no argument helper is resolved. A
source-inclusion host regression initially reproduced the ordinary input
diagnostic returning64 instead of safe describe/Incomplete2 for argc0; the
one-line `argc <= 1` default guard corrects it, and the regression now passes.
The explicit autorun variants already guarded argc0; this fix does not change
the neutral-only run observed above. The updated source/navigation hashes were
reported to the coordinator before any further approval/execution.

The coordinator granted a fresh exclusive window and initially approved the
pre-regression-fix hashes, then explicitly **superseded** that approval with the
current `7bf7...b082` source / `e79d...0039` ELF pair above before execution.
The owner before-state was home screen, no game, physical controller works,
owner watching the focused tile. The approved stock-runtime baseline persists;
no additional privilege permission was inferred. Exact run:

```sh
timeout 20 /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy -h 192.168.2.2 -p 9021 /Volumes/4TB_Drive/tmp/t040-t042/build/input-navigation-once.elf
```

One run, PID230 / firmware0x13600007, handle1704717. All **598** initial/periodic
reports were accepted. Source transition observations relative to loop start:

| Requested report state | Console monotonic elapsed observation (us) |
|---|---:|
| Right0x20 | 2,004,623 |
| Neutral0 | 2,125,255 |
| Left0x80 | 4,005,455 |
| Neutral0 | 4,126,088 |

These mark report selection on the loop, not physical application/display times.
The planned120ms holds span120,632/120,633us between those source observations;
timer sampling and driver/display latency are not silently subtracted. No
SC-005 measurement or sub-2ms latency claim follows from these values.

Cleanup neutral, DeleteDevice, HID close and owned UserService termination each
returned0; source still labels effective delivery/removal unverified pending
independent observation. The coordinator then relayed the owner's after-state:
**the focused home tile visibly moved one tile RIGHT and returned LEFT; no
dialogs or unexpected UI; normal final UI; physical controller still works;
no visible phantom/pending controllers**. This is positive **operator evidence
of one virtual controller's D-pad delivery on13.60**. It is not a four-player
game observation, a complete button/motion/touch mapping, reverse rumble proof
or an exact timestamp of console application.

For this researched path, “successful delivery” requires a correctly laid out
report accepted by the input subsystem **and** corroboration of its intended
console action. The ioctl-return timestamp is a candidate acceptance boundary;
the public reference warns that header acceptance alone can hide wrong contents.
The actual application/completion semantics and timing still need source or
independent instrumentation before it can serve SC-005 for all accepted events.
Do not use network acknowledgements or a neutral-state no-op as that proof.

Read-only network inventories ran before (PID229) and after (PID236), using the
same approved30154...6248 bytes. Both completed result0 and were independently
absent in final shsrv inventory, along with input PID230. The complete decoded
IPv4/IPv6/interface summaries are identical, no active BigApp, mount flags4160
unchanged; all four preexisting service ports listen. A repeated procstat query
again produced no credential rows, leaving caps/attrs/shared-ucred effects
unverified. No capture device ownership/mode or console setting was changed.
The worker immediately sent **CONSOLE WINDOW RELEASED** for the queued workers.

Exact executed navigation source/ELF copies are frozen under
`/Volumes/4TB_Drive/tmp/t040-t042/navigation-approved/`; raw output is
`input-navigation-once.log`, with `nav-preflight/` and `nav-postflight/` checks.
`manifest.json` retains raw-file hashes; serial/identifying console banners
remain local, outside this report/repository.

## Final task disposition

- **T040 done / checked:** source-pinned candidates, declarations/layouts,
  permission/runtime caveats and forbidden guesses are documented.
- **T041 Incomplete / unchecked:** inventory and truthful fail-closed diagnostic
  are built/run; sustained media modes, hardware encoder, ten-bit bitstream,
  bounded conversion, independent source metadata, protection and media lifecycle
  evidence remain unavailable. The separate media research report does not close them.
- **T042 Incomplete / unchecked:** four handles, clean returned neutral teardown,
  one positive D-pad sequence and one boundary-confounded held-Right normal-stop
  observation are retained; full four-controller functional
  assignment, required controls including PS/Create, reverse rumble, motion/touch
  coordinates, conflict transitions, rest cleanup and game-safe scheduling
  remain unverified/unavailable. No unsupported feature was exercised through a
  guessed ABI, and no missing observation was labeled a pass.

All requested local builds, formatting/static analysis, argument/layout/error
checks and eight Apple sanitizer lifecycle simulations pass. No shipping-host
readiness is approved; T045 remains open. No probe is running, all owned
DeleteDevice calls returned0 with normal owner after-state, and no
console/capture window or probe-defined hook remains held.

## T041 follow-up — Remote Play acquisition and unexecuted probes

**2026-10-25, direct owner assignment:** the owner confirmed proceeding despite
the unchecked review checklists and specifically directed reverse engineering
Remote Play for video. Checklist markers, acceptance requirements and T041/T045
remain unchanged. This follow-up does not replace earlier observations or claim
that a media interface has been recovered.

### Implemented and locally verified

`capture_encode.c` now contains an explicit five-second Avcap2 audio **candidate**
using the public caller declarations recorded in
[the separate media audit](research-r3-media-re.md). An internal header exposes
the same lifecycle to simulated tests. The default payload remains describe-only;
the separately named `capture-audio-five-seconds.elf` selects the experiment.
It reserves 8192 sample bytes and 16 opaque metadata bytes, bounds the polling
population, validates byte alignment/capacity and finite float samples, and
records empty/overrun/sample/nonzero counts separately. No samples are saved or
forwarded. Stop, Close, Terminate and owned-module unload results are preserved;
cleanup failure cannot become success on a second cleanup attempt. There is no
credential fallback, service restart or guessed video call.

**The audio candidate has not run.** The public metadata/argument disagreements,
source-clock/protection semantics, individual service-call deadlines, local
non-silence/stereo ordering and independent cleanup remain unresolved. The code
does not interpret opaque metadata or the public 48 kHz label as measured source
timing. Its default diagnostic still reports audio as unverified.

The new [Remote Play research exporter](../../tools/ps5-probes/re/README.md)
selects the resident Remote Play/AV Capture/AV Control processes and a fixed
media-module list. It reads only reported executable text and linker
symbol/string/relocation tables through sourced SDK facilities, with a firmware
13.60 layout table, cycle rejection and per-region/total bounds. It neither
loads nor calls media services, attaches a debugger, stops a Sony process,
changes page tables nor exports a process heap. Its private output directory
uses exclusive file creation; incomplete regions are removed. The normal
60-second window is cooperative; individual kernel/filesystem calls have no
verified hard deadline. Live/relocated regions are not original ELF files or
an atomic snapshot. **No exporter payload has run.**

The exporter retains the stock SDK startup caveat: process credential and
syscall-origin writes occur before main, with reversal/shared effects
unverified. The proposal includes exact source/header/ELF hashes, pre/post
network checks, bounded local retrieval, independent process/service checks and
cleanup of only the run's files. It is awaiting separate exact-payload/runtime
approval and a fresh exclusive window.

Local checks completed with no console capability inference:

- Warning-free host/PS5 builds with installed SDK v0.45 and LLVM 18.1.8 through
  `build-r3.sh`, including both separately named experimental ELFs.
- Two portable CMake suites registered under `unit;nonshipping-probe`, passing
  with Apple Clang AddressSanitizer and UndefinedBehaviorSanitizer. Cases include
  each audio startup/cleanup failure, persistent cleanup failure, invalid record
  sizes/nonfinite samples, empty/overrun accounting, host/argc0 refusal/defaults,
  page-boundary/overflow/size/deadline/cancellation checks, exclusive file
  creation, partial-file removal, module selection and module-list cycles.
- Clang-format and the repository's full clang-tidy configuration pass with the
  pinned LLVM 18 CMake compile database for the four new/modified translation
  units. No warning suppression was added. Shell syntax validation passes.
  The separate Apple sanitizer compile database triggers that LLVM analyzer's
  deprecated-buffer API diagnostics for bounded libc calls; it is not reported
  as a passing static-analysis configuration. The pinned LLVM build is the
  quality configuration, and Apple Clang supplies the working sanitizer runtime.

These are targeted probe checks, not a full repository/core regression run.
An initial LLVM 18 sanitizer executable did not complete and was terminated as
an owned local test; it is not counted as passing sanitizer evidence. The Apple
sanitizer build runs both suites successfully. Existing SDK/compiler provenance
limitations in the dependency record are not waived by a successful cross-build.

### Fresh read-only console observations

The active coordinator granted an exclusive ten-minute read-only window;
it was released at **22:04:30 UTC**. The scope excluded payload execution,
debugger attachment, service/setting changes and online sessions. No capture
device was accessed. The fresh shsrv process inventory identifies:

| Process | PID / title | Observation |
|---|---|---|
| `SceRemotePlay` | 85 / NPXS40102 | Resident, sleeping; no Remote Play session started |
| `SceAvCapture` | 59 / NPXS40100 | Resident, sleeping |
| `SceSysAvControl.elf` | 50 / NPXS40095 | Resident, sleeping |

FTP LIST `/system/vsh/app/NPXS40102` reports `eboot.bin`, 2,878,404 bytes;
LIST `/system/vsh/app/NPXS40100` reports `eboot.bin`, 82,625 bytes. These are
directory-list sizes, not retrieved files, executable layouts or permissions.

| Exact fresh-connection RETR | Result |
|---|---|
| `/system/vsh/app/NPXS40102/eboot.bin` | Timeout after 3.008 s; zero bytes |
| `/system/vsh/app/NPXS40100/eboot.bin` | Timeout after 3.007 s; zero bytes |
| `/system/vsh/SceSysAvControl.elf` | FTP 550; zero bytes; this candidate path is not verified |
| `/system/common/lib/libSceAvcap2.sprx` | Timeout after 3.009 s; zero bytes |
| `/system/common/lib/libSceVenc.sprx` | Timeout after 3.007 s; zero bytes |

LIST `/system/priv/daemon` also returns 550. No failure above proves absence,
encryption, access policy or a required privilege. Every transfer used a fresh
connection; incomplete local files were removed. There is still **no acquired
Remote Play code image, Ghidra decompilation or recovered video calling contract**.
The exporter is the concrete proposed next acquisition step, not completed RE.

Mac en9 has the expected 192.168.2.1 address and a link-local IPv6 address;
the Mac's IPv4 and IPv6 forwarding flags both read **1**. That alone establishes
neither isolation nor a console Internet route. The shsrv sysctl/procstat
requests did not yield the console's current interface/default-route inventory.
Consequently fresh IPv4/IPv6 isolation remains **unverified**, and no payload or
media experiment was executed. Routes, PF, NAT, Internet Sharing, console
settings and the existing homebrew environment were preserved.

Private raw logs/manifests, build outputs and local validation remain under
`/Volumes/4TB_Drive/tmp/t041-audio-review/`, including `readonly/`. Console
banners/identifiers, binaries and future code/table exports stay outside Git.
The coordinator received the window release and these limitations. **T041 stays
Incomplete/unchecked; T045 stays open.** Sustained capture/encoding modes,
hardware H.264/HEVC/Main10, bounded HDR-to-SDR, source HDR metadata/protection,
audio and on-device resource/lifecycle evidence still require actual observations.

## T041 exporter execution window (2026-10-25, worker task_82d1542f8324)

The coordinator granted an exclusive console window under the owner's standing
2026-10-25 direction (scope: isolation preflight, single exporter run, FTP
retrieval/cleanup; the five-second audio candidate explicitly deferred to a
later owner-observed window). Approval record: reply to `msg_68fd833885d9`.

Mac preflight (`/Volumes/4TB_Drive/tmp/t041-hermes/evidence/preflight/`):
en9 active with 192.168.2.1, forwarding flags retained, console reachable
(0% loss), loader 9021 and all four development services listening, dead
gateway 192.168.2.254 100% loss with incomplete ARP. The approved read-only
network-inventory ELF (`30154bcf…6248`) ran as PID259 and returned result=0;
the fully decoded interface/route dump matches the accepted isolation
disposition: only eth0 192.168.2.2 + link-local IPv6 + loopback, sole IPv4
default 192.168.2.254, **no IPv6 default route and no global IPv6 address**.

### First approved exporter run — failed harmlessly

Executed exactly as approved (ELF SHA-256 `ec1a8af8223881aebe899d98e36ef21507822c85ba9af9d80023ae40bcd5ae44`,
verified before launch):

```sh
timeout 90 /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy \
  -h 192.168.2.2 -p 9021 \
  /Volumes/4TB_Drive/tmp/t041-audio-review/build/remoteplay-export.elf
```

PID260 printed only `R3_RE_CONTEXT firmware_raw=0x13600007` and
`R3_RE_BEGIN …`, then exited **1** about six seconds after launch; the kernel
log independently records `process pid=260, payload.elf calls exit()
exit_value=1`. No crash, panic or reboot occurred. FTP LIST verified the
exporter's private output directory `/data/ps5-dev/t041-remoteplay-260/`
exists and is **empty** (correct per design: no region completed). All three
target processes (SceRemotePlay PID85, SceAvCapture PID59,
SceSysAvControl.elf PID50) remained resident and sleeping after the run, and
the shsrv inventory shows the loader/services unchanged. No media call,
debugger attach, page-table write or service change occurred; only the empty
directory was created.

Local source review identified the defect: the exporter rejected the
`sysctl(KERN_PROC_PROC)` buffer whenever its total size was not a multiple of
`sizeof(struct kinfo_proc)`. The console emits **variable-size** records; the
pinned SDK's own `samples/ps/main.c` walks them by each record's
`ki_structsize` field instead. The exporter was corrected to walk records the
same sourced way, with bounded per-step diagnostics (sysctl errno, buffer size,
record size, up to five sample entries) and all prior bounds, cleanup and
no-media-call guarantees unchanged. Corrected artifacts: source
`tools/ps5-probes/re/remoteplay_export.c` SHA-256
`272456e5e7b5ea00c6612697bb7e62e3e6fe1e678013c394de411a8e5befebd4`,
header unchanged
`c9b91ae67da86c7f8b82a8ff803a9af947c39d19ea6da62b887f7a5ed404c2b2`,
ELF `/Volumes/4TB_Drive/tmp/t041-hermes/build/remoteplay-export.elf` SHA-256
`7dbd2253b483e32de0a474d62b4eef584d64b89926c25924fc9d73e1518a71a5`.
Both build-r3.sh suites pass warning-free; clang-format and the repository
clang-tidy configuration (pinned LLVM 18 compile database) are clean; a
macOS-bash-3.2 empty-array expansion bug in `build-r3.sh` was also fixed.
The corrected binary has **not** been executed without the fresh exact-hash
approval below.

A kernel-log observation from this window also shows etaHEN's FTP daemon
decrypted `/system/vsh/app/NPXS40102/eboot.bin` and
`/system/vsh/app/NPXS40100/eboot.bin` into `/user/temp/PS5_temp_file_*`
during the earlier read-only window's short RETR attempts; whether those
decrypted originals persist was proposed for read-only check in the same
approval. Nothing was retrieved or deleted from the console in this record.

### Corrected exporter run — success, tables retrieved, directories cleaned

The coordinator re-approved the corrected exporter (reply continuing the same
window; source `272456e5e7b5ea00c6612697bb7e62e3e6fe1e678013c394de411a8e5befebd4`,
ELF `7dbd2253b483e32de0a474d62b4eef584d64b89926c25924fc9d73e1518a71a5`,
verified before launch). One run:

```sh
timeout 90 /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy \
  -h 192.168.2.2 -p 9021 \
  /Volumes/4TB_Drive/tmp/t041-hermes/build/remoteplay-export.elf
```

PID263 confirmed the diagnosis: the `sysctl(KERN_PROC_PROC)` size query
returned 104,120 bytes and the filled buffer 98,640 bytes as **90
variable-size records of 1,096 bytes each** (`kinfo_proc_size=1088` in the
SDK header; every record's `ki_structsize=1096`), walked exactly as the SDK
sample does. Three targets selected: **SceRemotePlay PID85** (47 module
nodes), **SceAvCapture PID59** (20 nodes), **SceSysAvControl.elf PID50**
(1 node). 24 linker-table regions exported (102,768 bytes): for PID85
`eboot.bin`, `libSceIpmi.sprx` and `libSceAvcap2.sprx` (symbols, strings,
pltrela, rela each); for PID59 `eboot.bin`, `libSceIpmi.sprx` and
`libSceVenc.sprx`; PID50's single module reported `mapbase=0` and no dynsec,
so it exported nothing.

**Text regions were not exported**: the live 13.60 `dynlib_obj` field at the
SDK `textsize` offset (0x40) holds a `0xffffff80xxxxxxxx` kernel pointer for
every module, while `handle`, `mapbase` and `dynsec` (0x28/0x30/0x148)
verified correct; `textsize > mapsize` then skips the text region silently by
design. This is a **live 13.60 layout divergence from the pinned SDK struct
between offsets 0x38 and 0x148**, recorded as a fact from the run; no text
bytes were guessed or fabricated. The linker tables above are unaffected.

Retrieval via etaHEN FTP (port 1337, fresh connection per file, listed-size
verification, SHA-256 each) copied all 24 files byte-exact into
`/Volumes/4TB_Drive/tmp/t041-hermes/evidence/remoteplay-263/` (manifest:
`retrieval-manifest.json`); the earlier system-path RETR timeouts do not apply
to `/data` paths. Postflight: network-inventory PID264 result=0 with route
dump identical to preflight; exporter/network/first-run PIDs 263/264/260 all
absent; all three target processes still resident and sleeping; both run
directories (`t041-remoteplay-260`, `t041-remoteplay-263`) DELE/RMD-removed
after verified retrieval and confirmed gone by LIST. The read-only
`/user/temp` LIST showed the earlier decrypted `PS5_temp_file` entries were
already cleaned by etaHEN; the decrypted eboots were therefore requested
directly (below). No media API was called at any point in this window.

### First RE finding from the retrieved tables (offline, local analysis)

Decoding the exported `Elf64_Sym`/`Elf64_Rela` tables (SDK `crt/elf.h`
layouts; 24-byte entries) shows the PS5 13.60 dynlib string table names
symbols `NID#X#Y`. Matching by NID prefix and recomputing NIDs with the
SDK's published name-hash scheme (`crt/nid.c`, also used by
`tools/ps5-probes/re/audit_media_sources.py`) produced these verified,
cross-checked facts (live export table AND real caller PLT imports agree):

- `libSceAvcap2.sprx` loaded in SceRemotePlay (PID85) exports
  `sceAvcap2OpenVideo` = NID `4jwUgirLFrA` (export function size 973 bytes)
  and `sceAvcap2ReadVideo` = NID `pN0hIQZsMzc` (295 bytes), alongside all
  seven public audio functions (`sceAvcap2OpenAudio` 461 B, `ReadAudio`,
  `Start`, `Stop`, `Close`, `Initialize`, `Terminate`) at their recorded NIDs.
- SceRemotePlay's own eboot PLT (JUMP_SLOT relocations) imports all of the
  above at recorded PLT offsets (0x56be90–0x56bf48 range), plus 14 more
  Avcap2 exports of currently unknown names and Ipmi IPC functions
  (`#F#G`-suffixed).
- `libSceVenc.sprx` loaded in SceAvCapture (PID59) exports 27 defined
  functions including `sceVencCreateEncoder` = NID `irD6EyOPBZE` (587 B);
  SceAvCapture's eboot PLT imports 25 of them (PLT 0x29b70–0x29f28 range).
- Architectural consequence: **SceRemotePlay is the Avcap2 capture client;
  SceAvCapture is the Venc hardware-encoder client.** The full argument
  contracts (request structures, ownership, completion) still require code
  disassembly of the two caller eboots.

These are live-module and real-caller observations, not guessed signatures.
No video/encoder function was called; T041 remains open pending the calling
contracts and device evidence. Full per-file analysis artifacts live under
`/Volumes/4TB_Drive/tmp/t041-hermes/analysis/` (module export tables, PLT
import maps, NID computations); raw retrieved regions under
`/Volumes/4TB_Drive/tmp/t041-hermes/evidence/remoteplay-263/`.

### Approved decrypted-eboot RETR attempts — failed, temp files cleaned

The coordinator approved two read-only FTP `RETR`s (120-second timeout each,
fresh connection per file) of `/system/vsh/app/NPXS40102/eboot.bin` and
`/system/vsh/app/NPXS40100/eboot.bin`, after a `/user/temp` LIST showed the
earlier decrypted `PS5_temp_file` entries no longer existed. Both transfers
received **zero bytes in 120 s each**. The kernel log shows the etaHEN
utility daemon began `decrypt_self_ftp` for each attempt
(`/user/temp/PS5_temp_file_53_1792972561_3396`,
`/user/temp/PS5_temp_file_17_1792972681_4267`) but never logged completion
or streamed data, and a post-attempt `/user/temp` LIST showed both temp files
already removed on disconnect. Combined with the earlier 3-second failures,
decrypt-on-`RETR` has now failed at 3 s and 120 s on this console; the
failure mode (daemon starts decrypting, no data, no completion) is recorded
without claiming a cause. No console file was modified by these attempts; the
decrypted-original path is closed for now, and module **memory images**
became the acquisition path for the caller code.

### Exporter v3 (image dumps) and v4 (kept partial text) runs

The coordinator approved a third exporter variant (v3: raw 384-byte module
objects + full mapped images + the four tables per selected module; ELF
`8f833a3579f40dfd169d22d8a354c942d012c9a3deafdef5e31e9b16a2c1ff68`). One
run, PID267. The raw objects decode the live 13.60 `dynlib_obj` layout
definitively: `next/path`, `refcount`, `handle`, `mapbase`, `mapsize`,
`entry`, `vaddrbase`, `dynsec` all sit at the SDK offsets and carry sane
values (e.g. SceRemotePlay eboot mapbase `0x41bb8000` mapsize
`0x584000` entry `mapbase+0x80`; Avcap2 mapbase `0x814e48000` mapsize
`0x2c000`), but the SDK `textsize` slot 0x40 holds a kernel pointer
(`0xffffff82bb…` / `0xffff82bb…`) and slot 0x48 a small integer (4; 3 for
SceSysAvControl) — Sony's 13.60 layout replaced the size fields between
0x38 and 0x148. `mapsize` at 0x38 is therefore the correct image bound.

v3's image dumps stopped at the first unmapped page of each module (the
13.60 text/data mapping gap): SceAvCapture's eboot image **succeeded
completely** (360,448 B at `0x5badc000`), while SceRemotePlay's eboot
(147,456 B), Avcap2 (81,920 B), Venc (114,688 B) and Ipmi (81,920 B) failed
partway and were deleted per the then-current rule. The coordinator then
approved v4 (ELF
`cfb195bbda0537515f38c6978d658146fa17c6f361681fa50704177665f6474a`), whose
only semantic change was keeping a nonzero text prefix on the unmapped-page
failure, explicitly marked `status=partial`. One run, PID271, retrieved 37
files (973,808 B) into `evidence/remoteplay-271/`; postflight
network-inventory PID272 result=0 identical; all payloads absent and the
three targets resident; run directory DELE/RMD-cleaned and verified gone.

The v4 SceRemotePlay eboot text (147,456 B at `0x41bb8000`) is verified
real code — a proper entry prologue at `mapbase+0x80`, hundreds of
function prologues, an `NPXS4010A` title string — but its Avcap2 call
sites go through PLT stubs located beyond the first unmapped page (calls
target image offsets around `0x330e0`–`0x4667c0` while the readable
prefix ends at `0x24000`), so stub-to-GOT identification needed a
gap-skipping walk (v5, below). The Avcap2/Venc/Ipmi module texts are
readable prefixes of the same kind.

### Exporter v5 (gap-skipping segment walk) run — full images recovered

The coordinator approved v5 (source
`6b18a84d902674a86e734d7978838dbe3b3124865725bd897c2e4628b0b1a071`, ELF
`11b443de49fff827435d45e38ee8e291249ed99d822aebc96b757ee2c6feabcd`):
a 4 KiB-step walk over each selected module's `[mapbase, mapbase+mapsize)`
that keeps every readable run in one `<pid>-<module>-segs.bin` and prints
a per-run `R3_RE_SEGMENT` manifest plus an `R3_RE_SEGS` summary; unmapped
pages are skipped and counted, never interpolated. One run, PID275,
retrieved 37 files (3,578,864 B) into `evidence/remoteplay-275/`;
postflight network-inventory result=0 identical, all payloads absent,
targets resident; run directory cleaned and verified gone. Recovered
(hole-free, manifest-backed) image content: SceRemotePlay eboot 38 runs /
2,523,136 B readable of 0x584000 mapped (the rest unmapped, mostly BSS);
SceAvCapture eboot complete 360,448 B; Venc 4 runs / 344,064 B of
0xac000; Avcap2 and Ipmi 81,920 B each.

### Recovered calling contracts (offline RE of the v5/v4 images)

All facts below come from disassembly of the manifest-backed live images and
the exported `Elf64_Sym`/`Elf64_Rela` tables (SDK `crt/elf.h` layouts;
NID recomputation via the SDK's published name-hash scheme). The
SceRemotePlay PLT reconstruction found all **699 stubs** (contiguous,
absolute `0x4201e7c0`–`0x42021360`) and resolved **8,797 call sites**;
the SceAvCapture reconstruction (complete image) resolved 753 sites
through 175 stubs. These are observations of the resident firmware-13.60
clients, not guessed signatures; no media function was called.

**Architecture (verified):** SceRemotePlay (NPXS40102) is the Avcap2
capture client — its PLT imports 22 Avcap2 functions. SceAvCapture
(NPXS40100, build path `W:\Build\J03950016\vsh\daemon\av_capture_manager`)
is the Avcap2 manager daemon and the **libSceVenc encoder client** — its
PLT imports 25 Venc functions. Capture buffers flow through shared memory
named in SceAvCapture's image: `shm_acap.full_12ch`, `shm_acap.full`,
`shm_acap.share`, `shm_acap.restrict`, `shm_acap.aux`, `shm_ucap`,
`shm_scap`, `shm_image`; encoder memory through `/dmem_venc_cpugpu_r`,
`/dmem_venc_gpu_r`, `/physhm_venc_cpugpu_i`, `/physhm_venc_gpu_i/r`.

**Avcap2 video contract skeleton (SceRemotePlay disassembly):**
- `sceAvcap2OpenVideo` (NID `4jwUgirLFrA`) has exactly one call site
  (`0x41c2868c`) inside a session-setup function at `0x41c284c0`:
  called as `OpenVideo(&conn, request, 0)` where `conn` is a
  connection/handle storage and `request` is a structure at `ctx+0x40`
  whose exact fields are still to be recovered. Failure retried in a
  loop on `0x81950008` with 16,000 µs sleeps; success is followed by
  NID `j7ClJpPRVQI` (Avcap2 export, 171 B) called as
  `(handle, &out1, &out2)`.
- `sceAvcap2ReadVideo` (NID `pN0hIQZsMzc`) has exactly one call site
  (`0x41c28ba5`): called as `ReadVideo(handle, buffer, 0x200000, info,
  0)` — a **2 MiB data buffer**, a **112-byte info structure whose first
  u64 is initialized to 0x70** before the call, and **flags 0**
  (blocking). Post-call, `info+0x54` is compared against a context frame
  counter and a further result `0x8195000d` is handled specially.
- Before OpenVideo the same function calls Avcap2 exports `DHFDHdKAr9U`
  (127 B; fills a 4-byte status) and `RW88AiwmL+Y` (127 B; fills 16
  bytes) — query functions whose names are not yet recovered.
- `sceAvcap2Initialize` has 4 call sites, `sceAvcap2Start` 6,
  `sceAvcap2Close` 15, `sceAvcap2Terminate` 8 (cleanup paths).

**Avcap2 audio contract, confirmed against Sony's own caller:**
`sceAvcap2OpenAudio` (one site `0x41c2ad08`) is called as
`OpenAudio(&conn, request56, 0)`; Sony's own code fills the 56-byte
request's `+0x18` (the public caller's "mode" field) with 1/2/3 by
audio type, `+0x20` (u64) with 0x10/0x18 and `+0x28` (u32) with
0xfa00/0x1f400, and copies `+0x14` ("source") from context state. It
retries on `0x81950008` with 43,000 µs sleeps. `sceAvcap2ReadAudio`
(one site `0x41c2af4b`) is called as `ReadAudio(handle, ctx+0x48,
0xc000, info, 0)` — a **49,152-byte capacity**, a 16-byte info whose
first u64 is initialized to **0x10** (matching HearBridge's metadata
initialization), and **flags 0 (blocking)** rather than the public
caller's NOWAIT=1. This confirms the public audio ABI's shape and
metadata size while showing Sony's own capacity/flag choices; the
probe's public-caller-based request remains the admitted contract.

**Venc encoder contract skeleton (SceAvCapture disassembly):**
`sceVencCreateEncoder` (NID `irD6EyOPBZE`, one call site `0x5badfeb2`
inside a function at `0x5badfdd0`) is called as
`CreateEncoder(paramA, paramB, handle_out=ctx+0x418)` — two parameter
structures (`[rbp-0xa0]` and `[rbp-0xc0]`) filled by helper calls and one
64-bit handle storage. SceAvCapture's image embeds the function-name
strings for 18 of its 25 imported Venc functions; recomputing their NIDs
names them: `sceVencStartEncode`, `sceVencStopEncode`,
`sceVencSetPictureConfig` (resolution/format), `sceVencSetConfig`,
`sceVencGetConfig`, `sceVencSetRateControlConfig` (bitrate),
`sceVencGetHeader`, `sceVencQueryHeader`, `sceVencMapMemory`,
`sceVencQueryMemorySize`, `sceVencQueryPreset`, `sceVencQueryPreset2`,
`sceVencSetDecodedFrame`, `sceVencSetReferenceFrameInvalidationConfig`,
`sceVencSetImagePasteConfig`, `sceVencDeleteEncoder`,
`sceVencSetNormalPrivacyProtectionConfig` (**capture protection**),
plus `sceVencCreateEncoder`. Three imported NIDs remain unnamed. The
image also contains C++ class names `AvcapVideoCodecAvc` (H.264),
`AvcapVideoCodecHevc`, `AvcapScanInControl`, `AvcapVideoRecordingData`,
device names (`SceAvCaptureDeviceVideoNormal/ScanIn/Second`,
`AudioNormal/Aout/Second`, `SceAvCapturePrivacyImage`, `SceAvCaptureVout`)
and `SceAvcap2Ipc`, mapping the daemon's internal structure.

### Video request-structure increment (Avcap2 library disassembly)

Disassembling `sceAvcap2OpenVideo` inside the exported
`libSceAvcap2.sprx` text itself (module offset 0x360, 973 B) gives the
implementation-side request semantics. The request passed by SceRemotePlay
(at its ctx+0x40) is **not** the 56-byte audio template but a large
composite: the library validates `[req+0x1a8]` (a mode word with values
0/1/2 taking different paths), `[req+0x1c0]` and `[req+0x1c8]`
(pointer-nonzero requirements in modes 1/2 — buffer/surface pointers),
`[req+0x1e0]` (pointer) and `[req+0x1e8]` (a 64-bit flags word whose bits
40–43 are range-checked). Combined with SceRemotePlay's own writer code
(the tier function at `0x41c28190`), the recovered field map is:

| Request offset | Writer / meaning |
|---|---|
| `+0x18` | resolution tier: Sony's own mapping 1=640×360, 2=960×540, **3=1280×720**, **4=1920×1080**, 5=2560×1440, 6=3840×2160 |
| `+0x1c` | `fps != 30` flag (0 = 30 fps, 1 = 60 fps) |
| `+0x108`/`+0x10c` | fps-derived frame timing (×1000 arithmetic) |
| `+0x12c`–`+0x13c` | bitrate-range fields (double-precision clamped computation against `[ctx+0x8c]`) |
| `+0x20c`–`+0x214` | derived width/height-like words (aligned by 64/256 with rounding) |
| `+0x220` | `0x88` — an embedded 136-byte sub-structure |
| `+0x228`–`+0x2a8` | 128 bytes copied verbatim from a config template at ctx+0x200360 |
| `+0x1a8`/`+0x1c0`/`+0x1c8`/`+0x1e0`/`+0x1e8` | mode/pointers/flags validated inside the library |

The call chain is `0x41c252c0` (builds a 16-byte mode descriptor from
ctx+0x1c0 with a 2,000-retry count) → `0x41c28830` (retry loop) →
`0x41c27fa0` (tier selection and 0x41c28190 request fill) →
`0x41c284c0` (queries `DHFDHdKAr9U`/`RW88AiwmL+Y`, then
`sceAvcap2OpenVideo(&conn, req, 0)`, then `j7ClJpPRVQI(handle, &out1,
&out2)`), then `sceAvcap2Start`. The buffer pointers at `+0x1c0/+0x1c8`
mean a video probe must first allocate or attach capture surfaces —
Sony's own allocation path (SceAvCapture devices and `shm_acap.*`
shared memory) is mapped but not yet followed to a callable contract.

**Dispatch-close state:** the audio path is now admitted and evidenced
(run above); the video/encoder path has its full call graph, function
names, mode/tier semantics and request field map recovered from real
firmware-13.60 code, but the buffer-ownership/allocation prerequisite and
the remaining request fields keep a video probe unadmitted. Sustained
mode, bitrate, ten-bit, HDR conversion, metadata, protection and
error-cleanup device evidence remain open; T041 stays unchecked.

## First admitted media call: Avcap2 stereo audio capture on 13.60

**2026-10-25 ~23:13 UTC, owner-present window (direct owner decision
relayed by the coordinator).** The owner was at the console with audible
unprotected content playing and observed via their QuickTime preview.
After a passing isolation preflight (network-inventory PID277 result=0),
the previously prepared five-second Avcap2 audio candidate ran exactly as
approved (source `capture_encode.c` SHA-256
`18e1262defc1c25d7528550bff8359e56a497c922d17b8b079f5f4c406b71002`,
ELF `capture-audio-five-seconds.elf` SHA-256
`ca731612f47439c3813aeef7360f63f94f3f33f60175ec6a923830c0d3c280b3`,
verified before launch; public 7-NID ABI from SoloBridge/FGG-PlayPods,
Ipmi-first load order):

```sh
timeout 90 /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy \
  -h 192.168.2.2 -p 9021 \
  /Volumes/4TB_Drive/tmp/t041-hermes/build/capture-audio-five-seconds.elf
```

**PID278 / firmware 0x13600007 — all raw return codes, one run:**

| Operation | Raw result |
|---|---|
| `sceKernelLoadStartModule` libSceIpmi.sprx | handle 0x21, result 0 |
| `sceKernelLoadStartModule` libSceAvcap2.sprx | handle 0x22, result 0 |
| `sceAvcap2Initialize` | 0x00000000 |
| `sceAvcap2OpenAudio(request 56 B source=2 mode=0, kind 0)` | 0x00000000, handle present |
| `sceAvcap2Start` | 0x00000000 |
| 5-second NOWAIT read loop | 4,486 reads, 5,000,863 µs elapsed (console CLOCK_MONOTONIC) |
| `sceAvcap2Stop` | 0x00000000 |
| `sceAvcap2Close` | 0x00000000 |
| `sceAvcap2Terminate` | 0x00000000 |
| module unload Avcap2 / Ipmi (reverse order) | 0x00000000 / 0x00000000 |
| `stop_requested` | 0 |

**Captured audio population (first real media evidence on this console):**
239,616 stereo float32 frames = 1,916,928 bytes in 5.0009 s — internally
consistent with the public caller's 48 kHz stereo float32 record format
(239,616 frames × 2 ch × 4 B ≈ 5.0 s at 48 kHz). **Non-silence is
unambiguous:** 239,615/239,616 left and 239,614/239,616 right samples
nonzero, left peak 0.0464313, right peak 0.0318793 (quiet but real
content, matching the owner's low-volume playback). 4,252 of 4,486 reads
returned the source-defined empty code `0x81950002` (NOWAIT with 1 ms
polling), zero overruns, zero invalid records, zero nonfinite samples; all
234 data-carrying reads passed the 8-byte alignment and finite-float
validation. Samples were counted and discarded per design; no forwarding.

Postflight: network-inventory PID279 result=0 with route dump identical
to preflight; PID277/278 absent from the process list; SceRemotePlay,
SceAvCapture and SceSysAvControl.elf all resident and sleeping; no
capture-device, setting or service change. **This is successful
lifecycle-plus-data evidence for the Avcap2 audio path on firmware
13.60 from a payload context**, including complete normal cleanup — the
T041 evidence-ledger row "Stereo game audio" moves from *Unverified* to
*observed with data and clean teardown*. Still unverified for audio:
protection behavior (no protected content was played), the source clock
(the 48 kHz figure is the public caller's label; the 5.0009 s window
count is the probe's own clock), stereo channel ordering (peaks differ
per channel but ordering was not independently verified), and
driver/service memory accounting. Raw log:
`/Volumes/4TB_Drive/tmp/t041-hermes/evidence/audio-probe-run.log`.

The five sustained SDR video modes, hardware-encode bitrate bounds,
ten-bit HDR output, bounded HDR-to-SDR conversion, source HDR metadata,
capture protection and error/partial-start cleanup evidence remain
unavailable, so **T041 stays Incomplete/unchecked and T045 stays
open**; the audio evidence above is real but does not close those rows.

## T041 continuation — mode-0 video contract and unexecuted probe

**2026-10-25, offline analysis and local tests only.** The continuation did not
access the console or capture device. Fresh review of the v5 recovered images
corrects the prior statement that every OpenVideo request needs the two surface
pointers: at `libSceAvcap2` `0x814e48480` the implementation branches on the
32-bit mode at request `+0x1a8`; **mode 0 goes directly to the IPC call without
checking `+0x1c0/+0x1c8/+0x1e0/+0x1e8`**. Modes 1 and 2 have distinct pointer
requirements and are not used by this candidate. SceRemotePlay's constructor at
`0x41c271c0` zeroes all `0x2f0` request bytes and its caller supplies mode 0.
The immediate predecessor's pointer-allocation blocker therefore does not apply
to this narrower sourced path.

The probe reproduces only the fields written by Sony's request filler at
`0x41c28190`: tier 3/4, 30/60 fps, an explicitly declared test bitrate, timing
fields `+0x108/+0x10c/+0x110`, defaults `+0xd8=1`, `+0x1d0/+0x1d4=1`, codec word
`+0x2ec=2`, nested size `+0x220=0x88`, the constructor's 1.0 double at `+0x228`,
and `+0x24c=384`, `+0x250=384`, `+0x254=512`, `+0x258=0x48000` for tiers 3/4.
All other bytes remain zero. The 10,000 kbit/s initial target is a declared
experiment input, **not a measured encoder limit**. The exact constants and
zero-initialized template are visible at `0x41c27228–0x41c272ec` and in the
preserved Capstone disassembly under
`/Volumes/4TB_Drive/tmp/t041-hermes/analysis-continuation/`.

`ReadVideo` remains `ReadVideo(handle, buffer, 0x200000, info112, 1)` with the
first info byte `0x70`. The library implementation at `0x814e48dd0` writes the
raw result at info `+0x5c`, returns positive byte counts for data, and has
separate empty/special results. The probe accepts zero/`0x81950002`/
`0x8195000d` as empty and `0x81950004` as overrun, validates size/info and an
Annex-B start code, and counts bytes without saving or forwarding content.
SceRemotePlay does not call Venc: the resident SceAvCapture daemon owns encoder
creation and configuration. Its `CreateEncoder(paramA104, paramB32, handle)`
call at `0x5badfeb2` is preceded by `sceVencQueryPreset` and
`sceVencQueryMemorySize`; the latter returns three 32-byte descriptors, rounded
to 16 KiB, containing size and address/offset fields. This is provenance for the
encoded Avcap2 result, not authorization to invent a separate Venc client.

Implemented target-only `--video-ten-seconds` and four separately named ELFs:
`capture-video-{720p30,720p60,1080p30,1080p60}.elf`. Each is bounded by ten
seconds and 20,000 NOWAIT calls, uses one static 2 MiB output buffer, records
per-data-call timing up to 4,096 frames with explicit overflow, and attempts
Stop→Close→Terminate and owned-module unload. A successful run still returns
Incomplete because the mandatory HDR, protection, metadata, conversion and
resource evidence is separate. The ordinary payload remains describe-only.

Red-first local validation first failed to link all five new video entry points.
The implementation then passed: warning-free host/PS5 builds through
`R3_BUILD_DIR=/Volumes/4TB_Drive/tmp/t041-hermes/build bash
tools/ps5-probes/build-r3.sh`; exact request values and malformed record cases;
empty/overrun accounting; startup failures and idempotent cleanup; host refusal;
`ctest -LE pending-core -R 'test_media_probe|test_remoteplay_export'
--output-on-failure` (2/2 pass); and `scripts/check-quality.sh` with pinned
LLVM 18.1.8, which finished `Quality exit status: 0`. Apple Clang
AddressSanitizer/UndefinedBehaviorSanitizer builds of both probe suites also
passed; their logs are `evidence/sanitizer-{media,export}.log`. Build/quality
logs are `evidence/local-build-video.log` and `evidence/quality-video-final.log`
in the T041 scratch root. Current first-run source SHA-256 is
`5aac93680c3d8673f4c4300977d721558452c405b4f32608436a91349106f1f0`;
`capture-video-720p30.elf` SHA-256 is
`26fbb26364c01896b2e9b44726c167fd1a2673e1b86cb99206e55a07200626e5`.
An exclusive owner-observed window and exact-hash execution approval were
requested. Sustained SDR, bitrate, ten-bit HDR, conversion, metadata, protection
and device cleanup remain open, and T041 stays unchecked.

### First approved 720p30 run — transient open failure, clean teardown

The owner granted the exact source/ELF pair above and was present with
unprotected changing game/trailer content (title not supplied). Fresh preflight
passed the accepted IPv4/IPv6 isolation disposition: only direct
192.168.2.2/link-local/loopback, sole dead gateway 192.168.2.254, no global IPv6
or IPv6 default route; loader and all four development services were healthy.
The exact approved command ran once on 2026-10-25 at about 23:53 UTC:

```sh
timeout 30 /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy \
  -h 192.168.2.2 -p 9021 \
  /Volumes/4TB_Drive/tmp/t041-hermes/build/capture-video-720p30.elf
```

PID287 confirmed firmware `0x13600007`, loaded Ipmi/Avcap2 (`0x21/0x22`) and
Avcap2 Initialize returned 0. **OpenVideo attempt 1 returned `0x81950008` with
no handle**, so no Start/Read occurred. Terminate and both owned-module unloads
returned 0. Counts are zero, with elapsed/bitrate/p99 absent rather than zero.
This is an ordinary transient result already present in Sony's caller, which
sleeps 16 ms before retrying it; it is not proof that video is unsupported.
Postflight process and service checks match preflight, the payload is absent,
SceRemotePlay/SceAvCapture/SceSysAvControl remain resident, and the complete
route summary is unchanged. Raw logs are under
`evidence/video-720p30/`; the approved source/binary copies are frozen under
`approved/`.

A new red-first regression now requires two transient OpenVideo failures then
success. The retry-capable source attempts at most 64 opens with the sourced
16 ms interval, stops immediately on any other error or unexpected handle, and
preserves the same cleanup. New source SHA-256 is
`b40a8d45dd6eb9637de3066a9a74130934d82d320bf435f2177dd25fb551d753`;
internal header `6779d051bc1ee25f90fecc0c705b949c46c85f3c0f5a6180b8a45cfaf56ccb9a`;
new ELF SHA-256 is
`13b0b6a1880695a69c73da3a7bb766bfb76a77dc98a3c4d7bfb1c59226e3a6a1`.
Fresh exact-hash approval was requested and granted. T041 remains unchecked.

### Dashboard-only retry — persistent busy, not media evidence

The owner relayed a normal after-state for the first run and clarified that the
actual content in both runs was the **PS5 dashboard**, not a game or trailer.
The retry ELF above ran once at about 2026-10-26 00:00 UTC after a fresh passing
preflight. All **64** OpenVideo attempts returned `0x81950008` with no handle,
approximately 1.3 seconds for the whole bounded run. Reads/frames/bytes are zero;
Terminate and both owned-module unloads again returned 0. No timing or bitrate
is inferred. Postflight process/service/network state again matches preflight.
Raw log: `evidence/video-720p30-retry/run.log`.

This records a dashboard-only correlation, not a permanent capability refusal.
The owner directed a follow-up with an actual unprotected game/trailer rather
than further dashboard retries; that window was requested with the same approved
hash. All required T041 evidence remains open.

### God of War Ragnarok content-active run — blocker retained

The owner approved the same retry bytes with **God of War Ragnarok** running,
unprotected and producing changing visuals. Fresh preflight confirmed
`active_bigapp=57368` and the accepted isolation disposition. The same exact
command ran once at about 2026-10-26 00:09 UTC. PID315 again returned
**`0x81950008` on every one of 64 OpenVideo attempts**, with no handle and no
Start/Read. Terminate and both owned-module unloads returned 0. Elapsed,
bitrate and p99 remain absent. Postflight shows the same active BigApp,
unchanged routes, resident Sony services and no payload process.

Per the owner's explicit instruction, no further video experiments were run.
The persistent busy result is the current **content-independent blocker**;
it does not prove that the request is correct or that capture is impossible.
It also does not establish a supported mode, cleanup after a successful video
session, hardware-encoded bytes or measured bitrate. The other SDR modes,
HDR/conversion/metadata/protection and successful-lifecycle evidence remain
unavailable. T041 stays unchecked and the hardware gate stays open. Raw logs:
`evidence/video-720p30-game/`.

### Audio-first OpenVideo hypothesis — locally validated, not device-verified

The dashboard and God of War Ragnarok runs falsify content activity alone as the
missing precondition for `0x81950008`. After the game-active run the owner
released the console window and directed local reverse engineering only; no
console, capture-device, service, log or network-inventory access followed.

The next bounded candidate opens the already sourced 56-byte Avcap2 audio request
(source 2, mode 0, kind 0) before OpenVideo but does not start or read audio. This
tests only whether audio registration is an OpenVideo precondition. It is a
**hypothesis**, not a recovered requirement: the daemon path at `0x5bae88dc` can
return the same error before encoder creation when a predicate fails, but the
recovered client and caller chain do not prove that predicate is audio
registration. Video cleanup is Stop, Close, audio Close, Terminate, followed by
reverse-order unload of only owned modules; failure and idempotent-cleanup fakes
cover the added audio handle.

Local validation of this exact candidate passed warning-free host/PS5 C11 builds,
both focused CTest suites (2/2), full `scripts/check-quality.sh` (exit 0), pinned
LLVM 18.1.8 format/tidy checks, and Apple AddressSanitizer/
UndefinedBehaviorSanitizer runs of both probe suites. Logs are
`evidence/quality-video-audio-first.log` and
`evidence/sanitizer-{media,export}.log`. These are simulated lifecycle results,
not console evidence. The candidate requires a fresh owner-present game-active
window and exact-hash approval before execution; all mandatory T041 video/HDR/
protection evidence remains open and T041 stays unchecked. Frozen copies are under
`approved-audio-first/`; source SHA-256 is
`4a2b7ca49a241a8751bd31cbe43d7b77f4e0a13e68d17fe77fdf58a5f992369b`,
internal header SHA-256 is
`09c4eb9ad3d2d4ada8dfe7c0f2a1f8c2c7b9d6d3aff7394d2650df05a3537eff`,
and `capture-video-720p30.elf` SHA-256 is
`4a2f2f6e90e2571d4de0e38cf970f11b3c26d93752863d0e5dc9da4a934794ea`.

### Audio-first device result — hypothesis falsified

A fresh direct owner decision relayed by the coordinator approved the exact
source/header/ELF hashes above for one run with **God of War Ragnarok** actively
running and the owner observing through QuickTime. At about 2026-10-26 00:35 UTC,
preflight network-inventory PID320 returned result 0 with the accepted isolated
IPv4/IPv6 disposition and `active_bigapp=57368`. The exact approved command was:

```sh
timeout 30 /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy \
  -h 192.168.2.2 -p 9021 \
  /Volumes/4TB_Drive/tmp/t041-hermes/build/capture-video-720p30.elf
```

PID321 confirmed firmware `0x13600007`, loaded Ipmi/Avcap2 as `0x21/0x22`, and
Initialize returned 0. **OpenAudio returned 0 with a handle, then every one of
64 OpenVideo attempts returned `0x81950008` without a video handle.** No Start
or Read occurred; reads, frames, bytes and Annex-B records are zero, and elapsed
time, measured bitrate and p99 are absent. Audio Close, Terminate and both
owned-module unloads returned 0. Postflight PID322 returned result 0 with the
same active BigApp and matching interface summaries; all four development
services were listening. The owner confirmed the game/console state was normal
with no visible side effects and released the exclusive window. Raw logs are
`evidence/video-720p30-audio-first/`.

This falsifies the audio-first registration hypothesis as well as the earlier
dashboard-only and game-active/content-activity hypotheses. It supplies no
successful video lifecycle, encoded bytes or bitrate evidence.

### Final local error-origin trace — external selector-list gate

The owner then directed one final **local-only** RE pass; no console access
followed. The gap-aware recovered images and new disassembly under
`analysis-final/` establish this chain:

1. `sceAvcap2OpenVideo` at `0x814e48360` accepts mode 0 without the mode-1/2
   pointer checks and reaches `0x814e48612` -> `0x814e4c480`. That helper issues
   IPC command `0xa70001` and returns the IPC result at `0x814e4c522`;
   `0x81950008` is not synthesized by the client entry checks.
2. SceAvCapture dispatch `0x5badd320` constructs the video object through
   `0x5bae8640`. Its base constructor at `0x5baefc80` copies the request's first
   `0x178` bytes. In the open routine at `0x5bae87f0`, the getter at
   `0x5baefd10` therefore reads the original request's 32-bit `+0x30` selector.
3. At `0x5bae88d7`, the daemon calls predicate `0x5bae8b30`; at `0x5bae88dc` it
   sets `r13d = 0x81950008`, and a false predicate returns that error at
   `0x5bae8b10`. This is before device lookup at `0x5bae88fb`, all subsequent
   device calls and Venc creation. The predicate calls PLT stub `0x5baf6aa0`
   (import `fBX+DBOwNis#O#O`, GOT `0x5bb05b60`) with a 128-byte output area,
   then compares the selector against all 32 little-endian 32-bit entries. An
   import error or no matching entry yields false. The import's higher-level
   name and the list's live contents are not recovered, so this record does not
   invent either.
4. Sony's own mode-0 constructor at `0x41c271c0` zeroes the complete `0x2f0`
   request at `0x41c27284–0x41c27289`, and the tier/fill/OpenVideo chain at
   `0x41c27fa0`, `0x41c28190` and `0x41c284c0` does not write request `+0x30`.
   Thus **zero is also Sony's mode-0 selector**, not a missing nonzero probe
   field. Substituting a guessed value would be unsourced and would not recreate
   the daemon's external selector-list state.

The precise blocker is therefore an **external selector-availability/state gate
in SceAvCapture**, reached through Avcap2 IPC, not audio registration, content
activity, mode-0 pointer allocation or a client-side mode check. The recovered
Sony caller chain contains no caller-accessible setup operation that populates
this independent list. Whether its absence on the payload run reflects Sony
process registration, process identity, or another AV-service state remains
unresolved; the evidence does not justify choosing one. No concrete correctable
precondition was found, so no further payload or console window was requested.
All sustained SDR modes, hardware-encoded output and measured bitrate bounds,
ten-bit HDR, bounded HDR-to-SDR conversion, source HDR metadata, capture
protection and successful video cleanup remain unevidenced. **T041 remains
unchecked and the hardware gate remains open.**

Final local-only validation after correcting a stale file-top comment passed
`R3_BUILD_DIR=/Volumes/4TB_Drive/tmp/t041-hermes/build bash
tools/ps5-probes/build-r3.sh`, full `scripts/check-quality.sh` (exit 0), and
focused CTest (2/2). Logs are `evidence/local-build-final.log` and
`evidence/quality-final.log`. The comment-only final source SHA-256 is
`79517794a9d1f48931c004811c08ac115932b1dc145a009fefb45b65a9ffb8fc`;
it is not the device-executed source hash, which remains the frozen
`4a2b7ca49a241a8751bd31cbe43d7b77f4e0a13e68d17fe77fdf58a5f992369b`
documented above.


## T041 Codex retry — selector owner and request correction (2026-10-25)

This dated continuation preserves the preceding run records and predecessor
artifacts. New local analysis and evidence are under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/`; the complete daemon image
from PID275 remains 360,448 bytes at `0x5badc000`, SHA-256
`c297ed6dd2350260e0894673c1b58f58aab580779795304f30d42b4db569e029`.
No earlier video return code is reclassified as successful media evidence.

### Imported selector query and approved acquisition

The saved SceAvCapture GOT at `0x5bb05b60` resolves `fBX+DBOwNis#O#O`
to `0x833dad1f0`. PID275's module inventory places this address inside
`libSceResourceArbitrator.sprx`, base `0x833dac000`, mapped size `0x10000`.
All four daemon imports with suffix `#O#O` resolve to that module. The
query implementation was outside the previously exported daemon image.

A separate fixed exporter profile selects only ResourceArbitrator and resident
VideoRecording libraries in SceAvCapture, SceRemotePlay and SceShellCore. Its
red-first selection test initially failed on the missing ResourceArbitrator
selection; the implementation then passed both exporter profiles' tests.
The coordinator granted the exact payload and explicitly serialized its window
after T042's release. Approved source/header/ELF SHA-256:

- `remoteplay_export.c`: `be80762e6e32fac05245736ac8a559e4519a572224ac519155844ad7b0cf126f`.
- `remoteplay_export.h`: `cd0c361528a6a1a2150821c3e2158bdf7320674329efe6ef3372f9383e674ae9`.
- `resource-library-export.elf`: `4b47782865b379d0afd20880280b65c8fc7464eb9139e2917447f0f2f218575b`.

At approximately **2026-10-26 01:06 UTC**, the approved network inventory ran
as PID327 and returned result 0: only direct/link-local/loopback addresses,
dead IPv4 gateway `192.168.2.254`, no IPv6 default/global address, and
`active_bigapp=57368`. The exact approved command ran once:

```sh
timeout 150 /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy \
  -h 192.168.2.2 -p 9021 \
  /Volumes/4TB_Drive/tmp/t041-hermes/build/resource-library-export.elf
```

PID328 / firmware `0x13600007` exported **two complete 65,536-byte
ResourceArbitrator images**, one from each AV daemon/client, plus their
module objects and linker tables: **12 files / 139,626 bytes**. Neither
selected library was resident in SceShellCore's 63-module inventory, and
VideoRecording was not recovered. Thus the exporter's aggregate status is
Incomplete; both ResourceArbitrator images are complete. A first FTP LIST
mistakenly addressed directory PID324 and returned 550 without transferring
bytes; retrieval then used the actual reported PID328 directory. All 12
files were size/hash-verified against their manifest. Only the reported run
files/directory were deleted and absence was verified. Postflight PID329
returned result 0 with identical routes/interfaces/services and the same
active BigApp; PID328 is independently absent and Sony processes remain
resident. The window was released for T042 priority. No media call occurred.
Raw evidence: `t041-codex/evidence/resource-{preflight,postflight}/`,
`resource-export-run.log`, `resource-328/retrieval-manifest.json` and
`resource-cleanup.json`; exact approved bytes are in `approved-resource/`.

### Recovered ResourceArbitrator contract

The new complete library and its actual Remote Play callers establish:

| NID / module offset | Recovered contract and effect |
|---|---|
| `fBX+DBOwNis` / `0x11f0` | `int(int32_t selectors[32])`; opens/maps `/SceRASharedMemory`, size `0x360`, takes named semaphore `SceResourceArbitrator`, and copies its first 128 bytes to the caller |
| `kJvjeet+f9I` / `0xdb0` | `int(int32_t id, callback, void *context)`; registers a callback/context in the client map and sends IPC command 0 with the resource ID and callback token |
| `LqmYZGB-UEQ` / `0xee0` | `int(int32_t id)`; sends IPC command 1, then removes the client's registered callback entry on success |
| `CYjfezkWcaE` / `0x10b0` | `int(int32_t id, const char *state, void *token, int32_t pid)`; bounds the state name to 32 bytes and sends IPC command 2 |

The endpoint string is **`SceArbitratorIpc`**. Sony's caller at
`0x41c231a0` obtains `getpid()` through PLT `0x42020300`, registers
resource **`0xb4`** for source type 0 in its resource wrapper, and the
video setup later supplies Avcap source **2**. Its request/release methods
at `0x41c23500`/`0x41c23590` use state strings **`RUNNING` / `STOP`**;
`0x41c23440` unregisters. The library worker verifies the callback token,
inserts the registered context into its opaque event data, and calls a
three-argument callback (resource ID, operation, opaque event). The probe
counts only source-observed operations 1/2 and never dereferences that data.
The list writer is in the arbitration service, not this read-only query;
service-side insertion/policy checks are not yet recovered. A concrete
caller-facing registration/request path now exists in the recovered code,
while its acceptance from this payload context still needs a console result.
Disassembly: `analysis/resource-functions.txt`, `resource-shared-helpers.txt`,
`resource-init-and-worker.txt` and `remoteplay-resource-lifecycle.txt`.

### Correction to the predecessor's request interpretation

The preceding conclusion that Sony's final selector is zero is superseded:

1. Base constructor `0x5baefc80` places its copied request **after an
   eight-byte vtable pointer**. Getter object `+0x30` therefore reads
   **original request `+0x28`**, not original request `+0x30`.
2. The full video constructor at `0x5baf0270` copies **`0x2e8` bytes**,
   also after its vtable. Its selector getter at `0x5baf08c0` reads
   object `+0x30`. The video validation routine `0x5badfc50` reaches
   the equivalent 32-entry predicate at `0x5bade7b0` through vtable
   `+0x100`. The earlier `0x178` constructor/`0x5bae88dc` trace is a
   different request family sharing the same imported list query; it
   does not uniquely identify the device-executed OpenVideo failure.
3. Sony's separate initializer **`0x41c27840`** writes total size
   `0x2e8` at context `+0x40`, embedded size `0x28` at `+0x48`, source
   at `+0x54`, and the registered resource ID at context `+0x68`
   (`0x41c27914`), which is **request `+0x28`**. The setter at
   `0x41c29960` supplies this ID via context `+0x2003ec`.
4. The tier writer `0x41c28190` receives the **whole context**, not
   the request address. For example, its stores at context
   `+0x108/+0x10c/+0x110` are request `+0xc8/+0xcc/+0xd0`;
   context `+0xd8` is request `+0x98`; context `+0x2ec` is
   request `+0x2ac`. The previous probe omitted the full initializer
   and incorrectly placed several context-relative fields directly
   in the request. Its zeroed constructor alone was not a faithful
   reproduction of Sony's final request.

Consequently the old dashboard/game/audio-first failures do **not** prove
that a correctly initialized request plus normal resource registration is
unavailable. The existing video request needs correction and new exact-hash
approval before another run. The actual source still admits mode 0 without
surface pointers, while Sony's full initializer uses mode 1 and its IPC
`0xa70013` preparation returns two 32-byte memory descriptors. The remaining
allocation/cleanup mapping must be traced before adopting those modes.
New artifacts: `analysis/remoteplay-video-class.txt` and
`daemon-real-video-request.txt`. No guessed selector, signature or process
identity change was called.

### Bounded registration candidate, not device evidence

`capture-video-admission.elf` now selects a separate five-second experiment
using only the recovered ResourceArbitrator interfaces above. It loads Ipmi
before ResourceArbitrator, first refuses registration if `0xb4` is already
available, registers its own callback/context, requests `RUNNING` once, and
queries at most 320 times with 16 ms waits. Cleanup requests `STOP`,
unregisters, checks selector removal for at most 64 further polls, and
unloads only owned modules. Failure remains persistent; individual service
calls have no proven hard deadline. It makes no Avcap/media call and cannot
establish capture/encoding. Red-first host tests cover registration/request/
query/STOP/unregister failures, last-entry selector detection, persistent
cleanup failure, repeated cleanup, and host refusal. Full host/PS5 build-r3
and focused CTest **3/3** pass; repository CMake-based tidy/format checks on
T041 sources pass. Full quality currently reports only concurrent T042 input
format/float-comparison issues, which were sent to its owner for correction.

The frozen unexecuted candidate is under `approved-admission/`: source
`fe1d459ea22d57f69847d6d448b0b427310f350abdd081e8b6d78b017efe200f`,
header `cc4424ae36b01c5c8001fc9687428c15b9fec12edb86a7dede0ba51cd4c0f832`,
ELF `cd625f01ad2e2a301d8c4435a86351f77bb56569242a701f31b072cc3fede7e7`.
A new exact-hash/window approval was requested, queued behind T042; no
execution is implied by its build. All four sustained SDR modes, hardware
bitrate bounds, ten-bit HDR, conversion, source metadata, protection and
successful video cleanup remain unevidenced. **T041 remains unchecked.**


## T042 execution record — controller readback preparation, 2026-10-25

The owner authorized proceeding despite the unchanged reviewer-owned checklist
markers. The initial exclusive window was used only for Mac/en9 checks, console
ping/loader reachability and the dead-gateway check; no payload, input, capture
or service-inventory operation ran before that window was released for T041.
The first controls request was answered **HOLD** and was not executed. All
pre-lint and pre-readback hashes are superseded.

`input_system.c` now has explicit bounded `controls`, `assign-four` and
`readback` experiments, each in a separately named ELF. The first two retain the
previously admitted `/dev/hid` ABI. Controls sends one Cross assignment pulse,
Right, two left-stick magnitudes, distinct L2/R2 values, Create, Options and
touchpad click over 18 seconds. Assign-four sends ordered Cross pulses to four
owned slots and then a separate Right pulse for each slot over 18 seconds.
Neither sends PS or changes title, network, scheduling or probe-defined
privileges. Every path rebuilds complete neutral state before closing/removing
its owned resources.

The readback experiment additionally uses the typed contracts in SDL commit
`ee4c47dc0d617b3bc8f35108f9956baf228a1322`, `src/joystick/ps5/SDL_ps5joystick.c`
and `.h`: `scePadInit`, `scePadOpen(user,3,0,NULL)`, `scePadReadState` and
`scePadClose`. Its natural `PadData` is statically verified as 120 bytes, with
motion at `0x0c`, the touch block at `0x34`, connected at `0x4c` and timestamp at
`0x50`. One virtual pad is assigned with Cross at two seconds and opened at
three; Right, half/full stick, eight nonidentity motion floats and distinct
analog triggers follow through twelve seconds. ReadState results are compared
field-by-field against the injected state. This is independent software
readback, not a claim of reverse-rumble feedback; the two PadData motion floats
not represented in the ioctl block and touch coordinates remain unestablished.

Red-first `test_input_probe` compilation failed for absent sequence/motion
behavior before implementation. The test now links through `input_system.h`
rather than including the C file and groups case parameters to avoid swappable
arguments. `build-r3.sh` host/PS5 warning-free builds and simulated suites pass;
CTest `test_input_probe|test_media_probe|test_remoteplay_export` passes **4/4**;
host and PS5 clang-tidy, clang-format, shell syntax and full
`scripts/check-quality.sh` pass. These are software results only.

Current locally built, **unexecuted** hashes:

- source `ac0dcdd4cbaf2abb1ff3ab636cf30c6cbb0c37cdd8bfb46cb58c7dfec2525738`;
- controls ELF `b4502a820a1ef3488e8246262e378f8dfa46e7101ba1eb32d4bcd190dea62941`;
- assign-four ELF `6321b9a1ba0a4f3d3c39e335c4a532f712ade22d2a78da14913fc39feb655d01`;
- readback ELF `4b7f9e8d5b721bdf084c79c3738a4c633cf254bd25d1fd2e1ff8fec1484fb4b2`.

A fresh readback window/exact-hash/owner-observation request is pending. No new
input-visible run is authorized by these local checks.

Conflict-status audit: the recovered Remote Play import/call-site map contains
no call sites for `sceRemoteplayGetConnectionStatus` (`g3PNjYKWqnQ`),
`sceRemoteplayIsRemotePlaying` (`uYhiELUtLgA`) or
`sceShellCoreUtilGetSharePlayStatus` (`L5mESo+Iq+k`). SDK stubs still provide
names only. Public emulator SharePlay stubs are not console ABI evidence and
were not called. No typed conflict query or transition observation is admitted.
SDL's vibration API sets vibration on a separately opened local pad; it does
not establish console-to-virtual-controller reverse feedback.

The SDK exposes typed priority/affinity operations, but supplies no safe-core
selection or game-impact evidence. A future candidate may lower and restore
only its own priority while retaining affinity; it has not been implemented or
approved. Rest/wake, game coexistence, effective four-player assignment,
complete required-button delivery, reverse rumble, touch coordinates and
conflict transitions therefore remain unresolved. T042 remains unchecked.


### T042 approved readback attempt and negative result, 2026-10-25

After a direct owner decision relayed by the coordinator, the corrected source
`883cc1a4bbc6eab8fd082c0d9f1f9ffb694ea1a727c4697f8c44ce8a588b772a` and ELF
`5b96614c3b3ffe447306f0a2052472f7b9f2eb1e9d0f4c92a6a87d7bf542b781` ran once:

```sh
timeout 25 /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy -h 192.168.2.2 -p 9021 /Volumes/4TB_Drive/tmp/t042-hermes/build/input-readback-once.elf
```

PID334 observed firmware `0x13600007`, initialized UserService and Pad, created
one HID handle 1770253 and emitted the two-second Cross report for120ms.
`scePadOpen(user,3,0,NULL)` then returned **decimal -2137325439 = `0x809b0081`**.
No ReadState call occurred and the probe exited through its setup-failure path
before the later analog/motion events. Neutral, DeleteDevice, HID close and
UserService termination returned0;294 reports were accepted. Loader transport
returned0, which is not native capability evidence.

The owner observed **nothing visible**: no assignment dialog, navigation or UI
change during/after the run; the physical controller still worked and final home
UI was normal with no game. Thus Cross delivery was not corroborated in this
context. This is a negative/inconclusive readback result, not evidence that the
sourced Cross bit is effective everywhere or that virtual-pad reading works.

Network inventories PID333/PID336 returned result0 before/after; bounded decoding
found identical interface/default-route summaries: only 192.168.2.2/link-local
IPv6 and loopback, one deliberately dead IPv4 gateway, no IPv6 default/global
address. Both reported `active_bigapp=57368`; the owner explicitly confirmed
that the game had been closed and the visible before-state was normal home UI.
PID334/336 were absent from final shsrv inventory, all four development service
ports remained listening, and the window was released. Raw logs and decoded
summaries are under `/Volumes/4TB_Drive/tmp/t042-hermes/`.

A subsequent candidate replaces only read-handle acquisition with sourced
`scePadGetHandle(user,3,index)` over four bounded indices and ReadState on a
positive handle below64, as used by SDL/YACE. It does not open/close that borrowed
handle and does not infer success from the handle alone. The earlier source/ELF
hashes in the preparation record are superseded, not current candidates.


## T041 Codex retry — registered selector confirmed on console (2026-10-25)

The exact-hash approval for the bounded admission candidate was relayed through
Orca after the full quality gate passed and T042 released the exclusive window.
Approved `capture_encode.c` SHA-256 was
`fe1d459ea22d57f69847d6d448b0b427310f350abdd081e8b6d78b017efe200f`,
`media_probe.h` was
`cc4424ae36b01c5c8001fc9687428c15b9fec12edb86a7dede0ba51cd4c0f832`, and
`capture-video-admission.elf` was
`cd625f01ad2e2a301d8c4435a86351f77bb56569242a701f31b072cc3fede7e7`.
The exact payload command ran once:

```sh
timeout 30 /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy -h 192.168.2.2 -p 9021 /Volumes/4TB_Drive/tmp/t041-hermes/build/capture-video-admission.elf
```

PID340 reported firmware `0x13600007`. Before registration the 32-entry list was
`0, 0x64, 0x6e, 0xbe,` then unused `0xffffffff` entries. Recovered
ResourceArbitrator register NID `kJvjeet+f9I` accepted resource `0xb4` (return
0); one `CYjfezkWcaE` state request with `RUNNING` returned 0; and the next
selector snapshot included `0xb4` between `0x6e` and `0xbe`. One callback
operation 1 arrived. Five total queries sufficed. This is direct console evidence
that the selector is inserted by the ResourceArbitrator service path in response
to a caller-accessible registration plus IPC2 `RUNNING` state request; the
32-entry reader itself only copies shared memory. No ioctl or credential change
was used by the probe, and no service-side writer binary/process has yet been
recovered, so the server's internal policy is not inferred.

Cleanup `STOP` and unregister NID `LqmYZGB-UEQ` both returned 0. The bounded
post-cleanup query no longer found `0xb4`; both modules loaded by this payload
(ResourceArbitrator and Ipmi) stopped/unloaded with return/result 0. The probe
made no Avcap2/video/audio call and created no console files. Its explicit result
remains Incomplete because selector admission is not captured media evidence.
The original error is now causally explained as an external selector-state gate
with a caller-accessible path; a corrected video request must hold that
registration while opening Avcap2.

The one run was preceded and followed by approved network inventory using ELF
SHA-256 `30154bcf13d0ee3d870426f7d3cb328f3aadcaa3c8fa72344d6666d5fd556248`.
Both decoded snapshots show only console `192.168.2.2`, link-local IPv6 and
loopback, the intentionally dead IPv4 gateway `192.168.2.254`, no IPv6 default
route and no global IPv6 address. All four development services remained
listening; SceAvCapture, SceRemotePlay and SceShellCore remained resident; PID340
was absent postflight. The console window was released after these checks. Raw
preflight, payload and postflight logs, including route summaries, are under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/admission-window-20261025-1/`.

Current local verification: pinned host/PS5 C11 `build-r3.sh` passes; focused
CTest passes 4/4 (`test_media_probe`, the two exporter profiles, and the
VideoRecording profile); `scripts/check-quality.sh` exits 0, including
clang-format and clang-tidy. These are software checks; the run above only
confirms selector admission. All sustained 720p/1080p SDR modes, hardware
bitrate bounds, ten-bit HDR, bounded HDR-to-SDR conversion, source HDR metadata,
capture protection and successful media-session cleanup remain unevidenced.
**T041 remains unchecked.**


### T042 foreground gate and four-slot correction, 2026-10-25

Two later GetHandle preflights still returned `active_bigapp=57368` even though
the owner-visible screen was home. The coordinator confirmed the important
distinction: **PS5 home-screen state can coexist with a suspended active
BigApp**. The input payload was not executed over that state. The owner used
Close Game, not merely suspend-to-home; the next approved inventory returned
`active_bigapp=-1` with unchanged valid IPv4/IPv6 isolation. This foreground
check is a required input-preflight gate, not an inference from the visible UI.

Review also found that the shared report buffer was previously changed only on
a slot transition. In a four-controller loop, an unchanged neutral slot could
therefore inherit another slot's active button. A red-first regression now
covers active-slot output followed by an unchanged neutral slot;
`update_slot_report` rebuilds the complete report on **every** per-slot
submission and separately reports whether the slot state changed. The input
regression and target tidy pass. Source
`9bca293b3ab4904bebc66fa52abcf11356f04961f3b8bad74fad19a2cd56b89c` and
readback ELF `700b726268e42b8d3b318214ab13f82192b7532ef89490c179aab7d0f7295603`
are pending fresh approval; the earlier GetHandle approval was not executed.

A concurrent T041 edit temporarily broke `build-r3.sh` in
`tests/unit/test_media_probe.c` with incompatible/excess API initializer
entries. T042 did not modify those files and reported the external quality
blocker to the coordinator. It does not alter the input-only results above or
any hardware verdict.


### T042 GetHandle execution and operator-corroborated delivery, 2026-10-25

The slot-safe source/ELF above was directly owner-approved and executed once
after preflight showed `active_bigapp=-1` and valid IPv4/IPv6 isolation:

```sh
timeout 25 /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy -h 192.168.2.2 -p 9021 /Volumes/4TB_Drive/tmp/t042-hermes/build/input-readback-once.elf
```

PID351 created HID handle1966861. Cross was selected at2,004,906us and Right at
4,007,810us (source-loop times, not application timestamps). At five seconds all
four `scePadGetHandle(user,3,index)` calls returned decimal **-2137915384 =
`0x80920008`**; no ReadState occurred. Cleanup neutral/remove/close/terminate
returned0;498 reports were accepted and loader transport returned0.

The owner independently observed a **SELECT USER/controller notice** after the
Cross pulse and the focused icon **navigate RIGHT** after the Right pulse. Final
home UI was normal, the physical controller worked and no phantom controller
remained. This is positive operator corroboration of effective Cross and Right
delivery through `/dev/hid`, materially extending the earlier D-pad-only result.
It also establishes a precise distinction: driver-accepted, UI-effective input
can coexist with pad-service invisibility until the select-user assignment is
completed. The pad-service read error is not evidence of HID delivery failure,
and generic ioctl success alone was not used as the positive verdict.

Postflight routes matched preflight exactly, probe processes were absent and
services remained healthy; the window was released. A same-hash owner-assisted
rerun was requested in which the owner completes Select User before the
five-second GetHandle query. Four-controller execution remains queued behind
that observation. Raw evidence: `/Volumes/4TB_Drive/tmp/t042-hermes/input-gethandle-once.log`
and `gethandle-{approved-preflight,postflight}/`.


T042 reverse-feedback source follow-up: a further audit of FGG-XSense,
ps5-mcp and YetAnotherControllerEnabler found only virtual input publication,
not a game-to-virtual-client vibration read/output-report contract. No sourced
`/dev/hid` output read, feedback ioctl or vibration layout was found, and SDL's
`SetVibration` drives a separately opened local pad rather than returning game
feedback. No speculative HID read or guessed feedback signature was attempted.
Reverse rumble therefore remains unavailable even where UI input is effective.


### T042 owner-assisted readback rerun, 2026-10-25

The same slot-safe source/ELF ran once with explicit owner assistance approved:
the owner was to select the foreground user on a SELECT USER notice before the
five-second query. PID365 created HID handle2163469 and emitted Cross/Right;
all four GetHandle calls again returned `0x80920008`, so no ReadState occurred.
The owner observed **no Select User screen this time**, did see the virtual
controller move Right and disconnect, and confirmed normal physical-controller
operation with no phantom state. The owner's suggested previously-connected
identity/deduplication explanation is retained as an observation hypothesis,
not a proven console property.

Cleanup returned0 for neutral/remove/HID close/UserService termination;498
reports were accepted. Pre/post network snapshots were identical, showed
`active_bigapp=-1`, and preserved valid isolation; probe processes were absent.
Thus `/dev/hid` UI delivery is independently corroborated while exposure through
`scePadGetHandle(type3)` is not established by either assisted or unassisted
runs. The distinct HID and ScePadData interfaces in the source remain a boundary;
no alternate type/index or guessed read path was tried. The window was released
and the approved four-controller run requested next. Raw log:
`/Volumes/4TB_Drive/tmp/t042-hermes/input-assisted-readback.log`.


### T042 four-controller execution and typed-virtual preparation, 2026-10-25

The owner directly approved the four-controller source/ELF and executed it once
with focus on Settings and `active_bigapp=-1`. PID370 acquired four distinct
handles2360077/2425612/2491147/2556682. Cross pulses occurred at2/4/6/8s and
Right pulses at10/12/14/16s. Each slot had1788 accepted reports; all four neutral
reports preceded reverse-order DeleteDevice calls, and close/terminate returned0.

The owner observed **four SELECT USER prompts**, one per virtual controller, and
**one visible Right navigation after the last prompt**. The owner did not select
a user for any prompt. Physical controller and final home UI were normal with no
phantom controllers. Thus four independent registrations/prompt events are
corroborated, but four-player assignment is not: earlier Right pulses may have
been consumed by prompts and cannot be counted as four independent navigation
results. A future same-hash rerun needs owner-completed selection for each
prompt. Postflight routes matched, `active_bigapp=-1`, probe processes were
absent and all services stayed healthy. Raw log:
`/Volumes/4TB_Drive/tmp/t042-hermes/input-assign-four-once.log`.

A separate, unexecuted typed-virtual candidate now uses the YACE-declared local
`scePadVirtualDevice*` ABI (commit3fd0665c), without its ptrace, shell injection,
Mbus binding, kernel credential changes or arbitrary-handle deletion. It uses a
32-byte AddDevice parameter, type3, a 120-byte PadData, and ReadState on the exact
returned handle. SDL's typed Init/ReadState declarations and YACE's process-local
`scePadSetProcessPrivilege(1)` are explicit in the candidate; the setter is not
silently treated as part of the accepted stock-runtime baseline and needs
separate owner approval.

The 16-second sequence requests PS at2s, Create at8s, one touch point
(960,540,id1) plus touchpad click at10s, and half-stick/distinct triggers/eight
nonidentity motion floats at12s. Every transition is compared with ReadState;
PS/touch/motion success cannot be inferred from InsertData alone. Cleanup
rebuilds neutral PadData before deleting only the returned handle. Red-first
layout/timeline tests, sanitizer, build-r3 and target tidy pass. Source
`07d73dfe0cbd1bb6b3ce08b74b25a7bbc6bac123549963dd6efb72f785c69805`;
`input-typed-virtual.elf`
`93d10b2860efb729afcd9e60df60f1195772f75b931a21aeb1f1a1f6c8857a83`.
No execution is approved by this preparation record. Reverse rumble, conflict
transitions, rest/wake and game-safe scheduling remain unresolved.


### T042 typed-virtual execution result, 2026-10-25

The owner approved the typed source/ELF and the explicit process-local privilege
call, and the run occurred after `active_bigapp=-1` and valid isolation checks.
PID379 returned0 for UserService, PadInit and `scePadSetProcessPrivilege(1)`, but
`scePadVirtualDeviceAddDevice` immediately returned decimal **-2143617023 =
`0x803b0001`**. No handle was acquired, no InsertData/ReadState occurred, and no
PS/Create/touch/motion input was injected. UserService termination returned0.
The owner observed no UI change and normal home/physical-controller state.
Pre/post routes matched, `active_bigapp=-1`, and probe processes were absent.

The typed path therefore does not establish PS, Create, touch, motion or
readback on this console. The YACE reference's deeper credential changes and
shell injection were deliberately not attempted; no alias or guessed parameter
fallback is authorized. `/dev/hid` remains the only owner-corroborated delivery
mechanism. Raw log: `/Volumes/4TB_Drive/tmp/t042-hermes/input-typed-virtual.log`.
The console window was released before requesting the four-assignment rerun.


## T041 Codex retry — 30-second 720p30 result and selector-path closure (2026-10-25)

The complete firmware-13.60 SceAvCapture image was analyzed offline at
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/analysis/daemon-disasm.txt`
(360,448 bytes at `0x5badc000`, SHA-256
`c297ed6dd2350260e0894673c1b58f58aab580779795304f30d42b4db569e029`).
At the video OpenVideo path, the daemon queries the 32-entry selector list
through ResourceArbitrator's `fBX+DBOwNis` interface and rejects a selector not
present in that list before Venc setup. The list is not written by SceAvCapture
or an ioctl found in its image: the recovered user-process interfaces register
the resource (`kJvjeet+f9I`, IPC command 0), request state (`CYjfezkWcaE`, IPC
command 2 through `SceArbitratorIpc`), query the list (`fBX+DBOwNis`), and
unregister (`LqmYZGB-UEQ`, command 1). Sony's Remote Play caller registers
resource `0xb4`, requests `RUNNING`, and copies `0xb4` into the OpenVideo request
at `+0x28`. One admitted console run confirmed `0xb4` enters the list after the
normal-process registration / `RUNNING` request and disappears after `STOP` /
unregister. This resolves the original external-state blocker at the
caller-accessible API level; service-side policy remains unexamined. More
detail and alternatives are in
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/analysis/alternative-path-disposition.md`.

The two Avcap2 query calls omitted by the first corrected candidate are now
implemented from SceRemotePlay function `0x41c284c0`: `DHFDHdKAr9U` / IPC
`0xa7000e` supplies the four-byte flags copied to request `+0x2a8`, and
`RW88AiwmL+Y` / IPC `0xa7000c` supplies the 0x20-byte state record whose bytes
`+0x14` and `+0x17` drive the sourced 16 ms retry and request `+0x2bc` update.
The probe repeats the queries on OpenVideo `0x81950008`, bounded to 64 cycles.
The earlier three falsified video attempts and first 64-attempt corrected
failure are retained as historical results; they are not evidence that this
query-corrected candidate is unavailable.

The owner approved this exact-hash one-run window with unprotected dynamic
gameplay visible (game title not reported). Fresh preflight showed
`active_bigapp=24` and matched the accepted isolation state: console
`192.168.2.2` plus link-local IPv6 and loopback, only IPv4 default gateway
`192.168.2.254` (unreachable from Mac en9 `192.168.2.1`), no IPv6 default route
or global IPv6 address; Mac forwarding flags remained unchanged at 1. The
corrected `capture-video-720p30-30s.elf` ran once on firmware raw
`0x13600007`, PID400. It queried flags `0x144` and state `ready=0` / mode 0;
OpenVideo and Start returned 0. The list transitioned from
`[0x0,0x64,0x6e,0xbe,...]` to include `0xb4`, then returned to the absent state
after cleanup.

ReadVideo returned 900 positive Annex-B records / 23,405,408 bytes (23,310,644
nonzero) over 30.033111 seconds. There were no empty reads or overruns; p99
read interval was 33,303 microseconds and measured output was 6,234,561 bits/s
against the 10,000 kb/s request. This is one sustained 1280x720@30 SDR-like
observation; it does not establish the other three SDR mode combinations or a
supported bitrate bound. The source-reported route is Avcap2 → SceAvCapture →
Venc, but codec/profile and hardware-acceleration status were not independently
verified. The probe saved and forwarded no media. Avcap2 Stop/Close/Terminate,
ResourceArbitrator STOP/unregister, selector removal and unload of all three
owned modules returned 0; error/partial-start cleanup, driver allocations and
independent resource accounting remain unverified. Ten-bit HDR, bounded
HDR-to-SDR conversion, source HDR metadata, protection behavior and protected
audio silence were not tested. The owner QuickTime display/audio observation
was requested after this run and is pending at this record's write time.

Raw candidate and pre/post route inventories, plus decoded summaries, are under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/console-720p30-30s-20261025/`.
The run source SHA-256 was `7707921b2f3467f004e91bdf83dc7cce58e4825167b18c5feae9e57ec056f6c0`,
header `f6a6fac7213d91417dec8c6a8d67eb737dacf260e9b92aea586c18a466d8b531`,
and candidate ELF `95d837779d5a5dbf77ed2e280021f69ff3538c4af5dd5b25c4b83ce1788f22af`.
Network inventory ELF SHA-256 was
`30154bcf13d0ee3d870426f7d3cb328f3aadcaa3c8fa72344d6666d5fd556248`.

After the duration-variant change, `R3_BUILD_DIR=/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/build bash tools/ps5-probes/build-r3.sh`
completed successfully with warning-free host/PS5 builds and simulated suites;
`ctest --test-dir /Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/quality-build -LE pending-core -R 'test_media_probe|test_remoteplay_export' --output-on-failure`
passed **4/4**, and
`TMPDIR=/Volumes/4TB_Drive/tmp QUALITY_BUILD_DIR=/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/quality-build LLVM_BIN=/opt/homebrew/opt/llvm@18/bin bash scripts/check-quality.sh`
exited 0, including format and repository clang-tidy. Logs are
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/build-r3-resume.log` and
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/check-quality-resume.log`.
The required 720p60, 1080p30 and 1080p60 runs, bitrate bounds, verified
hardware-encoder output, ten-bit HDR, conversion, source metadata, protection
and complete cleanup evidence remain open. **T041 remains unchecked.**


### T042 assisted four-controller result and owner scope direction, 2026-10-25

PID384 ran the owner-assisted four-controller sequence with source
`07d73dfe0cbd1bb6b3ce08b74b25a7bbc6bac123549963dd6efb72f785c69805` and ELF
`8d9f3afdc8ccbdfc9d8114a62f1d7eefbd740b871b0bf320d6349ff87934c9d7`.
Handles2753293/2818828/2884363/2949898 each accepted1788 reports and were
neutralized then removed in reverse order with all calls returning0. The owner
observed four user-switch prompts and **four Right navigations**. The physical
controller disconnected during the run, was reconnected and worked normally;
final home UI had no phantoms. Pre/post routes matched and probe processes were
absent. Raw log: `/Volumes/4TB_Drive/tmp/t042-hermes/input-four-assisted.log`.

**Owner scope direction:** the owner defers multi-controller support for now
("1 controller support is enough"). No further multi-controller experiments are
authorized. The gathered four-handle/four-prompt/four-Right and clean
release/removal evidence stands as the recorded four-controller observation;
single-controller work is the supported scope going forward. This deferral is
recorded without rewriting the specification or claiming four-player game
assignment beyond the observed prompts/navigation.

Two single-controller candidates are locally prepared. `input-stop-held.elf`
holds Right from four seconds until ordinary six-second cleanup, which must
neutralize before removal; it tests normal-stop release of a held direction.
`input-game-coexistence.elf` covers one assignment Cross, every D-pad/face/
shoulder/stick-click button, proportional L2/R2, Options, Create, touchpad click,
and half/full magnitudes for both sticks over26 seconds. It saves the probe's
scheduler policy/priority, selects `sched_get_priority_min` for that policy,
leaves affinity unchanged and restores the original priority during cleanup.
No game scheduler, title, network or other process is modified. These are
candidates only; owner game observation and execution approval are still needed.
Current source `568b7300a99340e9abe00420fff6199db7f9e4ea78f730515afa613f6bc01c7f`;
stop ELF `de165661e27d2ab036df1f684b1144cdc1c060a71dc619fc872affdb1a5db288`;
game ELF `0446374b1c3d6588ad282a6f1adae7a220a72d68a50ddf3593ac61db199e79b6`.


## T042 final disposition — Incomplete, further runs owner-canceled, 2026-10-25

The coordinator relayed a **direct owner cancellation** of all further T042 input
runs. The stop-held payload was explicitly canceled; game coexistence was
declined as prepared-but-owner-deferred; rest/wake was expressly not approved.
No stop-held, game-coexistence, motion-only or rest/wake payload was executed.
T042 remains **unchecked** because its complete task text is not satisfied.

This paragraph records the 2026-10-25 decision. The owner's later 2026-10-31
authorization superseded it only for one newly reviewed, hardened, bounded
held-Right normal-stop diagnostic; that later observation is recorded at the
end of this report. It did not authorize or establish the other canceled cases.

### Executed evidence and exact artifacts

- Open-based readback: source
  `883cc1a4bbc6eab8fd082c0d9f1f9ffb694ea1a727c4697f8c44ce8a588b772a`,
  ELF `5b96614c3b3ffe447306f0a2052472f7b9f2eb1e9d0f4c92a6a87d7bf542b781`.
  PID334 returned `0x809b0081` at virtual Open before ReadState. Owner saw no
  dialog/navigation; cleanup and postflight were normal.
- GetHandle readback and assisted rerun: source
  `9bca293b3ab4904bebc66fa52abcf11356f04961f3b8bad74fad19a2cd56b89c`,
  ELF `700b726268e42b8d3b318214ab13f82192b7532ef89490c179aab7d0f7295603`.
  PID351 and PID365 each returned `0x80920008` for all four virtual type3
  queries. The first owner observation corroborated a SELECT USER notice and
  Right navigation; the assisted run showed no new prompt but did show Right and
  clean disconnect. Neither produced ReadState evidence.
- Four-handle first run: the same source as above, ELF
  `fe3d56f28c75c5c92ee5b0126923739099c3f23cec3b84184d45e8d277876060`.
  PID370 produced four prompts and one visible post-prompt Right. No users were
  selected, so this did not prove four-player assignment.
- Typed virtual: source
  `07d73dfe0cbd1bb6b3ce08b74b25a7bbc6bac123549963dd6efb72f785c69805`,
  ELF `93d10b2860efb729afcd9e60df60f1195772f75b931a21aeb1f1a1f6c8857a83`.
  PID379 returned `0x803b0001` at AddDevice despite successful process-local
  privilege setup. No input/ReadState occurred and the owner saw no UI change.
  No deeper credential or shell-injection fallback was attempted.
- Assisted four-handle run: the same source, ELF
  `8d9f3afdc8ccbdfc9d8114a62f1d7eefbd740b871b0bf320d6349ff87934c9d7`.
  PID384 produced four prompts and four Right navigations, with clean reverse
  removal and normal final physical-controller/home state. Multi-controller
  support is now owner-deferred as recorded above.

Every executed input path attempted complete neutralization before removing
only its owned handles, then closed/terminated owned services and restored
signals. All acquired-handle cleanup calls returned0. Pre/post network
inventories verified the same isolated direct-link route disposition; final
process inventories showed no input probe remaining and development services
healthy. These are ordinary-stop results, not held-release, rest/wake or stock
SDK credential-reversal proof.

### Requirement-level settlement

- **FR-030 / successful delivery meaning:** partial. Cross and D-pad effects are
  independently owner-corroborated; exact application-time semantics are still
  unknown. Create and proportional delivery were not observed. PS has no
  successful path; typed AddDevice failed before input.
- **FR-031 / four-controller lifecycle:** the owner-approved evidence is four
  handles/prompts/four Right actions and clean neutral/removal, with the
  multi-controller deferral explicitly recorded. It is not a four-player game
  certification.
- **FR-032 / reverse rumble:** blocked. No sourced feedback function, output
  report or read semantics were found; local vibration setters are not reverse
  feedback. No speculative read was attempted.
- **FR-033 / motion and touch surface:** blocked. Pad-service readback failed
  through both Open and GetHandle; typed AddDevice failed. Identity quaternion
  and accepted analog/motion reports are not effective delivery evidence.
- **FR-034 / release/removal:** ordinary exits are evidenced. Held-stop,
  disconnect and partial-failure hardware cases were not observed; the
  held-stop run was canceled.
- **FR-037 / rest and stop:** ordinary cleanup is evidenced; rest/wake is
  owner-declined and stock-runtime shared-credential reversal remains unknown.
- **FR-038 / Remote Play and Share Play conflicts:** blocked. Recovered Remote
  Play code supplied no candidate-query call sites or typed status/transition
  ABI; no online service or guessed function was used.
- **Game-safe scheduling:** unverified. The priority-minimum/restore candidate
  and full game-control sequence were owner-declined; affinity was never
  changed and no game-impact observation exists.

Prepared but **unexecuted** source
`568b7300a99340e9abe00420fff6199db7f9e4ea78f730515afa613f6bc01c7f`:
stop-held ELF `de165661e27d2ab036df1f684b1144cdc1c060a71dc619fc872affdb1a5db288`
and game ELF `0446374b1c3d6588ad282a6f1adae7a220a72d68a50ddf3593ac61db199e79b6`.
These bytes must not be represented as hardware evidence.

Final local verification: warning-free host/PS5 `build-r3.sh`, focused CTest
5/5, sanitizer, formatting and host/target clang-tidy pass;
`scripts/check-quality.sh` exits0. All raw logs, candidate hashes, pre/post route
summaries and quality logs are under `/Volumes/4TB_Drive/tmp/t042-hermes/`.
No console window is held and no payload remains running. **T042 Incomplete.**


## T041 Codex retry — post-lag diagnostics and owner follow-up (2026-10-25)

The owner directly selected option B, relayed by the coordinator: pause capture
payloads, collect a separate bounded read-only console snapshot, then decide
whether to resume the 1080p30/1080p60 runs. The coordinator granted that exact
exclusive window. Its scope excluded capture payloads, media APIs, application
or service changes, reboot, rest and jailbreak recovery. The window was released
after collection; the only deployed ELF was the previously approved read-only
network inventory.

The route inventory ELF SHA-256 was
`30154bcf13d0ee3d870426f7d3cb328f3aadcaa3c8fa72344d6666d5fd556248`; the
`prospero-deploy` binary SHA-256 was
`e6d8ab29026bf9949d05ae5023301391abe3da7d1a6595a8f31fa7d1eef7ddb9`. It ran
once, bounded to 20 seconds, and returned transport status 0. The process and
route views used the fixed read-only shsrv wrapper
`console-readonly-diagnostics.py` (SHA-256
`500b3e67c7aa565ea99ede5d45883128b1594f359d9177637cca89012d7274aa`); the
service-port check used `services.sh` SHA-256
`39559fdd76aa53c899665e242195dd5b91157eadb2f5863eca5ee9c61fee3950`. Each
shsrv request was bounded to five seconds and 128 KiB and exited its own shell
connection. Mac-side `ifconfig en9` and IPv4/IPv6 route snapshots were read-only.
No media or capture interface was called and no console or Mac network setting
was changed.

The current console snapshot reports firmware `13.600.007`,
`active_bigapp=-1`, only eth0 `192.168.2.2`, link-local
`fe80::2e4:21ff:fefd:ec6b` and loopback addresses, sole IPv4 default gateway
`192.168.2.254`, and no IPv6 default route or global IPv6 address. The decoded
route summary is `evidence/console-postlag-diagnostics-20261025/network-routes-summary.json`;
raw output is in the same directory. Mac en9 was active at `192.168.2.1/24`
with link-local IPv6 and an additional `169.254.68.120` address. Its IPv4
defaults use en8/en1 and its IPv6 defaults use en8/en1/utun interfaces; the
direct en9 route remains the isolated `192.168.2.0/24` link. These observations
match the accepted owner-attested console route disposition; they do not infer
that the Mac has no other network routes.

The shsrv `ps` list contains no capture probe process. It shows the previously
observed services still sleeping: SceAvCapture PID59 (`9.9 / 30.2 MiB` as
displayed), SceRemotePlay PID85 (`13.6 / 91.6 MiB`), SceVideoCore2K PID76
(`86.0 / 556.2 MiB`), SceMediaCoreServer PID64 (`5.7 / 46.3 MiB`), the row
labelled SceGameLiveStreamin PID62 (`9.0 / 48.6 MiB`), SceShellUI PID84
(`812.3 / 4016.5 MiB`) and SceSysAvControl.elf PID50 (`100.2 / 100.4 MiB`).
No separate process name containing ResourceArbitrator or Venc appears in this
process listing; this does not establish whether either service is hosted
inside another process. The requested `procstat -s 59 62 64 76 84 85` returned
only the shsrv prompt, without process or thread detail. `ps` provides state and
memory columns but no CPU-use or thread metrics. The sampled shsrv banners
showed SoC 43°C, CPU 45–47°C and CPU frequency 800 MHz; they are point readings,
not utilization measurements. The four service ports reported LISTENING:
websrv 8080, gdbsrv 2159, klogsrv 3232 and shsrv 2323. This is port-level
readiness; the `help` command also returned the shsrv v0.20 builtin list.

This snapshot neither identifies a stuck capture process nor explains the
persistent display lag. The owner had reported that QuickTime remained laggy
through the 720p60 run and the home UI was still choppy after probe termination
and game closure; the read-only process/route snapshot did not include a new
visual observation or evidence of recovery. The current recommendation is to
keep 1080p capture paused until the owner reviews these results and decides
whether to proceed. No further capture run is implied by that recommendation;
each resumed run would still require a fresh exact-hash approval and exclusive
window.

The 30-second 720p60 candidate itself ran on firmware `0x13600007` with
`active_bigapp=24`, flags `0x144`, state ready 0/mode 0, and successful
Avcap2 OpenVideo/Start. Over 30.016582 seconds it returned 1,799 positive
Annex-B records / 14,348,885 bytes (14,288,129 nonzero), with no empty reads
or overruns, p99 interval 16,667 microseconds, and 3,824,255 measured bits/s
against the declared 10,000 kb/s request. Avcap2 Stop/Close/Terminate,
ResourceArbitrator STOP/unregister, selector removal and all three owned-module
unloads returned 0. Source SHA-256 was
`7707921b2f3467f004e91bdf83dc7cce58e4825167b18c5feae9e57ec056f6c0`, header
`f6a6fac7213d91417dec8c6a8d67eb737dacf260e9b92aea586c18a466d8b531`, and
30-second 720p60 ELF
`865cc3a3ce2ff6790c34d7a40f1fd01e4514ea4c60a3845344ffeaf565e2ec35`. The
owner observed laggy gameplay/UI throughout, with no abnormal audio reported,
and choppy home UI after closing the game. For the 720p30 run, the owner
reported clean audio but slightly choppy gameplay/UI that did not return to
normal smoothness after probe termination. Both are observed incomplete
reversal co-occurring with the sessions, not proof of capture causality; they
remain T045/R5 cleanup concerns.

The 720p30 and 720p60 samples do not establish validated hardware-encoder
provenance, supported bitrate bounds, all four SDR combinations, or independent
resource/partial-start cleanup. The 1080p30/1080p60 runs remain unapproved and
unexecuted. Ten-bit HDR, bounded HDR-to-SDR conversion, source HDR metadata and
transitions, capture-protection behavior and protected-content audio silence
remain unverified. **T041 remains unchecked.** Full diagnostic evidence is under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/console-postlag-diagnostics-20261025/`;
video evidence is under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/console-720p30-30s-20261025/`
and
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/console-720p60-30s-20261025/`.

## T041 Codex retry — 1080p60 and final media disposition (2026-10-25)

This dated addendum supersedes earlier T041 notes that treated the selector
producer as unknown, reported the corrected candidate as still failing, or
listed 1080p30/1080p60 as pending. The task checkbox remains `[ ]`: the four
requested SDR runs are now observed, but the hardware/HDR/protection and full
cleanup criteria below are not established.

### Selector-list producer and mode-0 gate

The complete firmware-13.60 SceAvCapture image is 360,448 bytes at
`0x5badc000`, SHA-256
`c297ed6dd2350260e0894673c1b58f58aab580779795304f30d42b4db569e029`. Its
imports and 175 recovered PLT stubs/753 call sites show SceAvCapture querying
the 32-entry selector list through ResourceArbitrator; no insertion routine or
registration ioctl is called from that daemon image. The import
`fBX+DBOwNis#O#O` resolves to the ResourceArbitrator client library. The
separately recovered complete client image shows that `fBX+DBOwNis` at offset
`0x11f0` copies 32 selectors from shared memory, while `kJvjeet+f9I`
registers a resource client and sends `SceArbitratorIpc` command 0,
`CYjfezkWcaE` requests state and sends command 2, and `LqmYZGB-UEQ`
unregisters with command 1. The service-side writer/policy remains outside
the recovered images; the caller-facing path is available through the
exported ResourceArbitrator library.

Sony's Remote Play path registers resource `0xb4`, requests `RUNNING`, and
copies the ID into the video request at original request `+0x28`. The Avcap2
IPC envelope adds an eight-byte object/vtable prefix, so the SceAvCapture
object predicate's `+0x30` is that same selector, not a probe-only guessed
field. The older failure trace at `0x5bae88dc` uses the same imported list
predicate in a different request family; the full OpenVideo path also reaches
the equivalent list check at `0x5bade7b0` before Venc setup. Thus the blocker
is external selector availability. It is resolved by the ordinary caller
registration plus `RUNNING` request, not by inventing a selector value or
changing the mode-0 request. The corrected probe observed `0xb4` absent before
registration, present while requested, and absent after `STOP`/unregister.

The source evidence is retained in
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/analysis/resource-functions.txt`,
`remoteplay-resource-lifecycle.txt`, `daemon-disasm.txt`, and
`alternative-path-disposition.md`, plus the read-only predecessor artifacts
`/Volumes/4TB_Drive/tmp/t041-hermes/analysis-final/avcap2-openvideo-ipc.txt`
and `remoteplay-openvideo-caller.txt`. No predecessor artifact was modified.

### Four owner-approved SDR runs

The corrected `capture_encode.c` reproduced Sony's mode-0 request, the
ResourceArbitrator lifecycle, and Remote Play's mode-flag/state queries before
OpenVideo. Each approved run used unprotected dynamic gameplay and ran about
30 seconds with the declared 10,000 kb/s request, flags `0x144`, capture state
ready 0/mode 0, successful OpenVideo/Start, positive Annex-B records, no empty
reads or overruns, then zero returns from Avcap2 Stop/Close/Terminate,
ResourceArbitrator STOP/unregister, selector removal and the three owned-module
unloads.

| Mode | ELF SHA-256 | Elapsed | Annex-B records / bytes (nonzero) | Measured output rate | p99 read interval |
|---|---|---:|---:|---:|---:|
| 1280×720 at 30 fps | `95d837779d5a5dbf77ed2e280021f69ff3538c4af5dd5b25c4b83ce1788f22af` | 30.033111 s | 900 / 23,405,408 (23,310,644) | 6,234,561 b/s | 33,303 µs |
| 1280×720 at 60 fps | `865cc3a3ce2ff6790c34d7a40f1fd01e4514ea4c60a3845344ffeaf565e2ec35` | 30.016582 s | 1,799 / 14,348,885 (14,288,129) | 3,824,255 b/s | 16,667 µs |
| 1920×1080 at 30 fps | `50cae47680a23de615e776c27f432456a4ef7a0726ecc9ccdb83c1682e745440` | 30.001605 s | 899 / 9,892,303 (9,848,799) | 2,637,806 b/s | 33,290 µs |
| 1920×1080 at 60 fps | `bf1af7f003951081835b211a97993bca2cbd386e012bb5883b0d6a242e9ed142` | 30.002574 s | 1,798 / 565,407 (526,097) | 150,762 b/s | 16,582 µs |

All four used source SHA-256
`7707921b2f3467f004e91bdf83dc7cce58e4825167b18c5feae9e57ec056f6c0` and
header SHA-256
`f6a6fac7213d91417dec8c6a8d67eb737dacf260e9b92aea586c18a466d8b531`. These
are one run per requested combination, not a decoded validation of actual
resolution, cadence or codec/profile. The four output rates are samples at one
declared request setting, not supported bitrate bounds. Although SceAvCapture
internally calls Venc configuration/encoder functions, no independent
hardware-acceleration indication, encoded dimensions or codec/profile was
recovered. The results therefore do not establish the task's hardware-encoder
and measured-bounds requirement.

Exact 1080p deployments used these approved commands:

```sh
/opt/homebrew/bin/timeout 60s /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy -h 192.168.2.2 -p 9021 /Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/build/capture-video-1080p30-30s.elf
/opt/homebrew/bin/timeout 60s /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy -h 192.168.2.2 -p 9021 /Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/build/capture-video-1080p60-30s.elf
```

For 1080p60, an initial network-inventory invocation referenced a stale
nonexistent path and returned `Unknown payload format`; that output is
preserved as `network-preflight-invalid-path.log`. No capture candidate ran on
that attempt. The approved inventory ELF was reverified at
`/Volumes/4TB_Drive/tmp/t040-t042/build/network-inventory.elf`, SHA-256
`30154bcf13d0ee3d870426f7d3cb328f3aadcaa3c8fa72344d6666d5fd556248`, then
returned result 0 before and after capture. Both route summaries match: the
console had `192.168.2.2`, link-local IPv6 and loopback, IPv4 default gateway
`192.168.2.254`, no IPv6 default route or global IPv6 address, and
`active_bigapp=8216`. Mac en9 was `192.168.2.1`; all four dev-service ports
remained listening; no probe process remained after cleanup. Evidence for all
four runs is under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/evidence/console-{720p30,720p60,1080p30,1080p60}-30s-20261025/`.

### Owner observations and incomplete cleanup reversal

The owner reports normal audio and slightly choppy gameplay/UI during 720p30,
with visible smoothness not restored after probe termination. For 720p60,
QuickTime stayed laggy during the run; after closing the game, the home UI was
still choppy. For 1080p30, audio was fine, the game menu and home UI remained
laggy, and gameplay may have improved but the owner could not confirm. For
1080p60, gameplay, game menu and home UI were all still laggy, with no audio
change reported. The visible output degradation co-occurs across all four
runs despite zero-return API/resource cleanup; this does not establish that
capture caused it. The owner considers a console output-mode change that
teardown does not exit as a hypothesis. Any reboot is a separate owner decision
because jailbreak recovery would be required; none was performed.

### Remaining requirements and no further candidate run

No additional exact-hash console payload is proposed: the recovered contracts
do not make a remaining-row experiment both safe and conclusive. The alternatives
were evaluated in the local disposition above:

- **Ten-bit HDR:** the mode-0 Remote Play/Avcap2 contract exposes no sourced
  Main10/output-depth control, and the daemon's internal Venc calls have no
  recovered external configuration ABI. Owner confirmation of an HDR-capable
  game would not supply the missing capture control or validate bit depth.
- **Bounded HDR-to-SDR conversion:** no source conversion entry point, source
  HDR epoch, queue/memory bound or timing contract was recovered.
- **Source HDR metadata:** the recovered output-status declaration describes
  the current process's display; `sceVideoOutSysSetHdrMetadata` is a setter.
  No getter associates mastering/light metadata with the captured game source.
- **Capture protection:** SceAvCapture's internal
  `sceVencSetNormalPrivacyProtectionConfig` setter does not define protected
  content behavior, state-getter semantics or a safe silence/flush boundary.
  No protected content was accessed.
- **Error/partial-start cleanup:** all successful-start API, selector and
  owned-module cleanup returned zero. Error and partial-start hardware cleanup
  remain unobserved; the recovered caller contracts offer no source-defined
  safe console fault trigger, and host simulations are not hardware evidence.
- **Bitrate bounds/hardware identity:** the same 10,000 kb/s request produced
  the rates above. A setpoint sweep would add empirical samples but cannot
  establish a supported encoder range or hardware implementation without the
  missing encoder semantics. Given the persistent owner-observed output lag,
  no further capture run is proposed without a newly identified row it can
  close.

Alternatives that remain unavailable are Avcap2 modes 1/2 (caller mappings lack
complete allocation/ownership/cleanup contracts), `libSceVideoRecording`
(stat-present on console, but no recovered image or caller ABI), and direct Venc
(only internal SceAvCapture call sites; no normal-process interface). No
signature or offset was guessed, no protected content was tested, and the audio
row was not repeated. T041 remains **Incomplete / unchecked** because ten-bit
HDR, bounded conversion, source metadata, protection behavior, verified
hardware encoding/bitrate bounds, and complete error/partial-start cleanup do
not have real qualifying evidence.

Final verification after these documentation updates:
`R3_BUILD_DIR=/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/build bash tools/ps5-probes/build-r3.sh`
exited 0; `TMPDIR=/Volumes/4TB_Drive/tmp QUALITY_BUILD_DIR=/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/quality-build LLVM_BIN=/opt/homebrew/opt/llvm@18/bin bash scripts/check-quality.sh`
exited 0; and
`ctest --test-dir /Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/quality-build -LE pending-core -R 'test_media_probe|test_remoteplay_export' --output-on-failure`
passed 4/4. Logs are
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/build-r3-final-20261025.log`,
`check-quality-final-20261025.log` and `ctest-final-20261025.log`.

## T041 completion follow-up — restart baseline and service hold (2026-10-25)

Before this dispatch the owner restarted the PS5 through the UI and physical
power button and reported that the UI was **still slow after restart, with no
probes running**. This supersedes any implication above that a reboot was not
performed, or that the observed lag necessarily represents a capture-session
state which teardown failed to reverse. The earlier before/during/after reports
remain observations, but they do not isolate capture as the cause: the console
may already be slow from boot, and the Elgato/QuickTime observation chain may
contribute. Neither explanation is established. Zero-return API cleanup still
does not independently prove resource or output-state restoration.

The required initial Mac TCP checks against `192.168.2.2` ports **9021, 8080,
2159, 3232 and 2323** all returned `Connection refused`. No console payload or
media API ran. The worker immediately asked the coordinator to arrange owner
jailbreak/service recovery (Orca question `msg_c3702ff8219d`) and waited; after
the blocking call timed out, the same pending question was resumed. The
coordinator confirmed the Mac host/link was healthy, said the owner would
restore the jailbreak with coordinator-driven payload steps, and explicitly
authorized **local RE only until an explicit console go**. No exploitation,
service startup or console retry was performed by this worker. New private
analysis/evidence is under `/Volumes/4TB_Drive/tmp/t041-codex2/`; predecessor
artifacts remain read-only. This service hold is not a media-capability result.

### Recovery GO and isolated preflight follow-up

At 2026-10-26 04:24 UTC, the coordinator reported owner recovery complete and
granted the exact frozen cleanup window. Its recovery report states firmware
`0x13600007`, hypervisor cookie `0xcafedead00001337`, `hook_state=0x0`, and
`RESULT=hypervisor_success_cookie_verified`; this worker did not perform that
recovery. Mac reachability checks at 04:25:39 UTC found loader 9021 and dev ports
8080/2159/3232/2323 open. The approved network-inventory ELF
`30154bcf13d0ee3d870426f7d3cb328f3aadcaa3c8fa72344d6666d5fd556248`
then returned `R4_NETWORK_END result=0`. Its live interfaces were console
`192.168.2.2`, link-local IPv6 and loopback; the sole IPv4 default was the dead
`192.168.2.254`, with no IPv6 default or global IPv6 address. The gateway had an
incomplete ARP entry and no ping reply; read-only shsrv `ps` also completed.
Evidence is under
`/Volumes/4TB_Drive/tmp/t041-codex2/evidence/cleanup-open/preflight-initial-hold/`, with the
service check in `evidence/recovery-services.json`.

The worker initially stopped because an added local assertion expected Mac
forwarding flags zero. Both flags were one, which is already the explicitly
accepted retained baseline documented above; the zero assertion was a checking
mistake, not evidence of a new console route. No network configuration was
changed and neither cleanup payload had run at this checkpoint. The coordinator
was asked to renew the window using the recorded accepted isolation disposition.
Owner observation is not asserted for these read-only checks.

The final local candidate source is
`47f06956ac3e7319c780f1186a102a4fb91ac69705d93d189cead1df47c66d79`
with `media_probe.h`
`04db79959c7172f85ccf07441cf2ada4fa5e3acaf31b03d2ec02ec5a316057c7`.
The collector now retains distinct valid HDR SEI NALs sharing a NAL type,
ignores unrelated UUID/timing SEIs, unescapes payloads and rejects malformed or
over-budget records. A red-first host test exposed the prior first-per-type
loss; the corrected syntax-only fixtures pass and remain **software tests**.
`evidence/build-r3-final.log`, `check-quality-final.log`, `tidy-ps5-final.log`,
`ctest-final.log` (four tests) and `sanitizer-final.log` record passing local
C11/build/format/tidy/ASan/UBSan checks. The target tidy output contains only
system-header diagnostics excluded by the existing repository configuration;
no project diagnostic or new suppression was added. Frozen cleanup binaries
retain their separately approved earlier source hash; the fourteen newly built
stream candidates require a separate window and have no hardware result yet.

### Approved error and partial-open cleanup runs

The coordinator renewed the exact cleanup window using the recorded accepted
isolation disposition. Each frozen ELF below ran **once**, with fresh live
network-inventory, Mac checks and read-only shsrv `ps` before and after. All four
network inventory transports/results and route decodes passed, with identical
pre/post route summaries and all five service ports reachable. Neither test
called Start/ReadVideo or sent input, and the owner was **not observing**.

| Checkpoint | Frozen ELF SHA-256 | Real result |
|---|---|---|
| Open then close before Start, PID118 | `5f31795cdb294027ad6b81be8e49223f61361bf49d565dfc9cae424d606c7a84` | Initialize, two mode queries and OpenVideo returned zero; a handle was acquired and `started=0`. Close, Terminate, Resource STOP/unregister and all three owned module unloads returned zero. |
| Rejected mode 3, PID127 | `17d3ae8fec779c0be4dc4d69dfdb032b5bfd3b89353ffa53a192045bf3ab9294` | Initialize and mode queries returned zero; OpenVideo returned the sourced `0x81950001`, with no handle and `started=0`. Terminate, Resource STOP/unregister and all three owned module unloads returned zero; no unacquired Close/Stop was attempted. |

Both reported `registered=0 requested=0 selector_available=0`, five selector
queries, zero reads/frames/bytes and `status=incomplete readiness_gate=open`.
Transport exit zero is recorded separately from that deliberately incomplete
T041 result. Source/header hashes were the separately frozen and approved
`bf116959d5cb7e97ee517fa27dd2de39930d7148a85cc80c586e08ca2ed5d0eb` /
`04db79959c7172f85ccf07441cf2ada4fa5e3acaf31b03d2ec02ec5a316057c7`.
Evidence is `evidence/cleanup-open/`, `evidence/cleanup-invalid/` and
`evidence/cleanup-summary.json` under the current scratch root.

Postflight confirms the payload PIDs absent and all **non-diagnostic PID/name
pairs** unchanged. Full `ps` text and complete PID sets differ: each inspection
spawns new `sh`/`ps` helpers, sampled states change, and Memory (MiB) values are
not identical. The first OpenVideo/Close test changed SceAvCapture PID59 from
8.3/27.3 to 8.8/28.2 MiB as displayed by `ps`; the rejected-mode test stayed at
8.8/28.2. SceRemotePlay PID91 stayed 17.4/91.6 MiB in both comparisons.
Per-run `ps.diff` and `comparison.json` preserve these facts. Two early coordinator
milestones overstated snapshot/PID identity and were explicitly corrected by
the verified comparison; those overstatements are not hardware evidence.
Allocator/driver-pool reclamation and UI responsiveness were **not verified**.
The tests demonstrate owned API/resource cleanup for these two checkpoints,
not all service failures or leak-free lifetime behavior. The console window
was released, and the fourteen stream candidates were submitted separately
for exact-hash approval.

### Conditional stream window, then owner-directed CamShot repair

The fourteen exact-hash stream candidates initially received a conditional
20-minute window: source context had to be independently known as unprotected.
Preflight showed `active_bigapp=24`, `PPSA08329` eboot PID134 and SceCdlgApp
PID135, with valid isolation, but that inventory did not identify the current
picture/protection state. The coordinator reported that a single existing
CamShot request failed with AVFoundation “Video device not found”; no fresh
screenshot was produced. All fourteen stream candidates were therefore
**skipped with zero executions** in that window, and the read-only postflight
preserved isolation. Per-candidate hashes/outcomes are in
`evidence/stream-candidates-skipped.json`. That historical skip is not a
hardware rejection or evidence that the candidates cannot work.

The owner then directly instructed the worker to repair CamShot, reported live
video in QuickTime, identified the device as **Elgato 4K X**, and requested
support for both it and **Cam Link 4K**. This explicitly expanded local file
scope to
`/Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/camshot.py`.
AVFoundation enumeration showed `Elgato 4K X`; the old tool hardcoded
`Cam Link 4K:none`. The repaired source
`aec20067e9fa77db696fe1520a438b1e7d0729c24f8d81a4d8eae664e6034e45`
selects only those two exact video-device names, preferring 4K X if both are
present, excludes camera/screen/audio-only fallbacks, bounds enumeration/capture
to 5/30 seconds and atomically publishes only a newly successful image. A failed
capture preserves the old image and reports failure. Startup no longer captures
implicitly. Local syntax, Ruff format/lint and synthetic selection/failure/
replacement tests passed; the Cam Link selection test is **not Cam Link hardware
evidence**.

Only the identified old localhost server PID50977 was restarted through its
existing Terminal launcher; QuickTime stayed running. A new exact-hash,
one-capture 60-second window was granted. The exact bounded `/shot` request
returned `ok` and `device=Elgato 4K X`; its new PNG had a post-request mtime,
valid PNG signature, **1920×1080** dimensions and **173,619 bytes**. SHA-256:
`050c4554baf8a41fa8e15ae5335f1e7d5ce0666296babffe4f96e19f030ccc27`.
The worker copied and viewed only that fresh frame, showing **God of War
Ragnarök's Options menu**. Evidence is `evidence/camshot-fixed-result.json`,
`camshot-fixed-response.txt`, `camshot-local-tests.log` and
`source-context-fixed.png`. Own temporary capture files were absent afterward;
the healthy repaired server PID81427 remains available to the owner. The camera
window was released and a fresh request for the unchanged fourteen stream
candidates was submitted. This source-context image does not establish the
Avcap2 protection state, HDR signaling or source mastering metadata.

### Two measured rate points and a preflight service hold

A fresh 20-minute window accepted the observed ordinary in-game Options menu
as the source for the unchanged fourteen frozen stream ELFs. The first two ran
once, with full per-run pre/post inventories, preserved live IPv4/IPv6 isolation,
applicable Stop/Close/Terminate/Resource STOP/unregister/module-unload returns
zero, selector `0xb4` absent afterward and payload PIDs absent. Both used frozen
source `47f06956ac3e7319c780f1186a102a4fb91ac69705d93d189cead1df47c66d79`
and header `04db79959c7172f85ccf07441cf2ada4fa5e3acaf31b03d2ec02ec5a316057c7`.

| Actual run | Requested target / peak field | Records / bytes | Console interval | Measured output | p99 blocking read |
|---|---|---|---|---|---|
| 720p30, PID163, ELF `b57ab5f929ee0fb2f3cdac07d67e477db4c94d38dac1b2f79e6a1e856c7407ef` | 2,000 kbit/s / 250,000 byte/s | 300 / 64,950 | 10,014,601 us | 51,884 bit/s | 34,200 us |
| 720p30, PID172, ELF `66821e225b16efe2e684157958043d97c72164e128facdff122aac63b08c30ea` | 6,000 kbit/s / 750,000 byte/s | 300 / 65,256 | 10,013,662 us | 52,133 bit/s | 34,145 us |

All records were Annex-B-shaped; neither run had empty/overrun reads or a
parameter-collection error/overflow. **No selected SPS/PPS/HDR-SEI NAL was
returned in ReadVideo**, so independent codec/profile/depth signaling remains
unverified. The static, dark game menu and these low VBR observations do not
establish a bitrate floor/ceiling, target adherence or high-motion performance.
No VCL picture was saved or decoded. Owner observation during these runs is
not asserted. Exact evidence and memory snapshots are in the corresponding
`evidence/rate-720p30-{2000,6000}/run-summary.json` and full logs.

The third candidate (`rate-720p30-10000`) **did not deploy**: its mandatory
preflight shsrv wrapper timed out after five seconds waiting for the initial
banner, before sending `ps`. The just-prior network inventory returned zero,
reported `active_bigapp=24` and matched isolation; all five TCP ports had been
open. Those facts do not prove shsrv or whole-console health. The worker stopped
and asked without retrying; the coordinator ordered HOLD and release, and
`CONSOLE WINDOW RELEASED` was sent. The other **twelve** stream ELFs remain
unexecuted pending owner UI confirmation and a new explicit grant. No retry,
service restart, new camera request, payload, input or console state change
followed that timeout. Evidence is
`evidence/rate-720p30-10000/preflight/stage.log`, `mac-summary.json` and
`network-inventory.log`.

### Final local disposition for this completion dispatch

The owner did not answer the coordinator's UI-responsiveness check before its
timeout. The coordinator kept STOP/HOLD in force and instructed **local
documentation/verification only**, with no console or capture-device access.
The window was already released; its release was acknowledged again. Neither
the twelve remaining frozen stream ELFs nor any new header-query ELF ran after
the timeout. No retry, restart, reboot, re-exploitation or state-changing
recovery was attempted by this worker. This is an unresolved five-second shsrv
initial-banner timeout, **not proof of a whole-console crash or capture causality**.
The earlier owner restart/slowness observation remains separately preserved.

The final local source is
`68de7cf7d5a7fffce55781f8b911275c65e8d835d02774f36e49d1fc7642b4df`,
header `350406188763668b3c835fa5dd02a88021d94aa74cb0d26fdf090e181379cee4`.
Three opt-in configuration-header candidates are frozen under
`/Volumes/4TB_Drive/tmp/t041-codex2/prepared-headers/`, explicitly **built only,
not approved or executed**:

| Candidate | ELF SHA-256 |
|---|---|
| AVC 720p30 | `541d9a7ccbe8fdf5c5690ef027e8492dfdbce13fe7ad597ab5ddb7b449620759` |
| HEVC eight-bit 1080p60 | `080a701e57840481ba99c5361e744a9a299e53ee60eb767c4b3515fcc071cb49` |
| HEVC ten-bit 1080p60 | `957eb166f1c00b81e28fb271a8728c616ab6f3ecad66762ebbde26dba2a84284` |

Current local verification passed: warning-free host and PS5 C11 builds via
`build-r3.sh` with LLVM 18.1.8, `clang-format --dry-run --Werror`, repository
clang-tidy including target `R3_MEDIA_HEADERS`, four focused CTest tests,
ASan/UBSan, and repository-wide `scripts/check-quality.sh` exit zero. Logs are
`build-r3-headers-final.log`, `clang-format-final.log`,
`tidy-ps5-headers.log`, `ctest-headers-final.log`,
`sanitizer-headers-final.log` and `check-quality-headers-final.log` under
`evidence/`. Red-first header-layout tests failed before implementation and
passed afterward; a repository tidy finding in the new fake query was fixed by
asserting distinct request/response buffers, without suppression. Synthetic
fixtures remain software validation, never hardware evidence.

| Mandatory T041 row | Recorded evidence / precise remaining blocker |
|---|---|
| Sustained baseline modes | The four predecessor-approved 30-second SDR-like mode runs and their clean teardown remain recorded; this dispatch does not discard or repeat them. Independent encoded format/decoded-picture confirmation remains limited. |
| Hardware encoding with measured bitrate bounds across modes | Two corrected 720p30 targets were accepted and yielded the measured byte/rate observations above, but only static-menu output was observed. The other ten rate points remain unexecuted after the service hold, and no supported bitrate floor/ceiling or target adherence across all modes was measured. Source-reported Avcap2→SceAvCapture→Venc provenance is recorded; independent driver allocation/usage counters are absent. |
| Ten-bit HDR output | The request/depth/HEVC contract and Venc rejection of ten-bit AVC are sourced. Neither frozen HEVC candidate nor any new header-query candidate ran; no Main10/VCL-depth/HDR output was observed. HDR-capable title availability alone and the eight-bit Mac PNG do not prove current HDR output. |
| Bounded HDR-to-SDR conversion | No fully sourced conversion control/ownership contract, confirmed HDR source epoch/reference or pixel/timing/queue/memory-bound comparison was recovered and exercised. No conversion result exists. |
| Source HDR metadata | No callable getter/response mapping for the captured foreign title's mastering-display/light record was recovered. Own-output status and HDR setters, configured SPS/VUI headers, opaque ReadVideo flags and synthetic metadata fixtures cannot substitute. |
| Capture protection | No protected-content comparison was authorized/available in these windows. The traced privacy-region setter, JSON/Remote Play policy fields and special Venc read returns do not establish a capture-protection getter, transition/flush boundary or placeholder behavior. |
| Error/partial-start and resource cleanup | Two real controlled checkpoints and both real streams returned zero for applicable owned cleanup and removed selector `0xb4`; payload PIDs disappeared. API/resource closure is observed. Driver/allocator reclamation remains unverified (including the recorded SceAvCapture memory increase), and the later preflight service timeout remains unresolved. |

**T041 remains `[ ]`; the full hardware gate is open.** Software quality gates,
recovered contracts, the CamShot repair and controlled cleanup evidence do not
turn the missing mandatory hardware rows into passes. A future console window
requires owner/coordinator health confirmation plus a new explicit grant; the
fixed old stream bytes and new built-only header bytes are separately retained.

## T041 dispatch 5 — post-restart service check and rate-envelope trace (2026-10-26)

The owner's restart observation remains a separate baseline: the owner reports
that the PS5 UI was still slow after a UI restart plus physical power-button
restart, with no probes running. Capture causality is therefore not established;
boot-time slowness and the Elgato/QuickTime observation chain remain possible
contributors, not findings.

Before the coordinator's current access hold arrived, this worker performed the
required Mac-only TCP check against `192.168.2.2`:

```sh
for port in 9021 8080 2159 3232 2323 1337; do
  if nc -G 2 -z 192.168.2.2 "$port" >/dev/null 2>&1; then
    printf 'IPv4 port %s reachable\n' "$port"
  else
    printf 'IPv4 port %s unavailable\n' "$port"
  fi
done
```

All six ports reported `reachable` (9021, 8080, 2159, 3232, 2323 and 1337).
This is only a point-in-time IPv4 TCP-connect result; it did not verify a
service protocol/banner, current jailbreak state, IPv4/IPv6 isolation, or media
health. In particular, the later read-only `shsrv` preflight for the third
candidate timed out waiting five seconds for its initial banner and stopped
before payload deployment. The coordinator then held all console, service,
camera and capture-device access; this worker made no later probes, retries,
or payload calls.

The local SceRemotePlay trace now recovers the AVC/HEVC writer branch for the
optional bitrate envelope (see `research-r3-media-re.md`, “Dispatch 5”). Its
double-precision calculations land as clamped u32 fields, and its clamp
inputs originate in dynamic context fields whose application-level meaning and
runtime values are not available in the partial image. Existing AVC candidates
left this branch zeroed, so the two 720p30 static-menu measurements are not a
complete sweep of Sony's optional rate policy. They remain measurements of
those exact requested bytes only: 2,000 and 6,000 kbit/s requests yielded
51,884 and 52,133 bit/s over 300 records each, with zero selected SPS/PPS/VPS/SEI
NALs. The other ten rate points, HEVC eight-/ten-bit candidates, and three
configuration-header candidates were not run after the service hold.

The prior real cleanup checkpoints and four historical 30-second SDR-like
mode runs remain recorded above; none supplies allocator-reclamation evidence.
No ten-bit Main10 output, bounded HDR-to-SDR comparison, independently observed
source HDR metadata, protected-content comparison, or complete partial-start
reclamation result was added in this dispatch. **T041 remains `[ ]` and is not
complete.** The T041 checkbox was not changed.

### Dispatch 5 local verification

The local R3 build completed with exit status zero using
`R3_BUILD_DIR=/Volumes/4TB_Drive/tmp/t041-codex2/build-dispatch5 bash tools/ps5-probes/build-r3.sh`;
its final test line was `T041 Remote Play export bounds/cleanup tests passed;
hardware evidence absent`. LLVM 18.1.8 host and PS5 C11 compilation completed
without warnings. The explicit focused command
`ctest --test-dir /Volumes/4TB_Drive/tmp/t041-codex2/quality-build-dispatch5
-LE pending-core -R 'test_media_probe|test_remoteplay_export'
--output-on-failure` passed all four tests. The task-owned C-file formatting
check with LLVM 18.1.8 `clang-format --dry-run --Werror` passed, and
`scripts/check-quality.sh` completed with `Quality exit status: 0` (including
repository clang-tidy). Durable command output is in
`/Volumes/4TB_Drive/tmp/t041-codex2/check-quality-dispatch5.log`,
`ctest-dispatch5.log`, `clang-format-dispatch5.log`, and
`build-r3-dispatch5-final.log`; the quality build directory is
`/Volumes/4TB_Drive/tmp/t041-codex2/quality-build-dispatch5/`. These are
software/build checks, not console evidence; no source C or test file was
changed in this dispatch, and T041 remains unchecked for the hardware gaps
listed above.

## T041 dispatch 6 — renewed window, two completions and fail-closed cleanup stop (2026-10-31)

The owner explicitly authorized whatever PS5 operations were needed to continue the blocked
items. Before any payload, the Mac retained `192.168.2.1/24` on `en9`, the console answered two
IPv4 pings with no loss, and websrv, gdbsrv, klogsrv and shsrv all passed the existing status
check. A fresh shsrv banner/help/`ps` exchange succeeded on firmware 13.60. PID 134,
`PPSA08329`, remained the active big application while the captured HDMI picture showed the
PS5 home screen. A fresh bounded CamShot request selected Elgato 4K X and produced a 1920x1080
frame. This established a healthy execution window and a suspended ordinary-game context; it
did not establish a dynamic, protected, HDR or independently calibrated source.

Each attempted candidate was the exact frozen byte sequence from
`/Volumes/4TB_Drive/tmp/t041-codex2/approved-streams/manifest.json`. The local wrapper verified
both frozen manifests, refused repeat payload logs, captured fresh before/after screens, ran the
fixed isolation/process preflight and postflight, and stopped on the first missing completion or
cleanup result. The completed observations were:

| Candidate | Records / bytes | Console interval | Measured output | p99 blocking read | Result boundary |
|---|---:|---:|---:|---:|---|
| 720p30, requested 10,000 kbit/s, PID 191 | 300 / 58,884 | 10,018,898 us | 47,018 bit/s | 33,269 us | Complete probe result; all applicable owned cleanup returned zero, selector `0xb4` absent and payload PID absent |
| 720p60, requested 2,000 kbit/s, PID 200 | 600 / 85,997 | 10,014,664 us | 68,696 bit/s | 16,652 us | Complete probe result; all applicable owned cleanup returned zero, selector `0xb4` absent and payload PID absent |

Both populations were Annex-B-shaped with zero empty/overrun/cancelled reads and zero selected
parameter/HDR-SEI NALs. Their pre/post SceAvCapture displays remained 9.8/30.0 MiB and
SceRemotePlay 17.5/91.6 MiB. The static suspended source and very low measured rates do not
establish requested-target adherence, a supported bitrate endpoint, codec/profile/depth,
picture decode, source metadata or driver/allocator reclamation. Evidence is under
`evidence/rate-720p30-10000/` and `evidence/rate-720p60-2000/` in the T041 scratch root.

The next once-only candidate, 720p60 requested 6,000 kbit/s, ran as PID 209 and produced 600
Annex-B-shaped records / 82,990 bytes over 10,011,039 us (66,318 bit/s; p99 blocking read
16,828 us). It returned zero for Stop, Close, Terminate, Resource STOP and unregister, showed
selector `0xb4` absent, and unloaded libSceAvcap2. It then exceeded the fixed 35-second sender
bound. The log has no `R3_VIDEO_RESULT` and no unload result for the still-owned
libSceResourceArbitrator or libSceIpmi modules. Two later read-only shsrv snapshots showed PID
209 still sleeping. This is a cleanup-completion failure, not a complete probe result and not
proof that the console crashed.

An initial shsrv `kill` request did not remove the process. A warning-free one-shot public-SDK
recovery ELF was therefore compiled for the exact observed PID only: source SHA-256
`36233c65c876e62b3dcbdfeb2abf85d887656f2c0acfb4a3619767c94bc7321b`, ELF SHA-256
`3caf3b6439bb28bf92af4cafb494af1fad52495144bfe0138fd8d9bce13d56eb`. Its sole operation was
`kill(209, SIGKILL)` and it reported result 0 / errno 0. The fixed postflight then passed, PID
209 was absent, all four development services were listening and a fresh Elgato frame was
available. Cleanup return values and the fresh home-screen image do not independently prove
driver reclamation or full UI responsiveness.

No later rate, HEVC or configuration-header candidate ran after this failure. In particular,
there is still no Main10 result, independent format/profile signaling, source HDR metadata,
HDR-to-SDR conversion, protected-content boundary or supported bitrate envelope. **T041 remains
`[ ]`; T042/T044 and production adapter work remain blocked.** The fail-closed stop preserves the
new cleanup failure instead of retrying or converting the partial observation into a pass.

## T041 dispatch 7 — cleanup hardening and one-second trace (2026-10-31)

The cleanup-completion failure in dispatch 6 motivated a source-preserving diagnostic change.
The shared fixed-capacity module cleanup helper visits owned modules in reverse order, records
an attempt before observation/invocation, and refuses to repeat an attempted call with unknown
completion. Service, observer, unload-return and stop-result failures remain sticky and prevent
later dependency unloads. Audio, video and resource-only paths retain the same
`sceKernelStopUnloadModule` ABI. They now flush a module/handle/monotonic-time begin record before
invocation and a completion record only after return. Exhausted, failed or interrupted
selector-removal polling sets cleanup failure and prevents module unloading. The platform call
and output flush themselves still have no verified hard deadline.

The [software evidence](../../.omo/evidence/cleanup-lifecycle-20261031/REPORT.md) retains the
test-first link failure and subsequent passing ordinary/NDEBUG tests, focused CTest 1/1,
host/PS5 builds, formatting and host/target static analysis. Tests cover partial ownership,
reverse order, separate return/result errors, observation failures, interrupted completion,
no retry and selector-query exhaustion. The
[independent review](../../.omo/evidence/cleanup-lifecycle-code-review.md) reports CLEAR/APPROVE.
These establish portable orchestration behavior; they do not prove that the firmware hang is fixed.

The owner-authorized diagnostic used a fresh firmware-13.60/service/process/HDMI preflight and
sent no controller input. The ordinary game `PPSA08329` remained suspended as PID 134 while
the PS5 home screen was visible. The first `-O0` ELF send returned zero in approximately
0.45 seconds with no payload stdout; a later process inventory found no surviving probe.
That attempt is invalid/unobserved evidence. An inventory-only control then returned normal
stdout as PID 233 without media calls. The trace was rebuilt with the repository's verified
`-O2 -g` target flags and a distinct ELF hash; the cause of the `-O0` outcome is not established.

The [manifest](../../.omo/evidence/t041-cleanup-trace-20261031/manifest.md) records both builds,
commands and preflight. The observed ELF SHA-256 is
`17e718f85954774986e3b78ebf48714f59f1ceaa115ddd40a12d0e524642548f`, built from source SHA-256
`eccdf497127f3b015b6ead8e8733de937b2ce0a53cbebfbb626d62846ca7e7d1` and header SHA-256
`674234c4a6132139e234fbc266c05d05863887c14f83322bb10838f19eba5f0e`.

The one-second 720p60/6,000-kbit/s trace ran once as PID 234. Its
[payload log](../../.omo/evidence/t041-cleanup-trace-20261031/payload.log) records 60
Annex-B-shaped records / 21,311 bytes over 1,006,144 us (169,446 bit/s), p99 blocking read
20,330 us, zero empty/overrun/cancelled reads and zero selected parameter NALs. Stop, Close,
Terminate, resource STOP and unregister all returned zero; selector `0xb4` was absent afterward.
All three owned module unloads completed with zero return and stop-result:

| Module | Begin (console monotonic us) | Completion (console monotonic us) | Observed span |
|---|---:|---:|---:|
| Avcap2 | 487202212075 | 487202212227 | 152 us |
| ResourceArbitrator | 487202212233 | 487202212426 | 193 us |
| Ipmi | 487202212431 | 487202212516 | 85 us |

These spans include observer/logging work between timestamps and are not isolated API latency
measurements. ResourceArbitrator completed in this run, but the earlier intermittent hang remains
unexplained and individual platform-call completion remains unbounded. A future begin record
without completion narrows the boundary; a blocked-thread stack is needed to prove the exact
blocking operation.

The [kernel-log excerpt](../../.omo/evidence/t041-cleanup-trace-20261031/klog-excerpt.log)
corroborates resource 180 RUNNING/STOP, callback-list/queue removal, session close, scan-in
start/stop and PID 234 exiting with value 2. The
[postflight record](../../.omo/evidence/t041-cleanup-trace-20261031/result.md) reports PID 234
absent, all four development services listening and a fresh retained home-screen image. These
observations do not independently establish driver reclamation, UI responsiveness or full output
restoration. The prior choppiness observation and cleanup failure remain relevant.

### Bounded ten-second reproductions (2026-10-31)

The [reproduction manifest](../../.omo/evidence/t041-cleanup-trace-20261031/reproduction-manifest.md)
froze the same instrumented source in a ten-second 1280x720/60-fps, requested 6,000-kbit/s
AVC/8-bit configuration. Its ELF SHA-256 is
`e8ffc3fdb6f5ffc57ad5fa6db611734d811824c8db82fce6cc00507ca98b355`. The maximum was two
consecutive executions with immediate stop on missing final output, cleanup failure, uncertain
selector removal, a surviving payload or failed service/HDMI postflight. Both executions
completed; the maximum was reached without reproducing the hang.

| PID | Records / bytes | Interval | Measured rate | p99 blocking read | Avcap2 / ResourceArbitrator / Ipmi observed unload spans |
|---|---:|---:|---:|---:|---|
| 238 | 600 / 78,400 | 10,015,774 us | 62,621 bit/s | 16,655 us | 160 / 204 / 85 us |
| 239 | 600 / 81,903 | 10,015,657 us | 65,419 bit/s | 16,617 us | 155 / 208 / 92 us |

The [PID 238 payload log](../../.omo/evidence/t041-cleanup-trace-20261031/reproduction-run-1.log)
and [PID 239 payload log](../../.omo/evidence/t041-cleanup-trace-20261031/reproduction-run-2.log)
retain the exact populations and begin/completion timestamps. All cleanup return and
stop-result values were zero; selector `0xb4` was absent after each cleanup. The
[curated kernel observation notes](../../.omo/evidence/t041-cleanup-trace-20261031/reproduction-klog-excerpt.log)
record RUNNING-to-STOP transitions, callback/queue/session removal and each process exiting
with value 2; they are not a raw, timestamp-preserving kernel stream. The
[result record](../../.omo/evidence/t041-cleanup-trace-20261031/reproduction-result.md) reports
listening services; the [postflight process inventory](../../.omo/evidence/t041-cleanup-trace-20261031/reproduction-postflight-ps.log)
contains neither payload PID, and the [postflight image](../../.omo/evidence/t041-cleanup-trace-20261031/reproduction-postflight.png)
shows the PS5 home screen without an error dialog. These do not establish UI responsiveness.
As with the one-second trace, these spans include observer/logging overhead. The explicit
`service_call_deadline=unknown` remains: two completed repetitions neither explain the prior
intermittent hang nor bound a future platform call.

The final payload marker remains `status=incomplete readiness_gate=open`. These cleanup
traces do not extend the supported bitrate envelope or establish decoded format, Main10,
HDR/protection or resource budgets. **T041 remains `[ ]`, and T045 readiness remains ERROR.**

## T041 dispatch 8 — HEVC10 configuration-header observation (2026-10-31)

One separately reviewed, owner-authorized diagnostic ran as PID 254 on firmware 13.600.007.
The [manifest](../../.omo/evidence/t041-hevc10-header-20261031/manifest.md) binds the ELF
SHA-256 `4986f141087062c0cdae99d678d5ac1d2e50f56d624582a99d5736664460c4f7` to the
instrumented source and a one-run limit. It requested 1920x1080/60, HEVC/ten-bit and
10,000 kbit/s. The [payload log](../../.omo/evidence/t041-hevc10-header-20261031/payload.log)
records header-query return zero/valid, 600 Annex-B-shaped records / 528,559 bytes over
10,014,875 us (422,219 bit/s), p99 blocking read 16,642 us, and three configuration NALs
totaling 103 bytes. No picture payload was saved; configuration-to-VCL association is unverified.

Independent FFmpeg 8.0.1 [parsing](../../.omo/evidence/t041-hevc10-header-20261031/parse-summary.md)
of the retained VPS/SPS/PPS reports Main10 profile identifier 2, level identifier 150,
4:2:0, ten-bit luma/chroma, coded 1920x1088 with an eight-row conformance crop to 1920x1080,
and limited-range VUI primaries/transfer/matrix 9/16/9 (BT.2020/PQ/BT.2020 non-constant
luminance). These are configured SPS/VUI values. `ffprobe` also reports missing picture,
unknown profile/pixel format and zero display dimensions; the header-only `trace_headers`
null-output invocation exits 234 because dimensions are not set. Neither result is a decoded
Main10 picture population or an independent source-HDR observation.

All service and module cleanup returned zero, selector `0xb4` was absent afterward, and
Avcap2/ResourceArbitrator/Ipmi observed unload spans were 191/224/89 us including observer
overhead. The [kernel excerpt](../../.omo/evidence/t041-hevc10-header-20261031/klog-excerpt.log)
records resource RUNNING-to-STOP, callback/queue/session removal and PID 254 exit value 2.
The [postflight record](../../.omo/evidence/t041-hevc10-header-20261031/result.md) retains PID
absence and home-screen frames; the operator reported all four development services listening.
These observations do not establish driver reclamation, UI responsiveness or game impact.

The earlier intermittent cleanup hang remains unexplained and platform-call deadlines remain
unknown. The one-run limit was reached without retry. Hardware provenance, decoded Main10,
source metadata, protection, conversion, bitrate bounds and complete resource accounting remain
unverified. **T041 remains `[ ]`; T045 readiness remains ERROR / Incomplete.**

## T041 dispatch 9 — retained HEVC10 pictures and independent decode (2026-10-31)

One reviewed diagnostic ran as PID 261 on firmware 13.600.007, using the opt-in
2 MiB / 1024-record retention path. The
[manifest](../../.omo/evidence/t041-hevc10-vcl-20261031/manifest.md) binds source and ELF
SHA-256 `710a5f4d312250ce8a8e338ed8e7bf9e9a540616d1d11ea732b776f21894053a` and a
one-execution limit with a 40-second sender timeout. It requested 1920x1080/60, HEVC,
ten-bit and 10,000 kbit/s. The
[payload log](../../.omo/evidence/t041-hevc10-vcl-20261031/payload.log) retains 600 ordered
ReadVideo records / 542,607 bytes over 10,014,179 us (433,470 bit/s), p99 blocking read
16,618 us, no overruns/cancellation, and 103 bytes of VPS/SPS/PPS. All 4,539 indexed
export chunks and the valid terminal marker are present. Private raw video is retained
as local investigation evidence, not a release artifact.

The [reconstruction and decode](../../.omo/evidence/t041-hevc10-vcl-20261031/parse-summary.md)
prepend those exact configuration headers to the exact retained records. The resulting
542,710-byte stream has SHA-256
`be189a68d008e35922a2e589a6bafd0a3c4fb25709ae702fb2bb8c5fe519e4e9`.
FFmpeg/ffprobe 8.0.1 independently decode all 600 frames without errors: HEVC Main 10,
`yuv420p10le`, display 1920x1080, coded 1920x1088, level 150, limited range and
BT.2020/PQ/BT.2020 non-constant-luminance signaling. The strict decode uses `-xerror
-err_detect explode` and exits zero. First/last decoded images show the PS5 home screen.
This establishes decoded Main10 and configuration-header compatibility with this retained
VCL population. The decoder's inferred frame rate and timestamps do not supply measured
source cadence, and signaled color fields do not establish independent source HDR metadata.

Video/resource/module cleanup returned zero and selector `0xb4` was absent. Export began
after all three owned module unloads; their observed spans were 183/220/92 us for
Avcap2/ResourceArbitrator/Ipmi, including observer overhead. The
[postflight record](../../.omo/evidence/t041-hevc10-vcl-20261031/result.md) retains PID 261
absence and fresh pre/post home-screen images; the coordinator observed all four services
listening. No separate kernel exit record was retained for this run. Process absence and
still frames do not establish driver reclamation, UI responsiveness or game impact.

The prior intermittent cleanup hang and unknown platform-call deadlines remain. Independent
source HDR metadata, protection, HDR-to-SDR conversion, hardware provenance, supported
bitrate/audio bounds and complete resource accounting remain unverified. The one-run limit
was reached without retry. **T041 remains `[ ]`; T045 readiness remains ERROR / Incomplete.**

## T042 hardened held-Right normal-stop observation (2026-10-31)

The owner's renewed PS5 authorization superseded the earlier cancellation for one
separately reviewed `STOP_HELD` diagnostic only. The hardened implementation keeps
all cleanup assertions active under `NDEBUG`, extracts neutral-before-remove into
a host-testable seam, and covers all 16 ownership subsets and 64 injected cleanup
failure positions. Its [implementation evidence](../../.omo/evidence/t042-test-hardening-20261031/REPORT.md)
and [pre-dispatch review](../../.omo/evidence/t042-stop-held-final-code-review.md)
approved one supervised observation without claiming platform semantics.

The exact ELF SHA-256
`f90b2f4bda506aa40f4fb15816727f87a70d388a0cc8e3f5209d49a6710eaaa2`
ran once as PID 275 on firmware 13.600.007. The
[payload log](../../.omo/evidence/t042-stop-held-device-20261031/payload.log)
records one handle, 594 accepted reports, Cross at 2,006,746 us, neutral at
2,128,254 us and Right at 4,000,207 us. Ordinary cleanup submitted a complete
neutral report before handle removal, then closed HID and terminated UserService;
all four calls returned zero and the final marker says
`cleanup_calls=returned_without_error`. It also correctly retains
`delivery=unverified removal=unverified hardware_acceptance=incomplete` and
`stop_requested=0`. This is a bounded normal stop, not signal, transport-disconnect,
rest or partial-failure cleanup, and individual platform calls remain without a
verified deadline.

The [ordered HDMI record](../../.omo/evidence/t042-stop-held-device-20261031/snapshot-timing.md)
shows a controller-assignment dialog, navigation from Homebrew Launcher to the
rightmost Game Library item, a “DualSense Wireless Controller disconnected”
toast, and later frames retaining Game Library. This supports a visible input
effect and disconnect UI event. It does **not** prove visible neutral release:
the rightmost boundary masks a continuing Right report, while CamShot exposure
instants are not synchronized with payload events and the filenames describe
only intended phases.

The fresh postflight shsrv inventory contains no PID 275, retains all four
development-service processes and reports the same firmware; separate TCP checks
found all four ports listening. The background title PID 134 is present before
and after, but its sampled state changes from SLEEP to RUN, so no game-safe
coexistence or impact claim follows. The [evidence report](../../.omo/evidence/t042-stop-held-device-20261031/REPORT.md)
and [independent review](../../.omo/evidence/t042-stop-held-device-code-review.md)
preserve these limits. Rumble, motion/touch, Create/PS/proportional coverage,
four-player functional assignment, conflicts, rest/wake, disconnect/partial
failure, exact delivery timing and scheduling remain open. **T042 stays `[ ]`;
T045 readiness remains ERROR / Incomplete.**

## T042 neutral-observation preparation and no-run disposition (2026-10-31)

The follow-up `NEUTRAL_OBSERVE` diagnostic owns one controller, sends a bounded
Cross assignment pulse, remains neutral through a setup interval, then holds
Down for one second. On an ordinary deadline completion it submits a freshly
initialized complete neutral report, retains the registered handle for a
three-second observation, and only then removes it. Signals, setup/report
errors, failed neutral submission, clock failures/reversal/overflow, wait
failure and a frozen-clock iteration cap all bypass or interrupt the dwell and
continue to removal. Existing multi-handle cleanup behavior is unchanged.

Ordinary, optimized `NDEBUG`, Apple ASan/UBSan, host-layout and target-build
checks passed, including all existing ownership/failure combinations. The
[implementation report](../../.omo/evidence/t042-neutral-observe-20261031/REPORT.md)
and [independent pre-dispatch review](../../.omo/evidence/t042-neutral-observe-predispatch-code-review.md)
approve only one supervised experiment after a verified no-active-game state,
continuous advancing HDMI capture and a safe interior Settings list. The
candidate ELF SHA-256 is
`058bf6c7fdac01d300e83fc14c646cb64eb00267e83c7c8aeefd098bf1e00a02`.

Those prerequisites were not met. PID 134 / `PPSA08329` remained in `RUN`
state after two exact-PID termination requests were interrupted before
dispatch. The payload was therefore not uploaded or launched, and no controller
report or Settings launch was sent. Capture scripts in the evidence directory
are preparation helpers, not observations. The
[device-attempt record](../../.omo/evidence/t042-neutral-observe-device-20261031/REPORT.md)
was explicitly **NOT RUN** at that time. No neutral-release, cleanup, timing or
coexistence claim followed from that first attempt.

### T042 active-game closure and neutral-observation continuation (2026-10-31)

The owner then authorized deliberate PS5 changes. An independently reviewed
one-shot helper required both LncUtil and SystemService to report exactly AppId
24 before issuing one `sceLncUtilKillApp(24)`. On firmware 13.600.007 it observed
PID 134 / `PPSA08329` / AppId 24, made that single call, received zero, and then
observed the established SystemService no-big-app sentinel. Separate shsrv
postflight contained neither the title nor AppId, all four development services
remained available, and a fresh HDMI frame showed Game Library. The
[implementation](../../.omo/evidence/t042-close-active-game-20261031/REPORT.md),
[independent review](../../.omo/evidence/t042-close-active-game-code-review.md)
and [device result](../../.omo/evidence/t042-close-active-game-device-20261031/REPORT.md)
retain the exact artifact identity and limits. This clears only the active-game
prerequisite; it does not itself satisfy T042.

The reviewed neutral-observation ELF subsequently ran once with no active game.
A corrected wall-clock/CFR Elgato path retained 45.000000 seconds / 1,350
1920x1080 frames. The payload logged one handle, bounded Cross and Down events,
successful complete-neutral submission, a completed three-second registered
handle dwell, removal, HID close and UserService termination. All cleanup
platform calls returned zero and the observation helper reported its completed
status. The final marker retained delivery/removal unverified and
`hardware_acceptance=incomplete`.

The required setup failed: `launch NPXS40008` returned `0x80940031` /
`Resource temporarily unavailable` and displayed “Can't find the game or app.”
An exact-PID TERM request did not interrupt the payload; its final record says
`stop_requested=0`. The video shows a Game Library transition around capture
seconds 32–33, onboarding through about second 41, and the launch-error dialog
at seconds 42–44; Settings never appears. There is no exact host↔payload clock
bridge. If the visible transition corresponds to the two-second Cross pulse,
the Down/neutral/removal phases occur after the 45-second recording; conservatively,
their HDMI coverage is unestablished. The
[updated device report](../../.omo/evidence/t042-neutral-observe-device-20261031/REPORT.md)
and [independent audit](../../.omo/evidence/t042-neutral-device-run-code-review.md)
therefore retain the run only as Incomplete cleanup/setup evidence. It does not
prove visible neutral release or satisfy the remaining four-controller,
feedback, motion/touch, conflict, rest/failure and scheduling obligations.
**T042 remains `[ ]`; T045 readiness remains ERROR / Incomplete.**

## T050 storage syscall diagnosis and bounded target validation (2026-10-31)

The nonshipping codex3 preflight confirmed an installed SDK syscall ABI defect without
mutating storage: missing-path `openat`/`fstatat` exposed positive errno 2 with carry set
instead of returning -1 and setting errno. The apparent descriptor 2 was inherited stderr
and was preserved. Project-local target wrappers in `platform/system/storage_syscalls.h`
now capture carry immediately and translate errors for `openat`, `fstatat`, `renameat`
and `unlinkat`; host libc calls are unchanged.

Codex4 and codex5 passed production storage generation, atomic writes and loading, but
probe-only symlink and hard-link fixtures respectively reported ENOTSUP/45. Their partial
synthetic trees were independently reviewed and exactly cleaned. Codex6 explicitly records
those unsupported fixtures as skipped, with secure restoration and unchanged reload checks;
it does not claim target symlink/multilink rejection coverage. Host defenses remain tested.

Codex6 init passed identity generation, settings/paired-registry persistence and recovery
checks. A fresh process reopened the same certificate fingerprint,
`0e9a7fabca91159c082543437873fba9089670950c3cdf7e689f516355c32e9c`, retained `paired=1`,
and completed a second atomic settings replacement (`metrics=0`, `pass=1`). Exact cleanup
removed the synthetic root, leaving only `/data/ps5-dev/services`; all four development
services remained listening. The [device sequence and raw transcripts](../../.omo/evidence/t050-storage-device-20261031/REPORT.md)
and [independent wrapper/probe review](../../.omo/evidence/t050-storage-probe-code-review.md)
record the binary identities, observations and limitations.

The active game was preserved: no console reboot, rest or power-loss scenario ran.
Fresh-process continuity therefore does not establish those durability requirements.
**T049 and T050 remain `[ ]`; readiness remains ERROR / Incomplete.**

## T050 recovery acquisition implementation (2026-11-01)

The approved fixed-endpoint adapter now has conservative host acquisition building blocks, not a
complete live provider. Bounded web, RSP, kernel-log and interactive-shell probes retain raw
diagnostic transcripts but always report `verified:false`; loader has no reviewed nondestructive
identity request and is never probed. Host interface/route/forwarding/NAT acquisition can establish
necessary local conditions only and never returns console IPv4/IPv6/exclusive isolation as true.
The sole adapter-compatible callback is HDMI, and only when an asynchronous classifier function is
bound to the digest of its actual source file. No production classifier is shipped. Socket framing,
deadlines/cancellation, quotas, stale/replaced capture files, source changes and owned-process
cleanup are covered by 142 focused tests. Known-exited process groups are never signalled; once a
leader has exited, descendant cleanup is explicitly unproven. The
[implementation report](../../.omo/evidence/t050-live-callbacks-20261101/REPORT.md) and
[clear re-review](../../.omo/evidence/t050-live-callbacks-rereview-20261101.md) approve this diagnostic-only
scope. It leaves all seven required live callbacks unbound.

A separate approved read-only inspector core queries `CTL_KERN/KERN_BOOTTIME` and walks the fixed
codex6 root through retained no-follow descriptors. It bounds depth, entries, per-file and total
bytes; rejects links, special files, cross-device/duplicate inodes and mutations; hashes contents
without emitting them; and revalidates every ancestor/root binding after traversal. Deterministic
tests refuse root/parent replacement and consumed close errors without retrying uncertain closes.
The PS5 build deliberately produces only a relocatable object because the stock SDK CRT applies
privileged bootstrap patches. It has no reviewed firmware-safe launcher, reports
`atomic_snapshot:false` and `firmware_validated:false`, and leaves process-start, active-game and
writer claims null. The [inspector report](../../.omo/evidence/t050-recovery-inspector-20261101/REPORT.md)
and [clear review](../../.omo/evidence/t050-recovery-inspector-code-review.md) therefore approve
portable acquisition infrastructure only. Current boot time cannot reconstruct the absent genuine
pre-rest boot identity. No verifier or cleanup dispatch follows; **T050 remains `[ ]` and readiness
remains ERROR / Incomplete.**

## T041 five-mode acquisition and T042 neutral/stop (2026-11-01)

The preserved codex6 population was verified after a controlled rest/wake and then
cleaned. Read-only `kern.boottime` (`1793013931`) preceded the rest attempt by
469,434 seconds and the pre-rest service PIDs 103–106 were unchanged, proving a
resume rather than a reboot. The exact 40-entry layout passed, the approved
verifier ran once (`5cf7efd1…`, fingerprint `c25403d7…`, settings 61→66,
`paired=1 metrics=0 pass=1`), and the approved cleanup reported `removed=1
pass=1`. `/data/ps5-dev` again contains only `services`. This establishes limited
rest/wake persistence and recovery cleanup, not full reboot or power-loss
durability. See the [retained run](../../.omo/evidence/t050-resume-verify-20261101/REPORT.md).

The reviewed interactive helper then ran once for READY → `1 neutral` → `2 stop`
as PID 682 with a 45-second continuous 1920x1080@30 HDMI capture. It returned
`ACK seq=1 mask=0x0000`, ordered begin/release/end milliseconds and
`DONE code=0 owned=1` with zero neutral/remove/close/terminate returns. The PID
was absent afterward, all services remained listening, and independent frame/video
review found no dialog, error or active game and no pixel-visible change beyond
the clock. This proves loader-interactive transport, neutral submission and
ordinary stop/removal. It does not prove visible release of a held input. See the
[T042 run](../../.omo/evidence/t042-neutral-stop-device-20261101/REPORT.md).

Five additional reviewed matrix candidates then ran once each: HEVC eight-bit
1080p60 (600 decoded frames), AVC 1080p60 (601), AVC 1080p30 (302), AVC 720p60
(601) and AVC 720p30 (301). Every transcript had ordered chunked export,
`complete=1`, zero module-unload results, selector removal and the final
Incomplete marker; every postflight had no probe process, all services listening
and a clean home screen. The reused Main10 1080p60 population remains the prior
600-frame decode. The AVC 60 Hz summaries show ffprobe `r_frame_rate=120000/1001`
while explicit codec timing is `60000/1001`; actual cadence is unvalidated. See
the [T041 run](../../.omo/evidence/t041-six-mode-device-20261101/REPORT.md).

T041 still lacks hardware-encoder provenance, source HDR metadata, protection,
HDR-to-SDR conversion, cadence, game capture, bitrate endpoints and driver
accounting. T042 still lacks four-controller function, rumble/motion/touch,
conflicts, held-input visible release and rest/signal cleanup. **T041 and T042
remain `[ ]`; T045 readiness remains ERROR.**

## T070–T074 provisional console adapters (2026-11-01)

The first production-facing adapter set now compiles warning-free for the pinned
target and host: exact firmware/privileged boundary, shared Avcap2 lifetime,
capture, Avcap2→SceAvCapture→Venc forwarding, stereo audio and `/dev/hid`
virtual-controller input. The shared lifetime prevents capture/audio from
double-initializing or unloading each other's modules; close paths are retryable
and retain their first diagnostic. Input reports `delivery=UNCONFIRMED` always,
rumble is unavailable, and motion/touch return `UNSUPPORTED`. HDR is refused
until the request mapping is sourced.

This is provisional code, not a production host: capture cannot emit a frame
until a sourced protection observation exists, audio cannot emit samples until a
source state is observed, `ReadVideo` remains blocking and no console run has
exercised these adapters. No `platform/main.c`, PS5 build script or live session
exists. **T070–T074 remain `[ ]`; T045 readiness remains ERROR.** See the
[adapter report](../../.omo/evidence/t070-t074-adapters-20261101/REPORT.md).

### T042 visible D-pad/assignment negative result (2026-11-01)

Two approved interactive runs tested whether accepted virtual-controller reports
reach the PS5 UI. A Down-only run returned `ACK seq=1 mask=0x0040` and a
Cross-then-Down run returned `ACK seq=1 mask=0x4000` then `seq=2 mask=0x0040`,
both with ordered milliseconds, neutral-before-ACK and `DONE code=0`. Continuous
45-second HDMI captures showed no controller-assignment prompt, no dialog or
game, and no selection movement from the etaHEN Toolbox tile in any sampled
frame. PID absence, all five services and live HDMI were confirmed after both
runs.

This establishes that API insertion, report acceptance and ACK are **not**
console delivery or assignment for this helper path. A sourced assignment step
(or different sourced path) is required before a held-input visible-release run
can close B06. The earlier 2026-10-25 four-controller observation did show four
user-switch prompts and four Right navigations; the difference remains
unexplained and is retained separately. See the
[negative-result report](../../.omo/evidence/t042-visible-pulse-device-20261101/REPORT.md).

The provisional T075 system adapter now implements the `sp5_system_ops` boundary:
exact firmware gate, no-op prepare/rollback with zero owned hooks, and an
explicitly `INVALID`/`STAGE_UNAVAILABLE` state because no verified Remote
Play/Share Play/rest query exists. Notification returns `STAGE_UNAVAILABLE`
rather than guessing an API. It compiles warning-free for target and host and
passes the quality gate, but has no console run. See the
[system adapter report](../../.omo/evidence/t075-system-adapter-20261101/REPORT.md).

### T078 console pair/web/status composition (2026-11-01)

A first console-only composition now builds warning-free with the pinned SDK
and target OpenSSL: `platform/main.c` serves the core web app/status on 8081 and
Moonlight `/serverinfo` on 47989 over the private link, over POSIX storage and
the provisional T075 system/firmware adapters. The artifact ran once on firmware
13.60 for its 30-second provisional bound. `serverinfo`, the web index, status
JSON and the empty pairing JSON all returned 200; the host emitted a clean
ready/stopped pair. No HTTPS 47984, RTSP/control/media, launch session, capture
or completed pairing is composed yet. The state directory was created 0700/uid 0.
See the [console report](../../.omo/evidence/t078-console-pair-web-20261101/REPORT.md).
**T076/T077/T078 remain `[ ]`; T045 stays ERROR.**

### T078 HTTPS pairing and process-restart persistence (2026-11-01)

The console composition gained an HTTPS 47984 listener using the core pairing
TLS context. A real Rust client (`moonlight-mcp 0.2.0`) paired against the
console with PIN `1234` through the web page token/Origin flow: the pending
request appeared in `/api/v1/pairing`, `POST /api/v1/pairing/pin` returned 200
`proof_pending`, and the client exited 0 with
`Paired with SunshinePS5 (8573990b…)`. `/api/v1/devices` listed `Rust MCP`, and
the same registry survived a full host-process restart. Plain-HTTP `/serverinfo`
still reports `PairStatus=0` because no client certificate is presented there.
This is real-client pairing and process-restart persistence, not an official
Moonlight-client matrix or console-reboot persistence. See the
[T078 report](../../.omo/evidence/t078-console-pair-web-20261101/REPORT.md).

### T079 diagnostic console SDR session and sourced system state (2026-11-01)

The production system adapter now uses console-observed getters:
`sceRemoteplayIsRemotePlaying()` (0 idle), `sceSystemStateMgrGetCurrentState()`
(1000 running), and the pointer forms
`sceShellCoreUtilGetRemotePlayStatus(int32_t *)` /
`sceShellCoreUtilGetSharePlayStatus(int32_t *)` (rc 0, idle -256). The
no-argument SharePlay form crashed; the pointer form is required. The adapter
now reports `VALID` state on this console and still leaves notification
`STAGE_UNAVAILABLE`. See the [state probe](../../.omo/evidence/t078-state-probe-20261101/REPORT.md).

A full diagnostic console host (`dist/sunshineps5-stream.elf`) then ran a real
10-second 1080p60 H.264 SDR session over the T071–T075 adapters. It delivered
584 video frames and 1,962 Opus packets, recorded 9 host input events imported through the session-bound JSONL endpoint; the console_diagnostic CON-input criterion passes for host receipt while delivery remains UNCONFIRMED, and
ended with `clean=true`. The client measured 57.89 fps, 24.17 ms frame
intervals, 2.80 Mbps, non-silent audio and zero audio packet loss; the host
chosen target was 20,000 kbps. Protection CLEAR was an explicit unsourced diagnostic override,
now defined by the reviewed [diagnostic-protection contract](contracts/diagnostic-protection.md). The client report remains overall Incomplete (reference
criteria/recording absent), and 57.89 fps / 2.80 Mbps over a static home screen remain diagnostic gaps. A later actual game-content run reached 57.64 fps and 2.85 Mbps with CON-input Pass. The 2.85 Mbps figure was the old bytes/s request; after correcting request +0xcc to bits/s (`kbps * 1000`) and parking the virtual pad across sessions, 20-second game-content runs measured 16.52-17.54 Mbps at about 57.2 fps, with no controller-disconnected dialog after teardown. Audio is real 48 kHz stereo Opus at 96 kbps CBR from a
target-build libopus 1.5.2 (5 ms, 60-byte packets); the client reports non-silent
audio and zero packet loss. The client launch now carries `x-sp5-session=<report UUID>`; the console exports read-only `host_received` JSONL from `/api/v1/host-observations`, and the client fetches/imports it after teardown. The report retains `input.host_received=[host-observations.jsonl]` with no import limitation. A `console_diagnostic` profile/CON-input criterion now evaluates those records; run 15 reports CON-input Pass for host receipt while console delivery remains UNCONFIRMED. See the [T079 diagnostic report](../../.omo/evidence/t079-console-sdr-diagnostic-20261101/REPORT.md).
**T045/T079 remain ERROR/unchecked.**
