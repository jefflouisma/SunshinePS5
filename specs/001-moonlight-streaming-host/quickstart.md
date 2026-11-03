# Quickstart: validating the feature

**Feature**: `001-moonlight-streaming-host` | **Date**: 2026-10-24
**Status**: Validation design; host readiness remains open in [plan.md](plan.md).

Run commands from the repository root. This guide distinguishes the existing spike from future
acceptance procedures. No complete host, scenario runner, HDR capture path, or passing release
report was established by this planning pass.

## What can be checked now

With the installed Rust toolchain, libopus, and FFmpeg:

```sh
cargo run --locked --manifest-path tools/moonlight-mcp/Cargo.toml -- --help
ffmpeg -hide_banner -f avfoundation -list_devices true -i ''
```

The first command lists the spike's actual commands; it does not run acceptance. FFmpeg lists
devices and then exits nonzero because no input was selected. See the
[client contract](contracts/test-client-cli.md) for the current/planned command distinction.

The attached device is **Elgato 4K X, 0fd9:009c, at 5 Gbit/s**. Its USB descriptors advertise
P010 format index 7, frame index 5, 1920×1080 at 60 fps. The standard macOS driver exposes only
eight-bit modes. Descriptor support is not evidence of actual HDR frames or a valid HDMI signal.
The reference USB helper hardcodes a different product ID, so it needs descriptor-based device
selection before use. [Research R4/R14](research.md) records the observations and inventory commands.

## Prerequisites for hardware acceptance

| Requirement | Evidence to retain |
|---|---|
| Firmware 13.60 with existing homebrew environment | Detected firmware, environment version, native build/run provenance |
| Pinned host/test-client builds | Revision, compiler/SDK/dependency versions, artifact checksums |
| Console reachable by permitted private clients | Actual peer addresses, topology, wired baseline; IPv4 and IPv6 internet isolation |
| Verified console capture, hardware encode, audio, input and cleanup | R3 probe record with sourced interfaces and observed results |
| Connected reference capture chain and HDR display | Actual selected mode, timestamps, frame precision, HDR source state, driver recovery |
| Independent reference host | Exact host version and reference-client compatibility results |
| Official clients on every supported platform | Client version, device/display, SDR/HDR capabilities and outcome |
| Comparable scenes and at least five games | Scene IDs, settings, repeatable controller sequence and calibration identity |

Before each hardware run, confirm no usable console internet path over either IPv4 or IPv6.
Routed private client subnets are allowed; they do not require internet access. No network or
console configuration was changed during this planning pass.

Close R3 capture/encode/input/source-metadata evidence, R4 actual ten-bit recording/calibration,
and R5 on-device timing/resource evidence before claiming implementation readiness. The
256 MiB/one-core-equivalent budgets are proposed ceilings, not established available resources.

## Build and run after implementation

The implementation must provide the pinned clean-checkout host build entry point
`./scripts/build-ps5.sh` and CI jobs for Linux/macOS core, PS5 artifact, and the Rust client.
These build/CI entries are not present yet.
Do not invent a successful host build from the existing Rust spike.

The following scenario commands are the **planned interface**, not runnable acceptance today.
The referenced scenario files are also future implementation artifacts:

```sh
cargo test --locked --manifest-path tools/moonlight-mcp/Cargo.toml
cargo run --locked --manifest-path tools/moonlight-mcp/Cargo.toml -- run --scenario tests/scenarios/reference-host.json --report artifacts/reference-host/report.json
cargo run --locked --manifest-path tools/moonlight-mcp/Cargo.toml -- run --scenario tests/scenarios/ps5-sdr-1080p60.json --report artifacts/sdr/report.json
cargo run --locked --manifest-path tools/moonlight-mcp/Cargo.toml -- run --scenario tests/scenarios/ps5-hdr-1080p60.json --report artifacts/hdr/report.json
```

Declare host, PIN-delivery mechanism, mode/bitrate, duration, input workload, required criteria,
and evidence sources in each scenario before running it. Keep credentials outside reports.
The planned report exits are 0 Pass, 1 Fail, 2 Incomplete, and 64 invalid invocation. An existing
spike command's success is not one of those acceptance results.

## A. Establish the independent test client first — User Story 5

1. Build/test the client. Exercise the complete official pairing proof set against the
   independent reference host; reject deliberately invalid proofs.
2. Pair, list applications, start a stream with declared settings, send controller input, and
   tear down. Verify the reference host actually receives/applies the defined input.
3. Inspect a report containing real video/audio/loss/latency/HDR observations and explicit
   availability. No missing value is replaced with zero.
4. Record and compare a known local signal. Verify geometry, frame/timestamp counts, clock
   mapping, alignment, SSIM/PSNR, and the expected result for identical and altered fixtures.
5. Run the same transport/session path against core fakes in CI; test cleanup and invalid inputs.

The current 960×540 eight-bit comparison path and default 300-frame sample cap cannot prove
native 1080p/HDR or an undeclared whole-run population. Upgrade and validate those paths before
using them to judge SunshinePS5. Reference-host success does not establish PS5 hardware success.

## B. Pair, play, and reconnect — User Story 1

1. Launch the verified host. Confirm firmware refusal on unsupported versions using a controlled
   test, and the web address notification on supported hardware.
2. On Apple TV, discover the host, request pairing, and enter the PIN in the phone web app.
   Check both console notifications and client completion within three seconds.
3. Start the sole PS5 app entry. Verify video, stereo audio, every required controller action,
   the single required controller assignment, and available rumble/motion/touchpad feedback.
