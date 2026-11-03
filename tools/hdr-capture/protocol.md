# Isolated native P010 capture

This helper implements T030's direct-USB acquisition boundary, using the probe/commit,
async bulk-transfer and rollback approach in [elgato-hdr at
7799d3c53520bcc3dd049211244bfaf49b905cb0](https://github.com/talkaboutdesign/elgato-hdr/blob/7799d3c53520bcc3dd049211244bfaf49b905cb0/tools/uvc_capture.c).
It does not import the upstream daemon, world-writable shared memory, audio path,
frame-rate fallback, or invented HDR mastering metadata. Acquisition is not calibration.
The Rust process has no USB FFI and never invokes sudo.

## Build and invocation

```sh
export TMPDIR=/Volumes/4TB_Drive/tmp
cmake -S tools/hdr-capture -B "$TMPDIR/sunshine-t030/build" \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm@18/bin/clang
cmake --build "$TMPDIR/sunshine-t030/build"
ctest --test-dir "$TMPDIR/sunshine-t030/build" --output-on-failure
"$TMPDIR/sunshine-t030/build/uvc_capture" --enumerate
```

C11 and `-Wall -Wextra -Wpedantic -Werror`; pkg-config must find exactly libusb
1.0.29. The macOS build statically links its libusb archive and dynamically links
only macOS system libraries; a root-owned installation therefore does not execute
a user-writable Homebrew dylib. Verify with `otool -L` before installation.
`--self-test` runs synthetic descriptor/parser contracts without a device.
`--enumerate` reads descriptors without opening/claiming/detaching an interface.
Capture invocation is exactly `uvc_capture --count N --clock-id ID`, with integer
`N` as 1..5 ASCII decimal digits, value in 1..36000 and an ASCII lowercase/digit/hyphen clock ID of 1..95 bytes.
The caller supplies stdin as a pipe held open throughout acquisition. Any stdin
byte, EOF, SIGINT, or SIGTERM requests rollback. Closing stdout/EPIPE or two
seconds of output backpressure also aborts. Do not use `/dev/null` as capture stdin.

A privileged deployment is an explicit operator action; the binary does not install
itself, modify permissions or configuration, unload drivers, or reset the USB device.
On this macOS host, detaching via libusb requires root and re-enumerates the device.
The owner must approve the precise elevated command before measurement. Do not change
console settings or the HDMI chain without separate authorization. No service listens
on a network/socket; frames pass only through a caller-owned local pipe.

Selection requires exactly one device with VID/PID **0fd9:009c**, a video-control
interface, a video-streaming interface with a bulk IN endpoint, and the full standard
P010 GUID with 24 bits per pixel. Interface/endpoint and format/frame indices come
from descriptors, never a hardcoded 009b/format index. The attached 5 Gb/s fixture
advertises format 7/frame 5, 1920x1080, interval **166666** in 100 ns units
(approximately 60.00024 fps). No alternate geometry, frame rate, MJPEG, eight-bit
format, or SDR substitution is allowed. PROBE must return the selected indices,
exact interval, 6220800-byte frame and a payload size in 12..65536 bytes before
COMMIT. Incompatible negotiation fails without emitting a session header.

## Version 1 binary stream

All integers are explicitly little-endian; no native struct layouts appear on wire.
There is one 128-byte session header followed by bounded records. No serial is emitted.

| Offset | Bytes | Session header |
|---|---:|---|
| 0 | 4 | ASCII `SP5H` |
| 4 | 4 | version 1 |
| 8 | 4 | frame bytes 6220800 |
| 12 | 4 | descriptor interval 166666 (100 ns) |
| 16, 20 | 4 each | width 1920, height 1080 |
| 24 | 4 | ASCII `P010` |
| 28 | 4 | reserved zero |
| 32 | 96 | Rust-provided boot-domain ID, NUL terminated and zero padded |

Each record has a 48-byte header:

| Offset | Bytes | Record header |
|---|---:|---|
| 0 | 4 | `FRAM` or final `DONE` |
| 4 | 4 | payload bytes: 6220800 for FRAM, zero for DONE |
| 8 | 8 | `CLOCK_MONOTONIC` integer microseconds |
| 16 | 8 | observed USB frame-boundary sequence (starts at one) |
| 24 | 8 | cumulative rejected observed frames |
| 32 | 4 | ownership restored: zero for FRAM, one required for successful DONE |
| 36 | 4 | completion reason: zero success; one incomplete/aborted |
| 40 | 8 | reserved zero |

FRAM is followed immediately by exactly one tightly packed P010 image: 1920x1080
little-endian 16-bit luma, then 1920x540 interleaved UV words, each ten-bit signal
sample in its high ten bits (low six padding bits zero). No conversion or resampling
occurs. The Rust consumer validates sample packing before writing `frames.p010`.
An incomplete, overlong, error-marked, malformed-header or boundary-lost frame is
rejected and counted, never padded into a valid frame. Sequence minus rejected
count must equal the complete-frame count. USB does not provide an HDMI-source
frame counter: whole source frames absent from USB remain unobservable; cadence
gaps and USB rejects cannot be reported as Sunshine host drops.

A successful DONE is emitted **after** transfer cancellation callbacks, endpoint stop,
reverse interface release, driver reattachment for each observed detach obligation,
and active-driver verification. On macOS all detach references are reversed before
checking either driver: libusb captures the entire device and its final attach
re-enumerates it. UVC reattachment has a two-second verification deadline.
Exit code must also be zero and stdout must end
immediately. Exit 1 means incomplete acquisition with software rollback completed;
exit 2 means ownership restoration is unverified, and exit 64 rejects invalid arguments
before device access. Missing DONE, nonzero exit, premature EOF, trailing bytes, wrong mode,
clock fault or restoration failure means required acquisition **Incomplete**.
Startup failures may emit no header; stderr contains bounded local diagnostics.
USB disconnect can prevent proving reattachment: it must remain unverified until
reconnection and independent driver observation. The helper does not reconnect or
silently continue a new source epoch.

## Clocks, bounds and cleanup

The C producer calls `clock_gettime(CLOCK_MONOTONIC)` and computes
`seconds*1000000 + floor(nanoseconds/1000)`, matching `src/clock.rs`. It retains the
Rust boot-domain ID without inventing a process epoch. Rust checks the echoed ID
against its local boot ID, strictly increasing timestamps, and timestamps within
the consumer's start/read bounds. These checks establish structural compatibility;
real cross-process timestamp agreement still requires R4 evidence. Timestamp means
complete UVC frame assembled **before** pipe output, not HDMI exposure, console PTS,
or calibrated optical latency. OS clock resolution does not establish accuracy.

The helper allocates one 6220800-byte frame and 16 fixed transfer buffers, each at
most 64x65536 bytes (approximately 70 MiB total plus libusb overhead). It has no
frame queue. A stalled consumer aborts after two seconds. The main event loop has
a five-second no-valid-frame deadline and an overall requested duration plus five
seconds. USB waits and cancellation callbacks are designated wait points; submitted
memory is never freed before its completion/cancellation callback. Driver/libusb
failure during cancellation can leave restoration unverified; preserve the lease
for operator inspection instead of pretending to have recovered. SIGKILL, process
crash, USB removal and machine power loss cannot guarantee software rollback.

The Rust `HdrCapture` implements the existing `CaptureBackend` interface and writes
the existing native `Recording`/`RecordingIndex` sidecars. It requires an explicit
`HdrSource` description/evidence ID establishing HDMI PQ/BT.2020 interpretation;
P010 alone cannot establish HDR color. Calibration and independent mastering/light
metadata remain absent. Acquisition `Pass` only means transport/recording integrity,
never SC-013 acceptance. Required unavailable HDR returns `Status::Incomplete`.
Its limits are 36000 frames, 128 GiB bytes, seven seconds per input/disk operation,
and requested duration plus fifteen seconds overall. P010 600-second captures exceed
128 GiB and are refused unless split into explicitly declared bounded populations.
Fresh output directories and local privacy rules follow `docs/evidence-layout.md`.

The capture service uses `CaptureBackend::manages_lease()` and
`reservation_bytes(seconds)` for this backend: HDR holds its physical-device lease
until asynchronous helper cleanup completes, and reserves P010 frame bytes plus
bounded metadata before admission. Defaults retain the SDR lease and byte budget.
The loopback client selects a fixed HDR artifact list (`frames.p010`, `frames.json`,
`recording.json`, `capture-summary.json`) only after validating the native recording;
it preserves the original fixed SDR list. There are no arbitrary artifact names.
Both local and loopback admission reject an invalid/oversized reservation; the
service includes previously retained artifacts in its 128 GiB limit.

The Rust guard closes the helper control pipe on error/cancellation and reaps it
before releasing its device lease. It does not SIGKILL a privileged helper; timeout
retains the lease for explicit inspection. Raw frames and unredacted inventory stay
under `/Volumes/4TB_Drive/tmp` or gitignored `evidence/local/`. Synthetic parser and
process-lifecycle tests are software evidence only. Actual HDR acquisition, cadence,
disconnect recovery and macOS driver restoration are recorded separately in
`specs/001-moonlight-streaming-host/research-r4-capture.md`.
