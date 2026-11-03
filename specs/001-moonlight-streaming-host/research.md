# Phase 0 Research: Self-Contained Moonlight Streaming Host

**Feature**: `001-moonlight-streaming-host` | **Reconciled**: 2026-10-31 (T045)

The overall readiness gate remains **ERROR** in [readiness.md](readiness.md), although the 2026-11-01 SDR scope closes protection/HDR and the production budget CPU/memory gate. R4 acquisition/
calibration research and T043 dependency smoke passed, while input delivery, pipeline latency,
driver attribution and soak evidence remain Incomplete. T045 stays unchecked. The owner's 2026-10-25 direction permits portable/
fake portions of T046–T069, portable US3/US4 and fuzzing despite ERROR; production console adapters,
on-device acceptance and release tasks remain blocked. The owner's 2026-10-31 PS5 authorization
allowed the renewed nonshipping R3 investigation recorded below; its cleanup failure halted the
candidate sequence, followed by a separate one-second cleanup trace and two bounded ten-second
repetitions. Neither authorization closes
an evidence gate.
Licensing remains outside this run by owner direction; no license or constitution change is made.
T045 reconciles documents only: no fresh hardware, reference-host or dependency operation occurred.
Hardware observations below belong to the linked earlier reports.

## R1. Moonlight protocol (SOURCE BASELINE ESTABLISHED)

**Decision**: Use the pinned facts in [protocol-notes.md](../../tools/moonlight-mcp/docs/protocol-notes.md)
for pairing, HTTP, RTSP/SDP, video/FEC, audio, control, HDR signaling, and input. Official clients remain
authoritative when observations disagree with these notes or the independent test client.

**Rationale**: The notes cite source paths, line numbers, and commits. This establishes reproducible
protocol research, not demonstrated SunshinePS5 interoperability. No proprietary implementation is copied.

**Alternatives**: Packet captures supplement source evidence but do not independently explain field
semantics; assuming the existing spike implements every documented field would hide missing behavior.

## R2. Protocol test client foundation (DECIDED; ACCEPTANCE UNVERIFIED)

**Decision**: Retain Rust 2021, pin the planning toolchain to installed Rust **1.93.0**, and build the
rewrite against the existing [Cargo.lock](../../tools/moonlight-mcp/Cargo.lock). Keep `unsafe_code =
"forbid"` in [Cargo.toml](../../tools/moonlight-mcp/Cargo.toml); isolate privileged USB work in an
external helper with bounded local IPC. The Rust client consumes frames using safe I/O. The lint
constrains this package, not all dependency internals.

**Evidence**: Implementation and live follow-ups are now recorded in
[validation.md](../../tools/moonlight-mcp/docs/validation.md). Pairing, streaming, host-OS controller
delivery and teardown have actual supplemental observations; runner/reporting and native comparison
software have verification records. **T038/FR-059 remains Incomplete**: independent host-output
recording, calibrated fidelity/HDR evidence and complete evidence integration are missing. The
emitted report remains Incomplete; supplemental observations do not rewrite it. T039 is locally
implemented/verified but unchecked pending T038. The client must not yet accept SunshinePS5.

**Rationale**: The original tool's framing, pairing validation, and host-specific API audit motivated
the rewrite and source-pinned regressions in the protocol notes. Rust suits untrusted network
parsing, and process separation preserves its safety policy while accommodating the USB helper's
native interfaces.

**Alternatives**: Direct USB FFI inside this package conflicts with its unsafe-code policy; automating
official-client UI cannot replace the required machine-readable frame/input evidence.

## R3. Console capture, encode, input, and resource capability (INCOMPLETE; BLOCKING)

**Decision**: Keep production console adapters gated. The source audit and admitted nonshipping
probes do not establish the mandatory foreground-game media path. Select neither built-in streaming
reuse nor a lower-level path as proven. Software encoding, external HDMI acquisition and unsourced
kernel mappings are not approved shipping fallbacks.

**Evidence**: [R3 findings](research-r3-findings.md) completes T040's pinned source/ABI/permission
audit; T041/T042 remain unchecked. The subsequent sourced Avcap2 audio probe captured 239,616
non-silent stereo float32 frames in 5.0009 seconds on this console, with normal Stop/Close/Terminate
and module unloads returning zero. Source timestamps/clock, independent channel ordering,
protection and driver/service accounting remain unverified. [Media RE](research-r3-media-re.md)
and the later R3 findings supply the source investigation; the earlier absence of a local audio
run is superseded by this observation.