4. Run the declared wired 1080p60 SDR scenario for 30 minutes. Measure SC-003 protocol latency,
   SC-004 host drops, SC-005 input delivery, and the separate full-pipeline constitution gate.
5. Quit, disconnect, simulate loss of contact, and revoke. Observe immediate input release,
   resource cleanup, correct end reasons, and the game continuing. Last-contact timeout is
   at most ten seconds; active revocation completes within two seconds.
6. Restart host and console, then select the previously paired host in an official client.
   Measure selection to first **presented** console image within five seconds (SC-002).
7. Exercise below-minimum, in-range, and above-maximum bitrate requests. Check the chosen
   boundary/target in both web status and report; measured bitrate remains separate.
8. Verify second-session refusal, unpaired/revoked refusal, cooldown/expiry, Remote Play/Share
   Play conflicts, and protected-content placeholder behavior without disrupting the game.

For SC-001, separately observe ten first-time participants using only prompts; at least nine
must pair Apple TV and begin play on their first attempt within five minutes. A developer's
successful walkthrough is not that evidence.

## C. HDR and recording — User Story 2

1. Establish a known HDR source and verify actual 1080p60 P010 frame acquisition, ten-bit
   precision, cadence/loss, PQ/BT.2020 interpretation, and clean helper/driver release.
   Do not rely solely on device enumeration, TV labeling, or container metadata.
2. Calibrate the reference chain and record format/timestamp provenance. Compare native
   resolution/precision without converting HDR to SDR.
3. On each supported platform with an HDR display, request HDR while the console outputs HDR.
   Retain client HDR status, side-by-side picture observations, and objective comparison evidence.
4. Run an explicit 30-minute wired HDR 1080p60 scenario applying SC-003/SC-004 (SC-012).
5. Compare signaled HDR color/brightness information with an independent console-source metadata
   record for the same output epoch. The helper's fallback mastering/light-level values cannot
   validate FR-025.
6. Exercise all four console-output/client-request combinations in the
   [protocol contract](contracts/moonlight-protocol.md). Switch output mid-session and verify
   the matching stream within two seconds without disconnecting; HDR-to-SDR conversion must
   preserve usable brightness and color.
7. After calibration, explicitly amend the spec with the objective HDR rule and any absolute
   SDR median target it calls for. Until then, those required results remain Incomplete.

## D. Phone management and metrics — User Stories 3 and 4

At 360 CSS pixels in the required browsers, inspect host/session/version/address fields; rename
host/device; revoke with confirmation; and stop with confirmation. Validate persistence across
restart and clear errors on failed durable writes. Record completed action paths for SC-011:
navigation, submission, and confirmation count; typing and keyboard dismissal do not. An action
submission still counts if it also dismisses the keyboard. The metrics journey is tested after
that control exists, alongside pairing and revocation.

During streaming, toggle metrics and confirm the setting applies within two seconds without
interruption, defaults off, and persists. Verify all stage/input/counter views, once-per-second
updates, budget highlighting, capture export, and diagnostics. Close every browser: enabled
collection continues, but browser-update work stops. With metrics off, only the timing required
by FR-021 remains; exported gaps must identify that interval.

Use a session longer than ten minutes to verify that old raw samples expire while whole-session
counts/percentiles remain correct. Force clock faults, queue exhaustion, missing stage evidence,
and histogram overflow in controlled tests. Affected values must be unavailable with reasons,
and required missing evidence makes the run Incomplete while preserving known failures.

## E. Performance, stability, and fidelity

Use [measurement-evidence.md](contracts/measurement-evidence.md) as the authority for clocks,
populations, threshold comparisons, missing values, and every SC-001–SC-014 evidence requirement.

- Compare matched on-device metrics-off/on runs for SC-008's ≤2% p99 overhead using the
  compatible protocol measure available in both states. Fakes cannot prove hardware overhead.
- Run the two-hour SC-006 soak; retain external first/last ten-minute summaries before raw
  eviction. Check protocol p99, full-pipeline timing, disconnect/crash logs, and all host memory
  domains, including attributable driver/media allocations.
- Measure CPU-time windows, pool high-water marks and foreground-game effects against the
  declared plan budgets. Missing accounting is not zero resource use.
- Compare the local display with/without the host across at least five games (SC-009).
- For SC-014, compare the declared full 1080p60 SDR frame population against simultaneous HDMI
  capture. At least 99% must reach SSIM 0.70. Sampling 300 frames is insufficient unless a
  separate, explicitly approved criterion defines that population. From release two, retain
  the identical-scene prior-release median baseline and apply the ≤0.01 regression rule.

## Evidence and release decision

Keep scenario/profile versions, revisions, environment, raw evidence references, coverage,
timing uncertainty, requested/chosen bitrate, end reasons, and per-criterion statuses locally.
Reports may combine automated evidence with declared official-client/operator observations.

Any required Incomplete makes the overall run Incomplete, even if other criteria Fail.
Known failures remain visible. Only complete valid passing evidence for every required criterion
permits Pass. Check the full official-client SDR/HDR matrix and every release criterion before
release; fake CI or a single device cannot replace them.

CI must actually run the required core/client builds, PS5 artifact build, unit/E2E tests,
formatters/linters, sanitizers, fuzz targets, and dependency audits from the plan. Record the
owner's licensing deferral without claiming that an unrun audit passed. Regenerate the old
tasks against the revised design only after the readiness gates are resolved.
