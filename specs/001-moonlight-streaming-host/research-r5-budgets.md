# R5 timing and resource findings — Incomplete

Recorded 2026-10-25 for **T044, which remains unchecked**. The nonshipping
[probe](../../tools/ps5-probes/budget.c) builds for host and PS5 and was executed
on the console in an approved exclusive window. The required streaming/input
measurement population cannot be produced from the currently established R3
path. No latency, bitrate range, allocation total or host CPU ceiling is estimated.
T043's dependency smoke passed separately; it does not close R5 or T045.

## Environment, authority and safety

Firmware: target 13.60, fresh shsrv banner 13.600.007. SDK v0.45 source
`a7685ce8e97ce1f8a4f38253a41d54241c9124f1`, LLVM 18.1.8, macOS arm64.
The installed compiler's archive-provenance limitation remains in
[dependencies.lock.json](../../cmake/dependencies.lock.json).
No Moonlight client, codec, dynamic range, negotiated bitrate, streaming session
or foreground game participated in this diagnostic. The related ENet fixture
uses `rusty_enet` 0.4.0; it is not a media or input-delivery measurement.

The coordinator granted the exact reviewed source/payload hashes and commands
after the T042 worker released the console. The transaction used Mac en9
192.168.2.1/24 to console 192.168.2.2. Fresh pre/post getifaddrs and complete IPv4
and IPv6 route dumps showed only private/link-local/loopback console addresses,
the sole IPv4 default to deliberately dead 192.168.2.254, and no IPv6 default or
global address. Mac ARP had no gateway entry. Mac forwarding flags were both 1
and were retained; no sudo/PF/NAT query or network change occurred. Isolation is
supported by the owner-attested sole attachment and these observed route/interface
conditions, not a claim of a host firewall audit or disabled forwarding.

Only SDK-declared POSIX malloc/free, clock_gettime and nanosleep are used. The
source of clock IDs 14/15 and their signatures is the pinned SDK's
`target/include/time.h`; the R3 source audit also pins the SDK kernel import
stubs. Symbol presence and successful clock calls do not independently establish
the semantics of the target kernel's process CPU clock. No guessed media ABI,
offset, kernel hook, scheduler affinity or priority was called/set.

The pinned stock SDK CRT (`crt/crt.c -> crt/patch.c`) performs the existing
per-process credential/syscall-bound writes accepted by the owner as baseline.
Their reversal is not independently established; no zero-privileged-change claim
is made. Probe code adds no privileged mutation. It acquires only its own 64 KiB
heap request, releases it, and exits. No console files, services, controller state,
capture device, game, firmware or settings were changed. Console window was
released after a second route/process inventory and existing service checks.
Only transient shell/ps helper PIDs differ; no probe process remains and
websrv/gdbsrv/klogsrv/shsrv still listen. No fresh visual-screen capture is claimed.

## Build and executed diagnostic

Scratch/evidence root: `/Volumes/4TB_Drive/tmp/t043-t044` (`<root>` below).
With `TMPDIR=/Volumes/4TB_Drive/tmp` and the pinned `LLVM_CONFIG`:

```sh
<sdk>/bin/prospero-clang -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
  -DBUDGET_IDLE tools/ps5-probes/budget.c -o <root>/budget-idle.elf
timeout 25 <sdk>/bin/prospero-deploy -h 192.168.2.2 -p 9021 <root>/budget-idle.elf
```

Host build uses `/opt/homebrew/opt/llvm@18/bin/clang` with the same flags. Default
admission and `BUDGET_IDLE` variants both build warning-free for host/PS5 (four
builds). The default variant only lists missing evidence and exits 2; it was
**not** approved for device execution and was not sent. The approved idle variant
lists the same gaps and measures a bounded sleep-only diagnostic; it returns 2
for Incomplete when clock calls succeed. Loader sender exit 0 is transport
success, not a substituted payload or acceptance exit status.

