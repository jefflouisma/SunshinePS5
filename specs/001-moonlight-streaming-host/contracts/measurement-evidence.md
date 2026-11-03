# Contract: Measurement and Acceptance Evidence

**Feature**: `001-moonlight-streaming-host` | **Date**: 2026-10-24
**Status**: Provisional design; overall readiness gates in [plan.md](../plan.md) remain open.

This contract defines what each measurement proves and how missing evidence affects acceptance.
It implements the accepted Incomplete policy in [FR-056](../spec.md). It does not convert
unverified hardware capabilities, proposed budgets, or uncalibrated fidelity thresholds into
passing results.

## Clock domains and observations

| Measurement | Start → end | Authority and limits |
|---|---|---|
| Protocol processing latency | Frame capture timestamp → compatible pre-send header measurement | Host-reported; serialized in 0.1 ms units. Zero means unavailable/repeated frame, not zero processing time. Supports FR-021 and SC-003. |
| Full host pipeline | Capture start → final packet handed to the network stack | Host monotonic clock; constitution II gate. Includes the send path and is separate from the earlier protocol field. |
| Capture/encode/packetize/send stage | Named stage boundary → completion for the same frame | Host monotonic clock. Boundaries must be documented by the platform interface. Independent percentiles cannot be added to obtain total p99. |
| Input delivery | Input arrival at the host → documented successful console-delivery boundary | Timestamp receipt before parsing/authentication; correlate only accepted authenticated inputs to event IDs. Same host clock; sending or acknowledging a packet is insufficient. |
| Client reception | First/last relevant packet or complete frame arrival | Test-machine monotonic clock. Includes different work and cannot replace host processing latency. |
| HDMI-relative stream delay | Aligned stream and reference observations | Same test-machine clock; includes capture-chain latency and alignment uncertainty. Not an absolute display-delay claim without calibration. |
| Paired-client startup | Official Moonlight selection → first presented console image | Observed official-client behavior for SC-002; test-tool request-to-first-received-frame is a separate diagnostic. |