Four thirty-second runs requested 720p/1080p at 30/60 fps and returned Annex-B-shaped video.
The later rate series includes two earlier 720p30 points and, in dispatch 6, completed ten-second
720p30/10,000-kbit/s and 720p60/2,000-kbit/s requests (58,884 and 85,997 bytes respectively).
These static-source observations do not prove decoded dimensions, codec/profile/depth, requested
target adherence, a supported bitrate envelope or independent hardware/driver accounting.
Dispatch 6's next 720p60/6,000-kbit/s candidate returned data but timed out before its final result
and two module-unload records; PID 209 remained sleeping. The operator recorded exact-PID SIGKILL
recovery in dispatch 6; raw recovery stdout was not retained. The retained postflight showed that
PID absent and development services reachable. No later candidate in that series ran.
Dispatch 7 adds host-tested reverse-order unload orchestration, sticky uncertain-attempt refusal,
flushed begin/completion records and selector-removal failure propagation. Ordinary and NDEBUG
tests, target builds, static analysis and independent review passed. The first `-O0` trace send
had no stdout or surviving probe and is invalid evidence; a separately built `-O2` one-second
720p60/6,000-kbit/s trace ran as PID 234, yielding 60 records / 21,311 bytes over 1,006,144 us.
All cleanup returned zero, with observed unload spans of 152 us for Avcap2, 193 us for
ResourceArbitrator and 85 us for Ipmi; kernel logs showed resource STOP/session close and exit
value 2. Two bounded ten-second repetitions (PIDs 238/239) produced 600 records each, respectively
78,400/81,903 bytes over 10,015,774/10,015,657 us, with ResourceArbitrator spans of 204/208 us.
Both completed zero-return cleanup, removed the selector and exited with value 2. The maximum
two consecutive repetitions was reached without recurrence; `service_call_deadline=unknown`.
The earlier
intermittent hang remains unexplained and individual platform-call completion remains unbounded.
See the [dispatch 7 trace](research-r3-findings.md#t041-dispatch-7--cleanup-hardening-and-one-second-trace-2026-10-31)
for artifact provenance and limits; T041 remains unchecked.
The later [dispatch 8](research-r3-findings.md#t041-dispatch-8--hevc10-configuration-header-observation-2026-10-31)
ran one HEVC10 header candidate as PID 254, producing 600 records / 528,559 bytes over
10,014,875 us and 103 bytes of VPS/SPS/PPS. Independent parsing establishes Main10,
ten-bit, cropped 1920x1080 and BT.2020/PQ configuration signaling. Header association with
the read records and decoded picture format were unverified in that header-only run. Cleanup returned zero,
the selector was absent and PID 254 exited with value 2; T041/T045 remain Incomplete.
Process removal and zero cleanup returns do not establish driver reclamation or full output/UI
restoration; earlier owner-reported persistent choppiness remains relevant.
[Dispatch 9](research-r3-findings.md#t041-dispatch-9--retained-hevc10-pictures-and-independent-decode-2026-10-31)
subsequently retained 600 exact VCL records / 542,607 bytes from PID 261 over 10,014,179 us,
with p99 read 16,618 us and zero-return cleanup. FFmpeg 8.0.1 strictly decodes all 600 frames
with the 103-byte configuration headers as 1920x1080 Main10 / `yuv420p10le`, limited-range
BT.2020/PQ signaling. Decoded Main10 and VCL/header compatibility are established for this
population; source HDR metadata, bounded HDR-to-SDR conversion, protection, hardware provenance,
supported bitrate/audio bounds, driver accounting and platform-call deadlines remain unverified.
Private raw video remains local evidence. T041/T045 remain Incomplete. The eight-bit display-ring
investigation still supplies no approved fallback.

Four distinct virtual-controller handles, neutral submissions/removals and one owner-corroborated
Right/Left D-pad sequence on 13.60 are established. A later hardened held-Right normal-stop run
accepted 594 reports, returned zero from neutral-before-remove cleanup, produced visible navigation
and a controller-disconnected toast, and left no payload process. The UI had reached the rightmost
boundary and capture exposure was not synchronized, so visible neutral release remains unproven.
These are positive limited observations, not full four-player assignment,
Create/PS/proportional/motion/touch coverage, reverse rumble or a validated SC-005 timing boundary.
Transport-disconnect/partial-failure/rest cleanup, Remote Play/Share Play transitions,
foreground-game scheduling and exact privileged restoration remain unverified. Stock SDK
credential/syscall-bound writes were owner-approved baseline; their reversal was not independently
established.

R4's own VideoOut rendering/HDR status is a source fixture, not foreign-title capture. A metadata
setter or dynamic-range flag supplies no independent mastering-display/MaxCLL/MaxFALL getter.

**Closure evidence**: Establish sourced permitted ABI/context and measured 720p/1080p 30/60 SDR,
1080p60 ten-bit HDR capture-to-hardware-bitstream, stereo audio, metadata epochs, bounded HDR-to-SDR,
protection boundaries and normal/error/partial-start cleanup. Complete controller delivery/feedback,
release/coexistence/rest and game-safe scheduling evidence. Retain raw results, timing populations,
bitstream provenance and all resource ownership. An unknown capability stays Incomplete; this
does not establish permanent hardware inability.
See [readiness blocker register](readiness.md#blocker-register) for downstream tasks and closures.

**Rationale**: These mandatory capabilities determine whether the specified host can exist within
console-safety/performance constraints. Successful loading, rendering and dependency smoke cannot
supply them. Any scope/design refinement requires a literal draft and explicit owner confirmation;
T045 applies no spec/contract amendment and invents no fallback.

### Reference, portable and management follow-up (T038/T039/T041/T042/T050/T056/T094/T096; INCOMPLETE)

The T038 runner now gives both independent observer bindings one shared ten-second publication
window after successful host cancellation. Only absence or EOF of the top-level manifest is
retryable; a complete foreign/invalid manifest or missing/truncated nested artifact is terminal.
Validation consumes the parsed manifest once and retains its already-hashed source bytes, so path
replacement cannot change the accepted evidence. Failed cancellation imports once without waiting.
All 214 Rust tests and an independent review passed. These synthetic synchronization cases do not
supply the missing calibrated SDR/HDR populations. A same-day read-only g913 audit found no live
session and only the EmuDeck application with `isHDRSupported:false`; that deployment cannot supply
the required HDR population. T038 remains Incomplete and T039 remains dependency-blocked.

A maintained [reference observer](../../tools/reference-observer/README.md) now provides bounded
read-only Wolf evdev acquisition, explicit READY/DONE coordination, conservative ordered diagnostic
matching and manifest-last atomic publication. Both raw and HostObservation bytes are checksum-bound;
16 parser/acquisition/publication scenarios and the actual Rust lifecycle/controller consumers pass,
and the fixture-only verifier is registered in portable CI. Evdev still cannot authenticate runner
session/event ownership, calibrate the Wolf mapping, prove full lifecycle cleanup or supply output,
audio, HDR and fidelity evidence. Every generated observation therefore remains unavailable with
`authenticated:false`; no live reference-host acquisition occurred and T038 stays Incomplete.

The controller consumer now joins the same bounded post-cancellation publication window. With both
observer bindings present, it selects the explicitly bound manifest artifact, consumes the lifecycle
loader's checksum-verified byte snapshot, requires matching provenance and rewrites only retained
artifact links. It never reopens or falls back to an unbound JSONL file. Delayed publication,
tampering, replacement-after-validation, quota, foreign-session, cancellation and standalone
compatibility regressions pass locally. These are synthetic integration checks; authenticated live
ownership, controller application and T038 acceptance remain absent.

An optional pre-launch readiness barrier now closes the corresponding start-boundary race. After
the paired host version is known, the runner publishes a quota-accounted binding containing the
run/session/scenario identifiers, streamed scenario digest, exact versions and a fresh nonce. When
configured, host launch cannot be constructed until a matching bounded regular-file READY envelope
has been validated and retained; stale, foreign, malformed, oversized, linked, cancelled, timed-out
or quota-exhausted exchanges finalize Incomplete with zero launch. Existing runs remain compatible
when the barrier is absent. The exchange coordinates but does not authenticate the external
observer or promote any criterion; live orchestration and ownership remain open.

The maintained collector now also accepts an optional inherited nonblocking finish pipe. After
READY, one bounded JSON line plus EOF must reproduce the entire runner binding. A valid request
takes a final inventory/read boundary and preserves any still-present devices before manifest-last
publication; malformed, foreign, premature, duplicate, timed-out or lost control cannot acknowledge
finish. The real subprocess test publishes within the runner window despite a 300-second watchdog,
and six retained populations pass the actual Rust lifecycle/controller consumers while every
unsupported criterion remains Incomplete. This removes the fixed-duration interface mismatch;
authenticated ownership and live SDR/HDR evidence remain open. See the
[finish-protocol report](../../.omo/evidence/t038-finish-protocol-20261031/REPORT.md).

The runner now publishes both `observer-binding.json` and `observer-cancel.json` with
native atomic no-replace rename, after file sync and checked close, eliminating the
temporary two-link/ctime race at first visibility. The trusted startup option
`--observer-finish-signal` requires `--observer-ready` and reserves an immutable cancellation
envelope containing the exact retained binding and `request_succeeded` before launch.
After bounded cancellation, true permits finish JSON plus EOF; false/error/timeout requires
EOF without finish and collector control error. Publication and both imports consume one
absolute deadline, with immediate import after failed cancellation. The runner bounds the
request at ten seconds. In exact deployed Fenrir commit
`5c8251c8a7a547466226c5f08f72d94a5de05dd0`,
[cancel](https://github.com/jefflouisma/homelab/blob/5c8251c8a7a547466226c5f08f72d94a5de05dd0/fenrir/pkg/moonlight/rest.go#L708-L716)
acknowledges foreground deletion requests without waiting for pod disappearance. Its separate
ninety-second wait belongs to launch-time prior-session cleanup, not cancel. Thus true permits
the final collector boundary but cannot establish host cleanup; later pod disappearance cannot
upgrade false, and later manifests are supplemental diagnostics. This supplies the runner
signal contract; authenticated ownership and live acceptance remain incomplete. See the
[runner signal report](../../.omo/evidence/t038-runner-cancel-signal-20261031/REPORT.md) and
[independent review](../../.omo/evidence/t038-runner-cancel-signal-code-review.md), plus the
[exact-source audit](../../.omo/evidence/t038-fenrir-cancel-provenance-20261031.md).

A [local supervisor](../../.omo/evidence/t038-local-supervisor-20261031/REPORT.md) now owns
the collector process, drains bounded logs, translates the exact cancellation signal and
validates the published manifest/source snapshots. Its uncertain-close correction detaches each
pipe descriptor before one close attempt and isolates cleanup faults so child reaping and final
reporting are still attempted; the [re-review](../../.omo/evidence/t038-local-supervisor-code-review.md)
approves that correction. The runner's [postlaunch barrier](../../.omo/evidence/t038-runner-attachment-20261031/REPORT.md)
checks attachment freshness before launch, reserves marker quota and keeps a UUID-v4 nonce private
until successful launch publishes `observer-launch.json`. Only an exact nonce-bound attachment
retained before RTSP/input opens the barrier; failures converge on cancellation. Its
[re-review](../../.omo/evidence/t038-runner-attachment-code-review.md) approves the causal boundary.

The [runtime attachment implementation](../../.omo/evidence/t038-runtime-attachment-20261031/REPORT.md)
separates prelaunch observer process identity from the supplied postlaunch Session → Deployment →
ReplicaSet → pod ownership chain, container identities and source hashes. Exact deployed Fenrir
`5c8251c` [retains](https://github.com/jefflouisma/homelab/blob/5c8251c8a7a547466226c5f08f72d94a5de05dd0/fenrir/pkg/controllers/agent_input.go#L14-L22)
`session_id`, `path`, `major`, `minor` and `generation` in its agent device API;
the contrary local-checkout finding does not describe that revision. Attachment mode uses only
those exact supplied device paths and descriptor identities. An empty initial device inventory
allows RTSP/controller arrival to create pads without a startup cycle. The
[runtime re-review](../../.omo/evidence/t038-runtime-attachment-code-review.md) approves that slice.
The subsequent approved [one-shot device feed](../../.omo/evidence/t038-late-device-feed-20261031/REPORT.md)
binds the exact runtime/launch identity and opens only supplied exact device paths. It emits
`late_attach` before later decoded events, retaining early-event coverage as unavailable and
continuity as unverified. Finish without a feed remains bounded and does not hang; see the
[feed review](../../.omo/evidence/t038-late-device-feed-code-review.md).
The approved [Fenrir provider](../../.omo/evidence/t038-fenrir-provider-20261031/REPORT.md)
and [review](../../.omo/evidence/t038-fenrir-provider-code-review.md) now implement the
prelaunch Session baseline, exact new owner-chain selection, empty-device attachment and
post-acknowledgement one-shot feed. The corrected proxy route resolves the selected wolf-agent
container's declared port to an explicit numeric HTTPS endpoint and rechecks the pod/runtime
around acquisition. Its frozen source SHA-256 is
`17d9d47ea5f3682e89b629d4930393295cfde292ca980b4f7bb74edf81e15f7f`;
the final retained verifier has 109 Python tests and 19 exit-zero stages.
The approved [SSH subsystem relay](../../.omo/evidence/t038-remote-relay-20261031/REPORT.md)
and [review](../../.omo/evidence/t038-remote-relay-code-review.md) implement pinned-host-key,
source-digest and exact binding/nonce validation across hosts, immutable bounded artifact
transfer and owned-process cleanup. That historical relay revision's SHA-256 was
`e8e3ca77fb33744029da3d92be38efeb25d2eb3e2b074643e30b221863cf5be5`;
23 retained stages, including 125 Python tests and actual Rust consumers, passed with synthetic
transport. Successful receipts establish owner-process reaping only; lost transport leaves
remote cleanup unconfirmed. These are implemented and fixture-tested software, not live acceptance.

A follow-up process-group audit corrected four paths that could signal a numeric PGID after the
direct child was already known to have exited: the Fenrir provider, supervisor, cross-host relay
and shared interactive `ProcessTransport`. Each now checks direct-child liveness before every
signal, stops escalation after observed exit and records descendant cleanup as unconfirmed.
Real-subprocess regressions cover exit 0/nonzero, graceful TERM/SIGINT and forced fallback; the
independent bundle review found no remaining HIGH/MEDIUM production defect. This is host-side
cleanup hardening only and supplies no console input-release or reference-host acceptance.

The approved [server deployment gate](../../.omo/evidence/t038-deployment-preflight-20261101/REPORT.md)
and [review](../../.omo/evidence/t038-deployment-preflight-code-review.md) now bind a direct sshd
subsystem to the configured non-root account, root-owned canonical source tree and exact source
hashes, imported module paths, literal Python/kubectl executables, private session root and
kubeconfig. It rejects bytecode/import shadows, ForceCommand ambiguity and client overrides of the
server root/provider. Fifteen focused tests pass. The current combined verifier, after the
process-group ownership corrections, atomic READY fixture repair and relay exit-scan correction,
passed 151 Python tests and all 23
retained stages; its source manifest also binds the deployment wrapper. The gate creates no
account, key, subsystem, executable installation, kubeconfig or RBAC and has not run on
Linux/g913, so the live deployment gap remains.

The [read-only g913 audit](../../.omo/evidence/t038-g913-relay-live-audit-20261031.md)
at `2026-11-01T00:26:43Z` found current user `jeff`'s mode-0600 kubeconfig selects verified
HTTPS at `127.0.0.1:6443` without insecure/proxy settings; all required read/proxy `auth can-i`
checks returned yes and zero Sessions existed. It found no standalone kubectl on PATH, no
provider/relay modules in the bounded home search, no relay sshd subsystem and no restricted
relay credential/account; the current authorized key is plain. Dedicated account/host-key/code
deployment, private remote root and account-specific RBAC still need validation. A regular
fixed-path `k3s-kubectl.sh` wrapper is now fixture-tested, but its installation and validation
under the dedicated account remain outstanding, including `/bin/sh`, delegated
`/usr/local/bin/k3s`, parent integrity and live compatibility. The self-signed backend proxy,
populated runtime, authenticated agent/event
ownership, subscription continuity, independent output/audio/HDR/fidelity/cleanup and official-client
cross-checks remain open. All acceptance observations remain unavailable/unauthenticated.

T039's portable workflow now discovers and executes every `tests/unit/test_*.py` suite, explicitly
requires the schema and release-evidence suites, and removed a stale continue-on-error placeholder
after two fresh build trees contained no pending-core tests. That run covered 186 Python tests;
the later ENet audit regressions bring the current suite to 205.
The implementation is reviewed, but its T038 predecessor is still open.

T041's export checks remain active under `NDEBUG`, and the existing bounded retained-picture path
now builds six local candidates: AVC 720p30/60 and 1080p30/60 plus HEVC eight/ten-bit 1080p60. A
single-read verifier strictly checks exact transcript bytes, cleanup/export totals, independent
codec/depth/geometry/timing and decode, including the historical 600-frame Main10 population and
path-mutation regressions. No new AVC/HEVC8 console acquisition occurred, so the missing other-mode
device evidence and hardware provenance remain open.

T042's `NEUTRAL_OBSERVE` diagnostic passed ordinary, optimized and sanitizer checks. Its initial
device attempt was NOT RUN while PID 134 / `PPSA08329` remained active. A later separately reviewed
one-shot helper closed exact AppId 24, and independent postflight established no active game.
The diagnostic then ran once and completed neutral submission, a three-second registered-handle
dwell and removal with zero cleanup platform-call returns. Settings launch failed with
`0x80940031`, and an exact-PID TERM request did not interrupt the payload (`stop_requested=0`).
The 45-second HDMI recording does not establish Down/dwell/removal coverage; those phases likely
followed its end, and no exact host-to-payload clock bridge was retained. Visible neutral release
therefore remains unproven and the later device record is Incomplete. The held-Right observation
also remains limited by its rightmost UI boundary.

A separate explicit-command interactive helper now accepts only sequenced neutral/stop and bounded
up/right/down/left/cross/circle pulses over the existing loader stream. It submits complete neutral
before every acknowledgement and attempts neutral, removal, HID close and UserService termination
on every tested terminal path. Normal, NDEBUG, ASan/UBSan, strict target, CMake/CTest and independent
review/gate checks pass. It has not run on the PS5; the first required device sequence remains
READY → neutral → stop, followed by visually synchronized one-action observations. T042 stays
Incomplete.

A device-disabled host coordinator now makes that first neutral/stop sequence event-driven instead
of a fixed capture window. It durably creates a no-follow run directory and command intents, requires
a decodable pre-dispatch segment, accepts only the exact READY/neutral ACK/DONE grammar, sends only
neutral then stop, requires zero-exit/EOF and independent process/service postflight, and retains
contiguous capture through a completed postflight segment before publishing a manifest last. Fifty
offline cases and review pass. The subsequent [command adapters](../../.omo/evidence/t042-command-adapters-20261031/REPORT.md)
provide opt-in continuous FFmpeg PPM acquisition, independent segment decoding, exact deployment
arguments and four configurable read-only postflight commands. Complete frames and receipt metadata
are retained before decoding and on failure; nested and outer manifests bind partial footage and
the coordinator outcome. The [re-review](../../.omo/evidence/t042-command-adapters-code-review.md)
confirms the failure-retention correction. Host frame-receipt continuity does not establish source
timestamp continuity or visible release. Live inspection commands, camera/TCC permission and loader
transport remain unverified; protocol cleanup returns do not complete T042.

T050 exercises the real POSIX replacement path at partial-write, file-sync, consumed-close, rename
and directory-sync failures, plus interrupted/short-write success. After two failed initial runs,
codex3 isolated the installed SDK's error convention without creating state: raw `openat`/`fstatat`
returned RAX=2 with carry set, but the SDK wrappers exposed positive 2 with unchanged errno.
Project-local target wrappers now preserve carry and translate errors into -1/thread-local errno
for `openat`, `fstatat`, `renameat` and `unlinkat`; host libc behavior is unchanged.

Codex4/5 reached working production storage but stopped at probe-only symlink/hard-link fixtures
that returned ENOTSUP/45 on this `/data` filesystem. Their exact partial trees were independently
reviewed and cleaned. Codex6 explicitly recorded those unavailable target cases after secure
restoration/reload checks, while ordinary host symlink/multilink rejection tests remained active.
Its complete init and separate-process verify retained certificate fingerprint
`0e9a7fabca91159c082543437873fba9089670950c3cdf7e689f516355c32e9c`, paired authorization
and settings; verify performed a second atomic replacement and reopened with `metrics=0 pass=1`.
The reviewed complete cleanup left only `/data/ps5-dev/services`, with all four services listening.
The subsequent exact-game closure cleared the prior active-game constraint. A separate fresh init
now exists at `/data/ps5-dev/t050-20261031-codex6`; its reviewed verifier is bound to fingerprint
`c25403d792a58565794dcd9ced9c12ef8b2e862e6f4c20e430d8b51e3c5f47b9` and has not run. One suspend
request was followed by loss of HDMI and all five listeners. A later read-only Remote Play discovery
response identified `Server Standby` on firmware `13600007`; two bounded WOL attempts did not
restore connectivity. Resume and persistence inspection remain pending; the fresh
population must not be reinitialized before inspection. Reboot and power-loss evidence remain
absent. See the [rest attempt](../../.omo/evidence/t050-rest-device-20261031/rest-attempt.md).
The retained results establish process-level continuity and a pending durability population, not
the remaining durability obligations; T049/T050 stay unchecked. Host normal/NDEBUG checks and all
59 C unit tests passed.

An inspection-only recovery coordinator now pins this exact console/root/fingerprint and both
reviewed one-shot artifacts. It strictly rejects Standby, boot, stale/foreign discovery, incomplete
or unsafe root inventories and missing protocol/service/process/isolation/HDMI evidence. Its injected
dispatch API fsyncs a no-follow journal directory and exclusive intent before any transport call;
every attempt, timeout, interruption or malformed result is permanently non-retryable. Verification
and cleanup require separate arming, exact output/exit evidence, fresh inventories, the original
cleanup artifact, root 404 and service/HDMI postflight. Fifty-three offline cases and review pass.
The coordinator CLI cannot dispatch and only reports inspection complete/unarmed. The approved
[recovery adapter](../../.omo/evidence/t050-live-adapter-20261031/REPORT.md) and
[review](../../.omo/evidence/t050-live-adapter-code-review.md) now add fixed-endpoint discovery
and bounded exact-ELF transfer, fresh nonce-bound evidence retention, mandatory independent exit
observation and separate postverify/cleanup arming. The descriptor-close correction is included
in source SHA-256 `b2f85da027bba93f1c0a1d282beed61fd2686a11ddbe256c386d3fab42671143`;
188 offline tests passed. The approved
[conservative acquisition components](../../.omo/evidence/t050-live-callbacks-20261101/REPORT.md)
add bounded diagnostic web/RSP/klog/shell and host-route acquisition plus an HDMI callback that
requires a digest-bound reviewed classifier; 142 focused tests pass. No classifier is shipped,
service observations remain explicitly diagnostic, and every non-HDMI callback refuses. The
approved [read-only inspector core](../../.omo/evidence/t050-recovery-inspector-20261101/REPORT.md)
adds bounded no-follow boot-time and inventory acquisition with ancestor/root revalidation.
Its [launcher increment](../../.omo/evidence/t050-recovery-launcher-20261101/REPORT.md) now links
the pinned full SDK CRT and gates observation on exact firmware `0x13600007`; host regressions,
target ELF inspection and independent code review pass. The CRT performs process-local
credential/syscall bootstrap before the gate. The [exact-loader audit](../../.omo/evidence/t050-relapse-loader-compat-20261101/REPORT.md)
establishes distribution provenance and a byte-identical SDK v0.45 full-CRT verifier that
historically executed through this loader on the same 13.60 console. Five CRT functions match
the inspector after masking only pinned object relocation fields, supporting the historically
exercised full-CRT ABI class. Inspector-specific behavior for 23 GLOB_DAT imports and one
R_X86_64_64 relocation, current-boot loader continuity, inspector execution/output and independent
exit remain unverified; exact loader build provenance remains unresolved. The observer's firmware-validation/atomic-snapshot claims
remain false and process, game and writer claims remain null. A reviewed public-certificate
fingerprint path and independent process-exit evidence are still missing. All seven live
callbacks remain unbound:
services, boot, process, isolation, HDMI, inventory and exit observer. A genuine pre-rest boot
identity is additionally missing; retained numeric service PIDs 103–106 can recur after reboot
and cannot prove continuity. There is no standalone dispatch CLI or autonomous recovery
readiness. The latest rest-attempt HDMI at `2026-11-01T00:31:59Z` again shows NO SIGNAL;
[fresh discovery](../../.omo/evidence/t050-rest-device-20261031/discovery-20261101T011503Z.log)
at `2026-11-01T01:15:03Z` returned `HTTP/1.1 620 Server Standby` for host
`00E421FDEC6B` on firmware `13600007`, with exploit/development-service ports absent. A read-only
[vphone check](../../.omo/evidence/t050-vphone-wake-recheck-20261101/REPORT.md) found no installed
PS Remote Play or Chiaki client and established no registered wake route. No verifier,
cleanup or inspector payload was dispatched.

Dependency follow-up removed both recorded client advisory dependencies: `opus` 0.4.0 now uses
`opusic-sys` 0.7.6 with bundled libopus 1.6.1, and AWS-LC (`aws-lc-rs` 1.19.0 /
`aws-lc-sys` 0.47.0) replaces `rsa` while retaining legacy PKCS#8 identity files and identical
SHA-256/PKCS#1 v1.5 signatures. The ENet auditor accepts only the exact commit-addressed fork
identity without inventing a release tag. Independent reviews passed; 217 Rust tests and an
OpenSSL signature cross-check passed. The current live audit completed 8 host and 238 client
queries with zero advisory matches and verified locked archives. Hosted CI, live pairing after
the backend change, installed compiler provenance and production/release acceptance remain open;
database coverage is not universal security proof. Licensing remains excluded.

The frozen-host official Moonlight Qt 6.1.0 rerun binds both launches to one retained executable
hash. Wrong-PIN rejection, fresh pairing through HTTPS challenge, the pre-restart app list,
rejected-identity refusal and unauthenticated app-list rejection passed. The first post-restart
process printed PS5 and then aborted in Qt's OpenSSL backend while converting its local
`QSslDiffieHellmanParameters`; one fresh-process repeat passed. The retained outcome is 5/6 and
FAIL. Investigation found no server-supplied custom-DH path and no justified host patch, but it
does not resolve the intermittent client lifecycle/environment failure. Console notification,
storage/restart and timing evidence also remain absent; T056 stays unchecked.

A later official-client diagnosis found that Moonlight Qt 6.1.0's CLI list path leaves an
unparented `ComputerManager` undeleted while its delayed-flush and polling threads remain alive at
libc exit. This is a concrete upstream lifetime defect consistent with an exit race, but the retained
debugger run did not reproduce the TLS fatal or capture its failed peer/error queue. Five sandboxed
offline unpaired runs completed normally under conditions that blocked the background public HTTPS
traffic implicated in the hypothesis. The original 5/6 FAIL is preserved; no SunshinePS5 TLS change,
unconditional retry or patched-client acceptance is justified.

A later isolated GUI diagnostic avoided that CLI-only ownership path. Within the live tool session,
official Moonlight 6.1.0 paired with the frozen fake host, displayed the single PS5 application,
closed normally, and after a clean process restart reused the portable client identity and displayed
the application without a new PIN. Raw UI/action/host transcripts were not retained, so this remains
a session-observed diagnostic rather than a standalone passing evidence case. The original CLI FAIL
is immutable, and the GUI diagnostic supplies neither console notification, on-device durability/
timing nor stream evidence. Its report also discloses bundle-level UI target ambiguity: the first
pairing was global-likely, the pre-existing global GUI was restored, and removal of its added test-host
entry awaits explicit owner confirmation. T056 remains unchecked.

T094 now retains cleanup retry ownership after durable revocation removal, preserves the first
session-end reason and admission denial, and exposes a bounded `cleanup_pending_revocations` API/UI
row. A real C management fixture and browser reload/cancel/confirmed-retry journey pass 12/12;
hostile names remain text, the other paired phone survives, and completed cleanup is not repeated.
This is portable lifecycle/browser behavior, not the supported-hardware two-second observation.

T096's fake web transport queues and exposes the complete stopping acknowledgement before
cleanup, retains ENDING state and resource ownership after an injected audio-close failure, retries
cleanup, and marks stopped only after success. Socketpair regression coverage also proves
input-before-media ordering and that completed cleanup is not repeated. This is portable fake-path
behavior, not a production adapter or active-console-session observation; T094/T096 and their
hardware evidence remain unchecked. The retained reports are
[T038 observer synchronization](../../.omo/evidence/t038-observer-sync-20261031/REPORT.md),
[T038 maintained reference observer](../../.omo/evidence/t038-reference-observer-20261031/REPORT.md),
[T038 manifest-backed controller integration](../../.omo/evidence/t038-controller-manifest-20261031/REPORT.md),
[T038 pre-launch readiness barrier](../../.omo/evidence/t038-readiness-barrier-20261031/REPORT.md),
[T038 collector READY publisher](../../.omo/evidence/t038-ready-publisher-20261031/REPORT.md),
[T038 collector finish protocol](../../.omo/evidence/t038-finish-protocol-20261031/REPORT.md),
[T038 runner cancellation signal](../../.omo/evidence/t038-runner-cancel-signal-20261031/REPORT.md),
[T038 local supervisor](../../.omo/evidence/t038-local-supervisor-20261031/REPORT.md),
[T038 postlaunch barrier](../../.omo/evidence/t038-runner-attachment-20261031/REPORT.md),
[T038 runtime attachment](../../.omo/evidence/t038-runtime-attachment-20261031/REPORT.md),
[T038 one-shot late device feed](../../.omo/evidence/t038-late-device-feed-20261031/REPORT.md),
[T039 Python CI coverage](../../.omo/evidence/python-release-ci-20261031/REPORT.md),
[T041 export matrix](../../.omo/evidence/t041-export-matrix-20261031/REPORT.md),
[T042 active-game closure](../../.omo/evidence/t042-close-active-game-device-20261031/REPORT.md),
[T042 incomplete neutral-observation run](../../.omo/evidence/t042-neutral-observe-device-20261031/REPORT.md),
[T042 interactive helper](../../.omo/evidence/t042-interactive-helper-20261031/REPORT.md),
[T042 neutral/stop coordinator](../../.omo/evidence/t042-interactive-driver-20261031/REPORT.md),
[T042 command adapters and partial capture retention](../../.omo/evidence/t042-command-adapters-20261031/REPORT.md),
[T050 POSIX failure hardening](../../.omo/evidence/t050-posix-failure-hardening-20261031/REPORT.md),
[T050 device sequence](../../.omo/evidence/t050-storage-device-20261031/REPORT.md),
[T050 guarded recovery coordinator](../../.omo/evidence/t050-recovery-coordinator-20261031/REPORT.md),
[Opus remediation](../../.omo/evidence/opus-advisory-remediation-20261031/REPORT.md),
[ENet audit correction](../../.omo/evidence/enet-audit-version-20261031/REPORT.md),
[RSA remediation and current audit](../../.omo/evidence/rsa-backend-remediation-20261031/REPORT.md),
[T056 frozen official-client rerun](../../.omo/evidence/t056-official-client-portable-rerun-20261031/REPORT.md),
[T056 official-client lifetime diagnosis](../../.omo/evidence/t056-qt-abort-diagnosis-20261031/REPORT.md),
[T056 official-client GUI diagnostic](../../.omo/evidence/t056-official-client-gui-20261031/REPORT.md),
[T094 revocation cleanup retry](../../.omo/evidence/t094-revoke-retry-20261031/REPORT.md),
and [T096 fake stop retry](../../.omo/evidence/t096-fake-stop-retry-20261031/REPORT.md).

## R4. HDR reference capture and metadata oracle (ACQUISITION/CALIBRATION RESEARCH COMPLETE)

**Decision**: Retain the implemented external privileged helper based on elgato-hdr commit
`7799d3c53520bcc3dd049211244bfaf49b905cb0`. Rust remains unprivileged. Normal/cancellation rollback
and independent UVC/audio restoration are evidenced. For FR-025, independently obtained console
source metadata before host serialization is still required; helper defaults cannot be the oracle.

**Acquisition**: [T032 report](research-r4-capture.md) supersedes the initial descriptor-only
inventory. Actual Elgato 4K X **0fd9:009c**, **5 Gbit/s**, P010 format 7/frame 5, 1920×1080
nominal 60 fps samples were acquired, including a fresh owner-confirmed HDR output epoch.
The known-HDR 120-frame run retains five startup rejects/125 observed boundaries; complete fps
is 57.588, not loss-free 60. Ten-bit sample packing and shared producer/consumer monotonic clock
were checked. Earlier provisional HDR epochs were invalidated and cannot be reused as authority.
AVFoundation remains eight-bit; no 3840×2160 P010 descriptor was advertised at this link speed.
The known T032 HDMI epoch was 2160p60 RGB HDR scaled/converted by the device to 1080p P010;
it is not native untouched 1080p output or independent mastering/light metadata evidence.

**Calibration**: [T033/T034 report](research-r4-calibration.md) records native still/motion source
error: 233 SDR frames scored and 466/480 HDR frames matched, with 14 startup exclusions retained.
SDR still/motion median Y SSIM is 0.986024190/0.984230476; HDR population medians span
0.988515247–0.990514306. Paired submit-to-reference causal bounds are run-specific, not exact
HDMI exposure corrections: SDR motion median lower/upper 73.840/1453.456 ms; HDR repeat
48.033/80.800 ms. The conversion oracle is a declared software interpretation, not an independently
measured HDMI raster/EOTF. Exact exposure, independent mastering/light metadata and future
mode/platform coverage remain unmeasured. Controlled source scenes do not establish stream/game
or 99%-whole-run release acceptance; exclusions and wide SDR bounds remain explicit.

**Owner dispositions, 2026-10-25**: Physical-disconnect recovery is **owner-deferred, untested,
non-blocking** for T032 after attempts failed to correlate removal with active capture. T032/T034
are checked. Approved/applied SC-014 adds full-population median luma SSIM >=0.95. SC-013 adds
median >=0.97, joint 99% SSIM >=0.95 and Y/Cb/Cr PSNR >=35/40/40 dB (perfect planes satisfy
floors), plus native ramp/patch rules: >=512 integer column-median levels, backward step <=4 codes,
neutral/patch median error <=8 codes and zero interior clipping after an 8-code endpoint guard.
All prior visual/platform/population requirements remain. These are already approved refinements,
not new T045 amendments; see [implementation decisions](contracts/implementation-decisions.md).

**Remaining acceptance obligations**: Use valid per-run/mode calibration and coverage, independently
source HDR metadata under R3, and settle FR-057 D5 wording before acceptance. Missing required
frames, invalid format or uncertain threshold crossings remain Incomplete. The disconnect deferral
is limited to T032; it waives no console or release requirement. No replacement capture path is
selected by this reconciliation.

## R5. Latency and resource feasibility (ERROR / INCOMPLETE)

**Decision**: Keep two separate timings: protocol-reported processing latency for FR-021/SC-003 and
capture-start → last-packet-handoff time for the constitution's complete pipeline constraint.
The pinned Sunshine implementation fills the frame header before packetization/send completes;
its 0.1 ms field cannot report a future final handoff. Zero means absent/repeated-frame timing, not a
measured zero-duration success. Preserve provenance and validity when evaluating quantized samples.

**Evidence**: [protocol-notes §6](../../tools/moonlight-mcp/docs/protocol-notes.md#6-per-frame-header-sunshine-short-frame-header)
and [Sunshine stream.cpp:1588–1609](https://github.com/LizardByte/Sunshine/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/stream.cpp#L1588)
establish the header semantics. A receiver reads host-reported values; this alone cannot detect a host
lying about its own timestamps. Independent HDMI/stream alignment measures another quantity, subject
to capture delay and uncertainty. These measurements must not be substituted for one another.

**Provisional budget**: **256 MiB total host memory**: 128 MiB media pools, 80 MiB metrics, 48 MiB core,
crypto, stacks, and management; **one core-equivalent of aggregate host CPU**. These are proposed limits,
not measured consumption or proven available resources. Record driver/encoder-owned allocations and
unknown accounting explicitly. On-device game-impact and sustained-run measurements remain required.

**T044 evidence**: [R5 report](research-r5-budgets.md) retains the bounded console sleep-only
diagnostic and subsequent sleep/active clock characterization. Its 65,536-byte requested heap
excludes allocator/runtime/stacks/media/driver domains. The elapsed-like process clock remains
rejected for aggregate CPU accounting. A later
[two-thread PS5 observation](../../.omo/evidence/t044-two-thread-device-20261031/result.md)
establishes concurrent thread-local CPU distinction for two owned workers: active CPU was
248,383,000 ns / 248,660,000 ns, sleep-like CPU 1,080,000 ns / 1,085,000 ns over comparable
250-ms intervals, and the distinction followed swapped roles. All four workers completed and
joined. Their sums span separate observed intervals. The subsequent
[shared-window diagnostic](../../.omo/evidence/t044-common-window-device-20261031/REPORT.md)
uses one coordinator-defined ten-second target and records 10,001,177,603 ns of guaranteed common
coverage, 2,726 ns begin skew and 89,567 ns end skew. Its enclosing two-worker CPU sum of
10,008,296,000 ns has a 2,451,508-ns summed outside-window wall bound and is not an exact
common-window CPU total or production host budget. The unchanged idle ceiling is
0.5 CPU-seconds per ten-second window (5% core); streaming ceiling is 10
CPU-seconds per such window. No production thread census, ten-second host workload, daemon/driver
CPU accounting, measured mode/bitrate endpoints, real media/input timing, five-game comparison,
metrics-overhead population or soak exists. T044/T045 remain unchecked and readiness stays ERROR.

R4's successful <=32 MiB direct-memory requests and failed 64 MiB request are process-specific;
they neither prove the 256 MiB design feasible nor prove it impossible. Unknown accounting remains
unknown. Obtain sourced resource ownership, validated CPU clocks, real stage/input boundaries and
matched approved game workloads before measurement. [Readiness](readiness.md) maps each gap to
production-host and release tasks. No ceiling has been revised or approved by the diagnostic.

**Rationale**: SC-003/004/005/006/008/009/012 and the constitution require evidence with different
endpoints and durations. Buffer bounds, sample retention, percentile coverage, metrics-off behavior,
and the host-timing evidence channel are specified once in
[measurement-evidence.md](contracts/measurement-evidence.md), not redefined here.

**Alternatives**: Treating overlay latency as full pipeline time hides send cost; keeping only a rolling
window cannot prove whole-session percentiles. If measurements invalidate a target/budget, revise the
design or explicitly amend the applicable requirement; do not silently relax it.

## R6. Test environment (DECIDED; EACH RUN MUST VERIFY)

**Decision**: Use the direct Mac–console link for development; acceptance uses private home-network
connectivity with a static console address and no usable internet gateway. Verify outbound isolation,
including IPv6 routes, before each hardware run. Routed private clients are permitted under R9.

**Rationale**: Official Apple TV/mobile clients need reachable local services without console internet
access. IPv4 gateway configuration alone does not establish IPv6 isolation.

**Alternatives**: Restricting all clients to the development cable cannot exercise the supported-client
matrix; assuming the home router blocks internet access is not evidence. No topology was changed here.

## R7. Web app delivery (PROVISIONAL DESIGN)

**Decision**: Serve static HTML/CSS/JavaScript and a small JSON API from the host; use one-second polling
while the page is visible for pairing, session state, and metrics. No console-side frontend build,
external assets, or runtime framework. Follow [web-app-api.md](contracts/web-app-api.md) for API details.

**Rationale**: Polling meets the one-second update requirement with bounded work and is simple to
verify on the 360-pixel phone layout. The accepted no-login risk is unchanged; access still uses R9.

**Alternatives**: Push transport adds connection/lifecycle work without an established requirement;
a frontend framework is optional development machinery, not needed for this small management surface.

## R8. Independent reference host (DECIDED; VALIDATION OUTSTANDING)

**Decision**: Use the existing Wolf/Direwolf installation as the first reference host and official
Moonlight clients as an independent cross-check. Record exact host/client versions and successful
pairing, streaming, input, reporting, and teardown before FR-059 acceptance.

**2026-10-25 status**: [Validation](../../tools/moonlight-mcp/docs/validation.md) retains exact
Wolf/Fenrir image digests and official Moonlight Qt 6.1.0 cross-checks. Supplemental live matrix:
REF-pairing, REF-stream, REF-controller (Wolf evdev boundary) and REF-teardown Pass;
REF-recording/REF-report Incomplete. The latest immutable runner report remains overall Incomplete,
exit 2, with REF-stream Pass and five Incomplete criteria. Supplemental observations do not change
it. Its 595 native SDR frames and 1,995 decoded stereo packets include digitally silent audio,
not non-silent game sound. Host timing/drops/encoder-selected bitrate are unavailable. Independent
host-output recording, calibrated native fidelity/HDR and complete evidence integration are missing.
T038/T039 stay unchecked; no SunshinePS5 acceptance is authorized. Recorded dependency-audit findings
also remain unresolved; T045 did not rerun the audit.

**Rationale**: A client tested only against SunshinePS5 can share its defects. Live transport/input
success cannot close the declared recording and complete-report criteria.

**Alternatives**: Sunshine can supply additional reference cases where Wolf differs. A passing fake
backend run establishes CI behavior, not console capability or official-client compatibility.

## R9. Routed private-network boundary (ACCEPTED CLARIFICATION)

**Decision**: Accept reachable private IPv4 sources in `10/8`, `172.16/12`, `192.168/16`, IPv6 ULA
`fc00::/7`, and on-link `fe80::/10`; reject other source ranges. Do not require a matching local subnet.
Classify the actual transport peer, including canonicalized IPv4-mapped addresses, not HTTP headers.
Automatic discovery across routers is unnecessary; manual host entry remains available (FR-040).

**Rationale**: The owner chose all reachable private home-network segments while retaining pairing and
single-session restrictions. A private address is an access boundary, not an authenticated owner.

**Alternatives**: Same-subnet-only filtering contradicts the accepted answer; public reachability and
address-derived trust exemptions are outside the requirement.

## R10. Evidence completeness and result precedence (ACCEPTED CLARIFICATION)

**Decision**: Every declared required criterion has Pass, Fail, or Incomplete. Missing/unreliable
required evidence makes the overall run Incomplete, preserves any demonstrated per-criterion Fail,
and returns a non-success automation outcome. Pass requires valid passing evidence for every required
criterion; unavailable evidence cannot make a required criterion inapplicable (FR-056).

**Rationale**: The owner chose incomplete evidence to block acceptance. See
[measurement-evidence.md](contracts/measurement-evidence.md) for validity and aggregation rules.

**Alternatives**: Omitting unknown measurements or treating an incomplete run as a pass would conceal
unverified requirements. A known failure must remain visible even when another criterion is incomplete.

## R11. Bitrate clamping and transparency (ACCEPTED CLARIFICATION)

**Decision**: Clamp an otherwise supported request outside the measured supported bitrate range to
the nearest boundary. Retain requested/chosen values and adjustment reason in session state, web UI,
and report. The actual supported bounds require R3 evidence; do not invent them during planning.

**Rationale**: This preserves the owner's requested session while making the negotiated compromise
visible. Encoder bitrate and network throughput remain distinct reported quantities.

**Alternatives**: Rejecting solely for an out-of-range bitrate contradicts the answer; silently changing
bitrate loses evidence and prevents the owner from understanding negotiated behavior.

## R12. Three-tap counting (ACCEPTED CLARIFICATION)

**Decision**: Count navigation, submission, and confirmation actions from the web home screen; exclude
typing and keyboard dismissal. An action submission still counts when it also dismisses the keyboard.
PIN entry, revoke-with-confirmation, and metrics toggle must each fit three taps (SC-011).

**Rationale**: The accepted rule permits text entry without concealing navigation or required safety
confirmation costs. Verify actual phone flows at 360 pixels, including focus and visible controls.

**Alternatives**: Counting keystrokes makes the target depend on text length; excluding confirmations
misrepresents the completed flow. Removing a required confirmation is not a valid shortcut.

## R13. Host toolchain and dependency baseline (T043 SMOKE PASSED; PRODUCTION VALIDATION OPEN)

**Decision**: Retain C11, public SDK v0.45 source `a7685ce8e97ce1f8a4f38253a41d54241c9124f1`,
LLVM 18.1.8 and OpenSSL 3.5.9 source `45e844fa2a14ec92d146bd8f5778ac130b6625fb`.
[T043](research-target-dependencies.md) cross-build/link and bounded 13.60 OS-entropy,
RSA/certificate, trusted/rejected mutual-TLS, TCP and pthread smoke passed. Select host ENet
**1.3.17 Moonlight fork**, `cgutman/enet` commit `aca87840b57f045a1f7f9299e4b1b9b8e2a5e2f1`;
real target UDP exchange with rusty_enet 0.4.0 passed channels/reliability/fragmentation/disconnect.
The report and dependency lock retain exact archive pins, patches, commands and evidence hashes.

**Evidence limits**: This is dependency feasibility, not production readiness. Target IPv6
transport, full security/session conformance, allocation costs and release/toolchain reproducibility
remain open. RAND API success is not a physical entropy-strength measurement. Setup recorded checked
SDK/dependency archives, but installed LLVM binary archive provenance remains unresolved; a version
string and source digest cannot establish the installed compiler's binary provenance.

**Rationale**: C11 fits the SDK without an additional runtime. The smoke supports OpenSSL/ENet
integration work while preserving the target resource, security and complete compatibility gates.

**Locked test baseline**: Rust 1.93.0 / edition 2021 (manifest minimum 1.85); tokio 1.49.0,
rustls 0.23.36, reqwest 0.12.28, rusty_enet 0.4.0, opus 0.4.0, opusic-sys 0.7.6
(bundled libopus 1.6.1), ring 0.17.14, rcgen 0.13.2, aws-lc-rs 1.19.0,
aws-lc-sys 0.47.0, aes 0.8.4, aes-gcm 0.10.3, cbc 0.1.2, sha2 0.10.9,
serde 1.0.228, serde_json 1.0.149, clap 4.5.54. Cargo.lock is authoritative for the complete graph.
Observed native tools: FFmpeg/ffprobe 8.0.1, system libopus 1.5.2 (distinct from the bundled
client library), libusb 1.0.29, Apple Swift 6.4.

**Alternatives**: C++17 remains available if a concrete API requires it, with its runtime cost validated.
Other maintained crypto libraries need the same primitive/API and cross-build checks; handwritten
cryptographic primitives are excluded by the constitution.

## R14. Source inventory and read-only reproduction (EVIDENCE SCOPE)

**Decision**: Keep exact upstream pins and local inventory commands separate from acceptance procedures.
Initial inventory observations were made on 2026-10-24; the linked research reports record
2026-10-25 updates. Repeat inventory after device/link/toolchain changes.

| Source | Pin / purpose |
|---|---|
| [Protocol notes](../../tools/moonlight-mcp/docs/protocol-notes.md) | Full Moonlight, Sunshine, ENet and nanors source pins; protocol facts |
| [SDK source](https://github.com/ps5-payload-dev/sdk/tree/a7685ce8e97ce1f8a4f38253a41d54241c9124f1) | v0.45; SDK/import candidates, not verified capture ABI |
| [OpenSSL source](https://github.com/openssl/openssl/tree/45e844fa2a14ec92d146bd8f5778ac130b6625fb) / [releases](https://openssl-library.org/source/) | 3.5.9; bounded target smoke passed under T043 |
| [elgato-hdr source](https://github.com/talkaboutdesign/elgato-hdr/tree/7799d3c53520bcc3dd049211244bfaf49b905cb0) | Direct-USB helper, P010 framing, metadata limitations |
| [Manifest](../../tools/moonlight-mcp/Cargo.toml) / [lockfile](../../tools/moonlight-mcp/Cargo.lock) | Existing dependency/lint baseline; not an acceptance result |

**Rationale**: Exact versions and commands let another reviewer reproduce environmental observations
without confusing them with console/HDR probes. The following prints no USB serial numbers and does
not start capture, detach a driver, or communicate with the console:

```sh
rustc --version
/opt/homebrew/opt/llvm@18/bin/llvm-config --version
pkg-config --modversion opus libusb-1.0
ffmpeg -version
ffprobe -version
/usr/bin/swift --version
ffmpeg -hide_banner -f avfoundation -list_devices true -i ''
python3 - <<'PY'
import json, plistlib, subprocess
def walk(x):
    if isinstance(x, dict):
        if x.get('idVendor') == 0x0fd9:
            print(json.dumps({k: x.get(k) for k in
                ('USB Product Name', 'USB Vendor Name', 'idVendor', 'idProduct', 'UsbLinkSpeed')}))
        for v in x.values(): walk(v)
    elif isinstance(x, list):
        for v in x: walk(v)
walk(plistlib.loads(subprocess.check_output(['ioreg', '-r', '-c', 'IOUSBHostDevice', '-a'])))
PY
```

FFmpeg's device-list command exits nonzero after listing because no input was selected; that is not a
capture failure. Initial descriptor-only P010 observations came from libusb `get_device_list`,
`get_device_descriptor` and `get_config_descriptor` reads; no persistent descriptor-inspection
tool was added during that inventory. AVFoundation
`AVCaptureDevice.formats` inspection established only advertised driver formats, without a session.

**Alternatives**: A bare `system_profiler SPUSBDataType` was empty on this machine despite connected
devices, so it is insufficient as the sole presence check. Neither an upstream feature claim nor an
enumerated descriptor substitutes for the required R3/R4/R5 measurements.

### 2026-11-01 device continuation (T041/T042/T050)

The console resumed from the controlled suspend; `kern.boottime` and unchanged
service PIDs classified it as resume. The preserved codex6 population passed the
exact layout gate, the approved verifier ran once (`paired=1 metrics=0 pass=1`,
settings 61→66) and the approved cleanup removed it. A reviewed interactive
READY → neutral → stop run completed with continuous 45-second HDMI capture,
zero cleanup returns and an absent PID. Five additional retained-picture modes
(HEVC8 1080p60; AVC 1080p60/30; AVC 720p60/30) decoded 600/601/302/601/301
pictures with ordered export, zero module-unload results and selector removal.
Four-controller evidence remains the historical owner-assisted 2026-10-25
observation (four prompts/four Right navigations, 1788 reports per slot, clean
removal); the owner deferred multi-controller support. T041/T042/T050 remain
unchecked, and readiness remains ERROR. See the retained
[T050](../../.omo/evidence/t050-resume-verify-20261101/REPORT.md),
[T042](../../.omo/evidence/t042-neutral-stop-device-20261101/REPORT.md) and
[T041](../../.omo/evidence/t041-six-mode-device-20261101/REPORT.md) reports.
