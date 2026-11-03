# Implementation decisions and executable scope

**Feature**: `001-moonlight-streaming-host` | **Recorded**: 2026-10-24 | **Task**: T001

This record makes the conditional backlog executable; it is not hardware acceptance.
The [spec clarifications](../spec.md#clarifications), [plan summary](../plan.md#summary),
and constitution remain authoritative. The regenerated [tasks](../tasks.md) authorize
setup, foundation, and independent-client preparation before T045. Production console
streaming still depends on R3/R4/R5 evidence and the T045 readiness decision.

## Scope and accepted decisions

- Deliver all five stories in the first release: phone pairing, one active stream,
  one required controller, SDR/HDR, management/revocation, metrics, and the independent client.
  US5 is the first development deliverable; validate it against an independent reference
  host before using it to accept SunshinePS5. Official clients remain authoritative.
- Target firmware 13.60 with the existing homebrew environment. Unsupported firmware
  is refused. Installing that environment and keyboard/mouse work are outside scope.
- Permit reachable routed private subnets; reject public/global peers. Discovery remains
  link-local, with manual address entry across permitted routed segments. Preserve the
  accepted no-login management policy and same-origin request protections.
- Missing or unreliable required evidence means Incomplete and blocks acceptance.
  Preserve demonstrated per-criterion failures under overall Incomplete.
- Clamp a valid out-of-range bitrate to the supported boundary; retain requested/chosen
  targets and the adjustment in status and reports, separately from measured bitrate.
- Count navigation, submission, and confirmation toward the three-tap journeys; exclude
  typing and keyboard dismissal. A submission that dismisses the keyboard still counts.
- Analysis and recordings stay local. Licensing work is excluded by owner direction;
  this record changes neither the license nor the constitution and claims no audit pass.

## Name length — owner-approved refinement (2026-10-25, T049–T051)

Host and paired-device names must contain 1–63 Unicode scalars AND at most
63 UTF-8 octets, with valid UTF-8 and no C0/C1 controls. Names exceeding either
limit are rejected with a clear “too long” message; names are never truncated.
This owner-approved decision applies equally to host display/discovery names and
device names. The 64-byte buffers include the terminating NUL. FR-005, FR-043,
FR-044 and the data-model name constraints explicitly incorporate this refinement.

## D1. Invalid or absent bitrate versus valid out-of-range bitrate

**Settled implementation policy — FR-018.** Clamping applies after strict parsing and
validation of the selected mode's bitrate field, in the units and field mapping sourced
from the pinned [protocol notes](../../../tools/moonlight-mcp/docs/protocol-notes.md).
A valid target is a positive integer representable by the negotiated field and internal
checked conversion. Reject malformed text, fractional values, negative values, zero,
duplicate/conflicting fields, and arithmetic overflow before allocating session resources.
An integer above a mode's supported maximum is still valid when representable: clamp it,
rather than misclassifying it as malformed. The same rule applies below the minimum.

An absent required target is invalid; do not manufacture a requested value or select an
undocumented default. If a supported official-client protocol variant defines a default
or fallback field, its pinned source and conformance fixture must establish that behavior
before implementation accepts it. Use the protocol's normal negotiation error, preserving
the current session. A malformed scenario is an invalid invocation (planned exit 64).
An invalid request observed in a valid negative-test scenario is a test observation,
not an excuse to discard that scenario's report.

Concrete minimum/maximum values remain **evidence pending**, per R3/T040–T045; no
numeric range is selected here. An otherwise supported valid clamp never causes refusal.

## D2. Protected audio

**Settled implementation policy — FR-022 and FR-019.** Protected content includes its
captured sound. While protection is active, emit the placeholder picture and stereo
silence on the existing session; never forward protected samples or replay queued
pre-protection audio/video. Flush or invalidate queued source content at the protection
epoch boundary. Resume source content only after the platform establishes that capture
is permitted. If protection status is unknown, withhold source content and expose the
reason rather than treating unknown as permission. Preserve media cadence and connection
when safely possible; a failure to enforce protection is a session error requiring teardown.

R3 must establish the actual protection signal and safe audio/video boundary. This policy
does not assert an SDK interface exists. Tests must distinguish intentional protected
silence from missing/failed ordinary game audio; protected intervals cannot prove an
unprotected non-silence criterion.

## D3. Measurement errors and acceptance

**Settled implementation policy — FR-047/FR-056.** A clock fault, missing stage, queue gap,
counter overflow, or unavailable delivery boundary invalidates affected evidence with a
stable reason, population/coverage, and absent numeric value. Preserve independent valid
channels and known failures. Follow the exact ranking, overflow, uncertainty, and verdict
rules in [measurement-evidence.md](measurement-evidence.md); do not replace missing
observations with zero, bounds with exact values, or independent stage percentiles with a
sum advertised as total p99.

Isolate a measurement-only failure from streaming when content safety, resource ownership,
and independent input progress remain intact. Record it immediately through bounded
error/counter state; show unavailable evidence and produce an Incomplete run when required
evidence is affected. A fault compromising those invariants ends the session with a
specific error reason and the normal input-first teardown. Never block input to repair
telemetry or collect replacement samples. These error paths do not waive the requirement
to deliver complete metrics under supported conditions. Metrics-off gaps are explicit;
they cannot satisfy a criterion that required continuous detailed collection.

## D4. Immediate input release

**Settled implementation policy — FR-034/FR-035, constitution III.** The trigger is detected
termination, disconnect, loss of contact, revocation, stop, or partial-start failure.
Invalidate admission of further session input, clear held buttons/sticks/triggers and
transient controller state, deliver neutral state where required by the platform, and
remove every session controller as the first teardown operation. Cleanup is idempotent
and covers partially initialized controllers. It never waits for media draining, network
send, exports, statistics, or a browser.

The ten-second limit is the maximum last-contact-to-session-end detection deadline,
not an additional grace period after detection. Active revocation separately has a
two-second maximum. “Immediately” orders release on the independent input/termination
path at detection; it does not invent a new numerical release SLA or reuse SC-005's
normal-input latency threshold. Record detection, neutralization, removal and failures
on one monotonic clock. R3/T042 must establish and observe the real delivery/removal
boundary; transport ACKs and fake cleanup counters cannot prove console release.
Do not claim successful release if a platform operation fails; surface the safety failure
and perform all remaining safe cleanup without retrying indefinitely or blocking video.

## D5. FR-057 reference-recording wording

**Owner decision pending — wording clarification, not a hardware question.** FR-057 says
to record HDMI simultaneously with a stream “with nothing running on the console.”
Taken literally, that excludes the host that produces the stream and the foreground
game/screens the fidelity requirements compare. No accepted clarification resolves this
phrase, so it is not silently amended here.

**Recommended default for owner approval:** “Record the console's HDMI output on the
test machine at the same time as the stream, without an additional console-side recorder,
test overlay, or measurement agent; SunshinePS5 and the selected foreground content may
run normally.” This preserves external observation, local analysis, native geometry,
10-bit HDR precision, and the independent reference chain. A source-metadata probe, if
needed, must be declared separately, including its possible observer cost; it is not
silently exempted by this recommendation.

Record the disposition by explicit spec clarification/amendment before FR-057 acceptance
(T034/T118). Portable recording models, fixtures and client preparation can proceed;
they do not establish acceptance of the unresolved wording. The client and measurement
contracts reference this open decision rather than asserting owner approval.

## Evidence gaps and closure ownership

| Item | Classification | Closure |
|---|---|---|
| Capture, hardware encode, audio/protection, input delivery/release, HDR source metadata and mode-specific bitrate bounds | Hardware evidence pending | R3, T040–T045; sourced interfaces plus real observed results |
| Actual native ten-bit capture, clock mapping, alignment, capture-chain error | Hardware/calibration evidence pending | R4, T030–T034 |
| Absolute SDR median target and objective HDR fidelity rule | Calibration-dependent spec decision pending | Measure chain error, then obtain explicit approved spec amendment in T034; do not invent thresholds |
| CPU/memory ceilings, sustainable modes and all timing budgets | Hardware evidence pending | R5, T044–T045; preserve proposed limits until explicitly amended |
| SC-009 observation rubric and fixed game/reference scenes | Acceptance-profile agreement pending | Record agreed rubric/scenes before observations in T044/T117 |
| Meaning of FR-057 phrase above | Owner decision pending | Explicit owner clarification before acceptance; recommended text in D5 |

These entries are neither passes nor waivers. Setup completion does not close them.
T001 is complete when the scope, policies, unresolved choices and their downstream
contract obligations are recorded; T045 and release remain blocked on their own evidence.

## T001/T002 setup verification record

Verified on this macOS arm64 host on 2026-10-24. These commands completed successfully,
with warning-free builds and **one setup test passed per compiler**:

```sh
export TMPDIR=/Volumes/4TB_Drive/tmp
cmake -S . -B /Volumes/4TB_Drive/tmp/sunshine-t001-t002/build-apple
cmake --build /Volumes/4TB_Drive/tmp/sunshine-t001-t002/build-apple --verbose
ctest --test-dir /Volumes/4TB_Drive/tmp/sunshine-t001-t002/build-apple --output-on-failure --no-tests=error
cmake -S . -B /Volumes/4TB_Drive/tmp/sunshine-t001-t002/build-llvm18 -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm@18/bin/clang
cmake --build /Volumes/4TB_Drive/tmp/sunshine-t001-t002/build-llvm18
ctest --test-dir /Volumes/4TB_Drive/tmp/sunshine-t001-t002/build-llvm18 --output-on-failure --no-tests=error
python3 -m json.tool cmake/dependencies.lock.json
git diff --check
```

CMake 4.3.4 selected AppleClang 21.0.0 and Homebrew Clang 18.1.8 respectively.
Compile-command inspection confirmed `-std=c11 -Wall -Wextra -Wpedantic -Werror`
and no console SDK include paths/imports. A deliberate `#warning` translation unit
was rejected by each compiler with those flags. The `sunshine_core` interface target
carries portable usage requirements; it contains no pretend host implementation.
T008 must attach actual portable runtime sources. Later test tasks register behavior
with `sunshine_add_unit_test`, which also makes `check` build its registered tests.

The following negative check returned nonzero as expected, with “No tests were found”:

```sh
cmake --build /Volumes/4TB_Drive/tmp/sunshine-t001-t002/build-apple --target check
```

`check` requires the `unit` label; the `setup` test cannot mask an absent behavior suite.
There are no core behavior tests or host acceptance results at this setup stage.

All five downloaded SDK source/release, LLVM source, OpenSSL source, and CMake source
archives were rehashed from complete local files. The SDK release, OpenSSL release,
and CMake source digests also matched publisher-provided GitHub asset digests.
The [dependency lock](../../../cmake/dependencies.lock.json) records exact URLs,
sizes, hashes, pin provenance, and verification limits. SDK source and LLVM source
hashes identify downloaded bytes without a claim of independent signature verification.
The cached local SDK release matches the downloaded release, and the installed compiler
wrapper matches that archive. The installed LLVM bottle is absent from cache and its
receipt does not identify a digest/rebuild: its binary archive provenance is explicitly
**unverified**, separately from the verified downloaded source hash. Future release
tooling must rebuild or select a verified binary rather than infer provenance from its
version string. No console, capture device or reference host was accessed.

Local-link validation passed for the five affected contracts, and all D1–D5 decisions
are present. This record does not close the owner/evidence gaps in the preceding table.

- **2026-10-25 owner decision, T032:** Physical-disconnect recovery is **owner-deferred, untested, non-blocking** for T032 after two attempts failed to correlate USB removal with a running capture; cancellation/rollback and post-run UVC restoration are evidenced; owner rationale: “timing too hard to coordinate; revisit only if it becomes the cause of a problem”.

- **2026-10-25 owner-approved spec refinement, T034:** SC-014 additionally requires
  median luma SSIM >=0.95 over the full declared compared-frame population, informed
  by the native SDR known-source measurements in [R4 calibration](../research-r4-calibration.md).
  The existing 99%-at-0.70 floor and applicable prior-release regression <=0.01 remain.
  Valid per-run calibration, complete populations and uncertainty handling are still
  required; the two short calibration scenes do not establish every gameplay/mode
  configuration. At this SDR decision, SC-013's objective HDR target remained
  unresolved; the subsequent native HDR evidence/refinement is recorded below. The owner
  approved the literal spec/profile diff before those files were edited; this
  refinement does not complete T034 or hardware readiness.

- **2026-10-25 owner-approved spec refinement, T034 HDR:** The owner approved the
  literal SC-013/Assumptions and acceptance-profile diff after two actual native
  ten-bit known-source runs. Full-population median luma SSIM >=0.97 and a joint
  99% per-frame floor of SSIM >=0.95 and Y/Cb/Cr PSNR >=35/40/40 dB supplement all
  existing HDR platform/display/visual requirements. Perfect planes satisfy their
  PSNR floor; required unmatched frames remain Incomplete. Frozen HDR ramp/patch
  rules require >=512 distinct integer column-median levels, backward step <=4
  native codes, neutral/patch median error <=8 codes, and no interior clipping
  after an 8-code reference-endpoint guard. See [calibration](../research-r4-calibration.md)
  for the measured source error, declared conversion oracle, explicit startup
  exclusions and paired causal timing bounds. Exact HDMI exposure time remains
  unmeasured; no exact display-delay correction or release pass is inferred.
  Per-run calibration, independent source metadata and complete supported-platform
  evidence remain required. This refinement does not waive any existing criterion.
