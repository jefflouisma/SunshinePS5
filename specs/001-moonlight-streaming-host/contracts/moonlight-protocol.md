# Contract: Moonlight protocol (host side)

**Feature**: `001-moonlight-streaming-host` | **Status**: Provisional; see [readiness gates](../plan.md).

Unmodified official Moonlight clients define interoperability. The pinned
[protocol notes](../../../tools/moonlight-mcp/docs/protocol-notes.md) supply wire references;
this contract records obligations, not a second set of invented wire constants. Every constant
and packet layout in implementation must cite its source.

## Network admission and identity

- Apply FR-040 to the actual transport peer on every exposed service: accept RFC1918 IPv4,
  IPv6 ULA `fc00::/7`, and on-link IPv6 `fe80::/10`; reject all other source addresses.
  Normalize IPv4-mapped IPv6 before classification. Do not trust forwarded-address headers.
- Reachable private routed subnets are permitted. Automatic discovery is tested on the local
  link; manual address entry covers routed segments without requiring multicast forwarding.
- No UPnP, NAT-PMP, relay, cloud dependency, PSN interaction, or telemetry.
- Generate the host identity on the console at first run; persist it with restricted file access.
  Pairing proves device identity. A private source address alone never authorizes a session.

## Required surfaces

| Surface | Behavior | Requirements |
|---|---|---|
| Discovery | Default name SunshinePS5; discovery plus manual entry; renamed host appears on refresh | FR-004, FR-005, FR-044 |
| Pairing | Standard PIN exchange, including every certificate/signature/challenge check official clients enforce | FR-006–FR-012 |
| App list | Exactly one entry named PS5; start streams current console output; quit ends only the stream | FR-023, FR-036 |
| Session setup | Authenticate, reserve resources, negotiate sustainable mode, and admit one active session | FR-012, FR-017–FR-020 |
| Video/audio | Console output and stereo game audio; protected content replaced by a placeholder without disconnecting | FR-016, FR-019, FR-022 |
| Processing latency | Populate the field supported clients show in their overlay, with upstream units and unavailable semantics | FR-021 |
| HDR | Negotiate and signal actual output state and console-source metadata, including transitions | FR-024–FR-029 |
| Controller input | One required separately assigned controller (up to four accepted), required buttons/axes, motion and touchpad data when supplied | FR-030, FR-031, FR-033 |
| Feedback | Forward rumble when the client controller supports it | FR-032 |
| End/disconnect | Release input immediately on termination/loss detection by neutralizing the virtual controller (park it registered to avoid a disconnect notice), free resources, log reason | FR-034–FR-039 |

## Pairing and session lifecycle

Only one pairing request may be pending; it expires after five minutes. Additional requests
are refused without replacing it. Five consecutive failed PIN entries start a 60-second
cooldown; neither resubmission nor a new request bypasses it. Console notifications identify
the request and completed pairing. Correct PIN completion is visible to the client within
three seconds (US1 acceptance), and pairing survives host and console restarts.

A session transitions through negotiation, reserved resources, active streaming, and teardown.
Authorization is checked at admission and on control/input operations; stale session credentials
cannot bypass revocation. Refuse a second session without disturbing the first. Revocation ends
the affected active session within two seconds, persists across restart, and requires re-pairing.

Loss of contact ends the session no later than ten seconds after the last contact. On detected
loss or any termination, release every held input before freeing the other session resources.
Partial initialization follows the same cleanup rules. Remote Play or Share Play in use refuses
new sessions; either starting mid-session ends it cleanly with a recorded reason.