Source SHA-256: `6d53b0dd85911aa30373568570e7b79659bede7683c735a42624b2d385cdad77`.
Executed ELF SHA-256: `1496cf0679ad68703fbe64315e06c47203b0a7a1f730fb5f1795a50ba3f7c591`.
Raw stdout: `<root>/budget-device.log`; before/after process and route evidence:
`<root>/preflight/`, `<root>/postflight/`, `<root>/process-cleanup-summary.json`.
The corresponding host-only raw observation is `<root>/budget-idle-host.log`.

Actual console output (selected diagnostic fields, no host-performance inference):

```text
attributable_requested_heap_bytes=65536
allocator_overhead=unknown runtime_heap=unknown main_stack=unknown media_driver_bytes=unknown
wall_window_ns=10000033116 requested_window_ns=10000000000 exact_10s_bucket=no
owned_main_thread_cpu_ns=16000 owned_threads=1
process_cpu_ns=10000033357 scope=probe_process
allocation_released=yes workload=sleep_only
host_streaming_or_idle_budget=unresolved reason=probe_sleep_is_not_host_workload
```

The process-clock delta almost equals the ten-second wall interval even though
the only probe thread slept and its thread-clock delta was 16 microseconds.
**The process-clock value is a raw return with unverified target semantics; it
is not accepted as aggregate CPU consumption.** No conclusion that this probe
used a full core follows. The thread value is likewise preliminary clock API
evidence, not validated whole-process accounting or a host budget result. A
future sourced CPU-accounting validation needs matched wall/thread/process
boundaries, a known bounded active/sleep workload and all host threads, without
adding guessed clock IDs or firmware offsets. No extra experiment was attempted
after releasing the approved console window.

The wall window is 10 seconds plus observed scheduling/call overhead; it is not
an exact ten-second bucket. Sampling one sleep window cannot prove **every**
ten-second host interval. The touched/requested 65,536 bytes exclude allocator
metadata, SDK/CRT runtime allocations, stacks and every driver/media pool. They
cannot be called the total process, core, or host footprint. No inference of
256 MiB availability is made from successful malloc or nominal process limits.

## Required measurements and comparison to the plan

The [R3 findings](research-r3-findings.md) and
[media RE record](research-r3-media-re.md) establish no qualifying foreground
capture plus hardware H.264/HEVC/Main10 encoder path. T042 observes neutral
single-handle lifecycle and one positive D-pad response, but not the required
single-controller workload or a precisely validated host-arrival/delivery timebase.
R4's known-source VideoOut/HDMI calibration is an independent reference fixture;
it cannot supply console host encode/network boundaries. No stub or fixture is
promoted to one of the following missing observations.

