# Reference recording and report evidence

`run` and `mcp` accept `--reference-recording FILE` as a trusted process-startup
binding. The runner reads it after host cancellation, evaluates it, and retains
the checked source bytes under `reference-recording/` within the run's quota.
Only this run's REF-recording and REF-report criteria are updated. Historical
reports and observer files are read-only. MCP tool arguments cannot supply this
binding.

After `host_cancel_attempted` with `request_succeeded: true`, publication has a
ten-second window shared with `--reference-lifecycle`, polling every 100 ms.
Write all artifacts first, then atomically rename the complete manifest onto the
bound path. Correlate it with this run's `declared` session ID, scenario and observed
versions. The manifest is the final publication marker: complete snapshots with
missing cases remain Incomplete and are not retried as drafts. Only a missing
manifest path or truncated top-level manifest JSON is retried until the deadline.
The parsed manifest is passed directly into validation without rereading its path.
A complete manifest with a missing source returns Incomplete immediately; nested
report or score JSON truncation returns Fail immediately. Neither accepts a later
replacement publication. Complete invalid or foreign
snapshots are rejected immediately. At timeout, missing sources remain Incomplete;
truncated/malformed JSON remains Fail. Existing Fail results retain precedence.
Validated bytes are held and copied without rereading the observer's files.
Failed cancellation performs one immediate import attempt, without waiting, and
the run retains its cancellation failure. This binding does not launch an observer
or establish that cleanup succeeded simply because the cancellation request did.

The independent observer supplies acquisition provenance, calibration, known-scene
expectations and report oracles. The evaluator verifies data against those
expectations. Hashes bind bytes; they do not authenticate an observer or prove an
acquisition occurred. Review the observation method and oracle before binding it.
Synthetic tests exercise the mechanism and supply no live acceptance evidence.

## Manifest 1.0.0

The root has exactly these fields:

- `schema_version`: `1.0.0`.
- `session_id`, `scenario_id`, `host_version`: this run's session, immutable
  scenario and observed server-info version.
- `test_client_version`: current package version; `test_client_revision` and
  `official_client_version`: exact versions recorded by the observer.
- `profile_id`: `reference_host_compatibility`; `profile_version`: `1.0.0`;
  `profile_sha256`: SHA-256 of the exact compiled
  `tests/scenarios/profiles/acceptance.json` bytes.
- `provenance`: only `reference_host` may establish acceptance.
- `artifacts`: up to 64 `{path, sha256}` objects. Paths are unique relative paths
  beneath the manifest directory; `manifest.json` is reserved. Digests are
  lowercase SHA-256. Files must be nonempty regular files without symlinks in any
  path component. Traversal and digest mismatches are rejected.
- `populations`: up to four distinct native population objects.
- `reports`: up to four distinct report correctness cases.

The manifest limit is 4 MiB, each artifact is at most 64 MiB and all artifacts
together are at most 256 MiB. This supports small declared conformance populations,
not long release recordings. The bounded snapshot ensures retained bytes are the
evaluated bytes. A smaller scenario byte/file quota also prevents promotion when
retention fails.

## Native populations

Required kinds are `sdr`, `hdr`, `sdr_known_difference` and
`hdr_known_difference`. Each object has:

- `kind`, `stream`, `reference`: existing native Recording records whose `frames`
  paths bind retained raw sequences. Every timestamp/frame must be present.
  Geometry, cadence, color space and clock domain agree. Supported samples are
  yuv420p BT.709 eight-bit and p010le/yuv420p10le PQ/BT.2020 ten-bit, at least
  16x16 for plane scoring.
- `calibration`: null when unavailable, otherwise `{id, provenance, source,
  uncertainty_ms}`. Both recordings name this calibration ID; `source` binds
  retained calibration evidence. Uncertainty is finite/nonnegative and provenance
  must be `reference_host`.
- `source_hdr`, `stream_hdr`: null for SDR; HDR requires both with identical
  epoch/values and independent retained source evidence. Fields are `epoch`,
  `provenance`, `source`, `mastering_max_nits`, `mastering_min_nits`,
  `max_cll_nits`, `max_fall_nits`, `primaries_xy` (three x/y pairs), `white_xy`.
  Both recordings' `metadata_evidence` names the same epoch. Luminance and
  chromaticity ranges are checked. USB packing alone cannot supply source metadata.
- `delay_ms`, `expected_delay_ms` (lower/upper), `expected_ssim` (lower/upper),
  `oracle` (retained independent known-scene evidence). Freeze these expectations
  before testing. Known-difference SSIM upper bounds must exclude 1.0. These
  intervals test detection of known inputs, not release fidelity thresholds.
- `scores`: retained JSONL using existing `compare::FrameResult` records, one per
  stream frame in order. All frames require a unique reference partner, actual
  indexed timestamps and the signed residual
  `(stream_time - reference_time) * 1000 - delay_ms`. Exclusions/partial populations
  are rejected. Every native Y/Cb/Cr plane is unpacked and rescored; SSIM, exact
  SSIM bits, finite PSNR or perfect-plane markers must match emitted records.
  Ten-bit packing is validated first.

Complete calibrated populations pass only when the full delay interval, every
timestamp residual and every luma SSIM meet the independent oracle. A delay
interval wholly outside the accepted interval fails; an interval that overlaps
the boundary is Incomplete. An independently demonstrated score or residual
failure is preserved even when delay uncertainty overlaps a boundary. Missing
calibration/source HDR yields Incomplete. All four
kinds are required; a demonstrated Fail survives missing kinds. Foreign provenance
always remains Incomplete.

## Report matrix

Required kinds are `pass`, `fail`, `incomplete` and `fail_under_incomplete`.
Each has `kind`, `expected`, `emitted`, `oracle`; the last three bind artifacts.
Prepare the expected full report and oracle independently. Neither may use the
emitted file's path.

Both reports must pass existing `Report::validate`, including all fields,
measurement validity, session correlation, criterion coverage and status
consistency. The expected report names this host/client version and the case's
prescribed status. The final case retains both Fail and Incomplete criteria under
overall Incomplete. All report/measurement artifact references must be bound.
Full typed report equality checks every measurement and criterion. Any unequal
value yields Fail; missing cases yield Incomplete. A failure survives missing cases.

This finite matrix validates the frozen reference population, not every possible
future report. The independent oracle must cover the declared SDR/HDR and
known-difference populations. Missing/unreadable files remain Incomplete; malformed
or contradictory evidence fails validation. Retention quota failure blocks Pass
promotion but preserves each already evaluated Fail, records the retention
limitation, and cites no unretained source references. Demonstrated failures are
preserved when later evidence becomes unavailable.

No live calibrated recording/report matrix is supplied by this implementation.
T038 remains open pending a fresh independent reference-host run.
