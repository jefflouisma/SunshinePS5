# Phase 1 Data Model: Self-Contained Moonlight Streaming Host

**Feature**: `001-moonlight-streaming-host` | **Date**: 2026-10-24
**Status**: Provisional design; overall readiness gates in [plan.md](./plan.md) remain open.

This model follows the [spec](./spec.md). It defines proposed fields and invariants, not verified
hardware capabilities. Capture, encoding, clock behavior, and CPU/memory budgets still need
on-device evidence. Measurement semantics are defined in the
[measurement evidence contract](./contracts/measurement-evidence.md).

## Host

One console installation.

| Field | Type | Rules |
|---|---|---|
| `display_name` | text | Default `SunshinePS5`; owner-editable, 1–63 Unicode scalars AND at most 63 UTF-8 octets; reject over-long names with a clear “too long” message (FR-005, FR-044). |
| `identity` | key pair and certificate | Generated on the console at first run, persistent across restarts, never committed. |
| `address` | observed network address | Shown in the startup notification and web app; not a replacement for identity. |
| `state` | enum | `starting`, `running`, `stopping`. |
| `version`, `firmware_version` | text | Detected values; firmware other than 13.60 is refused (FR-003). |
| `supported_modes` | bounded capability list | Each mode names geometry, frame rate, codec, dynamic range, and declared minimum/maximum bitrate. Only validated modes are offered. |

A renamed host becomes visible on the next client refresh. Source-address eligibility follows
FR-040, including reachable routed private subnets; eligibility does not bypass pairing or the
one-session limit. Concrete bitrate bounds are evidence-dependent capabilities, not invented
constants in this model.

## Paired Device

| Field | Type | Rules |
|---|---|---|
| `id`, `certificate` | certificate identity | Identity comes from the paired certificate, not a client-supplied name or identifier. |
| `name` | text | Owner-editable, 1–63 Unicode scalars AND at most 63 UTF-8 octets; reject over-long names with a clear “too long” message; client-supplied text is untrusted. |
| `paired_at` | timestamp | Established once on successful pairing. |
| `last_session_at` | timestamp or absent | Updated after a session ends. |

Lifecycle: successful pairing creates the record; rename and session completion update it;
revocation deletes it durably and ends that device's active session within two seconds.
Requests from identities outside the paired set are refused (FR-012). An unreadable paired-device
store starts empty with an explanation; partially read authorization state is never accepted.

## Pairing Request

At most one request is pending (FR-009).

| Field | Type | Rules |
|---|---|---|
| `id` | unique request identifier | Changes for every request; PIN submission must match the current pending request, so stale submissions are rejected. |
| `started_at`, `expires_at` | monotonic deadline plus display time | Expiration is five minutes after the request starts. |
| `state` | enum | `pending` → `completed`, `failed`, or `expired`; terminal states are final. |
| `client_name` | text | Untrusted display metadata. |

The host's pairing-attempt state holds the consecutive-failure counter across requests, rather
than discarding it with each request. Five failed PIN entries trigger a 60-second refusal window;
successful pairing resets the counter (FR-010). A retry after a terminal failure is a new
request. Every request and completed pairing generates the specified console notification.

## Streaming Session

At most one active session, belonging to a currently paired device (FR-020).

| Field | Type | Rules |
|---|---|---|
| `id` | session identifier | Correlates host, client, metrics, and evidence records. |
| `device` | Paired Device reference | Must remain authorized. |
| `state` | enum | `negotiating`, `active`, `ending`, `ended`. |
| `started_at` | timestamp | Monotonic duration and wall-clock display context are separate. |
| `width`, `height`, `fps`, `codec` | negotiated values | Must identify a validated offered mode. |
| `dynamic_range` | enum | `sdr` or `hdr`; changes are timestamped and signaled without reconnecting. |
| `requested_bitrate_kbps` | integer | Preserved exactly from the client request. |
| `chosen_bitrate_kbps` | integer | Request within bounds; otherwise the nearest supported minimum or maximum (FR-018). |
| `bitrate_adjustment` | enum | `none`, `raised_to_minimum`, or `lowered_to_maximum`. |
| `measured_bitrate_bps` | measurement or absent | Observed bytes over a stated interval; never substituted for the chosen encoding target. |
| `controllers` | bounded list | Up to four; IDs, connection order, state, and delivery-result counters. |
| `end_reason` | reason or absent | Assigned exactly once; retained in diagnostics. |

Requested/chosen bitrate and any adjustment appear in both status and the report. Adjustment
alone does not refuse an otherwise supported session.

End reasons include `client_quit`, `client_lost`, `revoked`, `host_stopping`,
`console_conflict`, and `error` with a specific diagnostic code. Every end path, including
partial establishment and errors, releases all injected input, removes the session's controllers,
frees capture/encode resources, and preserves the foreground game. Loss of contact ends the
session within ten seconds of last contact. No input-delivery path waits on video work.

