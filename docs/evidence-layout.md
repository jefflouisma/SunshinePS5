# Local evidence layout

This is the T005 storage and review policy for [FR-050, FR-053 and
FR-060](../specs/001-moonlight-streaming-host/spec.md). It defines destinations and
limits for future evidence writers; it does not claim that retention enforcement,
exporters, recording, calibration or hardware acceptance is implemented. The
[measurement contract](../specs/001-moonlight-streaming-host/contracts/measurement-evidence.md)
remains authoritative for clocks, populations and verdicts.

## Destinations and ownership

All collection, decoding, comparison and reports stay on the test machine. Use a
local disk outside cloud-synced folders; never upload evidence through telemetry,
CI artifact upload, an external analysis service or a repository remote. The
following paths are repository-relative unless described as external:

| Destination | Contents | Version control |
|---|---|---|
| `evidence/local/runs/<run-id>/` | Original reports, frame scores, timestamps, metric exports, diagnostics, reference/stream recordings and sample images | Entirely ignored |
| `evidence/local/state/<identity-id>/` | Separate test-client identity/config directory, pinned certificates and pairing state; never reuse a production identity in fixtures | Entirely ignored |
| `evidence/local/private/` | Device serial inventory and optional mapping from opaque device aliases to physical devices | Entirely ignored |
| `evidence/runs/<run-id>/` | Only the reviewed metadata filenames below | Allowlisted for local Git tracking |
| External local disk, referenced by an opaque artifact ID | Large captures exceeding the working disk reservation | Outside Git; same privacy and retention policy |
| `build/`, `out/`, `target/` | Generated host, helper and client binaries and intermediate outputs | Ignored |

Use run IDs such as `20261024T210000Z-sdr-001`, never a serial, account name,
address, PIN or token. Each run owns a new directory; do not mix independent runs.
The planned runner's `output_dir` must resolve inside its declared local root,
reject traversal/symlink escapes and write bounded files outside input/media hot
paths. Point the existing client's `--config-dir` at its isolated local state.
Use owner-only directories/files (`0700`/`0600`) for local state and evidence.
Scratch work belongs under `/Volumes/4TB_Drive/tmp/<task-or-run-id>/`, with
`TMPDIR=/Volumes/4TB_Drive/tmp`; it is not durable acceptance evidence.

## Small retained records

Only these exact filenames directly inside `evidence/runs/<run-id>/` are eligible
for tracking; subdirectories and all other filenames remain ignored:

| Filename | Required retained information |
|---|---|
| `manifest.json` | Schema/run/session IDs, immutable scenario and acceptance-profile IDs/versions/digests, declared criterion set, real/fake/reference-host scope, start/end context, requested/chosen mode and bitrate, end reason, criteria and Pass/Fail/Incomplete, coverage/limitations, artifact IDs and retention disposition |
| `provenance.json` | Source/build/environment provenance described below |
| `checksums.sha256` | SHA-256, byte size referenced in manifest, and relative local artifact ID for each closed file; retain even after an artifact expires |
| `first-ten-minutes.json` | External first soak-window summary, including protocol and full-pipeline timing and memory accounting |
| `last-ten-minutes.json` | Matching final soak-window summary and drift comparison |

These are redacted summaries, not copied raw reports. Git trackability is not
permission to publish them: FR-060 keeps actual reports local. Review every field
before local staging, and keep actual-run records out of any push or remote
artifact transfer. Synthetic, clearly labeled examples may be shared as fixtures
only when they contain no actual-run data or reusable identity material.

Preserve required firmware, host/client version, mode, codec, capture format,
clock/calibration context and the client device name required by FR-050 in local
exports. Use nonidentifying test device names; do not discard required context to
make an export look shareable. Exported diagnostics include recent bounded logs,
session end reasons, current metrics capture when present and version information.
An unavailable capture is explicitly absent with a reason. Neither export includes
private keys, PINs, passwords, tokens, raw pairing challenges/secrets or serials.

## Bounded retention

The defaults below are a local storage policy, separate from console memory
budgets. A run declares byte, file-count and duration limits before collection;
any override and its storage reservation go in its manifest. Limits never justify
sampling away a required population or substituting 8-bit/SDR for native HDR.

| Data | Limit and expiry |
|---|---|
| Host raw metric samples | At most 600 seconds, possibly less at fixed capacity; aggregate before eviction and export actual interval/gaps |
| Local bulk evidence | Default 2 TiB per run, 3 TiB across retained runs, 100,000 files per run and seven days after closure; declare a shorter rotation/expiry if space is tighter |
| Diagnostics/log export | At most 32 MiB per run within the bulk quota; rotate with explicit lost/retained intervals |
| Retained metadata | At most 1 MiB per file and 8 MiB per run; retain at most 100 unreferenced development run summaries for 30 days, in addition to explicitly named release/baseline records |

