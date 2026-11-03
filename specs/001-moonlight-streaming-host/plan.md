# Implementation Plan: Self-Contained Moonlight Streaming Host

**Branch**: 001-moonlight-streaming-host | **Reconciled**: 2026-10-31 (T045) | **Spec**: [spec.md](./spec.md)
**Status**: **Implementation-readiness gate: ERROR — scoped SDR budget now measured, but input-delivery, pipeline-latency and soak evidence remain open.**
**Scope**: R3/R4/R5 and dependency reconciliation; [readiness.md](./readiness.md) records the decision and blocked tasks. T045 remains unchecked.
**Owner direction**: Licensing concerns are set aside for this planning run, per the owner's explicit instruction. LICENSE and the constitution are unchanged.

## Summary

Build a self-contained PS5 Moonlight host with phone-based pairing, one active session, four
controllers, SDR/HDR, and optional metrics. The independent test client is the first delivery
milestone and is validated against a reference host before it judges SunshinePS5.

The four accepted clarifications are authoritative: reachable private routed subnets are allowed;
missing required evidence makes a run Incomplete and blocks acceptance; out-of-range bitrate is
clamped to the supported boundary with requested/chosen values visible; app navigation,
submission, and confirmation count toward the three-tap limit, while typing and keyboard
dismissal do not.

R4 now establishes actual native SDR/HDR reference acquisition and calibration research, and
T043 establishes target crypto/TLS and ENet smoke feasibility. R3 establishes four neutral
controller handles, one owner-corroborated D-pad sequence, five seconds of non-silent stereo
audio and Annex-B-shaped video output for four requested 720p/1080p 30/60 modes. Dispatch 6
adds two completed ten-second rate probes, then a cleanup-completion failure requiring removal
of the exact leftover payload PID. Dispatch 7 adds tested cleanup orchestration and one completed
one-second trace: ResourceArbitrator's observed unload span was 193 us. Two subsequent bounded
ten-second reproductions completed without recurrence (ResourceArbitrator 204 us and 208 us).
The earlier intermittent
hang remains unexplained and individual platform-call completion remains unbounded.
Dispatch 8 returned HEVC Main10 configuration headers with ten-bit, cropped 1920x1080 and
BT.2020/PQ signaling; that run retained no pictures. Dispatch 9 retains 600 exact VCL records
and independently decodes them as 1920x1080 HEVC Main10 / `yuv420p10le`, establishing
configuration-header compatibility for that population. Other-mode decoded coverage,
hardware provenance, supported bitrate endpoints,
HDR/protection, complete cleanup/safety and R5 budgets remain Incomplete. These findings do
not establish a working console streaming host; [R3 findings](./research-r3-findings.md)
retains the observations and their evidence limits.

**Owner decision, 2026-10-25:** PORTABLE/fake-platform work may proceed despite ERROR:
portable portions of T046–T069, portable US3/US4 and fuzzing. This authorizes software work
and fake evidence only; console adapters, on-device evidence and release tasks remain blocked.
Preserve test-before-behavior order and interface dependencies. The task list remains the
execution authority, subject to this explicit owner direction; no unchecked gate becomes PASS.