## Metrics Capture and Session Aggregates

These are separate records: raw retention ends after at most 600 seconds, while session
aggregates retain counts for every valid recorded sample until the session is replaced.
Detailed timing is recorded only while metrics are enabled (FR-046–FR-052).

| Field | Type | Rules |
|---|---|---|
| `session_id` | identifier | Links every record to one session; no cross-session aggregation. |
| `clock` | clock description | Clock domain, observed native resolution, conversion rule, and measurement uncertainty. |
| `enabled_intervals` | bounded coverage description | Records enabled/disabled transitions and coverage gaps; overflow of this description invalidates coverage rather than growing memory. |
| `frame_samples` | fixed raw ring | Frame ID/time and capture, encode, packetize, send, full-pipeline durations; outcome/validity flags. |
| `input_samples` | fixed raw ring | Event ID, controller, time, receipt-to-console-delivery duration, delivery result, validity flags. |
| `retained_interval` | actual first/last retained times | Report separately for frame and input records; never promise ten minutes when capacity retained less. |
| `whole_session` | six fixed histograms | Counts all valid detailed samples from enabled periods, including samples whose raw records expired. |
| `last_10_seconds` | six fixed histograms | Exact rolling window over recorded timestamps; expiry removes the original observation from its bin. |
| `population` | counts per channel/window | Seen, represented in bins, overflow, missing/lost, and failed-delivery counts. |
| `validity` | state and reasons | Includes queue gaps, counter overflow, invalid clocks, unavailable stages, and insufficient coverage. |
| `continuous` | bounded counters | Host frame outcomes, measured bitrate, and separately identified client loss/recovery reports. |
| `context` | record | Versions, mode, dynamic range, requested/chosen bitrate, and client name for export. |

### Proposed fixed allocation

These are design ceilings inside the host's proposed 256 MiB total budget, not measurements.
The metrics allocation is **80 MiB**; the other planned allocations are 128 MiB for media and
48 MiB for core/management. CPU and total memory gates remain open in the plan.

| Allocation | Capacity | Bytes |
|---|---|---:|
| Input raw records | 2,097,152 records × 32 bytes | 67,108,864 (64 MiB) |
| Frame raw records | 131,072 records × 64 bytes | 8,388,608 (8 MiB) |
| Whole-session and rolling histograms | 2 × 6 × 65,536 bins × 8-byte counters | 6,291,456 (6 MiB) |
| Producer queues, coverage state, counters, snapshots | Fixed pool | 2,097,152 (2 MiB) |
| **Total** | Allocated before active streaming | **83,886,080 (80 MiB)** |

Records must fit these declared fixed sizes. No sample list, gap list, or export staging area
may grow with session duration. Export streams retained records through bounded scratch space.
The proposed 64-byte frame record holds an 8-byte frame ID, an 8-byte observation timestamp,
five 8-byte durations, and 8 bytes of flags/reserved fields. The 32-byte input record holds an
8-byte event ID, an 8-byte observation timestamp, an 8-byte duration, and 4-byte controller and
flags fields. Raw durations retain their recorded values even when a histogram rank overflows.
The statistics worker owns both histogram sets; published snapshots contain derived values and
coverage, not copies of entire histograms. Steady-state producers perform no heap allocation.

### Recording and expiry rules

- Each channel records nonnegative elapsed durations in integer microseconds, rounding down
  after converting the native clock delta. The conversion and its less-than-one-microsecond
  quantization bound accompany the samples, separately from native clock resolution and
  uncertainty; a one-microsecond storage unit is not a claim of one-microsecond clock accuracy.
- Each histogram has bins for recorded values 0–65,535 microseconds and separate counts for
  larger observations. Quantiles use nearest rank over the full population, including overflow.
  An in-range selected bin gives an exact percentile of the recorded integer values. If the
  selected rank lies in overflow, the value is unavailable with a lower bound; it is never
  clipped, interpolated, or advertised as exact. A known population gap invalidates affected
  percentiles. See the evidence contract for acceptance consequences.
- Producers use separate preallocated queues. Input production never waits for video or for
  the statistics worker. Queue exhaustion is an explicit measurement/session error with a
  recorded reason and an Incomplete evidence outcome, not permission to discard samples silently.
- The worker aggregates every frame/input raw record before that record becomes evictable.
  It updates whole-session and rolling counts before expiring raw records.
- Raw records older than 600 seconds are discarded. Capacity may shorten raw retention, but
  actual coverage is exported. If capacity cannot preserve the observations needed to expire
  the rolling ten-second window correctly, that window becomes invalid.
- With metrics off, no extra stage/input timestamps or detailed samples are collected.
  FR-021's compatible protocol latency remains available. Existing aggregates are preserved
  with explicit disabled intervals; no zero values or invented samples fill the gaps.
- The last-ten-second population and whole-session population cover enabled observations only.
  Their coverage metadata must prevent an uninterrupted-session interpretation when gaps exist.
  No observations yields absent percentiles, not zero latency.
