# T045 readiness decision

**Decision: ERROR — gate not passed.** Reconciled 2026-10-31 for firmware 13.60.
**T045 remains unchecked.** One-controller delivery semantics, pipeline latency, driver attribution
and soak evidence are not established; the scoped SDR budget CPU/memory gate and cadence do pass.
This is an evidence-based negative decision, not a claim that the feature is impossible. T041, T042
and T044 remain Incomplete; no shipping fallback is selected.

**2026-11-01 scoped update:** the owner deferred HDR/protected content; the production budget CPU/memory gate passes; client cadence is 59.61-59.72 fps; a 30-minute SDR run and a two-hour soak pass the measured drop/pipeline thresholds; a one-controller Options press visibly opened the console pause menu. ERROR remains for driver allocations outside the measured host mappings, initial pad assignment on the first creator, and the T041/T042/T044 checkbox evidence. The five-game matrix is waived by the owner; one game and the official-client non-game run are sufficient for this scope. Subsequent takeovers reuse the creator handle without a prompt; the creator holder must stay alive until reboot. Host-arrival-to-visible-effect is now measured at 311.66 ms (60 fps burst, 16.7 ms resolution); host receipt is sub-millisecond.

**2026-11-02 v0.1.0 MVP release:** the owner released the scoped SDR/one-controller/one-game
build to the official Moonlight macOS client as v0.1.0. Its hardware validation is recorded in
[docs/evidence/v0.1.0-validation.md](../../docs/evidence/v0.1.0-validation.md). That release
decision does not change this record: T045 and the out-of-scope gates above remain as stated.

The assigned reconciliation is complete as a document deliverable. Under the task's checkbox
rule, only PASS could complete T045, so its dispatch outcome is failed (gate not passed).
No console, capture device or reference host was accessed and no probe was run by T045.
Evidence below is attributed to the existing reports; source-only or fake results never become
hardware observations. Neither a successful loader send nor a successful syscall establishes
an unobserved media/input capability.

## Evidence considered

| Record | Established evidence | Limits relevant to this decision |
|---|---|---|
| [R3 findings](research-r3-findings.md), T040–T042 | Pinned source/ABI/permission audit; four neutral controller handles and one corroborated D-pad sequence; one held-Right normal stop with returned neutral-before-remove cleanup, visible navigation and disconnect UI; non-silent stereo audio; four thirty-second requested-mode video populations and four completed ten-second rate points; dispatch 9 independently decoded 600 retained 1920x1080 Main10 frames | T040 checked; T041/T042 unchecked. The held-input UI reached the rightmost boundary and does not prove visible release; disconnect/partial-failure/rest/coexistence coverage remains absent. Main10 decode/header compatibility applies to that population; other-mode decoded coverage, source HDR, hardware provenance and bitrate bounds remain unverified. Dispatch 6 stopped on a later cleanup-completion failure; full protection/scheduling/restoration evidence remains absent |
| [R3 media RE](research-r3-media-re.md) and subsequent local R3 probes | Typed Avcap2 callers and later source investigation underpin admitted local audio/video calls | Earlier source-only/no-local-audio status is superseded by R3 observations. Independent hardware/driver accounting, source HDR metadata and complete normal/error cleanup remain unverified |
| [R4 acquisition](research-r4-capture.md), T032 | Actual 0fd9:009c / 5 Gbit/s 1080p P010 acquisition; fresh known HDR epoch; ten-bit packing and clock checks; normal/cancellation UVC/audio restoration | T032 checked under explicit owner deferral of physical-disconnect recovery. Startup rejects/gaps retained; external reference capture is not a console host media path |
| [R4 calibration](research-r4-calibration.md), T033/T034 | Native SDR/HDR still/motion source-error populations, repeatable scenes, paired causal delay bounds; approved SC-013/SC-014 refinements applied | Research complete, not release acceptance. Exact HDMI exposure/mastering metadata unmeasured; startup exclusions, conversion-oracle and future coverage limits remain |
| [Target dependencies](research-target-dependencies.md), T043 | OpenSSL cross-build/link and bounded on-device entropy/certificate/mutual-TLS/TCP/thread smoke; selected ENet and real target UDP exchange | T043 checked. Target IPv6, full production security/conformance, resource cost and installed-LLVM binary provenance remain open |
| [R5 budgets](research-r5-budgets.md), T044 | Bounded sleep/active clock observations; two concurrent owned workers with swapped roles establish thread-local CPU distinction; a later coordinator-defined ten-second window quantifies 10,001,177,603 ns guaranteed common coverage and edge skew | T044 unchecked. The enclosing two-worker CPU sum is not an exact common-window total. Process clock remains rejected for aggregate CPU accounting; production thread census, ten-second host budgets, real media/input workload, driver accounting, game comparison and bitrate endpoints remain absent |
| [Reference-host validation](../../tools/moonlight-mcp/docs/validation.md), T038 | Live Wolf/Fenrir pairing/stream/input/cleanup observations and official Qt 6.1.0 cross-checks | T038/FR-059 and T039 remain unchecked; recording/fidelity/HDR and complete reporting integration are Incomplete |

The [spec](spec.md), [plan](plan.md), [data model](data-model.md),
[implementation decisions](contracts/implementation-decisions.md),
[measurement contract](contracts/measurement-evidence.md), other feature contracts,
[quickstart](quickstart.md), [constitution](../../.specify/memory/constitution.md) and pinned
[protocol notes](../../tools/moonlight-mcp/docs/protocol-notes.md) supply requirements and
interpretation. Earlier planning-pass statements about absent helper/calibration/dependency work
are superseded by the reports above; they are not current negative hardware results. No contract
or spec is amended by this decision.

## 2026-11-01 scoped re-evaluation: SDR + CLEAR and production budget

**Owner scope decision:** the first release is SDR 1920x1080 H.264 at up to 60 fps with one
controller; HDR and capture-protected content are deferred and unsupported. The host advertises
SDR only and uses the explicit diagnostic CLEAR premise. This removes B03 and B04 from the
release gate; T041/T042 remain unchecked for their out-of-scope HDR/protection/feedback work but
the scoped SDR path has real console evidence.

**Production host budget (firmware 13.60, 1080p60 SDR H.264, one client):**

| Window | Idle | Streaming |
|---|---:|---:|
| 10-second thread CPU | 0.40 s, then 0.148 s | 0.16-0.83 s |
| Ceiling | <=0.5 CPU-s / 10 s | <=10 CPU-s / 10 s |
| Peak RSS (`getrusage`) | 14.8 MiB | 146.6 MiB (game content) |