**2026-11-01 device continuation:** The console physically resumed from the controlled
suspend. `kern.boottime` and unchanged service PIDs classified it as resume; the preserved
codex6 population passed the exact 40-entry gate, the approved verifier ran once
(`paired=1 metrics=0 pass=1`, settings 61→66) and the approved cleanup removed it. A reviewed
interactive READY → neutral → stop run completed with a continuous 45-second HDMI capture,
zero cleanup returns and an absent PID. Five additional retained-picture modes (HEVC8
1080p60, AVC 1080p60/30, AVC 720p60/30) then decoded 600/601/302/601/301 pictures with
ordered export, selector removal and clean postflight. T041/T042/T050 remain unchecked
because hardware provenance, source HDR/protection, single-controller/held-release, budgets
and reboot/power-loss durability are still open. See the
[T050 run](../../.omo/evidence/t050-resume-verify-20261101/REPORT.md),
[T042 run](../../.omo/evidence/t042-neutral-stop-device-20261101/REPORT.md) and
[T041 run](../../.omo/evidence/t041-six-mode-device-20261101/REPORT.md). A first console-only pair/web/status composition now builds and ran once on firmware 13.60 (`serverinfo`, web index, status and empty pairing JSON all 200); HTTPS pairing with a real Rust client and process-restart persistence also pass. See the [T078 console run](../../.omo/evidence/t078-console-pair-web-20261101/REPORT.md). A full diagnostic SDR host then ran a 10-second 1080p60 H.264 session (584 video frames, 1,962 Opus packets, 9 host input events imported through the session-bound JSONL endpoint; the console_diagnostic CON-input criterion passes for host receipt while delivery remains UNCONFIRMED, clean teardown) with an explicit unsourced protection CLEAR override; the client measured 57.89 fps and 2.80 Mbps, and its report remains Incomplete; a later actual game-content run reached 57.64 fps and 2.85 Mbps. Corrected bitrate units and parked-pad teardown then produced 16.52-17.54 Mbps over moving game content with no post-session controller dialog; separating audio from the video thread and fixing stdin POLLHUP pacing produced 59.61-59.72 fps on 10/30-second runs. The 2026-11-01 owner scope defers HDR/protected content; production budget events measure 0.148-0.40 idle and 0.16-0.83 thread CPU-s (2.9-5.1 RUSAGE CPU-s) per 10 s, with 146.6 MiB peak RSS. See the [T079 diagnostic](../../.omo/evidence/t079-console-sdr-diagnostic-20261101/REPORT.md).

The owner's 2026-10-31 PS5 authorization allowed renewed nonshipping R3 investigation. The
dispatch-6 candidate sequence stopped after the cleanup-completion failure. Dispatch 7 then
used a separately built one-second cleanup diagnostic; its first `-O0` attempt had no stdout
and is invalid evidence, while the `-O2` trace and two bounded ten-second repetitions completed.
Dispatch 8 subsequently ran one separately reviewed HEVC10 configuration-header candidate,
completed cleanup and retained an Incomplete result. Dispatch 9 added one bounded VCL export
and strict 600-frame Main10 decode, with zero-return cleanup and normal postflight observations.
Its [local evidence](../../.omo/evidence/t041-hevc10-vcl-20261031/REPORT.md) preserves the
remaining source HDR, protection, conversion, hardware and resource-accounting limits.
This research authorization does not close the production
or acceptance gates. T041/T045 remain unchecked and readiness remains ERROR.

## Technical Context

**Language/Version**: Host and portable core: C11. Known local console-development baseline:
public payload SDK v0.45 at a7685ce8e97ce1f8a4f38253a41d54241c9124f1 with LLVM 18.1.8.
Test client: Rust 2021, selected toolchain 1.93.0, dependencies locked by the existing Cargo.lock.
Web app: static HTML/CSS/JavaScript, with no runtime framework or externally fetched assets.

**Build entry point**: The planned `./scripts/build-ps5.sh` command must build the PS5 artifact
from a clean checkout using the pinned SDK/compiler/dependency manifest and verified archive
checksums. It must fail on missing or mismatched inputs. This script is a future deliverable;
the current Rust spike is not the host build.

**Primary Dependencies**: OpenSSL 3.5.9 at 45e844fa2a14ec92d146bd8f5778ac130b6625fb
cross-built and passed the bounded firmware-13.60 entropy/certificate/mutual-TLS/TCP/thread smoke.
Selected host ENet is cgutman/enet 1.3.17 Moonlight fork at
aca87840b57f045a1f7f9299e4b1b9b8e2a5e2f1; real target UDP exchange with rusty_enet 0.4.0
passed reliability/channel/fragmentation/disconnect checks. [T043 evidence](./research-target-dependencies.md)
and the dependency lock retain exact pins, patches and limits. Target IPv6 interoperability,
full session security/conformance, resource cost and installed-LLVM archive provenance remain
unverified. The client now locks `opus` 0.4.0 / `opusic-sys` 0.7.6 (bundled libopus 1.6.1)
and AWS-LC 1.19.0 for RSA, with legacy identity/signature compatibility checked. The latest
live archive/advisory audit completed 8 host and 238 client queries with zero matches after
the commit-only ENet inventory correction; hosted CI and live pairing remain unverified.
The client also retains FFmpeg/ffprobe. Parser constants cite the
protocol notes. The implemented external capture helper supports 0fd9:009c and stays outside
the unprivileged Rust process; external HDMI reference acquisition cannot supply console capture.

