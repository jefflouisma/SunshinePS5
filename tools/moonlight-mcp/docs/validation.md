# Independent reference-host validation (T038 / FR-059)

**Incomplete: T038 remains unchecked; this client must not accept SunshinePS5 yet.**

Software follow-up (2026-10-31): the runner now accepts `--reference-lifecycle FILE`
for independently collected pairing and teardown observations with session/scenario/version
correlation and SHA-256-bound source retention. The [snapshot contract](reference-lifecycle.md)
defines the required proof-rejection, official-client and complete cleanup checks.
Synthetic contract tests verify import, provenance, failure precedence and quota behavior;
they do not supply live reference acceptance. No historical report below is reevaluated.
Native calibrated SDR/HDR recording/comparison and the complete report correctness evidence
remain outstanding, and T038/T039 remain unchecked.

The new `--reference-recording FILE` [evaluator](reference-recording.md) validates
hashed native SDR/HDR and known-difference populations, recomputes their plane
scores, checks calibrated alignment and source/signaled metadata, and compares
complete emitted reports with independent expected reports across Pass, Fail,
Incomplete and preserved-Fail cases. It retains sources before promotion. The
portable tests are synthetic; no live calibrated population or historical report
has been promoted by this change.

The 2026-10-31 synchronization follow-up closes a runner race without changing this
acceptance result. After successful cancellation, lifecycle and recording bindings share
one ten-second publication deadline. Only the top-level manifest's own absence or JSON EOF
is retryable; complete invalid/foreign manifests and missing/truncated nested artifacts are
terminal. Each loader validates the one parsed manifest object and retains already-checked
source bytes, so later path replacement cannot change the accepted snapshot. Failed
cancellation imports once without waiting. Nine focused regressions, the complete 214-test
suite and independent review pass; all sources are synthetic and establish no live evidence.
See the [implementation report](../../../.omo/evidence/t038-observer-sync-20261031/REPORT.md).

The later controller integration uses that same deadline after cancellation. When both controller
JSONL and lifecycle bindings are supplied, the controller path must name a manifest artifact and
is evaluated from the lifecycle loader's already checksum-verified bytes. Provenance, session,
quota and every retained artifact reference must validate; missing, invalid, foreign, tampered or
unbound data remains Incomplete with no independent-file fallback. Replacement after validation
cannot change the evaluated snapshot. Standalone JSONL remains an explicit legacy path without a
publication wait or manifest binding. The synthetic regression matrix passes; it does not create
authenticated live controller evidence. See the
[controller integration report](../../../.omo/evidence/t038-controller-manifest-20261031/REPORT.md).

The optional `--observer-ready FILE` barrier covers the start boundary. After pairing and host-version
discovery but before constructing the launch future, the runner retains an exact binding of the
run/session/scenario IDs, streamed scenario digest, versions and a fresh nonce. A configured run
waits at most ten seconds for one matching bounded regular-file READY envelope and retains its exact
bytes before launch. Stale, linked, special, malformed, oversized, foreign, cancelled, timed-out or
quota-exhausted inputs prevent launch and finalize Incomplete; an unconfigured run preserves the
prior behavior. This coordinates an external collector but does not authenticate it or promote any
criterion. See the
[readiness-barrier report](../../../.omo/evidence/t038-readiness-barrier-20261031/REPORT.md).

The collector's opt-in READY publisher now reads the runner binding once with
bounded no-follow semantics, cross-checks supplied identities/versions, requires
an empty initial inventory, and atomically publishes without replacing an existing
destination. The file is fsynced and closed before visibility. Directory fsync is
attempted afterward and its result is reported in stdout READY, but a failure cannot
roll back the visible envelope or abort acquisition. The
[publisher verification](../../../.omo/evidence/t038-ready-publisher-20261031/REPORT.md)
includes an offline Python-to-Rust barrier test: the envelope is accepted and
retained before launch while every criterion stays Incomplete. The runner's
scenario digest is echoed as an opaque token, not recomputed by Python.

The same collector now has an opt-in inherited-pipe finish channel. One bounded JSON line plus EOF
must contain the exact runner binding; after READY and host cancellation it requests a final
inventory/read boundary and prompt manifest-last publication. Present devices and acquisition or
control errors remain explicit, and only a clean bound request reports `finish_status: finished`.
The [finish-protocol verification](../../../.omo/evidence/t038-finish-protocol-20261031/REPORT.md)
runs 55 Python cases, real inherited-FD subprocesses, six Rust readiness regressions and six
populations through the actual Rust consumers. It supplies coordination only, not authenticated
ownership or live acceptance.

The runner's trusted startup option `--observer-finish-signal` requires `--observer-ready`
and publishes a reserved, immutable `observer-cancel.json` from the exact retained
in-memory binding after cancellation. Its `request_succeeded` result precedes observer
import: true permits a supervisor to send finish JSON plus EOF; false/error/timeout
requires EOF without finish, yielding collector control error. Signal publication and
both imports share one absolute deadline; failed cancellation imports immediately,
and journal quota failure cannot suppress import. Binding publication now also uses native
atomic no-replace rename, eliminating the temporary two-link/ctime window rejected
by the Python collector. See the [runner signal report](../../../.omo/evidence/t038-runner-cancel-signal-20261031/REPORT.md)
and [independent review](../../../.omo/evidence/t038-runner-cancel-signal-code-review.md)
for executable publication, quota, binding-replacement and Python finish-pipe checks.