The host now publishes `budget` events with thread-CPU deltas. Separating audio onto its own thread and sleeping only to the next video deadline raised client cadence from 57.15 to 59.61-59.72 fps. A 30-minute run produced 59.597 fps / 18.647 Mbps with 14/107,289 dropped frames (0.013%) and a two-hour soak produced 59.636 fps with 27/429,404 dropped frames (0.0063%), host pipeline p99 118 us, and constant 91,904 KiB RSS. `CLOCK_PROCESS_CPUTIME_ID`
reports wall time on this firmware (10.35 s per 10 s idle), so it is retained only as a
contrast; `THREAD_CPUTIME`/`RUSAGE_SELF` agree with each other and show the real values. The
first idle windows also exposed and fixed a full-core busy loop: the loader closes stdin, and
`poll()` returned `POLLHUP` forever until stdin was removed from the watch set.

Still open for T045: one-controller console delivery/feedback semantics (B05/B06), full host
pipeline p99 and bitrate sweep (B08), complete attributable allocation/driver domains, game
impact and two-hour stability (B09/B10). The decision therefore remains **ERROR**; the budget
CPU/memory gate and the protection/HDR scope are closed.

## Established progress and its boundaries

R3's local audio probe captured 239,616 stereo float32 frames in 5.0009 seconds, with nonzero
samples in both channels and zero-return normal Stop/Close/Terminate/module unloads. Source
timestamps/clock, independent channel ordering, protection and driver/service accounting remain
unverified. The four thirty-second video runs produced Annex-B-shaped records for requested
720p/1080p 30/60 modes; actual encoded dimensions, codec/profile/depth and hardware usage still
need independent evidence. Earlier owner observations of persistent gameplay/UI choppiness are
retained despite zero-return cleanup.

The owner's renewed PS5 authorization enabled dispatch 6 on 2026-10-31. Two additional
ten-second requests completed: 720p30 at 10,000 kbit/s yielded 300 records / 58,884 bytes,
and 720p60 at 2,000 kbit/s yielded 600 records / 85,997 bytes. Both final probe records still
reported Incomplete; static-source output and absent parameter NALs establish no supported
bitrate endpoint or decoded format. The next 720p60/6,000-kbit/s request yielded data but timed
out before its final result and ResourceArbitrator/Ipmi unload records. Its sleeping PID 209
required exact-PID SIGKILL recovery according to dispatch 6's operator record; raw recovery stdout
was not retained. The retained process/service postflight passed, but process removal does not
prove driver reclamation or restored output behavior. No later rate,
HEVC or configuration-header candidate ran in that series. The cleanup-completion failure
remains open.