**Storage**: Atomic files for host identity, paired clients and settings; separate fixed-size
session pools for media, metrics and logs. Whole-session aggregates survive raw sample eviction.
Raw samples retain at most 600 seconds and expose their actual retained interval.
No host database, external assets, cloud analysis, or telemetry.

**Testing**: Unit coverage for every new portable-core behavior; fake-platform end-to-end sessions
through the independent client; sanitizer and fuzz jobs; official-client matrix and on-device
performance evidence. [Measurement contract](./contracts/measurement-evidence.md) defines
populations, clocks, missing evidence, and all acceptance gates.

**Target Platform**: PS5 firmware 13.60 with the existing homebrew environment; portable core on
Linux/macOS; test client on Linux/macOS; browsers listed in FR-014. The current USB fixture is
Elgato 4K X, 0fd9:009c, at 5 Gbit/s. Its P010 descriptor includes 1920×1080 at 60 fps; its
AVFoundation interface exposes only eight-bit formats. R4 subsequently acquired real P010 frames
and native SDR/HDR known-source populations; see its acquisition/calibration reports for coverage,
startup rejects, conversion error and run-specific delay bounds.

**Project Type**: Portable C core, thin console adapters, static management app, independent Rust
verification tool, and an isolated development-machine capture helper.

**Performance Goals**: Full host pipeline p99 below 1000/fps milliseconds, using the exact
negotiated interval rather than a rounded label; SC-003 also requires the Moonlight-visible
processing measure below 16.7 ms at 1080p60. Host drops below 0.1%, input receipt-to-delivery p99
below 2 ms, metrics overhead at most 2%, and two-hour stability. These are targets, not measured
achievements. Protocol processing latency and full pipeline latency are separate measurements.

**Constraints**: Fixed memory; no hot-path allocation or unbounded queues; independent input
execution; hardware video encoding; clean rollback and input release; firmware tables with
documented provenance; private-address boundary; no PSN/telemetry. The no-login decision remains
in effect. Browser-origin checks prevent unrelated web pages from issuing management changes
without introducing accounts.

**Scale/Scope**: One console, one active session, one required controller, 61 functional requirements and
14 release acceptance criteria. Keyboard/mouse input remains outside this feature, including new
test-client work. Client/helper progress is recorded in the research reports; a complete
production host and acceptance pass are not established.

**Remaining evidence and decisions**: R3 must establish usable foreground capture/hardware
encode, audio/protection, source HDR metadata/conversion, full input and lifecycle/safety paths.
R5 must verify resource/latency budgets and CPU accounting. R4 research is complete within its
recorded limitations; per-run acceptance calibration and independent source metadata are still
required. FR-057 wording (D5) and the SC-009 game rubric remain owner decisions. T038 remains
Incomplete, so the independent client may not yet accept SunshinePS5. Exact closures and task
effects are in [readiness.md](./readiness.md); no new spec/contract refinement is applied here.

### Declared resource budgets

These are provisional engineering ceilings selected for the baseline 1080p60 SDR/HDR design.
They satisfy the need for explicit targets, but **on-device verification is still absent**.

| Resource | Ceiling | Accounting / evidence |
|---|---:|---|
| Total host memory | 256 MiB | Includes host-resident allocations and attributable capture/encoder pools; record separately any driver allocation whose accounting is unavailable |
| Capture, encode, audio and transport pools | 128 MiB | Preallocated before session activation; pool and surface high-water marks |
| Metrics | 80 MiB | 64 MiB input records + 8 MiB frame records + 6 MiB histogram state + 2 MiB queues/snapshots |
| Core, TLS, web, settings, logs and stacks | 48 MiB | Includes fixed log/request buffers and thread stacks |
| Streaming CPU | One CPU-core equivalent | Sum of host thread CPU time at most 10 CPU-seconds in every ten-second observation window |
| Idle CPU, no session/browser | 5% of one core | At most 0.5 CPU-seconds in every ten-second observation window |

