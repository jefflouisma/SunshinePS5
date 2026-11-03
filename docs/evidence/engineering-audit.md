# T114 engineering audit — Incomplete

Recorded 2026-10-31. This is a portable source and synthetic behavior audit.
T114 remains open: no on-console timing, resource accounting, firmware restoration,
or release security approval was established by these checks. No producer code was
changed in this audit; the additions are a regression test and its CMake registration.

## Producer allocation and blocking guard

`tests/unit/test_hotpath_guards.c` scans every function body in the following source
files for direct heap-allocation/free, blocking-lock/wait, file/socket-I/O and logging
identifiers. Only explicitly named setup/teardown functions are exempt. New helpers
inside these files are checked automatically; a missing, empty, oversized or
structurally unreadable source file fails the test. Comments and string/character
literals do not generate matches. The scanner's negative fixtures cover allocation,
locking, logging, allocator function pointers, nested bodies and exemption isolation.

| Source scope | Inspected ownership/bounds |
|---|---|
| `core/protocol/video.c`, `audio.c`, `fec.c` | Video owns one fixed 255-shard pool and at most four blocks; an active frame rejects replacement. Audio retains six fixed shards and queues at most three output descriptors. Context creation/destruction is setup/teardown. |
| `core/protocol/control.c` | Receive/send queues have 32 entries. ENet callbacks use fixed 512×2048-byte and 8×65536-byte pools; exhaustion ends/refuses work. Service/drain loops are bounded by queue capacity and call `enet_host_service` with timeout zero. |
| `core/input/controller.c`, `core/session/session.c`, `lifecycle.c` | Input ownership is independently checked before delivery; setup owns allocation. The input receive path calls controller delivery and metrics producers without draining video. Termination and pool freeing are management work. |
| `core/metrics/records.c`, `coverage.c`, `core/runtime.c` | Queue storage is allocated at creation. Full producer queues report capacity loss, retain counters and reuse storage after the worker drains them. Continuous counters use bounded atomic load/store operations. |

The guard is a source regression tripwire, not a preprocessor or transitive call-graph
proof. It does not detect new allocator aliases/macros, calls through pointers, new
translation units, or allocations/blocking inside OpenSSL, ENet or PS5 callbacks.
Setup/teardown exemptions also require call-site review. Dependency paths outside
the measured follow-up below, designated platform wait/send-point validation, and
the complete production call graph remain audit obligations. It is incorrect to infer a complete
heap-free production session from this test alone.

## Synthetic queue and admission verification

The new runtime scenario runs 100 fill/drain cycles with a three-record queue and
eight-record retention capacity. Each cycle submits 20 frame and 20 input records
while holding the metrics worker mutex: exactly three of each are admitted and 17
of each report capacity loss. The test requires 1,700 losses per producer, unchanged
storage pointers/allocation accounting, and retention bounded at eight records.
A producer that begins waiting on that mutex fails CTest's timeout. No elapsed-time
measurement from this synthetic case is a PS5 latency observation.

The media scenario reuses the same video/audio contexts for 100 submissions each.
Video refuses a second active frame and an oversized frame, then drains each accepted
frame to a bounded six packets for this fixture. Audio refuses submission until its
existing output is drained, and emits one data packet or three data/FEC packets.
These are deterministic packetizer fixtures, not encoded console content.

Related existing input independence, controller, control saturation, media protocol,
metrics nonblocking and metrics queue-loss tests were rerun alongside the new guard.
Eleven focused CTest cases passed. The build used Homebrew Clang 18.1.8 on
arm64-apple-darwin27.0.0. LLVM 18 clang-format and clang-tidy passed for the new file.

Local logs are under `.omo/evidence/t114-hotpath-20261031/`: `red.log` records the
expected failing scanner assertion before implementation, `build.log` records the
warning-free build, `focused.log` records all eleven cases and the source inventory,
and `format.log`/`tidy.log` include successful exit status. `verification.md` records
the exact invocations. These logs contain synthetic/software results only.

## Measured portable dependency follow-up

