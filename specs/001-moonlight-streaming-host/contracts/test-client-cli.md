# Contract: Protocol test client

**Feature**: `001-moonlight-streaming-host` | **User Story 5**
**Status**: Provisional design; overall readiness gates in [plan.md](../plan.md) remain open.

This contract separates the existing spike's command surface from proposed interfaces. It does
not claim that the spike satisfies the [spec](../spec.md). Capture, encoding, metrics budgets,
and acceptance evidence remain subject to the plan's open gates.

## Existing spike commands

The current executable's help was inspected on 2026-10-24 and matches
[the command declarations](../../../tools/moonlight-mcp/src/main.rs). Only command parsing/help
was exercised in this planning pass; that does not validate pairing or capture behavior.

| Existing command | Current purpose |
|---|---|
| `serverinfo --host HOST` | Query server information. |
| `pair --host HOST` | Pair, with `--pin-delivery manual\|wolf\|command:...\|sunshine:...`. |
| `unpair --host HOST` | Remove the client's pairing. |
| `apps --host HOST` | List host applications. |
| `quit --host HOST` | End the host's app session. |
| `record-reference --seconds N --out DIR` | Record through the existing capture path. |
| `compare --stream DIR --reference DIR --out DIR` | Compare existing recording directories. |
| `capture-server` | Serve recordings; default bind is loopback. |

The global option `--config-dir DIR` selects separate client identity state. Recording options
currently include `--capture-server ADDRESS`, `--device NAME`,
`--device-size WIDTHxHEIGHT`, and `--device-fps FPS`.

Current recording uses an SDR analysis format, and comparison samples at most 300 frames by
default. These are spike limitations, not accepted HDR or whole-population fidelity behavior.
There is no current `run`/scenario, acceptance-verdict, or MCP-server command. A connected
capture device does not by itself validate a new 10-bit recording path.

## Planned interface

The following commands and flags are proposed contracts for implementation, **not currently
available commands**:

| Planned interface | Obligation |
|---|---|
| `run --scenario FILE --report FILE` | Perform the declared session, controller sequence, recordings, and measurements; write a report even when evidence is incomplete. |
| `mcp` | Expose the same bounded capabilities and typed statuses to agents. |
| Existing `record-reference` plus `--dynamic-range sdr\|hdr` | Explicitly require the selected signal format; report Incomplete rather than substitute 8-bit/SDR for required HDR evidence. |
| Existing `compare` plus `--profile FILE --report FILE` | Apply a declared comparison population and calibrated format; produce criterion results with coverage and reliability. |

Keep existing command names for compatibility. The new scenario runner may pair when needed,
using an explicitly declared PIN-delivery mechanism. It must perform all pairing checks required
by an official client. Noninteractive runs cannot silently wait for a manual PIN prompt.

Controller scenarios cover the specified buttons, sticks, triggers, the single required controller,
rumble observation, motion, and touchpad input where supported. Keyboard and mouse work is not
included in this feature.

## Scenario document

The planned versioned JSON scenario contains:

| Field | Meaning |
|---|---|
| `schema_version`, `id` | Reproducible format and scenario identity. |
| `host`, `pairing` | Host address and explicit PIN-delivery strategy; secrets are not embedded in retained artifacts. |
| `requested` | Width, height, fps, bitrate, codec, dynamic range. |
| `duration_seconds` | Required observation duration. |
| `controllers` | Controller IDs and timestamped input actions, with expected delivery/functional observations where applicable. |
| `reference` | Whether required, device/mode, format, and calibration identity. |
| `required_criteria` | Explicit criterion IDs and acceptance-profile version; fixed before execution. |
| `evidence` | Required host metrics, input-delivery evidence, reference recording, and official-client/operator observations. |
| `output_dir` | Local artifact destination with a bounded write policy. |

A scenario cannot become passing by removing an unmeasured criterion afterward. A reference-host
compatibility scenario may declare a different applicable set from a SunshinePS5 release
scenario, but the declaration precedes execution. Missing required capabilities produce
Incomplete evidence; they are not silently skipped.

## Report guarantees

The report follows the [data model](../data-model.md) and
[measurement evidence contract](./measurement-evidence.md). It contains:

- requested and chosen settings, explicit bitrate adjustment, and separately measured bitrate;
- frame rate and interval distribution, protocol-reported pre-send latency, full host pipeline
  latency when required, host drops, and separately identified client missing/incomplete frames;
- which incomplete frames had enough received recovery information, without claiming that the
  spike actually recovered them;
- audio packet loss and non-silence observations with their populations;
- input sent/received/delivered evidence, delivery failures, and receipt-to-delivery timing;
- timestamped HDR state changes, format inspection, and separate metadata evidence;
- per-frame SSIM and PSNR, stream delay relative to the HDMI capture, alignment reliability,
  compared/excluded frame counts, and local sample image pairs;
- a Pass, Fail, or Incomplete result for every declared required criterion, plus overall status,
  reasons, environment, and evidence references.

Missing values are absent, never zero. Quantization, clock resolution, coverage, and uncertainty
are explicit. Independent client timing is a cross-check; it does not replace a host-internal
measurement with a different observation boundary.

## Status and exit semantics

For the **planned report-producing interfaces**:

| Overall status | Rule | Planned CLI exit |
|---|---|---:|
| Incomplete | At least one required criterion lacks reliable evidence. Preserve all demonstrated Fail results. | 2 |
| Fail | All required evidence is available and at least one required criterion fails. | 1 |
| Pass | Every required criterion has valid evidence and passes. | 0 |
| Invalid invocation | Arguments or scenario schema prevent a valid run declaration. Return a diagnostic; do not claim an acceptance result. | 64 |

These exit codes are a future interface contract; the current spike has not implemented them.
The agent interface returns the same typed status and reasons. Transport success or an HTTP
success code never substitutes for Pass.

Required measurement failures encountered after a valid scenario has begun produce an
Incomplete report. A physically failed or canceled session still records all available criterion
failures and session-end evidence.

## Measurement authority

The FR-057 console-recording wording is **owner decision pending** under
[implementation decisions D5](implementation-decisions.md#d5-fr-057-reference-recording-wording).
External recording without an additional console-side recorder is the recommended interpretation,
not an approved amendment. Client preparation may proceed, but FR-057 acceptance requires
explicit disposition of that wording as well as valid hardware evidence.

The protocol frame-header latency is recorded before packet-header insertion, FEC, and transmission, in
0.1 ms units. The full host pipeline is measured separately from capture start to the final
packet handed to the network stack. Do not claim that the current frame's header contains a
future final-send timestamp. The exact compatible header behavior is sourced from
[Sunshine's pinned pre-send measurement](https://github.com/LizardByte/Sunshine/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/stream.cpp#L1599-L1618).

Input transport acknowledgement is not proof of console delivery. HDMI-relative delay is not
host processing latency. Official clients remain authoritative for compatibility; this tool
supplements their acceptance matrix.

## Privacy, lifecycle, and CI

All analysis and artifacts remain on the test machine (FR-060). The capture service binds only
to loopback, confines output paths, validates requests, and releases device ownership on every
exit. Client identity and pinned certificates live in separate state. Captures, logs, and
reports omit secret pairing material.

Core CI runs pair → session → controller input → teardown using fake platform backends and this
client. Fake results establish deterministic behavior, not PS5 hardware timing, resource use,
picture fidelity, or display compatibility. Those require the on-device and official-client
evidence matrix in the measurement contract. Required CI wiring and unit coverage belong in the
next regenerated task list.
