# Using the protocol test client

The client currently provides scenario execution and local stdio agent tools. Complete
reference-host acceptance is **Incomplete**; see [validation.md](validation.md). Portable
fixtures verify software behavior and cannot establish PS5 timing, fidelity or compatibility.
The historical planning contract/quickstart describe an earlier spike; this page records the
implemented command surface checked with package 0.2.0.

`run` and `mcp` also accept the trusted startup binding
`--reference-recording FILE` for a versioned, hashed independent recording/report
snapshot. See [reference-recording.md](reference-recording.md) for native
populations, oracle requirements, bounds and failure behavior. Missing calibration
or live evidence keeps reference acceptance Incomplete.

## Build and software checks

Run from the repository root with the [native prerequisites](build.md):

```sh
export TMPDIR=/Volumes/4TB_Drive/tmp
export CARGO_TARGET_DIR="$TMPDIR/sunshine-quality-cargo"
export PATH="$(dirname "$(rustup which --toolchain 1.93.0 cargo)"):$PATH"
RUSTFLAGS=-Dwarnings cargo build --locked --all-targets --all-features --manifest-path tools/moonlight-mcp/Cargo.toml
RUSTFLAGS=-Dwarnings cargo test --locked --all-targets --all-features --manifest-path tools/moonlight-mcp/Cargo.toml -- --include-ignored
cargo clippy --locked --all-targets --all-features --manifest-path tools/moonlight-mcp/Cargo.toml -- -D warnings
cargo fmt --manifest-path tools/moonlight-mcp/Cargo.toml --all -- --check
./scripts/check-quality.sh
"$CARGO_TARGET_DIR/debug/moonlight-mcp" --help
```

CI runs the library, binary, protocol, evidence, capture and runner integration suites on
Linux/macOS, checks required contract discovery, builds locked inputs with warnings as errors,
and runs format/lint and core sanitizers. FFmpeg 8.0.1 is built from the SHA-256-verified
[upstream release archive](https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz) with the H.264/HEVC
encoders, native decoders, rawvideo and PNG support needed by the tests. CI codec-library
packages supply x264/x265; their versions are not a hardware calibration result. No live
host, USB device or console is contacted by these suites.

Test scratch follows `TMPDIR`/the platform temporary directory; CI sets it to `RUNNER_TEMP`.
The shared scripts also support an unset `TMPDIR`, preferring `RUNNER_TEMP`, then the existing
local evidence root, then the system default. The separate `./scripts/audit-dependencies.sh`
currently exits 1 for the two existing advisories listed in [validation.md](validation.md);
passing unit/quality checks do not represent a passing dependency audit.

## Available commands

| Command | Current behavior |
|---|---|
| `serverinfo --host HOST [--http-port PORT]` | Query host information; pinned HTTPS when paired. |
| `pair --host HOST --pin-delivery STRATEGY` | Full certificate/signature/challenge proof checks, then pin the confirmed host certificate. |
| `unpair --host HOST` | Send the host's unpair request; verify persistent removal separately because hosts differ. |
| `apps --host HOST` | Query applications over pinned mutual TLS. |
| `quit --host HOST` | Request cancellation; check host-specific session scope before use. |
| `run --scenario FILE --report FILE` | Validate the immutable declaration, pair/connect, launch, negotiate, observe controller/media, release input and tear down, retaining an evidence report. |
| `mcp --output-root DIR` | Serve bounded local stdio tools using the same runner. |
| `record-reference --seconds N --out DIR --dynamic-range sdr` | Native SDR acquisition via explicit capture configuration. |
| `record-reference --seconds N --out DIR --dynamic-range hdr --hdr-helper PATH --hdr-source-evidence FILE` | External ten-bit helper; unavailable/invalid HDR produces Incomplete. |
| `compare --stream DIR --reference DIR --out DIR [--profile FILE] [--report FILE]` | Full-population native comparison; calibrated declared subchecks determine status. A comparison without an acceptance profile cannot claim Pass. |
| `capture-server` | Bounded loopback-only capture service; use `--help` for root, bind and device selection. |

