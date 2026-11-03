# Reference pairing and teardown observations

`run` and `mcp` accept `--reference-lifecycle FILE` as a trusted startup binding.
An independent reference-host observer publishes this JSON snapshot and its source
files. After `host_cancel_attempted` with `request_succeeded: true`, the runner
allows a ten-second publication window shared with `--reference-recording`.
It polls only a missing manifest path or truncated top-level manifest JSON every
100 ms. Once that manifest parses, source validation is terminal: missing source
files and malformed nested JSON are not publication waits. The parsed manifest is
passed directly into validation without rereading its path. The observer must finish
its independent cleanup checks, write all source artifacts, then atomically rename
the complete manifest onto the exact bound path. Use a fresh publication for the
session ID in this run's `declared` event; never reuse a previous run's manifest.
The complete manifest is the final publication marker, including any explicit
unavailable observations. A syntactically complete snapshot is evaluated immediately,
even when its observations are Incomplete; it is not a draft to update later.

The first validated snapshot and source bytes are held in memory until retention;
they are not reread while another binding is pending. Complete invalid snapshots
(including wrong session/scenario/version or hash mismatches) are rejected without
retry. A missing snapshot at the deadline or any invalid lifecycle snapshot leaves
the affected criteria Incomplete, preserving an existing Fail. If cancellation
fails, available snapshots are checked once without a publication wait, and the
run still reports cancellation failure. This option does not run an observer,
exercise rejection cases itself, or reevaluate an existing report.

The binding is available to the process owner, never through MCP tool arguments.
Its trust boundary is the independently operated observer. A checksum proves that
retained bytes match the supplied snapshot; it does not prove the observer's
assertions. The owner must inspect the actual observation method and sources.
Synthetic fixtures, console records and operator assertions cannot establish
reference-host acceptance by relabeling their provenance.

## Snapshot contract, version 1.0.0

The root object has exactly these fields:

| Field | Meaning |
|---|---|
| `schema_version` | `1.0.0` |
| `session_id` | Exact session ID in this run's `declared` lifecycle event |
| `scenario_id` | Exact immutable scenario ID |
| `host_version` | Exact version observed by this runner through server information |
| `test_client_version` | Exact running package version |
| `test_client_revision` | Exact tested source revision, recorded by the observer |
| `official_client_version` | Exact official client used for the independent cross-check |
| `provenance` | Only `reference_host` may establish Pass or Fail |
| `artifacts` | Up to 32 `{ "path": "relative/path", "sha256": "64 hex digits" }` objects |
| `observations` | Up to six distinct check objects defined below |

Each observation has exactly `check`, `satisfied`, `validity`, `reason`, and
`artifacts`. The last field is a nonempty array of paths bound in the root artifact
list. `validity: "valid"` requires `satisfied: true` or `false` and `reason: null`.
`validity: "invalid"` or `"unavailable"` requires `satisfied: null` and a nonempty
reason. Every supplied check needs retained sources, including sources explaining
why a measurement could not be made. Unknown fields or check names are rejected.

| Check | Required independent observation |
|---|---|
| `pairing_proof_flow` | Complete certificate/challenge/signature proof exchange, peer confirmation and persistence, correlated to this session's client identity |
| `pairing_invalid_proofs_rejected` | Deliberate invalid-proof cases rejected at the documented proof boundary, with the specific inputs and outcomes recorded in redacted source evidence |
| `pairing_official_client_cross_check` | Official client conformance for the same host version and proof flow, with exact version and source references |
| `teardown_session_terminated` | The reference host independently observes this session ended after the runner's cancellation |
| `teardown_controllers_released` | Every controller used by the declared workload is neutralized and removed; observed device state and inventory are retained |
| `teardown_resources_reclaimed` | Session-owned media, encoder, process and device resources are reclaimed, using a complete before/after ownership inventory |

Each of REF-pairing and REF-teardown passes only when all three corresponding checks
are valid and satisfied. A valid observed failure keeps that criterion Fail even
when another check is missing. Missing checks or foreign provenance leave it
Incomplete. A failed import leaves both criteria Incomplete and preserves any
earlier demonstrated Fail. Transport success, an empty session list alone, or
successful cleanup return codes alone cannot establish the complete checks.

The manifest is limited to 64 KiB. Each source is a nonempty regular file of at most
1 MiB, with at most 4 MiB of source bytes total. Sources are relative to the manifest
directory; traversal, absolute paths, symlinks, duplicate paths/checks, missing
files and digest mismatches are rejected. Keep raw private identities and PINs out
of these reviewed, redacted sources. Large recordings belong in the recording
workflow, not this bounded lifecycle snapshot.

Before changing any criterion, the runner copies the verified source bytes under
the scenario's artifact quota as `reference-lifecycle-source-NNN.dat`, then writes
`reference-lifecycle.json` with the retained paths. All retained files are listed
in the report. Exhausted byte/file quota prevents promotion. The original observer
files and historical reports are never modified.

REF-recording and REF-report remain separate obligations. Lifecycle observations
cannot establish native calibrated SDR and ten-bit HDR acquisition/comparison,
alignment, known-difference discrimination, or the report's complete correctness
matrix. Their current Incomplete status is preserved by this importer.