| Required evidence | Planned ceiling / condition | Observation and status |
|---|---|---|
| Capture start/completion and source formats | 720p/1080p, 30/60 fps SDR; 1080p60 HDR | **Unresolved**: no admitted background-game capture path; timestamps/population absent |
| Hardware encode submission/completion | Full negotiated codec and bit-depth output | **Unresolved**: no qualified encoder ABI/permissions/completion or produced bitstream |
| Packetize start/end and final packet handoff | Per-frame full pipeline p99 < exact 1000/fps ms | **Unresolved**: no captured/encoded frame; ENet fixture packets are control-transport checks |
| Protocol/overlay processing latency | SC-003: 99% <16.7 ms over 30 min at 1080p60 | **Unresolved**: no host frame/header population; pre-send header time cannot replace final handoff |
| Host captured/encoded/sent/drop counts | SC-004: drops <0.1% with host denominator | **Unresolved**: no media frame outcomes, denominators or attribution |
| Input arrival to successful console delivery | SC-005: 99% <2 ms | **Unresolved**: no streamed single-controller population or independently validated delivery boundary; fixture ACK/echo and observed UI response do not establish delivery latency |
| Media/audio/transport allocations and surface/pool high water | 128 MiB | **Unresolved**: media path absent; kernel/driver-owned allocation accounting absent |
| Metrics allocations | 80 MiB (64 input, 8 frame, 6 histograms, 2 queue/snapshots) | **Unresolved**: no full on-device host metrics allocation or high water; design sizes are not measurements |
| Core/TLS/web/settings/logs/stacks | 48 MiB | **Unresolved**: dependency smoke functionality passed, but all allocator/stack/runtime high-water costs were not measured |
| Total attributable host allocation | 256 MiB, including attributable driver allocations | **Unresolved**: no sum of all domains; unknown domains stay unknown, not zero |
| All streaming thread CPU in each ten-second window | <=10 CPU-seconds (one core-equivalent) | **Unresolved**: no streaming workload/all-thread census; process-clock semantics additionally unverified |
| Host idle without session/browser | <=0.5 CPU-seconds per ten seconds (5% core) | **Unresolved**: sleep-only probe is not an idle host, exact buckets/all host threads absent |
| Foreground-game impact | SC-009: matched baseline/stream across >=5 games, no visible stutter/slowdown | **Unresolved**: no admitted stream, game set or matched side-by-side populations; no owner game/controller operation requested |
| Baseline SDR | SC-003/004 at 1920x1080/60 over 30 min | **Unresolved**: duration, latency, drops and workload all absent |
| Baseline HDR | SC-012: same 30-min performance and loss conditions, ten-bit HDR | **Unresolved**: no capture-to-Main10 stream; R4 reference is not a host HDR stream |
| Supported bitrate minimum and maximum by mode | FR-018: honor valid in-range, clamp to measured supported endpoints | **Unresolved**: no encoder path to sweep; both endpoints absent; no numerical bounds selected |
| Two-hour stability and metrics overhead | SC-006/008; no growth, stable p99, <=2% overhead | **Unresolved**: no real matched streaming/metrics population, no soak |

The [R4 calibration](research-r4-calibration.md) also records raw-loader launch
context/presentation limitations and failure of direct-memory allocations above
32 MiB in its experiments. Those are process-specific observations, not proof
that 128 MiB media pools or the overall 256 MiB design can be allocated safely
in a background host. This probe deliberately does not repeat oversized requests,
allocate the proposed full budgets or select a scheduler/core/priority.

## Gate decision and next prerequisites

**R5 remains ERROR / Incomplete; T044 remains unchecked.** No measured result
approves, revises or relaxes the declared 256/128/80/48 MiB or CPU budgets. T045
must retain the readiness error and prevent dependent production-host work.

Before a complete R5 run: establish sourced capture/hardware-encode/audio and
safe input delivery/cleanup under R3; establish full resource/driver ownership
accounting and validated per-thread CPU clocks; instrument real media/input
boundaries on one measured clock; obtain a separately approved foreground-game
window and matched game rubric; then measure each required mode/bitrate endpoint,
the 30-minute SDR/HDR populations and the two-hour/metrics comparisons. Retain
independent valid channels, coverage and errors under
[measurement-evidence.md](contracts/measurement-evidence.md). Unknown measurements
must remain absent and make acceptance Incomplete.


Final software verification: all four budget build variants passed with
`-Wall -Wextra -Wpedantic -Werror`; `clang-format --dry-run --Werror` passed for both
new probes; the host idle diagnostic completed with the intentional Incomplete
exit 2. This is tool/probe verification, not a passed R5 budget. The pinned SDK
source of installed POSIX declarations is `include/freebsd/time.h` and related
headers, installed by `include/Makefile`. The raw device log SHA-256 is
`d486091e15cb24cf64ee50b752c397e831c13c0fbc1277bf87c5bd9e4b5fb070`.

## Bounded clock characterization preparation (2026-10-31)

The opt-in `BUDGET_CLOCK_CHARACTERIZE` build now observes the same sourced wall,
thread and process clocks across separate 250-ms sleep and active phases. Each
boundary reads monotonic, thread, then process time in the same order; these are
sequential samples, not an atomic snapshot. Raw begin/end values and checked
deltas are printed after each phase. The sleep phase has at most 1,000 one-ms
wait attempts. The active phase has at most 1,000,000 iterations of 256 volatile
unsigned arithmetic operations, checking monotonic time before each iteration.
Neither phase adds a deadline to a platform call that itself fails to return.