CPU ceilings do not authorize starving a game. Affinity and priority choices must be validated
against the foreground game; no core number or priority constant is invented here. Input never
waits on capture, encode, network send, or the metrics worker. The stricter SC-009 game-impact
criterion still applies. Missing driver accounting or infeasible ceilings blocks budget approval;
revise this plan explicitly rather than hiding the resource or substituting system-wide memory.

### Architecture and lifecycle decisions

- The portable core owns protocol, pairing, authorization, session state, metrics and persistence.
  Console adapters own capture/encode/audio/input/system services; tests substitute fakes.
- Every privileged operation is behind one minimal platform/privileged module. Firmware symbols
  and offsets belong in platform/firmware/13.60 data with source and rollback records. No guessing.
- A session reserves all pools before becoming active. Any partial start unwinds resources in
  reverse order. Every termination releases input first, removes controllers, frees media
  resources, and records a reason. App stop additionally removes installed hooks.
- Identity and pairing changes use temporary-file replacement with explicit durability/error
  handling; revocation invalidates authorization before acknowledgement and survives restart.
- Detailed timing is allocated with the session, but populated only while metrics are enabled.
  Collection can therefore be toggled without allocation on a frame/input path.
- The statistics worker aggregates every received sample before raw-ring eviction. It stores
  whole-session state independently of the ten-minute export window and publishes bounded
  snapshots; it never delays input or media threads.
- IPv4-mapped IPv6 sources are normalized before the FR-040 address rule. Accept RFC1918 IPv4,
  IPv6 unique-local sources across reachable routed private subnets, and link-local IPv6 on its
  local interface. Public sources are rejected. This does not create routing, relay or NAT rules.
- Unsupported modes are declined before activation. Bitrate is clamped independently of mode
  negotiation. Mode-specific limits must come from the capability evidence record, not arbitrary
  constants. No numeric bitrate range is claimed verified before that record exists.

## Constitution Check

Gate evaluated against constitution v1.1.0 before this revision and after draft reconciliation.

| Principle / gate | Before revision | Revised disposition and remaining evidence |
|---|---|---|
| I. Official client compatibility | PASS claimed | Contract aligned; supported-client versions frozen per release and every entry required. Runtime conformance remains a delivery gate. |
| II. Latency | PASS with dependency claimed | Distinct protocol/full-pipeline/input measurements and independent input path specified. **ERROR: hardware encode and timing evidence outstanding (R3/R5).** |
| III. Console safety and budgets | PASS without budgets | Numeric ceilings and ownership now declared; privileged module, per-firmware tables and rollback specified. **ERROR: budgets, safe scheduling, interfaces and rollback not verified on 13.60.** |
| IV. Network and pairing | Recorded no-login risk | FR-040 subnet decision, pairing checks, bounded parsers, origin checks, on-device identity and fuzz obligations explicit. No-login risk retained as accepted. |
| V. Portability and tests | Incomplete task coverage | Full unit/E2E/CI matrix allocated in current tasks; independent client remains subordinate to official clients. Fake results do not establish target acceptance. |
| VI. Provenance and truthful claims | PASS claimed | Candidate symbols and advertised formats explicitly separated from working capabilities; all absent evidence yields Incomplete. |
| Platform/licensing | Concern discovered during research | **Set aside for this planning run by explicit owner instruction.** Not a claim of completed audit or a change to repository licensing. |
| Workflow and release gates | Missing PS5/dependency/E2E jobs | Current tasks are the conditional backlog; portable work is owner-authorized, while console/release evidence remains gated. T038/FR-059 is still Incomplete. |

**Overall result: ERROR / not approved for production console-host implementation.** The
2026-10-25 owner decision permits portable/fake work as described above, without waiving any
hardware verification or acceptance criterion. R4 research and T043 have progressed; neither
substitutes for the outstanding R3 capabilities or R5 measurements.

