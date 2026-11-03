# R4 capture acquisition — complete with owner-deferred disconnect test

Recorded 2026-10-25 UTC on macOS 27.0.1 (26A434), arm64. T030 and T031 are
implemented and verified; T032 acquisition evidence is complete under the explicit
owner decision below. Physical-disconnect recovery remains owner-deferred, untested
and non-blocking; this is not a claim that every recovery path passed. This is
development-machine acquisition evidence, not console streaming, HDR fidelity or
calibration acceptance.
The existing PS5 firmware 13.60/homebrew setup was preserved; its firmware version
comes from the maintained console skill, not a new firmware probe. Client version
is moonlight-mcp 0.2.0, Rust 1.93.0; helper protocol version 1, native uncompressed
P010, 1920x1080, descriptor interval 166666 x 100 ns (nominal 60.00024 fps).

## Source authority and calibration limits

The coordinator initially relayed “console HDR ON”, followed by an explicit owner
NOT SURE answer about the setting and active HDMI mode. Earlier captures are
therefore P010 transport/packing evidence only. Their retained Rust recording
sidecars are marked `validity: invalid`, with original provisional declarations
and `source-context-invalidated.json` retained for audit. They must not be reused
as established HDR reference recordings.

The owner subsequently supplied read-only screenshots, relayed by the coordinator
on 2026-10-25; this worker did not independently inspect the original screenshots.
The observed settings were HDR Always On; Deep Color Output Automatic; RGB Range
Automatic (Recommended); Resolution Automatic; VRR Automatic; 120 Hz Output
Automatic; ALLM Off; Video Transfer Rate Automatic. **Current Video Output Signal:
3840x2160 - 60 Hz, RGB (HDR), HDCP Off.** The connected HDMI device advertises HDR
support at 2160p/1440p/1080p, VRR 48-120 Hz, HDR 1080p 24/50/60/120 Hz and 2160p
24/25/30/50/60/120 Hz (YUV422/YUV420). Capability advertising is separate from the
current-signal observation. No console setting or game/app was changed by this
worker; the owner only read the information.

A fresh capture **after** that observation establishes a known PS5 HDR10 source
for its epoch: 2160p60 RGB HDR output, captured as 1080p60 P010 by the Elgato. The
chain therefore includes device scaling and RGB-to-4:2:0 chroma conversion; those
errors must be characterized in T034. This is not a claim of native 1080p console
output or an untouched source image. The fresh recording's source-evidence ID is
`r4-owner-current-rgb-hdr-signal-20261025`; its source description preserves the
operator observation and conversion context. It preserves ten-bit PQ/BT.2020 without
an SDR conversion. Its calibration ID and independent mastering/light metadata
remain absent; MaxCLL/MaxFALL are not inferred from USB pixels or invented defaults.