Release begins at detection on the independent input/termination path, after refusing new
session input and before any media drain or export. Neutralize and remove even partially
initialized controllers; cleanup is idempotent. The ten-second loss deadline is not a grace
period after detection. Record release failures without claiming successful console delivery.
See [implementation decisions D4](implementation-decisions.md#d4-immediate-input-release).

Protection covers captured audio as well as video: maintain the session using a placeholder
and stereo silence, invalidate queued source content across the protection boundary, and
withhold source content when protection state is unknown. See
[D2](implementation-decisions.md#d2-protected-audio); the real platform signal remains unverified.

## Mode and bitrate negotiation

The capability record is per resolution, frame rate, codec, and dynamic range, with measured
hardware support, sustainable timing, and a supported bitrate interval. Baseline requirements
are 720p/1080p at 30/60 fps and HDR at 1080p60. Proposed codecs are H.264 for SDR and HEVC Main10
for HDR, subject to R3 hardware evidence and official-client conformance. Additional modes
require separate evidence; software video encoding is not a shipping fallback.

Decline unsupported mode requests through normal negotiation before activation. HDR is a
required feature: declining an unsupported HDR mode prevents a broken session but does not
satisfy the release requirement. Missing hardware support remains a readiness error.

For a supported mode, choose:

`chosen_bitrate = min(max(requested_bitrate, supported_min), supported_max)`

The lower/upper limits come from that mode's verified capability record. An adjustment alone
must not prevent starting. Preserve requested bitrate, chosen target, bounds, and adjustment
reason (`none`, `raised_to_minimum`, or `lowered_to_maximum`) in session state, the web UI, and the report.
Measured encoded bitrate and network throughput are separate observations. No supported numeric
bitrate range has been established by this planning run.

Clamp only a valid positive, representable target. Malformed, zero/negative, overflowing,
conflicting, or absent required bitrate fields fail negotiation before resource reservation.
Do not invent an omitted-field default; any supported official-client fallback needs pinned
source and a conformance fixture. A representable value outside the mode's supported range
remains eligible for clamping. See
[implementation decisions D1](implementation-decisions.md#d1-invalid-or-absent-bitrate-versus-valid-out-of-range-bitrate).

## HDR and timing semantics

| Console output | Client HDR request | Required stream |
|---|---|---|
| SDR | Off | SDR |
| SDR | On | SDR |
| HDR | Off | Converted SDR, with correct brightness and color |
| HDR | On | Ten-bit HDR with console-source color/brightness information |

Signal state changes and switch within two seconds without ending the session. Verify color
primaries, transfer function, bit depth, mastering-display metadata, and content-light metadata
against an independent console-source record for the same output epoch. Capture-helper defaults
are not that record: the Elgato USB path does not supply the source's HDMI metadata. Missing
source evidence is Incomplete for the affected checks.

The Moonlight-visible processing field is determined before packet-header insertion, FEC, and
transmission in the pinned Sunshine reference. Its 0.1 ms wire units and unavailable/repeated-frame
behavior must follow the source and supported clients. It cannot contain the later completion time of the last
packet of that same frame. Keep these measures distinct:

- Protocol processing latency: supports FR-021 and SC-003, including with detailed metrics off.
- Full pipeline latency: capture start to last packet submitted to the network stack; tests the
  constitution's exact negotiated frame-interval ceiling.
- Input latency: host receipt to confirmed platform delivery, not transport acknowledgement.

See [measurement-evidence.md](measurement-evidence.md) for timestamps, populations, validity,
and verdict rules. A decoded stream alone does not prove host timing, input delivery, or metadata fidelity.

## Conformance and evidence

Per-change fake-platform CI executes pairing, launch, stream, controller input, revoke, failure
cleanup, and teardown through the independent test client. Unit and fuzz coverage includes every
network parser, admission boundaries, PIN proof failures, negotiation, and revocation races.

Each release freezes exact official-client versions for Apple TV, iPhone/iPad, Android, Windows,
macOS, and Linux; every entry pairs and streams, and each platform's HDR entry uses an HDR display.
Where an official client and the test client disagree, correct the test client. Reference-host
validation must precede using it to accept SunshinePS5. Reports preserve Pass/Fail/Incomplete
and never replace missing required evidence with a successful protocol exchange.