Global `--config-dir DIR` isolates identity and certificate state. PIN strategies are `manual`,
`wolf`, `command:...` (PIN passed in `MOONLIGHT_PIN`), and `sunshine:URL`. Sunshine automation
also requires `--sunshine-user`, `--sunshine-password` and `--sunshine-certificate`; keep
credentials outside retained scenario/report files. Manual delivery is refused by unattended
scenarios. Wolf's pending-request and PIN flow is distinct from Sunshine's authenticated web API.
Official Moonlight clients are authoritative when behavior differs.

`run` and `mcp` accept trusted startup bindings: `--pin-provider-ref ID --pin-delivery STRATEGY`,
`--app-id ID` for multi-app hosts, `--http-port PORT`, and explicit capture bindings through
`--reference-device-ref ID`. Bindings do not change a scenario's required criteria. Devices open
only with an explicit binding; a required absent binding is Incomplete. The supplied g913
scenario uses a Fenrir deployment that pairs as Alex and cancels sessions by user. Check that
user is idle before pairing and launching, select `--app-id 4` for Cove, and follow the scoped
identity cleanup recorded in [validation.md](validation.md).

`--host-observations FILE` binds JSON-lines records
([host-observation.schema.json](../../../tests/schemas/host-observation.schema.json)) that an
independent observer on the reference host writes for this run; the runner's `declared`
lifecycle event carries the `session_id` those records must use. After host cancellation the
runner validates every record for that session, keeps a copy as `host-observations.jsonl`, and
evaluates REF-controller: Pass only when each action declaring `expected_delivery` has a valid,
authenticated `reference_host` `console_delivered` record for the same controller and
`event_id`; a `delivery_failed` the reference host observed is Fail; anything missing,
unreadable or for another session is Incomplete. Sends, ACKs and `host_received` never count,
and only `reference_host` provenance counts: `synthetic_fixture`, `console_hardware` and
`operator_observation` records can never establish reference acceptance. Agents cannot supply
this binding. When `--reference-lifecycle` is also supplied, the observation path must
identify a SHA-256-bound artifact beneath the manifest's directory, using the same
lexical directory prefix; the artifact suffix cannot contain parent traversal.
Equivalent paths using different directory aliases are rejected. The runner awaits that final manifest
and imports the exact validated source bytes, even if the source is later replaced.
Manifest and record provenance must agree. The verified manifest and source artifacts
must be retained within quota before controller acceptance is evaluated. An invalid or
missing manifest never falls back to standalone JSONL. Without a lifecycle binding,
the legacy JSONL import is attempted once after cancellation with no publication wait
or manifest checksum guarantee.

`--reference-lifecycle FILE` additionally binds an independent observer's bounded
pairing/teardown snapshot, awaited after host cancellation. Both reference snapshot
bindings share a ten-second publication window after successful cancellation;
observers write sources first and atomically publish their final manifests for this
run's session. Only a missing manifest path or truncated top-level manifest JSON
is polled every 100 ms; source-file errors and complete invalid snapshots are
terminal immediately. The manifest is parsed once and consumed without rereading
its path. Failed cancellation performs one immediate
import attempt. The lifecycle snapshot requires exact session,
scenario and version correlation, explicit proof/cleanup checks, and SHA-256-bound
source artifacts retained within the run quota. See [the snapshot contract](reference-lifecycle.md)
for the six checks and observer coordination. This binding is also unavailable to
agent tool input; it does not run a reference observer or supply missing hardware evidence.