A reversed/unavailable clock, wait failure, invalid bound or iteration-cap stop
fails the observation; a complete observation still returns 2 / Incomplete and
leaves host budgets unresolved. Duration comparisons use subtraction after
monotonic ordering checks, so deadline addition cannot overflow. Tests exercise
both phases, begin/end clock failures, reversal, stalled clocks, interrupted
waits, invalid timespec values and exact `UINT64_MAX` conversion.

The [software evidence](../../.omo/evidence/budget-clock-20261031/REPORT.md) records
passing ordinary/NDEBUG tests, all three host/PS5 build modes, a real host-only
clock observation, formatting and host/target static analysis. The prepared
artifact is `/private/tmp/budget-clock-20261031/budget-clock.elf`, SHA-256
`dc7230a962462458a0f57e83e317cb892559dc5267e0633ae65ed7a944e8fc31`.
No PS5 execution was performed for this preparation. These checks do not resolve
the target process-clock semantics or any all-thread host budget. **T044 remains
unchecked and R5/T045 remain ERROR / Incomplete.**

## One PS5 clock characterization (2026-10-31)

The owner-authorized execution used the prepared ELF above exactly once after
the [independent code review](../../.omo/evidence/budget-clock-code-review.md).
Its [execution manifest](../../.omo/evidence/t044-clock-device-20261031/manifest.md)
binds the source, ELF, command and ten-second sender timeout. The fresh shsrv
inventories report firmware 13.600.007. The sender returned zero after about
0.63 seconds and the [retained payload output](../../.omo/evidence/t044-clock-device-20261031/payload.log)
contains both deadline completions and the final Incomplete marker. The sender
status establishes transport completion; the payload's OS exit code and PID
were not separately retained.

| Phase | Iterations | Monotonic delta | Thread CPU delta | Process CPU delta |
|---|---:|---:|---:|---:|
| Sleep | 226 | 250,323,969 ns | 1,510,000 ns | 250,335,837 ns |
| Active | 205,523 | 250,002,105 ns | 247,983,000 ns | 250,002,025 ns |

The process clock differs from elapsed time by +11,868 ns during sleep and
-80 ns during active work, while the thread clock distinguishes the workloads.
This supports an **elapsed-like process-clock return for this observation**;
it remains rejected for aggregate CPU accounting. It does not identify the
kernel implementation or establish general clock semantics. The thread values
provide useful single-thread characterization, but sequential boundary sampling,
runtime overhead, and the absence of an all-thread census prevent promotion to
host CPU consumption or an exact ten-second budget. Neither phase validates
streaming, idle-host load, driver allocations or game impact.

The [postflight record](../../.omo/evidence/t044-clock-device-20261031/result.md)
links the retained process inventories and HDMI frames. Only the transient
inventory shell/`ps` identities differ; neither inventory contains a budget or
payload process. The game remained present as PID 134 (RUN in the preflight
snapshot, SLEEP afterward), with the home screen visible in both frames. The
operator's service checks reported websrv/gdbsrv/klogsrv/shsrv LISTENING before
and after. A still frame and process snapshot do not establish UI responsiveness
or an absence of game impact. No additional clock execution was performed.

**T044 remains unchecked; R5/T045 remain ERROR / Incomplete.** Future CPU budget
measurements require validated per-thread accounting across every owned host
thread and the declared observation windows; the process-clock return above
must not substitute for that accounting.

## Concurrent owned-thread characterization (2026-10-31)

The subsequent owner-authorized `BUDGET_THREADS` run used two concurrent owned
workers for each of two rounds, swapping the active and sleep-like roles in the
second round. Each worker bracketed its thread-CPU samples with monotonic reads.
The [implementation evidence](../../.omo/evidence/t044-two-thread-20261031/REPORT.md)
and [independent review](../../.omo/evidence/t044-two-thread-code-review.md)
preceded one execution of ELF SHA-256
`07af3e4dbf47f63fae1ec9fe5a9704720e09cc1a2f27a0f083c9dcd07b967254`.
The [manifest](../../.omo/evidence/t044-two-thread-device-20261031/manifest.md)
binds the source, command and ten-second sender timeout. The sender returned
zero; no payload PID or OS exit record was retained.