[Sony's HDR documentation](https://www.playstation.com/en-us/support/hardware/ps5-enable-adjust-hdr/)
establishes that Always On also converts non-HDR apps to HDR output; On When
Supported would leave unsupported apps in SDR. [Sony's video-output guide](https://www.playstation.com/en-us/support/hardware/ps5-4k-resolution-guide/)
documents the read-only Current Video Output Signal view. P010 alone is not this
source oracle. FR-025 independent source metadata and T033/T034 fidelity/calibration
remain separate open work; T032 completion is not HDR acceptance.

## Device, isolation and ownership

Read-only `system_profiler SPUSBHostDataType -json`, filtered to omit serials,
confirmed Elgato 4K X, **0fd9:009c**, **5 Gb/s**. Read-only descriptors select
P010 format 7/frame 5, VC interface 0, VS interface 1, bulk IN endpoint 0x82,
1920x1080, 24 descriptor bits per pixel, and interval 166666. Descriptor support
was followed by actual negotiated frames; there is no 009b assumption or fallback.

The coordinator/owner granted exclusive console/capture use. Repeated read-only
checks found en9 192.168.2.1/24 plus IPv4/IPv6 link-local addresses, no global IPv6
prefix; `arp -n 192.168.2.254` had no entry; external DNS through 192.168.2.1
returned 0.0.0.0; no rtadvd, bootpd, InternetSharing or natpmpd executable ran.
The owner attested the sole direct console link and documented dead static gateway.
Both Mac forwarding sysctls were 1 and remain unchanged at the coordinator's
explicit instruction; they are not represented as disabled isolation controls.
No sourced console-route-table command was available in the maintained skills,
so console static configuration/sole attachment remain owner-attested.

All root invocations used **only** `sudo -n /usr/local/libexec/sunshineps5/uvc_capture`
with validated count/clock arguments. The owner installed the final helper at that
path, root:wheel 0755, with root:wheel 0755 ancestors through `/usr/local`.
SHA-256 independently verified:
`1b681374ee01496ee2f4342651d4c279e0b6cbf86c82ecbe182f8b685a46499c`.
`sudo -n -l` confirmed the scoped rule. No user-writable tmp binary was run as root.
`otool -L` shows only system libraries/frameworks; pinned libusb 1.0.29 is statically
linked to avoid loading a user-writable Homebrew dylib in the root process.

The helper accepts no output paths or arbitrary-device arguments, limits count to
1..36000 (at most five ASCII decimal digits), and validates a 1..95-byte clock ID.
Its single frame and fixed transfer pool, bounded output wait, signal/control-pipe
cancellation and driver rollback are specified in
[protocol.md](../../tools/hdr-capture/protocol.md). Rust remains safe and unprivileged;
the local hardware harness used an operator-owned wrapper executing only the scoped
sudo command, never an internal Rust privilege escalation.

## Actual acquisition populations

All table populations include startup rejects and gaps; none are trimmed to improve
cadence. “Rejected” means observed malformed/incomplete USB frame boundaries, not
Sunshine host drops. Whole HDMI frames without a USB boundary are unobservable.

| Run | Complete / observed / rejected | Interval median / p99 / max (us) | Complete fps | Exit / restored |
|---|---|---|---:|---|
| Final helper, 120 requested | 120 / 125 / 5 | 16666 / 16810 / 100061 | 57.590 | 0 / yes |
| Control EOF after 30 frames | 30 / 35 / 5 | 16669 / 100019 / 100019 | 51.221 | 1 / yes |
| Rust debug local, 120 | 120 / 125 / 5 | 21913 / 23549 / 100050 | 44.448 | 0 / yes |
| Rust debug loopback, 120 | 120 / 125 / 5 | 22789 / 54932 / 90201 | 41.749 | 0 / yes |
| Rust release local, 120 | 120 / 125 / 5 | 16666 / 16803 / 99919 | 57.589 | 0 / yes |
| Rust release loopback, 120 | 120 / 125 / 5 | 16663 / 16833 / 99774 | 57.585 | 0 / yes |
| 60-second window, no removal observed | 3600 / 3605 / 5 | 16666 / 16828 / 100064 | 59.882 | 0 / yes |
| Timed retry, no removal observed | 3527 / 3532 / 5 | 16673 / 41070 / 142761 | 54.931 | 1 / yes, deadline |
| Fresh verified HDR release service, 120 | 120 / 125 / 5 | 16670 / 19246 / 99919 | 57.588 | 0 / yes |

The 100 ms gaps, lower debug cadence and five rejects are retained limitations,
not passing loss-free-60-fps claims. Every sample in the final helper and four Rust 120-frame
populations listed above was checked offline. For the final helper's 373248000
samples: zero nonzero low-six padding bits; signal values 0..628; low-two-signal-bit
populations [70544683, 85599933, 83204624, 133898760]. Thus ten-bit data survived
without eight-bit truncation; this does not establish HDR EOTF or capture fidelity.
The fresh verified-HDR population also checked all 373248000 samples: zero padding
violations, values 0..620, low-two-signal-bit counts
[96488507, 98982082, 92043166, 85734245]. Its 120 frames/timestamps and source/clock/
loss sidecars survived the actual loopback service transfer; the server artifact
root was empty afterward. The full 3600-frame raw stream was checksummed before
deletion, not promoted to a full-population pixel/color validation result.

Helper 120-frame wall time including USB startup/cleanup was 3.454733 s; child CPU
0.485039 s; `getrusage` maximum RSS 27279360 bytes. Rust release local/service,
including post-recording validation, took 3.56/3.75 s wall, 0.18/0.23 s user,
0.69/0.87 s system and maximum RSS 27262976/27279360 bytes (`/usr/bin/time -l`).
The fresh verified-HDR service run took 3.77 s wall, 0.20 s user, 0.88 s system,
maximum RSS 27295744 bytes. These are external acquisition baselines, not PS5 host
resource measurements.
Release builds are required for native-cadence evidence; debug timing is explicit.

C and Rust use POSIX CLOCK_MONOTONIC with seconds*1000000 + floor(ns/1000), sharing
the boot-derived ID. In the final helper run every producer timestamp lay inside
consumer start/read bounds. Header arrival minus producer stamp was 14..717 us,
median 20 us. This is pipe arrival latency, not exposure accuracy or optical delay.
An initial harness attempt incorrectly used Python time.monotonic_ns(); its clock
mismatch stopped it before any complete frame. The corrected harness uses
`time.clock_gettime_ns(time.CLOCK_MONOTONIC)`. No producer clock was changed.

## Recovery observations and limits

Normal exit and control-EOF cancellation both emitted terminal restored=1; immediate
independent `ioreg -p IOService -w0` snapshots showed UVCAssistant reattached to
Elgato interfaces 0 and 1, and audio clients returned. A fresh capture after the
owner-reported unplug/replug acquired 120 frames, exit 0, ownership restored.

The first requested physical-disconnect window completed all 3600 frames without a
removal diagnostic. The owner later reported unplugging for approximately two
seconds and reconnecting to the same port; the device was independently absent
in one subsequent IORegistry read and reappeared with both UVC clients. That proves
a physical disconnect/reconnection observation, but does not correlate it with the
already-ended capture. A second timed attempt ended at its bounded deadline with
3527 frames and restored=1, again without a USB-removal diagnostic. Neither run
is claimed as a tested NO_DEVICE/error-during-capture recovery. The owner then explicitly declined further attempts and made physical-disconnect
recovery **owner-deferred, untested, non-blocking**, with rationale “timing too hard
to coordinate; revisit only if it becomes the cause of a problem”. This is recorded
in [implementation-decisions.md](contracts/implementation-decisions.md). It no longer
blocks T032, and no NO_DEVICE recovery pass is claimed. Cancellation/rollback and
post-run UVC restoration remain evidenced. The final verified-HDR run also exited
zero/restored=1, with independent IORegistry UVC/audio reattachment and a fresh
system_profiler observation of 0fd9:009c at 5 Gb/s.

An initial helper version incorrectly checked driver activity between attach calls
and returned unverified despite independent UVC restoration. A failing two-detach
regression reproduced this. [libusb v1.0.29 Darwin](https://github.com/libusb/libusb/blob/v1.0.29/libusb/os/darwin_usb.c#L2821-L2885)
captures the whole device with a reference count and re-enumerates on the last
attach. The installed fix reverses all detach obligations before driver checks,
with a bounded two-second UVC verification wait. Normal/cancellation hardware runs
then passed. Exit 2, disconnect or missing terminal status remain Incomplete.

## Software changes and verification

The helper follows [pinned elgato-hdr 7799d3c53520bcc3dd049211244bfaf49b905cb0](https://github.com/talkaboutdesign/elgato-hdr/blob/7799d3c53520bcc3dd049211244bfaf49b905cb0/tools/uvc_capture.c).
The approved capture-service changes are default/managed lease ownership, native
byte reservation before admission, and a format-selected fixed artifact transfer
list. HDR retains its lease through asynchronous child cleanup and transfers
frames.p010/frames.json/recording.json/capture-summary.json; SDR defaults and its
original transfer list remain. Tests cover both ownership/budget paths, oversized
rejection, and a synthetic native HDR loopback roundtrip. No arbitrary names enter
the transfer allowlist. Required HDR acquisition without source/helper is Incomplete.

Packing validation checks four samples per safe integer load; padding-lane/tail
regressions and all-sample native fixtures pass. Acquisition errors now retain the
primary parse/coverage reason alongside child-exit failure. The scalar debug reader
originally exceeded the unchanged capture deadline; the optimized reader completed
all requested frames, while its remaining debug-cadence limit is recorded above.
One full test attempt overlapped real hardware capture and correctly hit its
exclusive lease in the unavailable-helper test; rerunning with hardware idle passed.

With TMPDIR=/Volumes/4TB_Drive/tmp and Rust 1.93.0 bin directory first on PATH:

```sh
cmake -S tools/hdr-capture -B /Volumes/4TB_Drive/tmp/sunshine-t030/build \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm@18/bin/clang
cmake --build /Volumes/4TB_Drive/tmp/sunshine-t030/build
ctest --test-dir /Volumes/4TB_Drive/tmp/sunshine-t030/build --output-on-failure
/opt/homebrew/opt/llvm@18/bin/clang-tidy \
  -p /Volumes/4TB_Drive/tmp/sunshine-t030/build --config-file=.clang-tidy \
  tools/hdr-capture/uvc_capture.c
cargo build --manifest-path tools/moonlight-mcp/Cargo.toml --locked
cargo test --manifest-path tools/moonlight-mcp/Cargo.toml --locked
cargo clippy --manifest-path tools/moonlight-mcp/Cargo.toml --locked \
  --all-targets --all-features -- -D warnings
cargo fmt --manifest-path tools/moonlight-mcp/Cargo.toml --all -- --check
scripts/check-quality.sh
```

All pass: LLVM 18.1.8 C11 -Wall/-Wextra/-Wpedantic/-Werror, CTest 1/1;
129 Rust tests (98 library, 1 binary, 11 capture, 10 evidence, 9 protocol), eight
unrelated future-task cases still ignored; Clippy/fmt/quality exit 0. Pending contract
activation counts: T030=0, T031=1, T032=0. The T031 contract uses production native
validation and disk loading, not a test-local byte copy. Synthetic results never
count as hardware evidence. AppleClang 21 ASan/UBSan helper build passes CTest 1/1;
LLVM18's sanitizer runtime on this macOS previously stalled before main, preserved
as a non-pass in local asan-sample.txt (no source/sanitizer suppression).

Hardware commands/harness source and pinned offline release-build provenance are
retained locally in /Volumes/4TB_Drive/tmp/sunshine-t030. The measurement command
was `python3 .../acquire.py --name NAME --count N [--cancel-after 30]`; its only
child root command is the installed helper with --count and --clock-id. The actual
Rust harness uses record_local and serve_with/record_via_server against that same
installed helper via the scoped wrapper; capture requests are two seconds/120 frames.

## Local retention and reproducibility

Raw frames, previews, unfiltered inventory and device serials stay outside version
control, following docs/evidence-layout.md. Per-frame timestamps/coverage and source
invalidation records are local. Both bulk approximately 22 GB disconnect-window
streams were removed after retaining their full checksums, first/last P010 images,
per-frame JSON and stderr. Neither deleted stream is claimed as an available full
recording for later fidelity scoring.

| Local retained artifact | SHA-256 |
|---|---|
| fixed-acquisition-120.sp5h | dce85c163a824dd918212a75d9fe576ffc062c17a9472aeb65b9bc0d8a86ac36 |
| rust-release-local-120/frames.p010 | 0774bedbc3c7d1a2171e6dd390bdb3ef3b487f5c59d9b0bfb0cbaa8f08915e15 |
| rust-release-service-120/frames.p010 (provisional HDR context invalidated) | d18e288160f415509ef3b01cc711cdee71b5ecc72611e4926a0f6d46cf212825 |
| verified-hdr-service-120/frames.p010 (fresh known HDR epoch) | 810ed46ddd9ec29d83e56b3491c97a88c4d1c1cd12ecdd5cd9c627d7a12e6932 |
| 3600-frame window, raw deleted | 8211bef2ac94e34aae35ca37370b8306750e89ffe6ed04609eff5a3c6fb9a974 |
| 3527-frame retry, raw deleted | ad3d4a39aaedf3f7901d1bf3cb22ee6a869a057ab2a2ca14df822fbe540bad68 |

T032 is complete under the recorded owner decision: real known-source HDR P010
frames, cadence/loss, shared-clock agreement and normal/cancellation/post-run UVC
restoration are evidenced; physical-disconnect recovery is owner-deferred and
untested. Calibration, scaling/chroma error and independent mastering metadata are
not established by this acquisition task and remain open for their assigned gates.