`--observer-ready FILE` optionally coordinates an independent observer before host launch.
It is a trusted CLI/MCP-startup binding, never a tool-call argument. After the paired connection
reveals the host version, the runner atomically publishes `observer-binding.json` in the scenario
artifact directory. The object contains `schema_version: "1.0.0"`, `run_id`, `session_id`,
`scenario_id`, `scenario_sha256`, `host_version`, `test_client_version`, and a fresh UUID-v4 `nonce`.
The digest is SHA-256 of compact `serde_json` serialization of the parsed immutable scenario
(not the original file's whitespace). A custom publisher may copy the published binding exactly,
bind and start acquisition, and only after its collector reports READY atomically publish:

```json
{"schema_version":"1.0.0","state":"ready","binding": {"...":"exact published binding object"}}
```

The example's `binding` placeholder must be replaced with the entire object. This custom-publisher
path is an alternative to the maintained collector's direct publisher described below; do not run
both for one readiness file. The READY file must
be absent before the binding appears, in an existing canonical directory with no symlink aliases.
It must be a regular non-symlink file of at most 16 KiB; only absence is polled (every 100 ms).
Malformed, partial, stale, foreign or unknown fields are terminal. Both binding and accepted READY
bytes are retained within the run quota before launch. Replacement during reading is rejected;
the accepted snapshot is used thereafter without rereading the path. The fixed ten-second wait
is cancellation-aware. Timeout, invalid publication, cancellation or retention failure leaves a
final Incomplete report with no host launch attempt. A fresh per-run file path is required when
reusing trusted MCP startup configuration, or the orchestrator must remove the previous READY
before the next run. No readiness files are produced or awaited unless this option is configured.

This handshake records operational coordination only. It does not authenticate session/event
ownership, certify readiness truth, or change any acceptance criterion. The runner does not start
or stop the observer process. The external orchestrator must also complete final manifest publication
within the separate post-cancellation deadline documented above.

The [reference evdev collector](../../reference-observer/README.md) can publish
this envelope directly with paired `--runner-binding observer-binding.json` and
`--ready-output FILE` arguments. It checks the supplied session/scenario/version
fields, echoes the Rust digest as an opaque token, and requires an empty initial
inventory before atomic no-replace publication. The envelope file is synced and
closed before it becomes visible; the subsequent directory-sync result is reported
but cannot revoke an envelope the runner may already have retained. Fixture replay
cannot publish READY. The local supervisor described below starts the collector within its
deadline, supplies the scenario and coordinates final publication after cancellation. In attachment
mode, READY explicitly defers runtime inventory until the postlaunch association is supplied.

The collector's optional `--finish-fd N` closes that local end-boundary mismatch without
changing runner ownership. The coordinator passes an inherited nonblocking read pipe, waits for
READY, and after host cancellation writes one JSON line containing `schema_version: "1.0.0"`,
`command: "finish"`, and the entire exact runner binding, then closes every writer copy. A valid
command causes a final bounded inventory/read check before manifest-last publication; devices still
present remain explicit and are not reported as removed. Invalid, foreign, premature, duplicate,
oversized or incomplete commands, control loss, acquisition faults and the duration watchdog cannot
report `finish_status: "finished"`. The pipe coordinates a trusted local process; it does not
authenticate cancellation timing, event ownership, teardown or any acceptance criterion.

Configure `--observer-finish-signal` with `--observer-ready` to publish the runner's
`observer-cancel.json` in the scenario artifact directory. This is a trusted startup
option for CLI and MCP-server configuration. Its strict envelope contains
`schema_version: "1.0.0"`, `state: "host_cancel_completed"`, the original in-memory
`binding`, and `request_succeeded`. The runner reserves its byte/file budget before
launch and publishes after the bounded host cancellation request returns, before
observer import. A supervisor may translate only `request_succeeded: true` into the
collector's finish command. False means error/timeout and should close the control
writer without a finish command. Missing or invalid signals require the supervisor's
own watchdog; lifecycle journals, final reports and disappearing pods are not substitutes.
Signal publication and both observer imports consume one absolute post-cancellation
deadline. Failed cancellation retains the existing immediate-import behavior.

An optional trusted startup `--observer-attachment FILE` also requires `--observer-ready`.
Before launch the runner requires the attachment destination absent and reserves bytes
and one artifact for `observer-launch.json`, keeping a fresh UUID-v4 nonce private.
After launch returns successfully, it atomically publishes that marker with exactly
`schema_version: "1.0.0"`, `state: "launched"`, the original `binding`, and `launch_nonce`.
Only then can the supervisor echo the nonce in a valid attachment. Publication uses the
same checked-close and atomic no-replace protocol as the binding/cancellation files;
a competing marker or failed publication prevents RTSP and proceeds to cancellation.
The ten-second attachment deadline starts immediately before marker publication.
Publish one strict UTF-8 JSON object with exactly
`schema_version: "1.0.0"`, `state: "attached"`, `binding` (the entire original runner
binding), `launch_nonce` (the exact newly published marker nonce), `observer_id`
(canonical UUID-v4), `association: "exclusive_launch_candidate"`,
`authenticated: false`, and `runtime_attachment_sha256` (64 lowercase hexadecimal digits).
The digest identifies a separately retained runtime attachment payload; the runner retains
it as an opaque coordination token and does not read or authenticate that payload.
An independent supervisor must retain and validate the underlying runtime/device evidence.

Use a fresh canonical trusted path and atomic publication. The envelope is limited to
16 KiB; links, special files, mutation/replacement during acquisition, duplicate/unknown
fields and invalid bindings are terminal failures. Only absence is polled. The exact bytes
are quota-accounted, retained and synced as `observer-attachment.json` before RTSP. One
absolute deadline covers waiting and retention. Failure/cancellation prevents RTSP/input
and flows through the existing bounded host cancellation and optional finish-signal path.
Cancellation can still fail and is reported explicitly; the barrier cannot guarantee host
cleanup. This check coordinates the first input boundary but does not prove event coverage,
runtime ownership or acceptance. The nonce establishes response-after-launch causality;
it does not validate the runtime evidence itself. Omitting the option preserves existing behavior.

The maintained [local supervisor and attachment protocol](../../reference-observer/README.md#local-process-supervisor)
implement collector process ownership, bounded logs/lifetime, exact cancellation translation and
manifest/source validation. Uncertain pipe closes are attempted once; cleanup faults cannot skip
the remaining child-reaping and final-report attempts. The opt-in runtime mode separates the
prelaunch observer process identity from a supplied postlaunch Session → Deployment → ReplicaSet →
pod chain and container/device evidence. It validates the exact launch nonce, immutable bytes,
source hashes and supplied device paths. Empty initial inventories allow RTSP/controller arrival.
The approved opt-in `--device-feed` supplies one later immutable device population bound to the
exact runtime/launch identity. Acquisition opens only its supplied exact paths and emits
`late_attach` before later decoded events; early-event coverage stays unavailable and continuity
unverified. Finish without a feed does not hang. See the
[feed report](../../../.omo/evidence/t038-late-device-feed-20261031/REPORT.md) and
[review](../../../.omo/evidence/t038-late-device-feed-code-review.md).
The deployed agent retains session/path/major/minor/generation but exposes no subscription history.
The approved provider and SSH subsystem relay are now implemented and fixture-tested. See the
[supervisor review](../../../.omo/evidence/t038-local-supervisor-code-review.md),
[barrier review](../../../.omo/evidence/t038-runner-attachment-code-review.md) and
[runtime review](../../../.omo/evidence/t038-runtime-attachment-code-review.md).

For the implemented local Linux publication path, `fenrir_provider.py --enable-provider`
requires an absolute kubectl-compatible executable, kubeconfig/context/namespace, exact
user/app/pairing selection and reviewed agent HTTPS port. Start it before the supervisor;
the supervisor's `--provider-ready` consumes the prelaunch baseline barrier before collector
READY. `--runtime-attachment` and `--device-feed` select its immutable runtime/feed files.
The provider resolves the exact wolf-agent container's declared port to a numeric HTTPS
pod-proxy endpoint and rechecks runtime identity around the read. See the
[provider contract](../../reference-observer/README.md#read-only-fenrir-provider),
[report](../../../.omo/evidence/t038-fenrir-provider-20261031/REPORT.md) and
[final review](../../../.omo/evidence/t038-fenrir-provider-code-review.md).

The implemented cross-host entry point is
`python3 tools/reference-observer/remote_relay.py --enable-relay --config /absolute/relay-config.json`.
It requires the separately installed `sunshine-reference-observer-v1` SSH subsystem running
`remote_relay.py --serve`, a dedicated account/credential, pinned host key/module digests and
private remote root. Its bounded immutable transfer validates exact binding, launch nonce and
artifact hashes; Linux monotonic checks stay on Linux. Successful cleanup receipts prove only
owner-process reaping, and transport loss leaves remote cleanup unconfirmed. See the
[relay report](../../../.omo/evidence/t038-remote-relay-20261031/REPORT.md) and
[approval review](../../../.omo/evidence/t038-remote-relay-code-review.md). The reviewed server
gate is `python3 -I -S /absolute/deployment_entrypoint.py --serve --config /absolute/config.json`
under a directly configured subsystem. It verifies the dedicated account, immutable pinned source
and executed module paths, literal executables, private root/kubeconfig and fixed provider/root
policy; it does not create those resources or test RBAC. See its
[report](../../../.omo/evidence/t038-deployment-preflight-20261101/REPORT.md) and
[review](../../../.omo/evidence/t038-deployment-preflight-code-review.md).
For g913, `tools/reference-observer/k3s-kubectl.sh` is a fixture-tested regular executable that
forwards exact argv and exit status through `exec /usr/local/bin/k3s kubectl`. It is not installed:
the administrator must still protect and validate the wrapper, `/bin/sh`, delegated k3s binary,
parent paths and live dedicated-account compatibility. The current combined portable verifier
passed 151 Python tests and all 23 retained stages.
These commands are current software interfaces, not a ready-to-run live deployment recipe.
The [read-only g913 audit](../../../.omo/evidence/t038-g913-relay-live-audit-20261031.md)
found current-user verified Kubernetes TLS/read/proxy access and zero Sessions, but no
standalone kubectl, deployed modules in the bounded search, relay sshd subsystem or restricted
relay credential/account. The current key is plain. Dedicated deployment/account RBAC,
self-signed backend proxy handling, populated runtime, authenticated agent/runner-to-Session/event
ownership, subscription continuity, independent output/audio/HDR/fidelity/cleanup and official-client
checks remain unverified. All acceptance observations remain unavailable.

For PS5 recovery, the approved fixed-endpoint `recovery_adapter.py` mechanics exist; there is no
standalone dispatch CLI. Approved conservative components can retain bounded service/host
diagnostics and can expose HDMI only through a digest-bound reviewed classifier; no classifier
ships and diagnostics do not arm recovery. A reviewed target inspector can collect bounded
boot-time/no-follow inventory data. Its [launcher](../../../.omo/evidence/t050-recovery-launcher-20261101/REPORT.md)
now links the pinned full SDK CRT and checks exact firmware `0x13600007` before observation;
host regressions, target ELF inspection and independent code review pass. The CRT's process-local
credential/syscall bootstrap precedes the gate. The [exact-loader audit](../../../.omo/evidence/t050-relapse-loader-compat-20261101/REPORT.md)
establishes distribution provenance and a byte-identical SDK v0.45 full-CRT verifier that
historically executed through this loader on the same 13.60 console. Five CRT functions match
the inspector after masking only pinned object relocation fields, supporting the historically
exercised full-CRT ABI class. Inspector-specific behavior for 23 GLOB_DAT imports and one
R_X86_64_64 relocation, current-boot loader continuity, inspector execution/output and independent
exit remain unverified; exact loader build provenance remains unresolved.
Observer firmware-validation/atomic/process/game/writer
claims remain false/null; the reviewed public-certificate fingerprint path and independent
process-exit evidence are still missing. All seven live callbacks (services, boot, process,
isolation, HDMI, inventory and exit observer) and a genuine pre-rest boot identity remain unbound.
Retained service PIDs cannot prove continuity. The
[adapter report](../../../.omo/evidence/t050-live-adapter-20261031/REPORT.md),
[live-components report](../../../.omo/evidence/t050-live-callbacks-20261101/REPORT.md), and
[inspector report](../../../.omo/evidence/t050-recovery-inspector-20261101/REPORT.md) cover
portable mechanics only.
The [latest rest check](../../../.omo/evidence/t050-rest-device-20261031/rest-attempt.md) at
`2026-11-01T00:31:59Z` still shows NO SIGNAL. [Fresh discovery](../../../.omo/evidence/t050-rest-device-20261031/discovery-20261101T011503Z.log)
at `2026-11-01T01:15:03Z` returned `HTTP/1.1 620 Server Standby` for host `00E421FDEC6B`
on firmware `13600007`, with exploit/development-service ports absent; no inspector, verifier
or cleanup payload was dispatched. Preserve the fresh codex6 population without reinitializing
it before inspection. A read-only
[vphone check](../../../.omo/evidence/t050-vphone-wake-recheck-20261101/REPORT.md) found no
installed PS Remote Play or Chiaki client and established no registered wake route.

The runner bounds the cancellation request at ten seconds. In the deployed Fenrir
revision, a successful response confirms that foreground Kubernetes deletion was
requested; cleanup can continue afterward. Thus `request_succeeded: true` permits
the collector's final observation boundary but does not establish pod or resource
removal. The separate ninety-second wait belongs to launch-time prior-session cleanup,
not cancellation; see [the deployed behavior](validation.md#earlier-fenrir-lifecycle-and-cleanup).
If cancellation fails or exceeds the runner bound, the supervisor must close the writer
without finish and the runner attempts import immediately. Later pod disappearance cannot
upgrade that immutable false outcome. Later manifests remain supplemental diagnostics.

Both binding and cancellation files use native atomic no-replace rename on macOS/Linux,
with content sync and writable-descriptor close before visibility. The visible inode
has one link and is not subsequently mutated. Directory sync is attempted afterward;
failure adds a limitation without revoking a signal another process may have retained.
Use a trusted canonical artifact directory. Other platforms/filesystems fail before
publication. This signal does not establish host cleanup or authenticated event ownership.

Run in an existing local output root, with a fresh report name and a fresh scenario artifact
directory. Reports cannot overwrite existing files or escape the selected root. The scenario
quota bounds streamed artifacts and turns unavailable evidence into Incomplete. SIGINT triggers
cleanup and report finalization; process kill/power loss can leave the initial journal, which
must not be interpreted as completion.

## Outcomes and local agent tools

Report-producing commands distinguish **0 Pass**, **1 Fail**, **2 Incomplete**, and
**64 invalid invocation**. Parse both the report and process status: missing reliable evidence
takes overall Incomplete precedence while demonstrated criterion Fail results remain Fail.
Legacy serverinfo/pair/apps/quit success is an operation result, not acceptance.

MCP uses newline-delimited JSON-RPC on stdin/stdout, protocol version `2025-06-18`.
Initialize, send `notifications/initialized`, then call:

| Tool | Arguments |
|---|---|
| `run_scenario` | `scenario`, `report` |
| `compare_recordings` | `stream`, `reference`, `out`, optional `profile`, `report` |
| `read_report` | `report` |

Paths are relative to `--output-root`. Tools return `structuredContent.overall_status`, reasons,
criterion statuses and a report path; `isError` is true for Fail and Incomplete. JSON-RPC success
is not acceptance. Cancellation/EOF finalizes active work, and overlapping operations are
refused. Agent input cannot supply credentials or arbitrary command/device bindings.

## Planned and gated work

Core-host pair/launch/stream/input/teardown E2E wiring belongs to US1 once the host exists;
CI currently exercises loopback protocol fixtures and local recordings. T038's independent
Wolf/Fenrir gate remains Incomplete, and T039 stays unchecked until that predecessor passes.
REF-controller can now consume bound independent host observations, but closing T038 needs a
fresh live run with such an observer; existing reports are immutable and are not re-evaluated.
REF-pairing and REF-teardown can consume the independent lifecycle snapshot described above;
absent, invalid or incomplete observations leave those criteria Incomplete. REF-recording
and REF-report still require complete independent validation beyond lifecycle observations.
PS5 release scenarios, all-platform official-client runs, host-application input observations,
full hardware timing, and calibrated SDR/HDR source comparisons remain required evidence.
An implemented command or passing software suite does not close those gates.

## Offline capture-cadence analysis (diagnostic only)

`analyze-cadence` reads the sidecars of a capture output directory that already exists on disk
and reports what the recorded frame timestamps show. It opens no capture device, contacts no
console and no network peer, reads no frame pictures, and never writes inside the capture
directory.

```sh
moonlight-mcp analyze-cadence --capture DIR [--report FILE] \
  [--gap-threshold 1.5] [--nominal-fps FPS] \
  [--startup-exclude-frames N --startup-exclude-reason TEXT]
```

`DIR` must hold `frames.json` and `recording.json`; `capture-summary.json` is used when present.
The nominal frame interval comes from the capture summary's `descriptor_interval_100ns` when
there is one, otherwise from `recording.json` `capture_mode.fps`; `--nominal-fps` is an explicit
operator override that wins when supplied. The report always names the source it used and lists
every candidate it displaced. `--gap-threshold` is the multiple of the nominal interval above
which an interval is classified as a gap (default `1.5`). A gap's implied missing frame count is
derived, not guessed: an interval of `d` occupies `k = round(d / nominal)` nominal frame periods
(at least one), so `k - 1` nominal frame slots hold no recorded frame. `--startup-exclude-frames`
with its required `--startup-exclude-reason` declares a leading window; the excluded frame and
interval counts, the reason, and the full-population figures are all reported together, so
nothing is dropped silently. `--report` writes the JSON to a new path and never overwrites one.

The report separates whole-population interval statistics from steady-state statistics over the
non-gap intervals, which is the question it exists to settle: a low average fps in a short
capture is usually one startup gap rather than a steady-state fault. It also cross-checks the
capture summary — `recorded_frames` against the timestamp count, and `rejected_usb_frames`
against the implied missing frames — and reports agreement or disagreement instead of assuming
either, and reports `met_requested_frame_budget` so a met budget cannot read as lost frames.
Percentiles use this crate's nearest-rank convention, so each reported percentile is a
value that was actually observed; a different rank rule can land on the adjacent order statistic.

Two figures are easy to misread, so the report labels both in place. `total_implied_missing_frames`
counts nominal **arrival slots** holding no recorded frame; it is **not** a count of frames lost
from the recording, and a capture that recorded its full requested budget can still show arrival
gaps — the R4 reference capture is exactly that case, with 120 of 120 frames recorded and five
implied missing at startup. `steady_state.effective_fps` is **selection-biased by construction**:
the intervals above the threshold were removed to form that population, so it cannot fall far
below nominal and may exceed it, in which case `exceeds_nominal` is set. Neither figure means the
source ran fast or lost frames; read both against the full-population statistics.

**Limitations. These are not optional caveats; read them before quoting any figure.**

- Each timestamp marks one complete capture frame assembled before pipe output. It is **not**
  HDMI exposure time, **not** console presentation/PTS, and **not** calibrated optical latency.
- USB provides no HDMI-source frame counter. Whole source frames absent from USB are
  **unobservable**, so no gap reported here can be attributed to the console rather than the
  USB/host capture path, and **no gap is a SunshinePS5 host drop**.
- This is a diagnostic/research report. It is **not acceptance evidence**, it claims no SC-003,
  SC-004, SC-012, SC-013 or SC-014, and it emits no Pass/Fail acceptance verdict. The command
  carries no acceptance outcome in its exit status: it exits 0 on a completed analysis, 1 on an
  analysis error, and 64 on an invalid invocation. A zero exit is not a pass.
- It sees recorded capture cadence only. It cannot measure input-to-display latency, on-console
  resource contention, encoder or network behaviour, or image fidelity.
- A short capture cannot characterize a session. The report carries the observed frame count and
  span for exactly this reason; read them before generalizing.
- Agreement between the implied missing-frame count and the recorded USB reject count is
  consistency, not causation. Neither establishes where a frame was lost.
- Recording validity and calibration status are carried through from the capture metadata. This
  analysis neither requires nor confers either of them, and an analyzable capture whose metadata
  is marked invalid stays invalid.