Recorded 2026-11-01 UTC. `tests/unit/test_dependency_allocations.c` installs the
supported OpenSSL memory hooks before any OpenSSL use, verifies that malloc,
realloc and non-null free each reach the counters, and measures serial calls after
context setup. Its checks remain active under `NDEBUG`. Both the normal test
translation unit and an `-O2 -DNDEBUG` build pass against the same freshly rebuilt
Debug core/dependency archives; this is not a Release core validation.

All three OpenSSL counters stay zero across 256 encrypted video/audio submissions
(1,280 video packets and 384 audio packets), including active-frame refusal and
undersized-output retries. They also stay zero across 256 authenticated control
receives, fake controller deliveries and encrypted feedback sends, including
undersized-output retries, and a separate invalid-tag termination/input-release
scenario. Client fixture encryption occurs before the measured interval. An
explicit injected OpenSSL allocation/free makes the same assertion fail in both
test builds, and the verifier requires exit 1 and the expected nonzero counters.

The same test exercises actual `enet_packet_create`/`enet_packet_destroy` calls
through the production fixed-pool callbacks: 256 mixed 1,024/4,096-byte packet
cycles, saturation at 260 simultaneous 1,024-byte packets (520 blocks), a refused
next packet, complete draining, and a successful 4,096-byte allocation afterward.
Observed final pool use is zero, high-water use is 520, and exhaustion count is one.
These pool observations cover packet allocation ownership and recovery; the
OpenSSL counters are not general-purpose libc heap counters. The subsequent
service scenario below adds bounded socket/reliability coverage. Arbitrary
dependency paths, concurrency and real platform callbacks remain unmeasured.

No production code changed. LLVM 18 format/tidy pass for the new test. Exact
invocations, binary hashes, positive and intentional-negative logs are retained in
[the dependency allocation report](../../.omo/evidence/t114-dependency-allocations-20261101/REPORT.md).
No console was accessed, no device timing is inferred, and T114 remains unchecked.

### Same-host ENet service exchange

The same registered test now runs `sp5_control_transport_step` against a real ENet
client on this host. Production admission refuses loopback addresses, so both
sockets bind one enumerated RFC1918 local interface address and use kernel-local
UDP delivery. The selected interface/address is printed; a runner lacking such
an address fails explicitly. The test neither changes interface configuration nor
contacts the PS5.

After connection setup, 256 authenticated controller envelopes traverse the real
service/receive/destruction path and reach the fake input adapter. Each produces
reliable encrypted feedback whose sequence, GCM tag and complete plaintext are
checked at the client. ACK processing restores the six-block persistent pool
baseline after every cycle. One channel-16 ACK datagram is deliberately dropped;
the test sets the peer's initial RTT to 1000 ms and advances ENet's public test
clock past the actual queued command deadline. Its send-attempt counter increases,
the retry is acknowledged, and no second application event appears. This is a
controlled retransmission scenario, not a network-latency measurement.

A 1052-byte authenticated event is split into two ENet fragments at the protocol's
minimum MTU and reassembles byte-for-byte. Malformed short input produces no
application event and leaves the session active. Graceful disconnect records
`client_quit`, releases fake input, and destruction returns pool use to zero with
no exhaustion in that scenario. Recorded exchange high-water use is 16 blocks.
A second connection repeats the exchange, then holds 257 packets to exhaust the
remaining pool. Production feedback service reports capacity failure and releases
input; after draining and closing both hosts, a new packet allocation succeeds and
pool use returns to zero. This fault interval also records zero OpenSSL hook calls.
The separate 520-block packet saturation/recovery scenario remains active.

The measured exchange includes both serial endpoints and client feedback decryption;
all three OpenSSL hook counters remain zero. A separate
`--inject-transport-allocation` control makes the same assertion fail with counters
1/0/1. Debug, optimized NDEBUG, test-unit UBSan and core-instrumented UBSan lanes
pass, as do ten optimized repetitions and the registered CTest. ENet/OpenSSL
archives are not sanitizer-instrumented by these UBSan lanes. Five-second
monotonic observation deadlines and one-millisecond client service waits bound the
fixture; no sleeps or timing acceptance thresholds are used.