### Mandatory verification allocation

| Area | Required work in the current tasks |
|---|---|
| Core unit coverage | HTTP/RTSP/control/input parsing; PIN proofs, expiry and cooldown; authorization/revocation races; negotiation and bitrate boundaries; controller translation; settings/identity durability and corrupt files; session partial-start/teardown; timing windows, retention, histogram overflow and toggle gaps |
| CI build matrix | Warning-free core build and unit tests on Linux and macOS; PS5 artifact build with pinned target toolchain; locked test-client build and tests |
| CI quality | clang-format/clang-tidy and rustfmt/Clippy; C host ASan/UBSan; dependency audit and the required license-job allocation, with owner-directed licensing deferral recorded rather than a fabricated result |
| End-to-end CI | Pair, launch, stream, input, revoke, failure cleanup and teardown through core + fakes + independent client; execute these in CI, not merely create test files |
| Fuzz CI | Every HTTP/HTTPS, RTSP/SDP, control and input parser; bounded per-PR runs and longer nightly runs |
| Console PR evidence | Firmware, client/version, mode/codec, before/after stage timings; >5% p99 regression blocks merge absent the constitution's documented review exception |
| Release evidence | Every SC-001–SC-014, all supported official clients including HDR devices, two-hour soak, required diagnostics, checksums and known issues |

Timing or memory results from fakes never establish console performance. Every bug fix requires
a regression test. CI jobs must fail on missing required jobs/evidence, rather than accepting
skipped or empty results as success.

## Project Structure

### Documentation

    specs/001-moonlight-streaming-host/
    ├── plan.md
    ├── research.md
    ├── data-model.md
    ├── quickstart.md
    ├── contracts/
    │   ├── moonlight-protocol.md
    │   ├── web-app-api.md
    │   ├── test-client-cli.md
    │   └── measurement-evidence.md
    ├── checklists/
    │   ├── requirements.md
    │   └── review.md
    ├── readiness.md
    └── tasks.md  (current conditional execution backlog)

### Intended source layout

    core/{protocol,session,input,metrics,settings}/
    platform/{capture,encode,audio,input,system,privileged,firmware}/
    webapp/
    tools/moonlight-mcp/{src,docs}/
    tools/hdr-capture/             (isolated development-machine helper)
    tests/{unit,e2e,fakes,fuzz,scenarios}/

Setup, client, helper and nonshipping probe work now exists. The source layout still includes
planned host modules and does not claim a functioning production host. Each implementation
task owns its listed paths; no generic framework is introduced for hypothetical features.

## Dependency order and evidence gates

Use these milestones with the current task dependencies and the explicit portable-work direction.

| Milestone | Prerequisites | Exit condition |
|---|---|---|
| Research closure | Existing native development evidence and attached capture fixture | **ERROR:** R4 research and T043 complete; R3 mandatory capabilities and R5 remain Incomplete |
| Test client | Portable build/test scaffold | T038/FR-059 still Incomplete: recording/fidelity/HDR and complete reporting evidence missing; no dependency on a functioning SunshinePS5 host |
| Core and fakes | Narrow interface definitions and tests first; owner-authorized despite ERROR | Portable T046–T069 unit/E2E coverage may proceed; no target mode/budget claim or independent-client acceptance before T038 |
| Pairing/web shell | Core protocol, identity and authorization | Official-client pairing, routed-private address policy, single-session admission and phone PIN flow |
| Console SDR/input | R3 capability decision, reserved pools, timing controls and validated test client | Console capture/encode/audio/input with clean teardown and on-device budget evidence |
| HDR | Verified ten-bit console path, actual reference capture, source metadata evidence | Four console/client HDR combinations, transitions and complete SC-012/SC-013 evidence |
| Management/metrics | Session state and metrics engine; portable US3/US4 owner-authorized | Portable behavior/UI and fake integration may proceed; console integration and on-device acceptance remain blocked |
| Release | All five user stories, every evidence gate | Official-client matrix, two-hour soak and complete required verdicts |