- At least once per second, the worker publishes a consistent small snapshot. The frame and
  input paths do not wait for readers, exports, or browsers. Collection while enabled is
  independent of whether any browser is connected.
- All counters have checked overflow handling. Buffer, timing-range, and population failures
  invalidate affected measurements and block dependent acceptance. They are failure handling,
  not permission to ship missing required metrics.

### Derived presentation

Median, p95, and p99 are computed from the retained aggregate state, not by reconstructing an
entire session from the ten-minute raw ring. Highlight the capture/encode/packetize/send stage
with the largest p99 share of the frame interval; show ties together. Input timing is separate.
The over-budget indication uses full-pipeline p99, not the sum of independently calculated
stage percentiles and not the protocol header's pre-send latency. An invalid required statistic
is shown as unavailable with its reason, never as an in-budget result.

## Settings

| Field | Type | Rules |
|---|---|---|
| `host_name` | text | Follows Host.`display_name`. |
| `metrics_enabled` | boolean | Default false; persisted after a toggle (FR-046). |

Unreadable settings fall back to defaults with a diagnostic. Toggling takes effect within two
seconds without interrupting an otherwise healthy session.

## Measurement and Input Evidence

| Field | Type | Rules |
|---|---|---|
| `id`, `session_id` | identifiers | Stable references from criterion results. |
| `kind`, `source` | enums | Host pipeline, protocol header, client receipt, reference recording, or operator observation. |
| `interval`, `clock_domain` | timestamps and clock ID | Define where the measurement starts/ends; unrelated clock values are never directly subtracted. |
| `units`, `resolution`, `uncertainty` | metadata | Explicit quantization and verified accuracy where available. |
| `population`, `coverage` | counts and validity | Identify expected versus observed data and all exclusions. |
| `value`, `lower_bound`, `upper_bound` | nullable values | Missing numeric evidence is absent, never zero. Bounds do not masquerade as exact values. |
| `validity`, `reason`, `artifact_refs` | state and references | Reproducible evidence and an explanation of any deficiency. |

Input evidence distinguishes `client_sent`, transport acknowledgement if available,
`host_received`, and `console_delivered`. Only a documented successful platform-delivery
boundary supplies the endpoint for SC-005. Expected visible console response is a separate
functional observation; a successful network send proves neither injection nor a visible effect.
Keyboard and mouse scenarios are outside this feature's planned input scope.

## Test Report

One versioned machine-readable record per run, including incomplete runs (FR-056).

| Field | Type | Rules |
|---|---|---|
| `schema_version`, `run_id`, `session_id` | identifiers | Keep format version and correlation explicit. |
| `scenario` | immutable declared input | Requested mode, duration, controller sequence, and required criterion IDs are fixed before the run. |
| `environment` | record | Firmware, host/client versions, network, devices, reference chain, clocks, and calibration IDs. |
| `requested`, `chosen`, `bitrate_adjustment` | records | Preserve both requested and actual chosen targets; measured bitrate is separate. |
| `video`, `audio`, `input`, `hdr_events`, `fidelity` | evidence references | Include all FR-056/FR-057 fields and their validity. |
| `criteria` | result list | Each required criterion has ID, threshold, evidence IDs, Pass/Fail/Incomplete, and reason. |
| `overall_status` | enum | Pass, Fail, or Incomplete according to the precedence below. |
| `limitations`, `artifact_refs` | bounded metadata | Point to local streamed evidence files; do not accumulate all frame data in report memory. |

**Accepted precedence**: any required Incomplete criterion makes the run Incomplete, while
demonstrated Fail results remain recorded. With complete required evidence, any Fail makes the
run Fail. Pass requires all required criteria to have valid evidence and pass. Missing evidence
cannot remove a required criterion or make it inapplicable. Both Incomplete and Fail are
non-success outcomes for automated callers.

## Reference Recording

| Field | Type | Rules |
|---|---|---|
| `source`, `capture_mode`, `calibration_id` | metadata | Identify device, geometry, frame rate, and calibration; device presence alone is not calibration evidence. |
| `frames` | local file sequence | Preserve the resolution and signal format needed by the criterion. |
| `timestamps` | one per frame | Use the same test-machine monotonic domain as stream receipt, including across capture-service processes. |
| `pixel_format`, `bit_depth`, `color_space` | explicit format | SDR and 10-bit HDR PQ/BT.2020 remain distinct. |
| `frame_count`, `timestamp_count`, `validity` | counts and state | A disagreement invalidates the recording. |
| `metadata_evidence` | separate evidence reference | Source HDR metadata cannot be inferred merely from USB pixel capture. |

HDR comparison never first converts either recording to SDR. Wall-clock time is optional
human-readable context, not an elapsed-time clock. A clock discontinuity or unestablished
cross-process clock mapping invalidates affected alignment. Reports and captures remain local.