The runner bounds cancellation at ten seconds. In deployed Fenrir commit
`5c8251c8a7a547466226c5f08f72d94a5de05dd0`,
[cancel](https://github.com/jefflouisma/homelab/blob/5c8251c8a7a547466226c5f08f72d94a5de05dd0/fenrir/pkg/moonlight/rest.go#L708-L716)
calls `beginStopSessionsForUser` and acknowledges foreground deletion requests; Kubernetes
cleanup can continue afterward. A true signal therefore cannot establish resource or pod
removal. The separate ninety-second wait in `stopSessionsForUser` applies to launch-time
prior-session cleanup, not cancel. False/error/timeout permits only immediate import;
later pod disappearance cannot upgrade the immutable signal, and later manifests remain
supplemental diagnostics. Authenticated ownership and live acceptance remain open. See the
[exact-source audit](../../../.omo/evidence/t038-fenrir-cancel-provenance-20261031.md).

An opt-in `--observer-attachment` startup binding now provides a second bounded barrier
after successful launch and before RTSP/input. It retains one exact-binding, explicitly
unauthenticated attachment envelope and opaque runtime-payload digest. A privately generated
nonce is published only after successful launch in reserved atomic `observer-launch.json`;
the attachment must echo it. A stale attachment is refused before launch, and a raced
prelaunch envelope cannot satisfy the new nonce. Invalid, missing,
cancelled or quota-exhausted attachment follows the existing host cancellation path.
Synthetic ordering and refusal tests are retained in the
[attachment-barrier report](../../../.omo/evidence/t038-runner-attachment-20261031/REPORT.md).
The [final review](../../../.omo/evidence/t038-runner-attachment-code-review.md) approves the
freshness/nonce correction; this coordination check cannot promote an acceptance criterion.

The [local supervisor](../../../.omo/evidence/t038-local-supervisor-20261031/REPORT.md) now
owns the collector process, bounds logs and lifetime, translates exact cancellation signals and
checks manifest/source snapshots. Its [re-review](../../../.omo/evidence/t038-local-supervisor-code-review.md)
approves the uncertain-close correction that preserves child cleanup and final reporting.
The [runtime attachment slice](../../../.omo/evidence/t038-runtime-attachment-20261031/REPORT.md)
validates a supplied Session → Deployment → ReplicaSet → pod chain, container identities,
source hashes and exact device paths against the postlaunch nonce. Prelaunch observer identity
is separate from the future pod. Exact deployed Fenrir `5c8251c`
[retains](https://github.com/jefflouisma/homelab/blob/5c8251c8a7a547466226c5f08f72d94a5de05dd0/fenrir/pkg/controllers/agent_input.go#L14-L22)
device `session_id`,
`path`, `major`, `minor` and `generation`; a different local checkout's missing session field
does not apply. Empty initial device inventories permit RTSP/controller arrival without a
startup cycle. The
[runtime re-review](../../../.omo/evidence/t038-runtime-attachment-code-review.md) approves that
bounded behavior. The approved [one-shot device feed](../../../.omo/evidence/t038-late-device-feed-20261031/REPORT.md)
then binds exact runtime/launch identity and opens only supplied exact paths. It records
`late_attach` before later decoded events, preserves unavailable early-event coverage and
unverified continuity, and finishes without hanging when no feed arrives. Its
[review](../../../.omo/evidence/t038-late-device-feed-code-review.md) approves this local contract.
The approved [Fenrir provider](../../../.omo/evidence/t038-fenrir-provider-20261031/REPORT.md)
and [HTTPS correction review](../../../.omo/evidence/t038-fenrir-provider-code-review.md)
cover baseline-before-READY, exact owner-chain selection and numeric HTTPS pod-proxy acquisition
with runtime revalidation. Frozen provider source SHA-256 is
`17d9d47ea5f3682e89b629d4930393295cfde292ca980b4f7bb74edf81e15f7f`;
the retained final attempt has 109 Python tests and 19 exit-zero stages.
The approved [SSH subsystem relay](../../../.omo/evidence/t038-remote-relay-20261031/REPORT.md)
and [final review](../../../.omo/evidence/t038-remote-relay-code-review.md) cover pinned host-key
and source-digest negotiation, exact binding/nonce checks, immutable artifact transfer and bounded
owner cleanup. Frozen relay SHA-256 is
`e8e3ca77fb33744029da3d92be38efeb25d2eb3e2b074643e30b221863cf5be5`.
Both implementation and independent full verification retained 23 exit-zero stages, 125 Python
tests and actual Rust imports of true-cancel, false-cancel and 36,929-byte collector populations.
All transport/acquisition populations are synthetic. Receipts establish owner-process reaping
only; connection loss leaves remote cleanup unconfirmed. Every acceptance observation remains
unavailable, and neither cancellation result can upgrade it.

The follow-up process-group audit corrected the Fenrir provider, supervisor, relay and shared
interactive transport so each checks direct-child liveness before every signal, stops escalation
after observed exit and reports descendant cleanup as unconfirmed. Real-subprocess regressions
cover ordinary/nonzero exit, graceful TERM/SIGINT and forced fallback. After the READY fixture's
publication was made atomic and relay exit state made consistent per artifact scan, the current
combined verifier passed 151 Python tests and all 23
retained stages; its source manifest includes the fixed k3s kubectl wrapper. This is portable
cleanup/deployment evidence, not live acceptance.

The [read-only g913 deployment audit](../../../.omo/evidence/t038-g913-relay-live-audit-20261031.md)
at `2026-11-01T00:26:43Z` found user `jeff`'s mode-0600 kubeconfig selects verified HTTPS at
`127.0.0.1:6443`, with no insecure/proxy setting. Required Sessions/deployments/ReplicaSets/pods/
nodes/namespaces/pods-proxy `auth can-i` checks all returned yes; zero Sessions existed.
Standalone kubectl, deployed provider/relay modules in the bounded home search, the relay
sshd subsystem and a restricted relay credential/account were absent; the current key is plain.
No mutation occurred. A regular fixed-path `k3s-kubectl.sh` wrapper is now fixture-tested but
not installed or validated there; `/bin/sh`, delegated `/usr/local/bin/k3s`, parent integrity
and live compatibility remain outstanding. This narrows current-user TLS/RBAC uncertainty, but dedicated account/key,
host-key/code/private-root deployment, account-specific RBAC, self-signed backend proxy handling
and populated runtime remain unverified. Authenticated agent/runner-to-Session/event ownership,
subscription continuity, independent output/audio/HDR/fidelity/cleanup and official-client checks
also remain open.

The approved [T050 recovery mechanics](../../../.omo/evidence/t050-live-adapter-20261031/REPORT.md)
and [descriptor-fix review](../../../.omo/evidence/t050-live-adapter-code-review.md) retain 188
offline tests with zero failures/errors/skips. Current adapter SHA-256 is
`b2f85da027bba93f1c0a1d282beed61fd2686a11ddbe256c386d3fab42671143`.
Fixed-endpoint discovery/transfer, independent exit gating and separate cleanup arming exist,
and the approved conservative live-components layer adds bounded diagnostic service/route
acquisition plus a source-digest-bound HDMI classifier seam. Its 142 focused tests pass, but no
classifier ships and diagnostics never become recovery proof. The approved target inspector
collects bounded boot-time/no-follow inventory data and now has a
[locally verified launcher](../../../.omo/evidence/t050-recovery-launcher-20261101/REPORT.md).
It links the pinned full SDK CRT and checks exact firmware `0x13600007` before observation;
host regressions, target ELF inspection and independent code review pass. CRT process-local
credential/syscall bootstrap precedes the gate. The [exact-loader audit](../../../.omo/evidence/t050-relapse-loader-compat-20261101/REPORT.md)
establishes distribution provenance and a byte-identical SDK v0.45 full-CRT verifier that
historically executed through this loader on the same 13.60 console. Five CRT functions match
the inspector after masking only pinned object relocation fields, supporting the historically
exercised full-CRT ABI class. Inspector-specific behavior for 23 GLOB_DAT imports and one
R_X86_64_64 relocation, current-boot loader continuity, inspector execution/output and independent
exit remain unverified; exact loader build provenance remains unresolved.
The observer retains false/null firmware-validation, atomic,
process, game and writer claims. A reviewed public-certificate fingerprint path and independent
process-exit evidence remain absent. Seven live
callbacks (services, boot, process, isolation, HDMI, inventory and exit observer) and a genuine
pre-rest boot identity therefore remain unbound. Numeric service PIDs cannot prove continuity.
The [latest rest attempt](../../../.omo/evidence/t050-rest-device-20261031/rest-attempt.md)
retains NO SIGNAL at `2026-11-01T00:31:59Z`; [fresh discovery](../../../.omo/evidence/t050-rest-device-20261031/discovery-20261101T011503Z.log)
at `2026-11-01T01:15:03Z` returned `HTTP/1.1 620 Server Standby` for host `00E421FDEC6B`
on firmware `13600007`, with exploit/development-service ports absent.
A read-only [vphone check](../../../.omo/evidence/t050-vphone-wake-recheck-20261101/REPORT.md)
found no installed PS Remote Play or Chiaki client and established no registered wake route.
No verifier, cleanup or inspector payload was dispatched. Preserve the fresh codex6 population
without reinitializing it before inspection. T050 and T038 remain unchecked; readiness and T045 remain ERROR.

The approved T038
[deployment gate](../../../.omo/evidence/t038-deployment-preflight-20261101/REPORT.md) binds the
dedicated Linux account, root-owned hash-pinned source without bytecode/import shadows, literal
executables, private session root and kubeconfig, and rejects client root/provider overrides.
The current complete reference-observer rerun passed 151 Python tests and all 23 retained stages. No
server installation, dedicated-account RBAC/backend proxy check or populated live Session occurred,
so this remains portable deployment preparation rather than T038 acceptance.

A fresh read-only g913 audit at 2026-10-31T20:02:29Z found no Session CRDs in namespace
`house` and no Alex or Jeff workload to preserve. The deployed Fenrir operator/proxy images
were `5c8251c`; the only declared application was EmuDeck ID 2, image `e543911`, with
`isHDRSupported:false`. No pairing, session, application or cluster state was mutated. This
deployment can support neither the required HDR population nor a complete four-population
recording/report oracle, so an SDR-only rerun would not close T038. The exact inventory and
limits are retained in
[the live audit](../../../.omo/evidence/t038-reference-host-live-audit-20261031.md).

T039 is implemented and verified locally, but remains unchecked pending T038. The portable
workflow discovers every Python unit suite, requires both schema and release-evidence coverage,
and no longer carries an obsolete continue-on-error pending-core step. Observations below were
made on 2026-10-25 UTC, using macOS arm64 and the existing g913 installation.
Software fixtures, actual reference-host observations and missing acceptance evidence remain
separate. No PS5 or Elgato device was used.

## Declaration and exact environment

The retained [reference scenario](../../../tests/scenarios/reference-host.json) requests
1920x1080 at 60 fps, H.264 SDR, 20,000 kbit/s and ten seconds. Controller 0 connects,
deflects the left stick, returns to neutral and disconnects. The follow-up declares the
independent response at Wolf's Linux evdev delivery boundary; no rendered application
response is claimed. All six reference-profile criteria remain required. Host-output recording is required but has no available device
binding or calibration; SDR execution does not establish HDR acceptance.

- g913: Ubuntu at 192.168.1.10, accessed through key-only SSH alias `g913`. Read-only
  Kubernetes inspection used `/usr/local/bin/k3s kubectl --kubeconfig="$HOME/.kube/config"`.
- Entry point: Fenrir `house/moonlight-proxy`, 192.168.1.221, TCP 47989/47984. Its serverinfo
  identifies Direwolf with compatibility appversion `7.1.431.-1`; this is not a Wolf release.
  Codec mask 65793 (0x10101) advertises H.264/HEVC/AV1 eight-bit, without ten-bit capability.
- Fenrir proxy image `ghcr.io/jefflouisma/fenrir-moonlight-proxy:5c8251c`, running digest
  `sha256:654e5d7f604dfaf26bd79b9da44c131d6d192ec0d29eaaff0f9c26f1e35307e9`.
- Fenrir operator image `ghcr.io/jefflouisma/fenrir-operator:5c8251c`, running digest
  `sha256:047f5f195672b5ac2a987f6ea9604e9a9c9beb715d71b0249643774b53d2239e`.
- Wolf image `ghcr.io/jefflouisma/fenrir-wolf:546a17d`, active test-pod digest
  `sha256:86159679457592b8fd2d10d4af34e5df961f9bbd87c2ae45f838c50f45e19d03`.
  This exact image identity is established; an upstream release number was not inferred.
- Wolf agent image `ghcr.io/jefflouisma/fenrir-wolf-agent:5c8251c`, active test-pod digest
  `sha256:65b29677b7c05446b844d5be88cb7f8cf8bd3d4363e9144c8ef94386d9ce4803`.
- Test application: Cove ID 4, image `ghcr.io/jefflouisma/fenrir-cove:af20c0f`, active digest
  `sha256:fc2c5ad6a76fcf0c7259d3548f19a0f1919593758cf615037c06920e30b2f9a4`.
  EmuDeck ID 2 was listed, not launched. The active image snapshot is retained locally.
- Test client package 0.2.0; Rust 1.93.0; FFmpeg/ffprobe 8.0.1; system Opus 1.5.2.
  Repository base `d494b30d8ebebdf11ad248a2fb08849c8236def8` plus uncommitted implementation;
  the local verification manifest records hashes of changed source and the client binary.
- Official client: `/Applications/Moonlight.app`, version **6.1.0**. Its pinned source
  reference is Moonlight Qt `f786e94c7b2f943e24e65d7d74deb539b827fc84`.

## Live results and remaining criteria

| Criterion | Latest result | Evidence and remaining gap |
|---|---|---|
| REF-pairing | Pass (declared live matrix) | Rust and official 6.1.0 wrong-PIN rejection; live host rejection of bad hash, bad client signature and truncated proof without persisted pairing; rejection of four altered live server responses; successful independent identities and fresh-process app-list/stream reconnects. The detailed matrix below states proof directions and limits. |
| REF-stream | Pass | Follow-up observed-run3 retained 595 native 1920x1080 eight-bit BT.709 frames at about 60.087 fps and decoded 1,995 stereo Opus packets with zero decode failures or observed sequence loss. Requested/announced bitrate is separate from measured throughput; encoder-chosen bitrate is unavailable. |
| REF-controller | Pass (host OS delivery) | Read-only Wolf evdev observation recorded ABS_X=8192 for x=0.25, then ABS_X=0, followed by device removal. This proves Wolf applied the declared input at its virtual controller boundary; application rendering and console injection are unobserved. |
| REF-teardown | Pass (independent follow-up) | Scripted disconnect removed the virtual controller (ENODEV); pinned TLS status polling observed busy/game 4 transition to free/effective game 0 after cancellation. Detailed cancellation/resource evidence below distinguishes delayed host cleanup from the runner's immediate unconfirmed result. |
| REF-recording | Incomplete | Headless Wolf cannot supply independent host-output recording, native SDR/HDR reference comparison, calibrated alignment or source HDR metadata. This criterion remains required; no PS5/Elgato device was used. |
| REF-report | Incomplete | Actual reports preserve all six criteria and missing values, exiting 2 overall. Independent live input/cleanup observations are supplemental, not imported into the runner report; complete fidelity/HDR evidence remains absent. Synthetic outcome tests are separate. |

The latest audio population was **digitally silent** (zero nonzero samples in both channels),
not non-silent game audio. Early observations without negotiated audio decryption were invalid:
1,912/1,984 decode failures. They are retained as failed observations, not used for acceptance.
SDR control observations contained no HDR transition; that is not evidence of HDR correctness.
Host latency/drops cannot be inferred from the observed client frame population.

## Earlier official-client cross-check

Official Moonlight ran in a task-local `portable.dat` directory with the isolated test identity
and current proxy certificate/UUID. Normal Moonlight preferences and unrelated running Moonlight
processes were not edited or controlled. Initial app-list attempts had stale portable metadata
and failed/unpaired/timed out; those were local setup failures, not host compatibility results.
After correcting the isolated settings, official `list` exited 0 and listed Cove and EmuDeck.

The official stream used `--1080 --fps 60 --bitrate 20000 --video-codec H.264 --no-hdr
--no-yuv444 --audio-config stereo --display-mode windowed --no-quit-after`, selecting Cove.
The retained log records launch, RTSP, control/video/audio startup, first video/audio packets,
1920x1080x60 video, stereo audio and final **60.00 fps incoming, decoding and rendering**.
The task-owned process was terminated at 45 seconds; startup took approximately 23 seconds.
The accessibility tree changed from “Starting Cove...” to a Direwolf stream window. The only
successful window screenshot shows startup, not rendered content; a later screenshot attempt
raced window termination. No picture-quality or audible-output observation is claimed.

This is a successful official transport/decoder/render-counter cross-check, not a complete
pairing, input, display-fidelity or HDR conformance matrix. The official client reused the test
identity; it did not independently perform a first-time official-client pairing ceremony.

## Earlier client defects corrected from reference observations

All corrections have failing-before/passing-after regressions and pinned sources in
[protocol-notes.md](protocol-notes.md):

- Fenrir returns app `IsHdrSupported` text `false`. Official Qt compares exact text to `1`;
  the client now does likewise instead of rejecting the app list. PairStatus remains strict.
- Wolf DESCRIBE sent 195 bytes without Content-Length. Like moonlight-common-c, the bounded
  parser accepts connection-close-delimited payload; explicit length mismatch still fails.
- Fenrir cancel returns root status 200 without a cancel tag. Like official Qt, the client
  verifies status and then current game, preserving failure if the host still reports busy.
- Legacy audio encryption opt-in is independent of x-ss encryption support. The runner now
  announces NV flag 167 and enables AES-CBC audio decoding consistently, without claiming
  unsupported x-ss features. Live audio decode failures fell to zero.
- Receipt measurements used an unscoped clock name, causing `clock domain mismatch` before
  native decode. They now use the boot-scoped source clock ID; frame rate explicitly uses
  `frames_per_second` in the model, evaluator and evidence schema.

No incompatibility was “fixed” by accepting behavior official clients reject.

## Earlier Fenrir lifecycle and cleanup

The exact deployed proxy source was initially absent locally, then resolved through the
repository API to `5c8251c8a7a547466226c5f08f72d94a5de05dd0`:

- [pairing.go:232](https://github.com/jefflouisma/homelab/blob/5c8251c8a7a547466226c5f08f72d94a5de05dd0/fenrir/pkg/moonlight/pairing.go#L232)
  hardcodes Alex; [PIN requests](https://github.com/jefflouisma/homelab/blob/5c8251c8a7a547466226c5f08f72d94a5de05dd0/fenrir/pkg/moonlight/rest.go#L384)
  have no user selection. Explicit coordinator authorization allowed Alex only while idle.
- [Launch](https://github.com/jefflouisma/homelab/blob/5c8251c8a7a547466226c5f08f72d94a5de05dd0/fenrir/pkg/moonlight/rest.go#L513)
  and [cancel](https://github.com/jefflouisma/homelab/blob/5c8251c8a7a547466226c5f08f72d94a5de05dd0/fenrir/pkg/moonlight/rest.go#L709)
  act by `direwolf/user`, not client identity. Fresh read-only checks found no Alex session
  immediately before pairing and each launch. Jeff sessions were never targeted. The initial
  `jeff-cove-7smp8` session was already absent before our launch; this work does not attribute
  its disappearance to any particular cause.
- In this exact revision, cancellation calls `beginStopSessionsForUser` to request foreground
  deletion and returns without waiting for pod disappearance. The separate
  [stop-and-wait helper](https://github.com/jefflouisma/homelab/blob/5c8251c8a7a547466226c5f08f72d94a5de05dd0/fenrir/pkg/moonlight/rest.go#L737-L765)
  allows up to ninety seconds and is used by launch for prior-session cleanup. The runner's
  ten-second cancellation bound therefore limits acknowledgement of the delete request;
  neither HTTP success nor its cancellation signal proves complete host cleanup.
- [Unpair](https://github.com/jefflouisma/homelab/blob/5c8251c8a7a547466226c5f08f72d94a5de05dd0/fenrir/pkg/moonlight/pairing.go#L114)
  clears memory but leaves the persisted Pairing CRD, corroborated in both cleanup rounds.

After debugging, `/unpair` exited 0. The persisted test Pairing name and certificate DER were
matched unambiguously to the isolated client, then its deletion exited 0 and absence was read
back. Final inspection found no sessions. The only two k3s writes were explicitly authorized
deletions of this same test-created identity across two pairing rounds. No sudo, deployment,
configuration, other-user session or console mutation was performed. Local task identity keys
and portable private configuration files were removed after final verification.

These lifecycle limitations belong to Fenrir's proxy; they are not inferred Wolf defects.

## Earlier local evidence and verification

Local-only evidence root: `/Volumes/4TB_Drive/tmp/sunshine-t038-t039/`. Nothing was published.
Important files: `active-session-versions.json`, `serverinfo.json`, `apps.json`,
`live-run4/scenario.json`, `live-run4/report.json`, its `reference-host-sdr/` artifacts,
`official-list.json`, `official-stream.json`, official log `Moonlight-1792945195.log` under
`official/`, and `cleanup-final.json`. Earlier runs/reports preserve the failed observations.
`reference-host-report.json` is explicitly an unbound-provider local admission check, not a
live run. Credentials, PINs and private key material are not included in this document.

The [verification record](/Volumes/4TB_Drive/tmp/sunshine-t038-t039/verification.txt) lists exact
commands and results. Local Rust tests, schema contracts, Clippy, format and shared quality
checks pass. Workflow discovery requires every client suite and the new regression anchors;
Linux/macOS CI configuration is validated locally, but no hosted CI run is claimed.
The dependency audit at that time executed correctly and **failed** for existing
`audiopus_sys@0.2.2 / RUSTSEC-2026-0150` and `rsa@0.9.10 / RUSTSEC-2023-0071` matches.
That run added no dependency remediation or license gate; licensing remains excluded, not passed.
The [2026-10-31 Opus update](../../../.omo/evidence/opus-advisory-remediation-20261031/REPORT.md)
replaces the unmaintained binding with `opus` 0.4.0 / `opusic-sys` 0.7.6 and bundled
libopus 1.6.1. All 214 client tests and the four focused audio tests pass locally.
A fresh audit completes all 244 client queries with only the RSA advisory remaining;
the overall audit is Incomplete (exit 2) because its existing host-version parser
rejects the ENet fork version `1.3.17-moonlight-aca8784`. This dependency update
does not revalidate earlier live audio populations or complete the reference-host gate.

The subsequent [RSA backend migration](../../../.omo/evidence/rsa-backend-remediation-20261031/REPORT.md)
replaces `rsa` with `aws-lc-rs` 1.19.0 / `aws-lc-sys` 0.47.0. All 217 client tests
pass, including unchanged legacy identity/signature, malformed-key, and weak-key
regressions. With the separately corrected ENet audit parser, a fresh complete
audit reports zero advisory matches across 238 client queries and the pinned host
dependencies. These are portable compatibility/security-maintenance observations;
no new live pairing, PS5, reference-host or hosted CI result is implied.


## 2026-10-25 live follow-up: pairing and reconnect

Follow-up evidence root: `/Volumes/4TB_Drive/tmp/sunshine-t038-followup/`.
The Wolf/Cove/proxy versions above were retained; `observed-pod.json` establishes
new pod `alex-cove-689f599449-qd6b9` and the same exact image digests. No client
source change was needed: immediate cancel confirmation already matches official Qt.
All Alex-idle checks were fresh before pairing and every launch. Rejected identities
used isolated state; official Moonlight created its own RSA certificate/key.

| Live case | Result | Local evidence |
|---|---|---|
| Client wrong PIN delivered through a separate helper | Rust exit 1; host challenge hash mismatch; no host pin persisted | `client-wrong-pin.json` |
| Official wrong PIN | Official log `Incorrect PIN`, followed by `/unpair`; no pairing persisted | `official-logs/official-wrong-pin-*.log`, `official-wrong-pin-result.json` |
| Corrupt client challenge answer hash | Host HTTP/XML 400, `paired=0`, `client hash mismatch`; no Pairing CRD | `negative-hash-result.json` |
| Corrupt client secret signature, valid answer hash | Host HTTP/XML 400, `paired=0`, RSA verification error; no Pairing CRD | `negative-signature-result.json` |
| Truncated client proof | Host HTTP/XML 400, `paired=0`, invalid pairing secret; no Pairing CRD | `negative-truncated-result.json` |
| Correct Rust pairing, then two new processes | Pair exit 0; both app lists exit 0, Cove/EmuDeck | `client-positive-pair.json`, `client-restart-apps-{0,1}.json` |
| Correct first-time official pairing | Official log records all five phases including final HTTPS pairchallenge; own certificate persisted as Alex Pairing | `official-logs/official-fresh-*.log` pairing transcript, cleanup certificate matching |
| Official app-list reconnect in two new processes | Both exit 0, Cove/EmuDeck | `official-bound-restart-list-{0,1}.json` |

These direct cases are actual deployed-host exchanges, including deliberately altered
*client* proofs. Four additional isolated client identities exercised rejection of altered
*server* responses through a task-local loopback HTTP relay forwarding to the real Fenrir
pairing endpoint. The relay changed exactly one field in a real host response per run;
these are controlled faults in the observation path, not naturally occurring host defects:

| Live-response fault | Actual client result | Local evidence |
|---|---|---|
| Corrupt server secret signature | Exit 1, secret not signed by host certificate | `live-fault-server-signature-result.json` |
| Corrupt encrypted server hash | Exit 1, host challenge hash mismatch | `live-fault-server-hash-result.json` |
| Truncated challenge response | Exit 1, 16 bytes instead of at least 48 | `live-fault-server-challenge-length-result.json` |
| Malformed advertised certificate | Exit 1, expected one host certificate | `live-fault-server-certificate-result.json` |

Each run mutated a live HTTP-200 response, left no host pin or persisted Pairing CRD,
unpaired, and removed its isolated private state. `live-faults.py` retains the exact
bounded relay/harness, and `fault-cleanup-final.json` records cleanup of those four
identities. Final HTTPS rejection/certificate substitution and the exhaustive parser
variants remain source-pinned synthetic regressions in T013/T020; those fixtures are
not relabeled as direct reference-host observations.

Official CLI pairing presents GUI completion/error dialogs, and returned zero after its
task-owned process was terminated even for the wrong PIN. The log and persisted pairing,
not that exit value, establish the result. The fresh official app-list attempt initially
exited -11 without complete portable host metadata; this setup failure is retained.
A task-local Qt helper bound the existing proxy UUID/address/port and already-proved host
certificate in portable settings, preserving the official-generated client identity.
Only then did both independent app-list reconnects succeed. Normal Moonlight settings
and unrelated processes were untouched. No dialog screenshot or visible application
response is claimed.

## 2026-10-25 live follow-up: input and teardown

The ten-second `scenario3.json` declares observation at the host evdev boundary before
execution. `evdev-run3.jsonl` was obtained by `kubectl exec -i ... -c wolf -- python3 -u -`
with a 45-second observation deadline, opening only the named virtual controller with
`O_RDONLY|O_NONBLOCK`. It did not read keyboard/mouse event streams. A read-only EVIOCGABS
query reported X range -32768..32767 and initial value zero. Independent kernel events:

- At observer monotonic 312711.349750258, EV_ABS (type 3), ABS_X (code 0), value **8192**,
  matching the declared x=0.25; a SYN_REPORT followed.
- At 312712.349935073, ABS_X **0**, followed by SYN_REPORT, matching neutralization.
- At 312713.469389044, the named device disappeared from sysfs; the existing read handle
  returned **ENODEV (19)**. Removal followed neutralization by about 1.12 seconds and
  occurred during the scripted disconnect, before media teardown.

This is independent evidence that Wolf created, updated, neutralized and removed its
virtual controller. It is not a client send/ACK, game rendering observation, PS5 input
measurement or calibrated client-to-host latency. The two machines' monotonic clocks
were not mapped. Earlier observer attempts attached late or to a terminating pod and
are retained as failed observation attempts, not input evidence.

`report3.json` remains the unmodified emitted client report: overall Incomplete, exit 2,
REF-stream Pass, five other criteria Incomplete. Independent observations supplement
that report; they are not fabricated host-provider measurements. Its population is
595 decoded native frames and 1,995 decoded stereo packets with zero decode failures
or missing packets; audio is digitally silent. Host latency/drops/encoder-selected
bitrate remain unavailable. Requested 20,000 kbit/s is distinct from the observed
13,617 kbit/s elementary-video throughput. No HDR event in an SDR run establishes HDR.

The runner retained `host_cancel_attempted/request_succeeded=false` because the immediate
serverinfo check was busy/game 4. Read-only pinned mutual-TLS polling then saw server
free with an empty currentgame, which official Qt interprets as effective game zero.
`status-poll3.json` retains the transition, and `post-run3-status.json` confirms it.
The empty raw field is preserved, not represented as a host-supplied numeric zero.
This follows [official Qt quitApp and getCurrentGame](https://github.com/moonlight-stream/moonlight-qt/blob/f786e94c7b2f943e24e65d7d74deb539b827fc84/app/backend/nvhttp.cpp#L107):
verify cancel status, then require effective current game zero. Delayed Kubernetes pod
termination explains why an immediate successful HTTP cancel is not confirmed cleanup.
No client retry was relaxed to treat a busy host as success.


Official stream reconnect used that same independently paired official identity in two
fresh processes. Both `official-stream-{0,1}.json` observations establish H.264 SDR
1920x1080x60, first video/audio packets, and **59.99 fps incoming/decoding/rendering**.
Each process ran for eight seconds after first video before task-only termination.
The official `quit` commands logged XML status **200**, then encountered the same
busy/game-4 condition; their GUI error path did not exit within the 15-second command
bound. `official-stream-{0,1}-teardown.json` retain that timeout and the subsequent
pinned-TLS busy-to-free/effective-game-zero polling, rather than claiming official CLI
quit success. Read-only `official-stream-{0,1}-resources.json` checks verify no Alex
session, pod, service or deployment remained after each cancellation. A fresh Alex-idle
check preceded the second stream; no existing Jeff session was targeted.


## Follow-up cleanup and verification

`cleanup-final.json` records `/unpair` for all seven isolated identities, exact certificate
DER/fingerprint matching, and deletion of **only the two test-created Pairing CRDs**
(Rust-positive and official-positive). Both deleted records belonged to Alex and their
names matched SHA-256 of their own certificate DER. Readback verified all seven test
fingerprints absent, no sessions, and no Alex pods/services/deployments. Rejected
identities created no persistent pairing. Four later live-response-fault identities were also unpaired, verified never persisted,
and removed (`fault-cleanup-final.json`), for eleven total isolated identities. Final
`cleanup-all-identities.json` verifies all eleven fingerprints absent. All local test
private keys, portable settings and exported identities were removed. Official transcripts retained in `official-logs/`
contain request field names only; raw query values and private portable state were
removed. No sudo, install, configuration/deployment change, PS5 or capture-device access
occurred; the only explicit Kubernetes writes were those two scoped Pairing deletions.
Streaming/pairing lifecycle mutations were authorized protocol operations.

`evidence-summary.json` is an explicitly supplemental operator record of the table above,
not a replacement for or edit of the client's immutable report. T038 and T039 remain
unchecked: host-output fidelity/HDR recording and complete evidence integration remain
Incomplete. These observations do not authorize acceptance of SunshinePS5.

Pinned verification used Rust 1.93.0 first on PATH, FFmpeg/ffprobe 8.0.1, existing system
Opus 1.5.2, TMPDIR under the follow-up evidence root, and the existing external Cargo
target directory. Exact commands/results and artifact hashes are in
`/Volumes/4TB_Drive/tmp/sunshine-t038-followup/verification.txt`:

- Warning-as-error locked all-target/all-feature build: pass.
- Locked all-target/all-feature Rust tests including ignored tests: **166 passed** on
  unchanged-source retry. The initial attempt failed the MCP cancellation fixture's
  ten-second accept timeout; `test.log` preserves that failure and `test-retry.log`
  preserves the full passing rerun. No timeout or warning was suppressed.
- Clippy `-D warnings` and rustfmt check: pass.
- Schema suite: **99 passed**, including the updated scenario.
- `./scripts/check-quality.sh`: exit 0, all checks passed.

No Rust client source changed in this follow-up, so no new behavior/regression test was
introduced. All earlier synthetic proof regressions remain passing; they are separate
from the live matrix. No hosted CI, source-output fidelity or HDR pass is claimed.

## 2026-10-31 reference-host continuation audit

The later read-only [live continuation audit](../../../.omo/evidence/t038-reference-host-live-audit-continuation-20261031/REPORT.md)
found the current g913/Wolf environment healthy but still unsuitable for closing
T038. The live Wolf revision differs from the earlier retained run, the only
published EmuDeck mode is SDR, the advertised codec mask lacks Main10, and no
session, physical connector/output-mode inventory, V4L capture source,
deterministic non-silent reference scene or independent observer generator was
available. No cluster mutation or stream launch was made. Existing protocol
observations remain useful, but T038 requires a newly controlled SDR/HDR
reference setup and independent recorder/calibration evidence; it stays
Incomplete.

## 2026-10-31 maintained reference observer foundation

The later [observer implementation](../../../.omo/evidence/t038-reference-observer-20261031/REPORT.md)
adds a bounded read-only Wolf evdev collector and conservative publisher under
`tools/reference-observer/`. It inventories devices before READY, handles partial
Linux input events and removals, rejects ambiguity/reuse/late attachment and
`SYN_DROPPED`, and publishes raw plus HostObservation bytes through checksum-bound,
manifest-last atomic files. Local verification passes sixteen synthetic
acquisition/publication scenarios, the actual Rust lifecycle/controller consumers
and tamper rejection; the fixture-only step is registered in portable CI, while
hosted CI remains unobserved.

This is infrastructure, not new reference acceptance. Evdev does not authenticate
the runner session or event IDs, the supplied runtime binding is not attested, and
no live g913 population, calibrated mapping, output/audio/HDR recording or fidelity
oracle was acquired. Every generated HostObservation remains unavailable with
`authenticated:false`, and controller/pairing/teardown outcomes remain Incomplete.
The runner now waits after cancellation for the final lifecycle manifest and consumes the bound
controller artifact from that verified snapshot. External orchestration must still await READY
before actions and finish collection/publication within the shared deadline; DONE remains useful
operational coordination, not a substitute for manifest validation. T038 remains open.