Invocations, output, negative controls and hashes are retained in the
[service exchange report](../../.omo/evidence/t114-enet-loopback-20261101/REPORT.md).
No production source changed, and console performance/restoration obligations remain open.

## Firmware, parser security and remaining acceptance

The dependency manifest pins public SDK v0.45 to
`a7685ce8e97ce1f8a4f38253a41d54241c9124f1`, OpenSSL 3.5.9 to
`45e844fa2a14ec92d146bd8f5778ac130b6625fb`, and Moonlight ENet to
`aca87840b57f045a1f7f9299e4b1b9b8e2a5e2f1`. These are source-declared identities
read from `cmake/dependencies.lock.json`; this audit did not reestablish target
binary provenance or reproduce historical device smoke results. That manifest
also records unresolved installed LLVM binary provenance and target validation of
the later ENet timeout patch. Licensing work remains owner-excluded.

The implemented storage adapter now includes project-local target syscall wrappers in
`platform/system/storage_syscalls.h`. Codex3 directly confirmed that the installed SDK's raw
`openat`/`fstatat` stubs discarded carry and exposed ENOENT as positive descriptor 2. The wrappers
capture carry before flags change and translate errors to -1/thread-local errno for the four
descriptor-relative storage operations; ordinary host libc behavior is preserved. Codex6 then
passed console identity generation, settings/registry persistence, recovery checks and a second
replacement in a fresh process with the same certificate fingerprint. Exact cleanup removed the
synthetic state and left the development services listening. The target link fixtures explicitly
record ENOTSUP/45; host symlink/multilink defenses remain tested. Console restart, rest and
power-loss evidence remain absent. See the [device sequence](../../.omo/evidence/t050-storage-device-20261031/REPORT.md)
and [wrapper/probe review](../../.omo/evidence/t050-storage-probe-code-review.md).

Production capture, encoder, input, privileged-access and firmware-table implementations are absent;
firmware 13.60 research/probes are not audited production adapters. Every shipping
offset/symbol, privilege change and restoration path still requires its sourced
firmware table and actual before/after console evidence.

HTTP, RTSP, control and input fuzz harnesses and required CI wiring exist, but their
existence does not close a parser security audit. Required sanitizer/CI results,
adversarial coverage review and production dependency costs remain separate gates.
No successful remote CI run is asserted here.

## Dependency remediation and current audit

The current client graph removes `audiopus_sys` and `rsa`. It uses `opus` 0.4.0 /
`opusic-sys` 0.7.6 with bundled libopus 1.6.1, plus `aws-lc-rs` 1.19.0 /
`aws-lc-sys` 0.47.0 for RSA. Legacy PKCS#8 identity files, certificate fingerprints and
SHA-256/PKCS#1 v1.5 signatures remain compatible; 217 Rust tests and an independent OpenSSL
signature comparison passed. The dependency auditor now queries the exact commit for the
pinned Moonlight ENet fork, retaining its local version and archive provenance without a
fictional release tag. Nineteen inventory regressions pass.

The [current retained audit](../../.omo/evidence/rsa-backend-remediation-20261031/dependency-audit.json)
completed 8 host and 238 client live queries with zero advisory matches and verified locked
archive hashes. The [RSA report](../../.omo/evidence/rsa-backend-remediation-20261031/REPORT.md),
[Opus report](../../.omo/evidence/opus-advisory-remediation-20261031/REPORT.md) and
[ENet report](../../.omo/evidence/enet-audit-version-20261031/REPORT.md) preserve the distinct
historical snapshots and independent review evidence. Database coverage is limited; hosted
Linux/macOS CI, live pairing after the backend change, installed compiler provenance and
production/release security acceptance remain open. Licensing remains excluded.

## Remaining performance acceptance

The mandatory before/after stage-p99 populations are absent. The constitution's
greater-than-5% regression rejection and review-exception rule remain in force;
no regression ratio can be computed and no exception is claimed. Complete CPU,
driver/media/runtime/stacks/pool memory accounting, scheduling/game impact,
resource reclamation, firmware restoration and sustained production timing remain
Incomplete under T041/T042/T044/T045 and the later on-device tasks.