The protocol timing boundary is confirmed by
[Sunshine's pinned header construction](https://github.com/LizardByte/Sunshine/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/stream.cpp#L1599-L1618).
Its value precedes packet insertion and transmission. Neither a later full-pipeline duration
nor a value from another frame may silently replace that compatible measurement.

Use monotonic elapsed-time clocks. Cross-process reference capture must establish a shared
test-machine monotonic domain and epoch. Wall-clock timestamps are human-readable context only.
A clock discontinuity, unsupported resolution, or unestablished mapping invalidates the affected
measurement. Reports declare native clock resolution, conversion/rounding, units, and verified
uncertainty; storage resolution alone is not accuracy evidence.

## Recorded populations and exact summaries

The proposed host representation is integer microseconds, with six channels and two fixed
histogram sets as specified in the [data model](../data-model.md). Convert a nonnegative native
clock delta to elapsed microseconds and round down, retaining the less-than-one-microsecond
quantization bound separately from clock uncertainty. Negative deltas or conversion overflow
invalidate the observation. The native clock's frequency/resolution and conversion are recorded
before acceptance; changing the rule creates a new measurement-profile version. This rule does
not replace the protocol field's compatible 0.1 ms rounding and saturation behavior.

For percentile p of N observations, select rank `ceil(p × N / 100)`, starting at rank one.
Counts include overflow observations. Return the selected recorded integer value when its rank
falls within bins 0–65,535. If the rank falls above the representable range, report an unavailable
value and the known lower bound, not a clipped value or a fabricated observation.

A known histogram population gap, lost queue record, counter overflow, or unavailable stage
invalidates affected summaries. A lower bound may demonstrate a failure, which remains in the
report, but does not replace another required missing exact measurement. Under FR-056, any
required unreliable or missing evidence makes the overall run Incomplete.

The rolling window uses observation timestamps, not ten approximate one-second buckets.
Whole-session counts survive raw-record eviction. Both summaries expose the actual measured
population and enabled intervals. Disabled intervals are never backfilled, extrapolated, or
counted as zero. An empty population has no percentile.

Frame and input raw records are aggregated before eviction. At most 600 seconds of raw samples
remain accessible, and fixed capacity may retain less; export declares the actual interval.
If capacity loss prevents an exact rolling window, that window is unavailable with its reason.
These rules describe explicit failure handling; releases must still demonstrate the required
metrics under their supported operating conditions and memory/CPU budgets.

Direct integer histograms preserve recorded values inside their declared domain. Approximate
quantile sketches or quantized equivalent-value histograms must not be substituted silently.
For comparison, [HdrHistogram documents configurable quantization and equivalent-value ranges](https://github.com/HdrHistogram/HdrHistogram/blob/de84b0a7de2378abfc405da503bf4898e84ea98e/README.md#equivalent-values-and-value-ranges);
its fixed footprint is not evidence that its outputs are exact original observations.

## Verdict completeness and precedence

Measurement-only faults invalidate affected evidence and are recorded with reasons/coverage;
they need not terminate an otherwise safe session. Faults compromising protection, ownership
or independent input progress require input-first teardown. Neither path may block input for
telemetry repair or count unavailable values as zero. See
[implementation decisions D3](implementation-decisions.md#d3-measurement-errors-and-acceptance).

The required criterion set and profile version are fixed in the scenario before execution.
Each criterion records its threshold, evidence references, population/interval, status, and
reason. An unavailable value is absent rather than zero.

1. If any required criterion is Incomplete, the overall run is Incomplete, even when other
   criteria demonstrably Fail. Preserve those Fail results.
2. Otherwise, any required Fail makes the overall run Fail.
3. Otherwise, all required criteria must have valid evidence and Pass for the run to Pass.

A criterion cannot be removed or relabeled inapplicable because evidence was not obtained.
Both Incomplete and Fail block acceptance. Known measurement uncertainty that straddles an
acceptance boundary must be resolved by valid evidence before that criterion can Pass; do not
round an uncertain result into acceptance.

## Success-criterion evidence matrix

| Criterion | Required evidence and comparison | Missing-evidence consequence |
|---|---|---|
| SC-001 | Ten first-time participants, official Apple TV client, prompts-only workflow; at least nine succeed on the first attempt within five minutes. Preserve start/end observations and participant outcomes. | Missing observations or an inadequate participant set: Incomplete. |
| SC-002 | A previously paired official client, selection event and first presented console image on a common observation timebase; duration ≤5 seconds. Include restart persistence as a separate functional observation. | Client receive time alone does not prove presentation time: Incomplete. |
| SC-003 | 1920×1080, 60 fps, wired, 30-minute run; protocol/overlay host latency for the declared frame population; at least 99% below 16.7 ms. Record wire quantization and unavailable-header count. | Inadequate duration, lost required population, or unknown latency values: Incomplete. |
| SC-004 | Same run conditions; host-side captured/sent/dropped evidence with a declared denominator, fewer than 0.1% host drops. Report network loss and incomplete client frames separately. | Client missing-frame counts without host attribution: Incomplete. |
| SC-005 | Correlated host input-arrival and successful console-delivery observations for the controller workload; at least 99% under 2 ms. Record failed deliveries separately. | Network send/ACK or client round-trip timing alone: Incomplete. |
| SC-006 | Two hours of uninterrupted on-device streaming; fixed-memory accounting including media/metrics/core allocations; no disconnect, crash, memory growth, or increase in protocol/overlay host-latency p99 between the first and last ten minutes. Retain those summaries externally before raw retention expires; also retain full-pipeline summaries for the constitution gate. | Fake-only results, missing memory domains, or absent first/last window evidence: Incomplete. |
| SC-007 | Versioned official-client matrix covering every specified platform, with pairing and streaming evidence for every entry. | Any unexercised required matrix entry: Incomplete. |
| SC-008 | Matched on-device workload, mode, network, and observation windows with detailed metrics on and off; protocol latency remains available in both states. p99 increase ≤2%; disclose repeat count and variation. | Fake backend data cannot prove hardware overhead; unmatched or unresolved noisy observations: Incomplete. |
| SC-009 | At least five named games with comparable local-display sequences while streaming and without SunshinePS5 running; no visible stutter or slowdown, with retained side-by-side observations and an agreed observation rubric. | Missing game baseline or undecided observation rubric: Incomplete. |
| SC-010 | Unpaired and revoked identities across the declared refusal scenarios; every attempted session refused, including revocation during a session and later reconnect. | Missing identity/scenario coverage: Incomplete. |
| SC-011 | 360-pixel phone viewport and required supported browsers; PIN, revoke, and metrics journeys each take at most three taps from home. Count navigation/submission/confirmation; exclude typing and keyboard dismissal, except an app submission still counts. No horizontal scrolling/zooming. | Missing journey or applicable browser evidence: Incomplete. |
| SC-012 | Explicit 30-minute 1920×1080/60 HDR run, applying SC-003 and SC-004 with the same populations and authorities. | SDR measurements or a short visual HDR check do not substitute: Incomplete. |
| SC-013 | Every supported platform using an HDR display; client HDR report when requested and sourced, agreed game/reference scenes, side-by-side observation, and 1080p60 10-bit PQ/BT.2020 reference/stream evidence showing no banding, clipping, or washed-out color; calibrated objective fidelity rule. | Missing platform, invalid format, or uncalibrated objective criterion: Incomplete. |
| SC-014 | Declared full compared-frame population in a 1080p60 SDR recording; ≥99% SSIM ≥0.70, and from release two median regression ≤0.01 on identical reference scenes. Record calibration and any subsequently approved absolute median target. | A 300-frame sample cannot establish an unspecified whole-run population; unmatched/unreliable required comparisons or absent prior-release baseline when applicable: Incomplete. |

The first release has no prior-release regression baseline; that exclusion is declared before
execution and does not waive the absolute floor. Later releases cannot omit the baseline
because it was lost. A reference-host compatibility scenario uses an explicitly different
criterion set; it must not be mistaken for SunshinePS5 release acceptance.

The constitution's full-pipeline gate is additional to SC-003: p99 must be below the negotiated
frame interval. Compare the actual interval, not a rounded display label. The proposed overall
host ceilings (256 MiB and at most one CPU-core equivalent during streaming) and their component
budgets are unverified limits from the plan, not measured results.

## Input proof and frame-loss attribution

Input records separate client transmission, any transport acknowledgement, host receipt, and
console delivery. Document the platform operation that establishes delivery; a successful call
without a known delivery meaning is not proof. Functional acceptance also observes the expected
console response to a defined controller sequence. Keyboard/mouse exercises are outside scope.

Host drop accounting distinguishes captured frames, frames accepted for encoding, completed
encodes, frames submitted to the network, and explicit host drops with reasons. Client packet
loss, unrecovered frames, and recoverable-but-not-recovered frames are separate observations.
Do not infer host drops from a missing client frame.

## Fidelity, HDR metadata, and recording integrity

FR-057's phrase “with nothing running on the console” remains **owner decision pending**.
[Decision D5](implementation-decisions.md#d5-fr-057-reference-recording-wording) recommends
external recording without additional console-side instrumentation, while the host and selected
content run normally. Require explicit clarification before FR-057 acceptance; recording
fixtures or a working capture device cannot settle that owner wording decision.

A recording preserves native resolution and the color format required by its criterion.
Normalization to the spike's 960×540 8-bit BT.709 format cannot prove native 1080p or HDR fidelity.
Comparison profiles name geometry, frame-selection population, alignment method, color
interpretation, calibration, and how unmatched frames affect completeness. Sample images aid
review but do not replace the required per-frame score population.

The stream and HDMI reference need matching timestamp/frame counts, compatible clock domains,
and sufficient temporal evidence. A still scene may permit pixel comparison while leaving
relative delay unavailable; validity is per measurement, and a required unavailable delay
still produces Incomplete.

FR-025 needs separate source-metadata evidence: compare the console's actual color/brightness
metadata with the stream-signaled metadata. The 4K X's USB picture alone is not a source-metadata
oracle. Device connection is a prerequisite; its 10-bit capture path and calibration still
require recorded evidence. The objective HDR rule and any absolute SDR median target remain
explicit hardware/calibration gates in the plan.

## Validation work required before acceptance

- Unit evidence for nearest-rank selection, threshold boundaries, overflow rank accounting,
  empty/partial windows, metrics toggles, counter overflow, and raw eviction after aggregation.
- Deterministic core tests for authorization, pairing, persistence recovery, input translation,
  session cleanup, negotiation/bitrate adjustment, and protocol parsers, in addition to
  end-to-end fake-platform sessions.
- Report tests for Incomplete precedence, preserved known failures, absent values, fixed
  required-criterion sets, and declared first-release exclusions.
- Clock/alignment tests with discontinuities and known delays; native-format SDR/HDR fidelity
  fixtures; no silent sample-cap acceptance.
- On-device CPU/memory, clock, stage timing, input delivery, and metrics-overhead evidence;
  official-client SDR/HDR matrix; the two-hour soak and game-impact observations.

These are evidence obligations for the next task-generation pass. They are not claims that the
tests, hardware paths, or planned CLI interfaces already exist.