The readiness ERROR blocks production-host use of T046–T121, with the bounded portable-work
authorization recorded above and detailed in readiness.md. It does not block reference-host
research. Portable implementations must consume explicit fake capabilities rather than invent
console mode/bitrate bounds, delivery semantics or budgets. Mixed portable/platform tasks are
not complete until their target obligations pass. Interface consumers follow interface
definitions. Phone acceptance follows the metrics toggle as well as pairing/revocation.

## Phase 1 draft outputs and remaining closures

[Data model](./data-model.md), [contracts](./contracts/measurement-evidence.md), and
[quickstart](./quickstart.md) remain design documents; earlier planning-pass descriptions are
not current hardware verdicts. The reconciled evidence and remaining closures are:

1. **R3 Incomplete**: build on the observed audio/video populations and dispatch 9's decoded
   Main10 population to establish remaining mode coverage/hardware provenance, bitrate bounds,
   independent source HDR metadata/conversion,
   protection, complete controller delivery/coexistence and verified teardown on firmware 13.60.
   Dispatch 6's cleanup failure remains unresolved by killing its leftover process. The bounded
   T042 held-Right run establishes returned neutral-before-remove cleanup plus visible navigation
   and disconnect UI, but the rightmost menu boundary prevents a visible release claim.
2. **R4 complete as research (T032/T034 checked)**: actual P010 acquisition, native SDR/HDR
   source error and conservative paired delay bounds recorded. Physical-disconnect recovery
   is owner-deferred, untested and non-blocking for T032. Exact HDMI exposure, independent
   mastering/light metadata and future mode/platform coverage are not established.
3. **T043 complete**: target OpenSSL and selected ENet smoke passed; full production/IPv6
   conformance, allocation costs and release reproducibility still require their later checks.
4. **R5 ERROR (T044 unchecked)**: verify the unchanged numeric ceilings, real stage/input
   boundaries, bitrate endpoints and foreground-game impact. The
   [two-thread PS5 observation](../../.omo/evidence/t044-two-thread-device-20261031/result.md)
   establishes concurrent thread-local CPU distinction for two owned workers with swapped
   active/sleep roles. The later
   [shared-window diagnostic](../../.omo/evidence/t044-common-window-device-20261031/REPORT.md)
   quantifies 10,001,177,603 ns of guaranteed common coverage around a coordinator-defined
   ten-second target, with bounded edge skew. Its enclosing two-worker CPU sum is not an exact
   common-window total; production thread census, ten-second host budgets, daemon/driver CPU
   and media/input populations remain unresolved.
   The elapsed-like process clock remains rejected for aggregate CPU accounting.
5. **Approved T034 refinements already applied**: SC-014 median luma SSIM >=0.95; SC-013
   median >=0.97, joint 99% SSIM >=0.95 and Y/Cb/Cr PSNR >=35/40/40 dB, plus the native
   ramp/patch rules in the spec. All existing requirements remain. No additional refinement
   is made by T045; any proposed change needs a literal draft and owner confirmation first.