Before starting, reserve sufficient space for the scenario's complete required
population and metadata. Native recordings can exceed the defaults: reserve an
adequate external local disk and declare finite higher limits, or record that the
run cannot supply the evidence. Stream records to bounded files; never accumulate
an entire recording/report population in memory. On quota exhaustion, storage
failure, cancellation or lost evidence, close what can be closed, record actual
coverage and an Incomplete result for affected required criteria, preserving known
Fail results. Never overwrite active evidence or silently drop frames to pass.

At run closure, checksum closed artifacts and finalize the manifest atomically.
Pruning is an explicit local maintenance operation on closed runs, oldest expired
first; record which artifact IDs expired/deleted and when, preserving their
checksums, summaries and known failures. Do not delete active runs or artifacts
reserved by an unresolved investigation. Reservations themselves require a finite
byte cap and expiry/review date and count against the declared disk quota; if they
leave insufficient space, refuse another run. Never prune tracked history as an
automatic side effect of a recording.

SC-006 needs a full two-hour on-device soak. Persist the external first-ten-minute
summary as observations arrive, before host raw eviction; finalize the last window
on completion. Each summary records exact interval, clock/conversion/uncertainty,
expected/observed counts and exclusions, enabled intervals, protocol latency p99,
full-pipeline p99, negotiated frame interval, all memory domains/high-water marks,
disconnect/crash evidence and validity. Whole-session counts survive raw expiry.
Missing first/last evidence or unknown memory domains means Incomplete. Keep the
manifest, checksums and both summaries after bulk expiry. Reserve named release
records and the identical-scene prior-release fidelity baseline needed from release
two; record a finite retention reservation and renew it before expiry or rerun the
required baseline. Loss is never permission to waive that criterion.

## Source and build provenance

Record provenance at build/run time, with unknown values explicitly absent:

- Full repository commit, branch as context, dirty flag and digest of the exact
  source snapshot used. For a dirty build retain a local source archive/patch
  (including relevant untracked inputs) and its checksum; a commit alone cannot
  reproduce it. Keep any sensitive patch content in local bulk storage.
- SHA-256 of `cmake/dependencies.lock.json`, `Cargo.lock`, toolchain files and
  applicable scenario/profile/calibration inputs. Record pinned upstream commits
  from [protocol notes](../tools/moonlight-mcp/docs/protocol-notes.md), downloaded
  archive origin/checksum and verified-versus-unverified provenance separately.
- Actual compiler, SDK, CMake, Rust/Cargo, OpenSSL, FFmpeg/ffprobe, Opus and USB-helper
  versions as applicable; sanitized configure/build commands, flags, target and
  host OS/architecture, plus executable/library hashes. Use the lockfiles as
  authority for pins rather than duplicating version numbers here.
- Firmware, host/test-client and official-client versions, reference-host identity
  as an opaque alias/version, network conditions, controller/capture model and
  mode, native geometry/bit depth/color space, clock domains/mapping, calibration
  IDs and source-HDR metadata provenance. Never record device serials or local
  private paths in the retained copies.
- Artifact IDs, sizes, checksums, schema/profile versions, exact verification
  commands and observed outcomes. Distinguish synthetic fixtures, reference-host
  compatibility and PS5 on-device acceptance. A built binary, enumerated device,
  fixture or successful ACK does not establish console capability or delivery.

Checksums establish file identity, not validity or calibration. If raw evidence
needed for a new comparison has expired, its checksum cannot replace it. Missing
or unreliable required evidence remains Incomplete; retain demonstrated failures.

## Ignore-policy verification

From the repository root, these non-writing probes must print the matching ignore
rule and return zero (the sample files need not exist):

```sh
git check-ignore -v evidence/local/state/test-client/client.pem
git check-ignore -v evidence/local/runs/example/reference/frame-000001.p010
git check-ignore -v evidence/local/private/device-serials.json
git check-ignore -v build/sunshineps5.elf
git check-ignore -v target/release/moonlight-mcp
git check-ignore -v evidence/runs/example/raw-capture.mkv
```

Each retained path below must print nothing and return one, indicating it is not
ignored. No force-add is required or permitted to bypass the evidence policy:

```sh
git check-ignore evidence/runs/example/manifest.json
git check-ignore evidence/runs/example/provenance.json
git check-ignore evidence/runs/example/checksums.sha256
git check-ignore evidence/runs/example/first-ten-minutes.json
git check-ignore evidence/runs/example/last-ten-minutes.json
```

Ignore rules operate on filenames, not contents, and do not remove files already
tracked. Review retained JSON/checksum contents for secrets, serials, private
paths and copyrighted frame content before staging. No retained-file exception
applies inside `evidence/local/`, a secrets directory or a raw-capture directory.