| Round | Worker | Workload | Interior monotonic interval | Thread CPU delta |
|---|---:|---|---:|---:|
| 0 | 0 | Sleep-like | 250,502,462 ns | 1,080,000 ns |
| 0 | 1 | Active | 250,001,143 ns | 248,383,000 ns |
| 1 | 0 | Active | 250,001,222 ns | 248,660,000 ns |
| 1 | 1 | Sleep-like | 250,225,902 ns | 1,085,000 ns |

The [retained output and independent arithmetic check](../../.omo/evidence/t044-two-thread-device-20261031/result.md)
show all four workers created, joined and complete. Both active workers consumed
more than 90% of their interior interval; both sleep-like workers consumed less
than 5%, with more than 100-fold active/sleep separation in each round. The role
change follows the workload. This establishes that `CLOCK_THREAD_CPUTIME_ID`
distinguishes concurrent thread-local work for these two owned workers on this
console observation. Diagnostic sums of 249,463,000 ns and 249,745,000 ns cover
the two independently sampled worker intervals in each round; they are not exact
common-window totals.

Pre/post HDMI frames show the home screen without an error dialog. The
postflight process inventory contains the four development services and no
budget/payload diagnostic; the coordinator separately observed all four service
ports listening. No separate raw port log or kernel exit record was retained.
Still frames and process absence do not establish game responsiveness or driver
reclamation. Platform-call deadlines remain unknown.

This result does not establish a production thread census, ten-second host CPU
budgets, main/SDK/library/daemon/driver CPU, real media/input populations, or game
impact. The earlier elapsed-like process-clock return remains rejected for
aggregate CPU accounting. **T044/T045 remain unchecked and readiness remains
ERROR / Incomplete.**

## Coordinator-defined ten-second window (2026-10-31)

The opt-in `BUDGET_WINDOW` follow-up replaces independently started worker
intervals with one coordinator-defined 10,000,000,000-ns target. Two owned
workers publish ready state, wait on the same start release, sample their own
thread clocks, and join before the coordinator reports explicit begin/end skew,
outside-window bounds and guaranteed common coverage. Ordinary, optimized
`NDEBUG`, ASan/UBSan, five host/target build modes, formatting and static checks
passed; the implementation and independent reviews are retained in
[software evidence](../../.omo/evidence/t044-common-window-20261031/REPORT.md),
[code review](../../.omo/evidence/t044-common-window-code-review.md) and
[device-evidence review](../../.omo/evidence/t044-device-evidence-code-review.md).

One exact-hash device diagnostic used ELF SHA-256
`6d8936615efd1e7e9aebd0f7dbe86d2a9ffa5ecfb60419bac8659e2412a21afc`.
Both workers completed and joined. Their enclosing thread-CPU sum was
10,008,296,000 ns; summed outside-window wall bound 2,451,508 ns; guaranteed
common coverage 10,001,177,603 ns; begin skew 2,726 ns; end skew 89,567 ns.
The payload correctly emitted `exact_common_window_cpu=no` and retained every
required production domain as absent/unresolved. The full limited result is in
[the device record](../../.omo/evidence/t044-common-window-device-20261031/REPORT.md).

This advances the probe from independent 250-ms samples to a shared coordinator
window with quantified coverage uncertainty. It is still a synthetic
active/sleep workload, not SunshinePS5 idle or streaming. It excludes the main
thread, coordinator/runtime overhead, production media/input/transport threads,
SDK/library/daemon/driver work, allocation high-water, bitrate endpoints and
matched foreground-game populations. The value slightly above ten CPU-seconds
is therefore neither a production PASS nor FAIL. **T044/T045 remain unchecked;
R5 readiness remains ERROR / Incomplete.**