6. **Independent acceptance**: complete T038 reference recording/report evidence, settle
   FR-057 D5 wording, agree the SC-009 rubric and supply the later official-client/game/soak
   evidence. The [readiness blocker register](./readiness.md#blocker-register) maps each gap
   to tasks and closure evidence. Do not regenerate or rewrite the authoritative task list here.
7. **Portable follow-up remains bounded**: race-safe T038 observer publication, manifest-backed
   post-cancellation controller import from checksum-verified snapshot bytes, an optional
   pre-launch READY barrier with exact run binding, a maintained
   read-only evdev collector/conservative publisher with portable CI and an exact-binding inherited
   finish channel for bounded post-cancellation publication, native atomic no-replace runner
   binding publication and an immutable cancellation signal with one absolute import deadline,
   a local process supervisor, a nonce-bound postlaunch barrier before RTSP/input and strict
   supplied runtime attachment retention,
   complete T039 Python-suite
   discovery, the local six-mode T041 export matrix, T042 neutral-observation hardening, a
   bounded explicit-command interactive helper, neutral/stop coordinator and opt-in command
   adapters with continuous capture, independent decode and partial failure-footage retention,
   T050 POSIX failure/probe coverage, an inspection-only one-shot recovery coordinator and
   approved fixed-endpoint recovery adapter mechanics, conservative diagnostic live components
   and a read-only target inspector with a locally verified full-CRT launcher,
   T094 revocation cleanup retry and T096 fake
   stop retry reduce software uncertainty. They do not close calibrated reference acquisition,
   target durability, console input timing, production cleanup or active-session evidence.
   T038 now has an approved opt-in one-shot device feed binding the exact runtime/launch identity
   and opening only supplied exact paths. Its `late_attach` precedes later decoded events;
   early coverage remains unavailable, continuity unverified, and finish without a feed is bounded.
   The approved [Fenrir provider](../../.omo/evidence/t038-fenrir-provider-20261031/REPORT.md)
   ([review](../../.omo/evidence/t038-fenrir-provider-code-review.md)) and
   [SSH subsystem relay](../../.omo/evidence/t038-remote-relay-20261031/REPORT.md)
   ([review](../../.omo/evidence/t038-remote-relay-code-review.md)) are implemented and
   fixture-tested. The approved
   [deployment gate](../../.omo/evidence/t038-deployment-preflight-20261101/REPORT.md)
   ([review](../../.omo/evidence/t038-deployment-preflight-code-review.md)) additionally pins the
   direct subsystem's account, source/import identities, executables, private root, kubeconfig and
   server policy without installing them. Baseline-before-READY, numeric HTTPS proxy routing,
   exact runtime/nonce binding and immutable cross-host transfer now have retained tests. The
   [live deployment audit](../../.omo/evidence/t038-g913-relay-live-audit-20261031.md)
   confirms current-user verified Kubernetes TLS/read/proxy access and zero Sessions, but
   the dedicated relay account/key, host-key/code deployment, sshd subsystem, private root
   remain unconfigured. A regular fixed-path `k3s-kubectl.sh` wrapper is now fixture-tested,
   but its installation and validation under the dedicated account remain outstanding,
   including `/bin/sh`, delegated `/usr/local/bin/k3s`, parent integrity and live compatibility.
   Dedicated-account RBAC, self-signed
   backend proxy handling, populated runtime, authenticated agent/event ownership, subscription
   continuity, independent output/audio/HDR/fidelity/cleanup and official-client checks remain
   unverified. Local attachment accepts zero initial devices so controller arrival
   can create pads after RTSP. The deployed
   agent's session/path/major/minor/generation fields support exact supplied device mapping, but
   expose no subscription history. The ten-second
   cancellation bound confirms only the request outcome: deployed Fenrir acknowledges foreground
   deletion without awaiting resource removal. Its separate ninety-second wait applies to
   launch-time prior-session cleanup. Later pod disappearance or supplemental artifacts cannot
   upgrade a false cancellation signal. The
   T042 candidate initially remained undispatched while a title was active. After a separately
   reviewed helper closed that exact game, one no-game diagnostic completed ordinary cleanup,
   but Settings launch failed, exact-PID TERM was ineffective and HDMI coverage of Down/dwell/
   removal was not established. The new interactive helper passed host, sanitizer, target-build and
   portable CTest gates. Its reviewed coordinator and command adapters retain continuous host
   frame receipts and partial footage after failures, with independent decode and hashed manifests.
   Live inspection bindings, camera/TCC permission, loader transport, source-clock continuity and
   visible release remain unverified; the new helper/adapters have not run on the console. T042 remains
   Incomplete. T050's target syscall error translation
   is now fixed: codex6 init and fresh-process verify preserved identity,
   settings and registry through a second replacement, then exact cleanup removed the probe
   state. The active-game constraint is now cleared. A new init at
   `/data/ps5-dev/t050-20261031-codex6` and a separately reviewed verifier bound to its fingerprint
   are preserved for the current durability attempt. Suspend was requested and HDMI plus all five
   listeners disappeared; two bounded WOL attempts did not restore them. A later read-only Remote
   Play discovery response identified the console as `Server Standby` on firmware `13600007`.
   The [latest rest check](../../.omo/evidence/t050-rest-device-20261031/rest-attempt.md) at
   `2026-11-01T00:31:59Z` still shows NO SIGNAL; the [fresh discovery](../../.omo/evidence/t050-rest-device-20261031/discovery-20261101T011503Z.log)
   at `2026-11-01T01:15:03Z` returned `HTTP/1.1 620 Server Standby` for host
   `00E421FDEC6B` on firmware `13600007`, with exploit/development-service ports absent. A read-only
   [vphone check](../../.omo/evidence/t050-vphone-wake-recheck-20261101/REPORT.md) found no
   installed PS Remote Play or Chiaki client and established no registered wake route. No
   verifier or cleanup was dispatched. Resume and persistence remain unverified. The reviewed coordinator
   pins the exact population/artifacts and makes any injected dispatch durably non-retryable.
   The approved [recovery adapter](../../.omo/evidence/t050-live-adapter-20261031/REPORT.md)
   ([review](../../.omo/evidence/t050-live-adapter-code-review.md)) implements fixed-endpoint
   acquisition/transfer mechanics, independent exit gating and separate cleanup arming, with
   188 offline tests. Approved conservative live-acquisition components add bounded diagnostic
   service/host observations and a digest-bound classifier seam for HDMI; 142 tests pass, but no
   classifier ships and none of those diagnostics can arm recovery. The approved read-only target
   inspector now has a [locally verified launcher](../../.omo/evidence/t050-recovery-launcher-20261101/REPORT.md)
   that checks exact firmware `0x13600007` before observation and links the pinned full SDK CRT;
   host regressions, target ELF inspection and independent code review pass. The CRT performs
   process-local credential/syscall bootstrap before this gate. The [exact-loader audit](../../.omo/evidence/t050-relapse-loader-compat-20261101/REPORT.md)
   establishes distribution provenance and a byte-identical SDK v0.45 full-CRT verifier that
   historically executed through this loader on the same 13.60 console. Five CRT functions
   match the inspector after masking only pinned object relocation fields, supporting the
   historically exercised full-CRT ABI class. The inspector's 23 GLOB_DAT imports and one
   R_X86_64_64 relocation, current-boot loader continuity, inspector execution/output and
   independent exit remain unverified; exact loader build provenance remains unresolved.
   Atomic-snapshot proof, process/game/writer evidence, a reviewed public-certificate fingerprint
   path and independent exit observation remain absent. No inspector payload was dispatched.
   Its seven live callbacks (services, boot, process, isolation, HDMI, inventory and exit observer)
   and a genuine pre-rest boot identity therefore remain unbound. Retained service PIDs cannot
   prove continuity; the coordinator CLI remains inspection-only and no standalone dispatch CLI
   exists. On recovery, do not reinitialize this population:
   inspect it first, and dispatch the approved verifier only if the expected state survived. Reboot and power-loss
   evidence remain absent; unsupported target link fixtures are explicit skips. Opus/RSA remediation and the ENet audit
   correction produced a
   complete live audit with zero advisory matches, without closing hosted CI, live pairing or
   release provenance. T056 remains FAIL at 5/6 despite one passing repeat after the post-restart
   Qt abort. A later diagnostic establishes an upstream official-client CLI lifetime defect but
   does not prove it caused that abort or justify a host workaround. A session-observed GUI
   diagnostic pairs, lists PS5, exits normally and reuses the portable identity after a clean
   restart, but lacks retained raw transcripts and does not alter the CLI FAIL or supply console
   durability/timing. These mixed tasks stay unchecked;
   see the [retained follow-up evidence](research.md#reference-portable-and-management-follow-up-t038t039t041t042t050t056t094t096-incomplete).

## Complexity Tracking

| Item | Disposition | Follow-up |
|---|---|---|
| Licensing review | Owner explicitly instructed this planning run to ignore the issue | Recorded as an owner scope override, not a compliance PASS; no license or constitution edit |
| Hardware/budget gates | No exception requested or assumed | R3/R4/R5 evidence required; retain ERROR until closed |
| Portable work while ERROR | Owner explicitly authorized 2026-10-25 | Portable T046–T069, portable US3/US4 and fuzzing may proceed; no waiver of console, evidence or release gates |