Dispatch 7 subsequently added tested cleanup orchestration and a separate one-second trace.
The first `-O0` attempt returned no payload stdout and left no probe process; it is invalid
evidence. The `-O2` trace ran as PID 234, produced 60 records / 21,311 bytes over 1,006,144 us,
and completed all service/module cleanup with zero results. ResourceArbitrator's observed
unload span was 193 us (Avcap2 152 us, Ipmi 85 us). Kernel logs showed resource STOP, session
close and payload exit value 2; postflight recorded PID absence, listening services and a fresh
home-screen image. Two bounded ten-second repetitions (PIDs 238/239) then completed with 600
records each, 78,400/81,903 bytes and ResourceArbitrator spans of 204/208 us. All cleanup returned
zero, the selector was absent and both processes exited with value 2. The maximum two consecutive
repetitions was reached without recurrence; `service_call_deadline=unknown`. The prior intermittent
hang remains unexplained and individual platform-call
completion remains unbounded. Portable ordering/error tests and this short observation do not
prove driver reclamation or full output restoration. **ERROR and unchecked T041/T045 remain.**
The [dispatch 7 findings](research-r3-findings.md#t041-dispatch-7--cleanup-hardening-and-one-second-trace-2026-10-31)
retain the exact artifact and measurement boundaries.

The subsequent [dispatch 8](research-r3-findings.md#t041-dispatch-8--hevc10-configuration-header-observation-2026-10-31)
ran one HEVC10 configuration-header diagnostic as PID 254: 600 records / 528,559 bytes over
10,014,875 us, p99 read 16,642 us. The independent header parse reports Main10, ten-bit,
cropped 1920x1080 and BT.2020/PQ configuration signaling. Saved picture payloads and linkage
between those headers and read records are absent for that run. Cleanup returned zero, the
selector disappeared and PID 254 exited with value 2;
postflight retained PID absence and a normal home-screen frame. The prior intermittent hang,
unknown platform-call deadline, protection and resource/budget gaps remain. **ERROR is unchanged.**

[Dispatch 9](research-r3-findings.md#t041-dispatch-9--retained-hevc10-pictures-and-independent-decode-2026-10-31)
then retained 600 exact VCL records / 542,607 bytes as PID 261 over 10,014,179 us
(433,470 bit/s; p99 read 16,618 us), plus 103 bytes of configuration headers. Strict
FFmpeg/ffprobe 8.0.1 decoding produces all 600 frames as HEVC Main10 / `yuv420p10le`,
1920x1080 (coded 1920x1088), level 150, limited-range BT.2020/PQ signaling. This establishes
decoded Main10 and configuration-header compatibility for this retained population.
Cleanup returned zero, the selector was removed, PID 261 was absent afterward and fresh
HDMI frames showed the home screen; the coordinator observed all four services listening.
The private raw export remains local evidence. Independent source HDR metadata, protection,
HDR-to-SDR conversion, hardware provenance, supported bitrate/audio bounds, driver accounting
and platform-call deadlines remain unverified. **T041/T045 remain unchecked and ERROR remains.**

R4 is beyond descriptor enumeration. Its known-HDR acquisition has 120 complete frames out of
125 observed boundaries, five startup rejects and 57.588 complete fps including startup. It
does not prove loss-free 60 fps or SC-004 host drops. Its earlier owner-confirmed epoch was
2160p60 RGB HDR scaled/converted to 1080p P010; that epoch is not reused as a later unmeasured
HDMI mode. T034's known-source populations score all 233 SDR frames and match 466 of 480 HDR
frames; 14 startup exclusions remain declared. The conversion oracle is a software
interpretation, not an independent measurement of the HDMI raster/EOTF.

SDR still/motion median luma SSIM is 0.986024190/0.984230476; HDR scene medians range
0.988515247–0.990514306. SDR motion median submit-to-reference lower/upper bounds are
73.840/1453.456 ms and HDR repeat bounds 48.033/80.800 ms. These paired causal intervals
are research uncertainty bounds for their own populations, not exact exposure timestamps,
universal delay corrections, host-pipeline latency or a release pass.

The owner's 2026-10-25 T034 refinements are already applied: SC-014 full-population median
luma SSIM >=0.95 supplements its 99%-at-0.70 floor and applicable <=0.01 prior-release
regression. SC-013 adds median >=0.97, joint 99% SSIM >=0.95 and Y/Cb/Cr PSNR >=35/40/40 dB
(perfect planes satisfy floors); frozen native ramp/patch scenes require >=512 distinct integer
column-median levels, backward step <=4 native codes, neutral/patch median error <=8 codes
and no interior clipping after an 8-code reference-endpoint guard. All previous visual,
platform/display, metadata and population obligations remain. No new refinement needs approval
for this reconciliation because none is proposed or applied.

Physical-disconnect recovery is **owner-deferred, untested, non-blocking for T032**, recorded
2026-10-25 after attempts did not correlate removal with an active capture. Normal/cancellation
rollback and subsequent UVC restoration remain evidenced. The deferral is not a NO_DEVICE
recovery pass and does not waive console lifecycle or release evidence.

T043 selects OpenSSL 3.5.9 at `45e844fa2a14ec92d146bd8f5778ac130b6625fb` and
cgutman/enet 1.3.17 Moonlight fork at `aca87840b57f045a1f7f9299e4b1b9b8e2a5e2f1`.
Its exact patches/pins and real UDP reliability/channel/fragmentation/disconnect evidence
support dependency feasibility. They do not establish a media session, physical entropy
strength, IPv6 interoperability, target allocation costs or complete official-client conformance.

## Blocker register

Every row is a mandatory gap under the current design or a separate acceptance prerequisite.
Task references identify direct consumers; ERROR also transitively blocks production-host
completion across T046–T121, subject only to the bounded portable-work direction below.
Unknown capability is not proof of permanent hardware inability. Closure requires retained,
source-cited results rather than further owner preference alone.

| ID / missing or nonqualifying capability | Blocked tasks / requirements | Evidence or explicit decision that closes it |
|---|---|---|
| B01 Foreground-game capture and hardware H.264/HEVC/Main10 encode: dispatch 9 decodes 600 retained home-screen frames as 1920x1080 Main10 with compatible configuration headers. The 2026-11-01 five-mode matrix adds decoded console populations for HEVC8 1080p60 (600 frames), AVC 1080p60 (601), AVC 1080p30 (302), AVC 720p60 (601) and AVC 720p30 (301), all with ordered export, zero reported cleanup results and selector removal. Hardware-encoder provenance, independent source HDR, protection, bitrate bounds and complete resource restoration remain unvalidated. Provisional T071/T072 adapters now compile warning-free but have no console run. Dispatch 6 exposed a cleanup-completion failure | T041/T044/T045; target use of T049/T058/T060/T067; T070–T072/T078–T080; T083–T089; later production acceptance | Source-pinned permitted API/process context, owned buffers and completion semantics; real 720p/1080p 30/60 SDR and 1080p60 ten-bit HDR populations with actual hardware bitstream provenance and clean resource ownership. No CPU-encoder/external-capture fallback is approved |
| B02 Stereo game audio: local non-silent stereo float32 data and normal API/module cleanup observed; source timestamps/clock, independent channel ordering, protection, bounded service calls and driver/service accounting remain unvalidated | T041/T044/T045; target T061/T067/T073/T079 and HDR sessions | Complete sourced local stereo/protection observations, bounded service calls, source timestamps and Stop/Close/Terminate success/partial-failure ownership and resource-accounting evidence |
| B03 Independent readable source HDR state/mastering-display/light metadata, output epochs and bounded HDR-to-SDR conversion missing; setter/status flag/P010 pixels cannot supply metadata | T041/T045; T071/T079; T081–T089, SC-012/013 | Independent source record before host serialization, correlated epochs and actual-versus-signaled metadata; measured ten-bit path and conversion timing/memory/fidelity; all four source/request combinations and <=2-second transitions |
| B04 Capture/audio protection boundary and safe queue invalidation not observable; protected source content cannot be presumed permitted | T041/T045; T067/T071/T073/T079/T081/T086–T088, FR-022/D2 | Sourced protection signal and permissible bounded validation of placeholder video/stereo silence, invalidation of stale queued samples and safe resume/teardown without forwarding protected content |
| B05 Single-controller functional assignment and full mapping: a 2026-10-25 owner-assisted run added four handles in order, recorded 1788 accepted reports per slot, four user-switch prompts and four Right navigations, then neutralized and removed all four with zero returns. The owner then deferred multi-controller support ("1 controller support is enough"), so Create/PS, proportional controls, rumble feedback, motion/touch coordinates and gameplay delivery remain unestablished; the recorded four-handle observation is retained as supplemental evidence; the provisional T074 adapter compiles but delivery stays UNCONFIRMED and motion/touch are unsupported | T042/T045; target T062/T067/T074/T079; SC-005 | Source-backed reports/feedback interfaces plus independent four-player ordered-delivery/control/rumble/motion/touch observations. Identity orientation and call return 0 are insufficient. Fresh multi-controller work is no longer required: FR-031 now specifies one simultaneous controller, and the production adapter parks one neutral pad across sessions |
| B06 Exact successful console-delivery timing boundary and complete termination neutralization/removal not validated. One held-Right normal stop returned from neutral-before-remove cleanup and showed a disconnect toast; the current production adapter parks the neutral pad instead, four post-session screenshots show no dialog, a one-second idle neutral heartbeat kept the pad alive for a five-minute post-session check, a stable assigned-controller menu measured host-arrival-to-visible-effect at 311.66 ms (60 fps burst, 16.7 ms resolution), a prompt-free handle handoff let two successive hosts drive the same assigned pad with no controller prompt, and the system-installed Moonlight 6.1.0 official client streamed the non-game system UI for 82 s at 59.86 fps rendering with 0.00% network drops, but the rightmost UI boundary masks continued Right. The neutral-observation payload later completed neutral submission, three-second registered-handle dwell and removal with zero cleanup platform-call returns, but Settings launch failed and the 45-second HDMI record likely ended before Down/dwell/removal. The reviewed explicit-command helper and coordinator sequence capture-ready → READY → neutral ACK → stop/DONE → postflight. Approved opt-in command adapters now provide continuous FFmpeg PPM host-receipt capture, independent decode and configurable read-only postflight commands; raw partial footage and receipt metadata survive failure and are hash-bound by nested/outer manifests. A 2026-11-01 READY → neutral → stop run completed on the console with a continuous 45-second 1080p30 capture, exact ACK/DONE markers, zero cleanup returns, absent payload PID and clean services/HDMI; no visible UI change was observed. A later one-controller session sent one Options press: host receipt passed and the console visibly opened the God of War pause menu, distinguishing receipt from effect for that action. Follow-up Down-only and Cross-then-Down runs returned exact `0x0040`/`0x4000` ACKs but produced no assignment prompt or selection movement, proving ACK is not delivery/assignment and a sourced assignment step is still required. The command adapters and coordinator remain unbound; visible release of a held input remains unverified | T042/T044/T045; target T063–T067/T074/T079/T094/T096/T098/T107; SC-005/010 | Validate the concrete live command bindings and capture ownership after preserved T050 recovery/cleanup, then run READY → neutral → stop and correlate each action/neutral acknowledgement with continuous independent HDMI and clock evidence. Retain documented console delivery semantics and input-first idempotent cleanup observations for transport disconnect, partial initialization/failure, rest and other termination paths. Host receipt continuity and successful API cleanup do not establish visible release |
| B07 Remote Play/Share Play detection/transitions, foreground-game-safe scheduling, rest/signal/transport stop and complete privileged restoration missing. The exact active game was deliberately closed and independently corroborated, allowing one no-game input diagnostic, but this is not matched game-impact evidence. Its exact-PID TERM request did not interrupt the payload; ordinary cleanup returned and no non-normal-stop observation exists. T094's portable API/UI retains cleanup retry after durable revocation, and T096's fake stop retry preserves input-first ownership after injected cleanup failure; neither is a production-adapter or active-console observation. The provisional T075 system adapter now compiles with exact firmware gating and explicit unknown-state semantics, but Remote Play/Share Play/rest detection and notifications remain `STAGE_UNAVAILABLE`. Stock SDK startup writes have owner-approved baseline status but unverified reversal | T042/T044/T045; T070/T075/T078–T080/T094/T096/T098/T114/T117, FR-036–FR-038 | Typed sourced conflict/status APIs and offline behavior; observed refusal/clean end; justified affinity/priority with matched game evidence; per-operation firmware provenance and verified restoration. Normal probe exit and fake cleanup cannot prove rest/hook cleanup |
| B08 Real capture/encode/packetize/send populations, full-pipeline timing, protocol latency, host frame/drop denominator and mode-specific sweep absent (one 20,000 kbps request now measures 16.5-18.6 Mbps) | T044/T045; production T049/T053/T058/T063–T067/T072/T079/T084/T087–T088/T107/T114/T116 | A 30-minute SDR run measures 59.597 fps, 18.647 Mbps, 0 incomplete client frames, 14/107,289 host-vs-client frames (0.013%), protocol p99 0.3 ms and audio loss 0.0. A two-hour soak adds 59.636 fps, 429,377 complete frames, 27/429,404 host-vs-client frames (0.0063%), host pipeline p99 118 us, and constant 91,904 KiB RSS; per-mode sweep and HDR remain open |
| B09 Host-attributable memory and CPU are now measured: idle 16.0 MiB current / 28.9 MiB peak, streaming 79.4/92.2 MiB, 30-minute game-content `getrusage` peak 146.6 MiB, two-hour soak RSS constant at 91,904 KiB, thread CPU 0.11-0.81 s/10 s. The 16 KiB-page `procstat -v` mapping is retained; driver allocations outside the host mappings remain unknown. | T044/T045; production T063–T067/T070–T079/T084/T086/T107/T114/T116 | All core/media/metrics/stacks/runtime/driver domains with ownership and high-water; validated CPU accounting and all-thread census for every ten-second window under actual idle/stream workloads; compare unchanged ceilings below, explicitly revise design if infeasible |
| B10 Two-hour stability is now observed (59.636 fps, constant RSS, clean teardown); five-game comparison/rubric, metrics overhead and matched game-impact remain absent. | T044/T045 for initial budget/game evidence; T079/T088/T107/T114–T119 for production acceptance | Agreed named scenes/settings and observation rubric for >=5 games; matched baseline/stream and metrics-on/off repeats (<=2% protocol p99 overhead); two-hour continuous stream without crash/disconnect/growth/p99 drift. Preserve before/after and first/last windows |
| B11 Official client non-game validation now passes by Computer Use GUI and CLI: system-installed Moonlight 6.1.0 paired and streamed the console system UI; the GUI run lasted 2:35 at 59.88 fps rendering, 0.00% network drops, 0.02% jitter drops, with the performance overlay captured and a clean host teardown (5,603 frames / 18,688 audio packets). T038 recording/report remain Incomplete despite supplemental live successes. Reviewed local supervision owns the collector, validates manifest/source snapshots and translates the exact immutable cancellation signal; uncertain pipe-close failures cannot skip child cleanup/reporting. Native no-replace binding/READY publication precedes launch. A second barrier checks prelaunch freshness, publishes a private launch nonce only in postlaunch `observer-launch.json`, and retains an exact nonce-bound attachment before RTSP/input. Supplied runtime attachment validates the Session → Deployment → ReplicaSet → pod chain, container/source identities and exact device paths. Deployed `5c8251c` retains session/path/major/minor/generation, but no subscription history. Zero initial devices avoid the controller-arrival startup cycle. An approved opt-in one-shot device feed now binds exact runtime/launch identity and opens only supplied exact paths; `late_attach` precedes later decoded events, early coverage stays unavailable and continuity unverified. Finish without a feed does not hang. Approved Fenrir provider and SSH subsystem relay software now implement baseline-before-READY, exact owner-chain/HTTPS numeric-port acquisition and pinned-key/hash-bound cross-host transfer. A reviewed server deployment gate now binds a dedicated non-root account, root-owned hash-pinned source without bytecode/import shadows, literal executables, private session root and kubeconfig while rejecting client root/provider overrides. Four process-group owners now check direct-child liveness before every signal, stop escalation after observed exit and report descendant cleanup as unconfirmed. Relay polling uses one owner-exit snapshot per artifact scan so a mid-scan exit cannot precede export of the completed supervisor report. The current complete fixture verifier has 151 Python tests and 23 exit-zero stages. The current-user live audit confirms verified Kubernetes TLS/read/proxy access and zero Sessions, but no dedicated relay installation. A regular fixed-path kubectl wrapper is fixture-tested but not installed or validated on g913. True cancellation confirms only a foreground delete request; false closes control without finish and imports immediately, and later disappearance cannot upgrade it. All acceptance observations remain unavailable/unauthenticated. The current g913 app is HDR-unsupported; digital silence is not game-audio evidence | T038/T039 and every use of the independent client to accept SunshinePS5, including T069/T079/T087–T089 and later acceptance | Install and verify the dedicated relay sshd subsystem/account/key with the reviewed gate, pinned host key/code, private root and fixed wrapper; validate `/bin/sh`, delegated `/usr/local/bin/k3s`, parent integrity, live compatibility, dedicated-account Kubernetes RBAC, the self-signed agent backend proxy and populated runtime. Establish authenticated agent/runner-to-Session/event ownership, independent cleanup evidence and a live controller population. Complete host-output recording, native calibrated SDR/HDR fidelity and required report integration against the declared profile, with exact versions and official-client cross-checks. Subscription continuity remains unverified until independently established. Do not edit immutable reports to import supplemental passes |
| B12 FR-057 “with nothing running on the console” wording unresolved (D5); R4 research completion does not resolve owner intent | FR-057 acceptance, T082/T088/T118/T119 and complete reference comparison claims | Owner explicitly confirms a literal spec/contract refinement before application; D5 recommends external recording without an additional console recorder while host/content run. This is a pending recommendation, not an approved fallback |
| B13 Per-run reference calibration/independent metadata and supported platform/game evidence incomplete beyond controlled research scenes | T079/T082/T087–T089/T115/T118/T119; SC-007/013/014 | Native full-population matching, calibration identity and honest uncertainty for each declared mode/run; source metadata separately; every required official-client/HDR-display entry, game-set/side-by-side evidence and applicable prior-release comparison |
| B14 Production dependency/security, release reproducibility and audits remain open. T050's confirmed SDK carry/errno defect is fixed by project-local target wrappers. Codex6's earlier init and fresh-process verify preserved identity, settings and paired registry through a second atomic replacement; exact cleanup removed that earlier synthetic state. On 2026-11-01 the console resumed from standby: `kern.boottime` `1793013931` preceded the rest attempt by 469,434 seconds, the pre-rest service PIDs were unchanged, the exact 40-entry codex6 layout passed, the approved verifier ran once (fingerprint `c25403d7…`, settings 61→66, `paired=1 metrics=0 pass=1`) and the approved cleanup removed the population (`removed=1 pass=1`), leaving only `services`. This establishes limited rest/wake persistence and recovery cleanup; full reboot, abrupt power-loss durability, privileged key isolation and official pairing remain open. See the [retained run](../../.omo/evidence/t050-resume-verify-20261101/REPORT.md). Do not reinitialize this population before recovery inspection. A reviewed inspection-only coordinator pins the exact population/fingerprint/artifacts and makes every injected verify/cleanup attempt durably non-retryable. Approved fixed-endpoint recovery adapter mechanics now provide discovery, exact-ELF bounded transfer, independent exit gating and separate cleanup arming, with 188 offline tests. Approved host acquisition components add bounded diagnostic service/route collection and a source-digest-bound HDMI classifier seam; 142 focused tests pass, but no classifier ships and diagnostics cannot arm recovery. Shared process cleanup now avoids signalling known-exited children and explicitly leaves descendant cleanup unconfirmed. The approved read-only target inspector now has a locally verified full-CRT launcher that gates observation on exact firmware `0x13600007`; host regressions, target ELF inspection and independent code review pass. SDK CRT/source provenance and public elfldr v0.27 sources were inspected. The CRT performs process-local credential/syscall bootstrap before the gate. Exact Relapse distribution provenance is established. A byte-identical SDK v0.45 full-CRT verifier historically executed through this loader on the same 13.60 console, and five CRT functions match the inspector after masking only pinned object relocation fields, supporting the historically exercised full-CRT ABI class. Inspector-specific behavior for 23 GLOB_DAT imports and one R_X86_64_64 relocation, current-boot loader continuity, inspector execution/output and independent exit remain unverified; exact loader build provenance remains unresolved. The observer's firmware-validation/atomic-snapshot claims remain false and process/game/writer claims remain null. A reviewed public-certificate fingerprint path and independent process-exit evidence remain missing. No inspector payload was dispatched. Seven live callbacks (services, boot, process, isolation, HDMI, inventory and exit observer) and a genuine pre-rest boot identity therefore remain unbound. Retained service PIDs cannot prove continuity, and historical incomplete inventory cannot arm dispatch. Target symlink/hard-link fixtures return ENOTSUP/45 and are explicitly skipped after secure restoration/reload checks. Reboot/power-loss durability remains absent. Opus/RSA dependencies were remediated and the ENet commit-only audit corrected; the current live audit completed 8 host and 238 client queries with zero advisory matches. The frozen T056 run still retains one post-restart Qt abort and a passing repeat, with outcome 5/6 FAIL. A later debugger run establishes an upstream official-client CLI lifetime defect but neither reproduces nor proves causality for that abort; no host workaround is justified. A session-observed GUI diagnostic pairs, lists PS5 and reuses its portable identity after a clean client restart, but lacks retained raw UI/action/host transcripts and does not alter the CLI FAIL or supply console durability/timing | T049/T050/T056/T059/T078/T114/T119–T121, plus later acceptance | Recover console visibility and establish current-boot loader continuity, review inspector-specific safe deployment including its imports/relocation case and retain its execution/output with independent exit evidence, bind and review all seven live callbacks plus genuine pre-rest boot evidence, identify whether this is a resume or boot, acquire the complete security-aware inventory/public fingerprint and only then arm the approved verifier if its prerequisites hold. Separately obtain reboot/power-loss evidence and independent review. Complete official-client observations and new-backend live pairing, IPv6/full TLS/control/session conformance, production allocation costs, checked installed/rebuilt compiler provenance and required passing hosted CI/release audits. The current database audit establishes only its declared coverage. Licensing remains owner-excluded, not audited |

B11's latest portable evidence is the [local supervisor report](../../.omo/evidence/t038-local-supervisor-20261031/REPORT.md)
and [close-fault re-review](../../.omo/evidence/t038-local-supervisor-code-review.md),
[postlaunch barrier report](../../.omo/evidence/t038-runner-attachment-20261031/REPORT.md)
and [nonce re-review](../../.omo/evidence/t038-runner-attachment-code-review.md), and
[runtime attachment report](../../.omo/evidence/t038-runtime-attachment-20261031/REPORT.md)
and [empty-inventory re-review](../../.omo/evidence/t038-runtime-attachment-code-review.md), plus
the [one-shot device-feed report](../../.omo/evidence/t038-late-device-feed-20261031/REPORT.md)
and [review](../../.omo/evidence/t038-late-device-feed-code-review.md), the
[Fenrir provider](../../.omo/evidence/t038-fenrir-provider-20261031/REPORT.md) and
[HTTPS correction review](../../.omo/evidence/t038-fenrir-provider-code-review.md), and the
[SSH subsystem relay](../../.omo/evidence/t038-remote-relay-20261031/REPORT.md) and
[final review](../../.omo/evidence/t038-remote-relay-code-review.md), plus the
[deployment preflight](../../.omo/evidence/t038-deployment-preflight-20261101/REPORT.md) and
[independent review](../../.omo/evidence/t038-deployment-preflight-code-review.md).
The follow-up [Fenrir](../../.omo/evidence/t038-fenrir-provider-pgid-20261101/REPORT.md),
[supervisor](../../.omo/evidence/t038-supervisor-pgid-20261101/REPORT.md),
[relay](../../.omo/evidence/t038-remote-relay-pgid-20261101/REPORT.md) and
[fixture-race](../../.omo/evidence/t038-supervisor-fixture-race-20261101/REPORT.md) reports plus
the [bundle review](../../.omo/evidence/pgid-wrapper-bundle-code-review.md) cover only portable
process ownership and verifier reliability; they do not complete T038.
The [read-only live deployment audit](../../.omo/evidence/t038-g913-relay-live-audit-20261031.md)
at `2026-11-01T00:26:43Z` found user `jeff`'s kubeconfig mode 0600, verified HTTPS to
`127.0.0.1:6443`, no insecure/proxy settings, yes for every required read/proxy permission and
zero Sessions. It found no standalone kubectl on PATH, provider/relay modules in the bounded
home search, relay subsystem or restricted relay credential/account; the current key is plain.
This narrows current-user TLS/RBAC uncertainty without validating the future dedicated account,
self-signed backend proxy or populated runtime. The audit made no mutation.
B14's [adapter report](../../.omo/evidence/t050-live-adapter-20261031/REPORT.md) and
[descriptor-fix review](../../.omo/evidence/t050-live-adapter-code-review.md) approve only
portable recovery mechanics. The [live-components report](../../.omo/evidence/t050-live-callbacks-20261101/REPORT.md)
and [clear re-review](../../.omo/evidence/t050-live-callbacks-rereview-20261101.md), plus the
[inspector report](../../.omo/evidence/t050-recovery-inspector-20261101/REPORT.md) and
[clear re-review](../../.omo/evidence/t050-recovery-inspector-code-review.md), approve only the
bounded partial acquisition described above. The [launcher report](../../.omo/evidence/t050-recovery-launcher-20261101/REPORT.md)
and [independent review](../../.omo/evidence/recovery-inspector-launcher-code-review.md)
verify the full-CRT launcher offline. The [exact-loader audit](../../.omo/evidence/t050-relapse-loader-compat-20261101/REPORT.md)
establishes distribution provenance and supports the historically exercised full-CRT ABI class;
inspector-specific deployment, current-boot continuity and all seven callback bindings remain
open. The [fresh discovery log](../../.omo/evidence/t050-rest-device-20261031/discovery-20261101T011503Z.log)
records standby at `2026-11-01T01:15:03Z`; the
[latest rest attempt](../../.omo/evidence/t050-rest-device-20261031/rest-attempt.md) retains
continued Standby and no dispatch. The read-only
[vphone check](../../.omo/evidence/t050-vphone-wake-recheck-20261101/REPORT.md) established no
installed Remote Play/Chiaki client or registered wake route.
B06's [command-adapter report](../../.omo/evidence/t042-command-adapters-20261031/REPORT.md)
and [failure-retention re-review](../../.omo/evidence/t042-command-adapters-code-review.md)
plus the [process-ownership correction](../../.omo/evidence/t042-process-transport-pgid-20261101/REPORT.md)
cover the portable acquisition/cleanup behavior; neither set completes hardware acceptance.

No row authorizes guessed firmware offsets, unsafe media calls, a new fallback or removal of a
required criterion. If a mandatory capability cannot be provided, retain ERROR and bring a
concrete scoped design/spec proposal to the owner before applying any refinement. Hardware
encoding remains a constitution requirement. An owner decision cannot replace its measurement.

## Budget disposition

| Required ceiling / condition | T045 disposition |
|---|---|
| 256 MiB total host memory: 128 MiB media/transport, 80 MiB metrics, 48 MiB core/TLS/web/settings/logs/stacks, including attributable driver domains | Peak production RSS 91,456 KiB (~89.3 MiB) during an SDR session; allocator/pool/driver domains still unverified. Unknown is not zero |
| Streaming <=10 CPU-seconds in every ten-second window; idle <=0.5 CPU-seconds with no session/browser | **Pass for the measured host**: 0.16-0.83 thread CPU-s (2.9-5.1 RUSAGE CPU-s) per 10 s streaming; 0.148-0.40 CPU-s/10 s idle. Full driver/kernel attribution remains unverified |
| Full host pipeline p99 below the exact negotiated frame interval; separate SC-003/004/005/012 populations | Unverified; R4 reference receipt and ENet fixture exchange cannot supply host media/input boundaries |
| Safe game scheduling, no growth, <=2% metrics overhead and two-hour stability | Unverified; real matched streaming/game/metrics/soak observations absent |

No numeric budget or bitrate boundary is approved, estimated or relaxed. The R5 diagnostic's
65,536 requested bytes exclude allocator/runtime/stacks/media/driver costs; its process clock
approximately equaling wall time while its sole thread slept cannot be interpreted as one
core consumed. R4's successful <=32 MiB and failed 64 MiB direct-memory requests are specific
to those launch contexts, not proof for or against the full host design.

The [two-thread PS5 observation](../../.omo/evidence/t044-two-thread-device-20261031/result.md)
adds four created/joined/complete worker records over two rounds. Active workers
used 248,383,000 ns and 248,660,000 ns of thread CPU, while sleep-like workers
used 1,080,000 ns and 1,085,000 ns over comparable 250-ms intervals. The workload
distinction follows the swapped roles. The sums span independently sampled
intervals; main/SDK/library/daemon/driver CPU, media/input populations and game
impact remain outside them. The later
[shared-window diagnostic](../../.omo/evidence/t044-common-window-device-20261031/REPORT.md)
quantifies common coverage and edge uncertainty around one coordinator-defined ten-second target,
as summarized above; exact production CPU totals and the thread census remain unresolved.
**T044/T045 remain unchecked; readiness stays ERROR.**

## Authorized work and blocked production scope

**Owner decision, 2026-10-25:** PORTABLE/fake-platform work proceeds despite ERROR: portable
parts of T046–T069, portable US3/US4 and fuzzing. Console adapters, on-device evidence and
release tasks stay blocked. This explicit scheduling direction permits software verification;
it does not change the T045 PASS condition, requirements or truthful-evidence obligations.

**Owner decision, 2026-10-31:** PS5 operations needed to investigate the remaining blocked items
were authorized. Dispatch 6 used that authorization for nonshipping probes and exact-process
recovery, then halted at the cleanup-completion failure; dispatch 7 used a separate supervised
cleanup trace and two bounded ten-second repetitions after software hardening. Dispatch 8 added
one separately reviewed HEVC10 configuration-header observation, followed by dispatch 9's
bounded VCL export and independent 600-frame Main10 decode. One separately reviewed T042
held-Right normal-stop diagnostic then added returned neutral-before-remove cleanup plus a
visible navigation/disconnect event, without proving neutral release at the rightmost UI boundary.
Portable follow-up also added T038 observer-publication synchronization, manifest-backed
post-cancellation controller import, an optional exact-binding pre-launch READY barrier, and a
maintained read-only evdev observer/conservative publisher with an exact-binding finish channel in
portable CI. Native atomic no-replace runner binding publication and an immutable cancellation
signal now coordinate that finish channel with one absolute import deadline; failed cancellation
imports immediately. The ten-second runner bound confirms only the cancellation request outcome:
deployed Fenrir acknowledges foreground deletion while cleanup can continue; its separate
ninety-second wait applies to launch-time prior-session cleanup. The approved local supervisor
preserves child cleanup/reporting through uncertain close failures. The nonce-bound postlaunch
barrier and strict supplied runtime attachment now precede RTSP/input; empty initial devices avoid
a controller-arrival cycle. The approved opt-in one-shot device feed binds exact runtime/launch
identity and opens only supplied exact paths, emitting `late_attach` before later decoded events.
Early coverage remains unavailable and continuity unverified; finish without a feed is bounded.
The approved Fenrir provider and SSH subsystem relay now supply fixture-tested acquisition and
verified cross-host transfer software. The current-user read-only live audit confirms verified
Kubernetes TLS/read/proxy access, but dedicated sshd subsystem/account/key, host-key/code deployment,
private root and wrapper installation remain outstanding. The regular fixed-path wrapper is
fixture-tested only; `/bin/sh`, delegated `/usr/local/bin/k3s`, parent integrity and live
compatibility still require dedicated-account validation. Dedicated-account RBAC,
self-signed backend proxy behavior, populated runtime, authenticated agent/runner-to-Session/event
ownership, subscription continuity and independent output/audio/HDR/fidelity/cleanup remain open;
later pod disappearance cannot upgrade a false signal. Follow-up also added complete Python-suite CI
discovery for T039, the six-mode local T041 export matrix, T042 neutral-observation hardening and
an explicit-command interactive helper, neutral/stop coordinator and opt-in command adapters
with continuous FFmpeg PPM capture, independent decode, read-only postflight commands and
hash-bound partial footage retained on failure. Live bindings/TCC/transport, source timestamps
and visible release remain unverified. Follow-up also added
real POSIX replacement-failure regressions, target syscall diagnosis/fix and an inspection-only
one-shot recovery coordinator plus approved fixed-endpoint adapter mechanics for T050, a frozen-binary
official Moonlight rerun for T056, revocation cleanup retry for T094, and retry-preserving fake stop
cleanup for T096. Codex3 confirmed the raw syscall carry/positive-errno defect without mutation;
codex4/5 exposed unsupported probe-only link fixtures and their partial trees were exactly cleaned.
Codex6 then completed init and fresh-process verification with unchanged identity/authorization
and a second settings replacement, followed by exact cleanup leaving only development services.
A separately reviewed one-shot helper later closed exact AppId 24, and independent postflight
established no active game. One T042 neutral-observation diagnostic then completed ordinary cleanup,
but Settings launch failed, exact-PID TERM did not interrupt it, and the 45-second HDMI record did
not establish Down/dwell/removal coverage. Its result remains Incomplete; see the
[device record](../../.omo/evidence/t042-neutral-observe-device-20261031/REPORT.md).
The active-game constraint is cleared. A fresh init now exists at
`/data/ps5-dev/t050-20261031-codex6`, and its separately reviewed verifier is bound to fingerprint
`c25403d792a58565794dcd9ced9c12ef8b2e862e6f4c20e430d8b51e3c5f47b9` but has not been dispatched.
One suspend request was followed by loss of HDMI and all five listeners. A later read-only Remote
Play discovery response identified `Server Standby` on firmware `13600007`; broadcast and unicast
WOL diagnostics did not restore connectivity. Resume and persisted-state inspection remain pending;
the preserved population must not be reinitialized before inspection. Reboot and power-loss
evidence remain absent. HDMI at `2026-11-01T00:31:59Z` still showed NO SIGNAL; fresh discovery
at `2026-11-01T01:15:03Z` returned `HTTP/1.1 620 Server Standby` for host `00E421FDEC6B` on
firmware `13600007`, with exploit/development-service ports absent and no inspector, verifier or cleanup dispatch. A read-only vphone inventory found no
installed PS Remote Play or Chiaki client and established no registered wake route. The reviewed
recovery adapter now implements fixed-endpoint discovery/transfer mechanics, fresh evidence
retention, independent exit gating and separate cleanup arming; 188 offline tests passed.
Its services, boot, process, isolation, HDMI, inventory and exit-observer callbacks remain
unbound, as does a genuine pre-rest boot identity. Retained service PIDs cannot establish
continuity. The coordinator CLI is still inspection-only, no standalone dispatch CLI exists,
and historical partial inventory cannot arm dispatch. See the
[rest attempt](../../.omo/evidence/t050-rest-device-20261031/rest-attempt.md).
Separate reviewed Opus/RSA remediation and ENet audit correction removed the recorded dependency
findings; the current live audit completed 8 host and 238 client queries with zero matches.
See the [storage device record](../../.omo/evidence/t050-storage-device-20261031/REPORT.md)
and [current dependency audit](../../.omo/evidence/rsa-backend-remediation-20261031/REPORT.md).
The T056 matrix remains FAIL because one of six original scenarios aborted after printing the
post-restart app list; one repeat passed. A later debugger run establishes an upstream official-client
CLI lifetime defect, but not causality for the TLS abort or a justified host workaround. A separate
session-observed GUI diagnostic paired, listed PS5, exited normally and reused its identity after a
clean restart, but lacks retained raw transcripts and does not reclassify the CLI matrix or establish console restart/power-loss durability,
notification/timing, production stop behavior or active-session cleanup. Production adapters and
acceptance still require their evidence prerequisites; these observations do not make T038, T039,
T041, T042, T045, T050, T056, T094 or T096 complete.

| Task range | Disposition while ERROR |
|---|---|
| T046–T069 | Portable tests/core/fake integration may proceed in test-before-behavior and interface dependency order. Mixed T050 target storage/identity obligations remain blocked; target mode/budget/delivery claims in other tasks require real evidence. Fake capabilities and bitrate bounds must be labeled fixtures. T069 fake CI is software verification, not FR-059 acceptance |
| T070–T080 | Console/privileged/media/input/system integration and on-device US1 evidence remain blocked. A first console-only pair/web/status composition (`platform/main.c`, build script, PS5 workflow) now builds warning-free and ran once on firmware 13.60; `serverinfo`, web index, status and empty pairing JSON returned 200. HTTPS 47984 and real-client pairing through the web PIN flow now pass, with the paired registry surviving a host-process restart. A diagnostic full host then ran a 10-second 1080p60 H.264 SDR session (584 video frames, 1,962 Opus packets, 9 host input events imported through the session-bound JSONL endpoint; the console_diagnostic CON-input criterion passes for host receipt while delivery remains UNCONFIRMED, clean teardown) with an unsourced protection CLEAR override; the client measured 57.89 fps and 2.80 Mbps and remains overall Incomplete; a later actual game-content run reached 57.64 fps and 2.85 Mbps with CON-input Pass. After correcting the Avcap2 bitrate unit to bits/s and parking the virtual pad across sessions, 20-second game-content runs measured 16.52 and 17.54 Mbps and the post-teardown controller dialog no longer appears. Persistence across console reboot, official-client matrix, sourced protection, full 30-minute SDR and release evidence remain open. The portable authorization above does not itself authorize this range's remaining integration tasks |
| T081–T089 | Production HDR adapters/signaling/conversion and acceptance remain blocked on the missing R3/R5 path and complete client/source/reference evidence |
| T090–T098 (US3) | Portable management tests/core/web/UI and fake behavior may proceed with real interface predecessors; T098 hardware evidence and console portions of stop/release remain blocked |
| T099–T108 (US4) | Portable metrics tests/core/web/UI/export and fake/browser verification may proceed; T107 on-device overhead/behavior and any console acceptance portions remain blocked. Browser fixtures alone do not complete all T108 obligations |
| T109–T113 | Portable parser fuzz targets and CI may proceed once their actual parser/interface prerequisites exist, under the owner's fuzzing direction; no target capability is inferred |
| T114–T121 | Engineering/release evidence, official-client matrix, soak/game/fidelity acceptance, truthful release documentation and release candidate remain blocked until their real prerequisites pass |

The current [tasks.md](tasks.md) remains authoritative for exact ownership, outputs and
predecessors except for this explicit owner authorization to advance portable work. Mixed
tasks stay unchecked until every obligation is verified. There is no blanket release from
remaining software dependencies, no authorization for concurrent hardware use, and no
permission to use the client for acceptance before T038 passes.

## Re-evaluation and verification

Re-run this decision after source-backed capability and complete on-device budget evidence
closes B01–B10. Keep B11–B14 explicit as separate reference/acceptance/release gates; a future
hardware readiness PASS cannot manufacture their completion. PASS requires every mandatory
capability and on-device budget, with no unknown domain hidden as zero. Any new spec/contract
refinement must be drafted and confirmed through the coordinator before application.

T045 changes only plan.md, research.md and this file. The task checkbox, spec/contracts and
reviewer checklists are unchanged. Read-only checklist scan found requirements.md 12/16
checked and review.md 0/64 checked; this assigned evidence reconciliation proceeds under the
explicit dispatch and makes no checklist approval claim. No extension hooks are configured.

Documentation verification commands (repository root, TMPDIR=/Volumes/4TB_Drive/tmp):

```sh
.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks
python3 /Volumes/4TB_Drive/tmp/t045-readiness/verify.py
git diff --check
```

The prerequisite check resolves this feature and required design artifacts. The local verifier
checks links, reconciliation invariants, ERROR/unchecked status, read-only boundaries and
whitespace using saved snapshots because the feature tree is currently untracked. All three
commands pass (exit 0); the verifier checks 47 local links/anchors and 14 blocker rows. The
initial verifier flagged a concurrent task-list update (T046 checked by another worker);
the final verifier preserves shared changes and checks T045 independently. Spec, contract and
checklist hashes match their snapshots. Results and document diffs are retained in
`/Volumes/4TB_Drive/tmp/t045-readiness/verification.txt`. No build/runtime behavior
changed, so no device, reference-host or unrelated implementation suite is rerun for this
documentation decision.

The 2026-10-31 reconciliation rechecked the local audio log, the three dispatch-6 payload logs,
the two completed execution summaries and the failed candidate's postflight. It updates only
the three T045 documents and leaves the task marker and all numeric/spec/contract requirements
unchanged. Current documentation verification and the exact source artifact inventory are
recorded in `.omo/evidence/t045-reconcile-20261031/verification.md`; the earlier verification
above remains a historical record of the 2026-10-25 decision.
