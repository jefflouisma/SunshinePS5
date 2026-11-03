# Contract: Web app and API

**Feature**: `001-moonlight-streaming-host` | **Status**: Provisional; see [plan.md](../plan.md).

The console serves a static app and JSON API under `/api/v1`. All assets are local. No runtime
frontend framework, login, account, external font, or internet connection is required.

## Access and request rules

Use the same FR-040 peer-address classifier as the streaming services: routed RFC1918 IPv4 and
IPv6 ULA are permitted; IPv6 link-local is limited to its link; all other sources are refused.
Normalize mapped IPv4 and ignore forwarded-address claims.

The accepted no-login decision allows every permitted-network browser to approve pairing,
revoke devices, and stop the host. Same-origin controls address requests originating from
unrelated websites; they do not authenticate an owner or narrow that accepted access policy.
Serve no permissive cross-origin policy. Validate Host and Origin against the console's served
address/name and port; use a page-scoped anti-CSRF token on mutations, including requests whose
browser omits Origin. Changing requests require JSON and that token. Embedded assets and tokens
remain usable offline. Tokens are not pairing credentials and never enter diagnostics.

Bound request/header/body sizes and concurrent work; reject malformed fields and unsupported
content types. Escape all client-supplied text when rendered. Device/host names are nonempty,
at most 63 characters, and reject control characters; storage and wire limits must agree.
Use atomic durable writes for identity, paired devices, and settings, with surfaced failures.

## Operations

All paths below are relative to `/api/v1`. Field definitions and relationships are in the
[data model](../data-model.md); timings and evidence validity are in
[measurement-evidence.md](measurement-evidence.md).

| Method and path | Request / result | Required behavior |
|---|---|---|
| GET `/status` | Host state, versions, address, active session or null | Active session includes device name, resolution, fps, dynamic range, requested/chosen bitrate, adjustment and duration (FR-042) |
| GET `/pairing` | Pending request or null, expiry, cooldown remaining | One pending request; preserve client name as untrusted display text |
| POST `/pairing/pin` | Request identifier, PIN, optional device name | Complete standard proof checks, notify console/client; correct pairing visible within three seconds |
| GET `/devices` | Paired devices, pending durable removals and pending cleanup | Name, pairing date, last session time; never expose key material |
| PATCH `/devices/{id}` | New name | Persist a validated rename |
| DELETE `/devices/{id}` | Explicit confirmation flag | Revoke, persist, invalidate authorization and terminate its session within two seconds |
| PATCH `/settings` | Host name and/or metrics-enabled value | Validate atomically; host name visible on next client refresh; metrics effective within two seconds and persistent |
| GET `/metrics` | Current/session snapshot and coverage | Detailed channels when enabled; preserved aggregates and gaps explicitly labeled when disabled |
| GET `/metrics/export` | Downloadable capture | Retained samples, whole-session summaries, coverage and version/mode/client context |
| GET `/diagnostics` | Downloadable diagnostic bundle | Bounded recent logs, session-end reasons, capture if any, versions |
| POST `/host/stop` | Explicit confirmation flag | Acknowledge stopping, release inputs/resources, remove hooks, exit, leave game running |

Success responses identify the operation and resulting state; asynchronous settings expose
requested and effective values until the two-second deadline. A stop acknowledgement confirms
the request was accepted, not that post-exit cleanup has already been independently verified.
The browser shows stopping and then unavailable; it must not claim a live stopped server response.

Pairing expiry and failed-PIN cooldown use monotonic deadlines. Wrong PIN responses explain how
to retry. After five consecutive failures, report the remaining 60-second cooldown and refuse
new pairing attempts; do not reset the counter by refreshing the page. Pending-request IDs
prevent a stale form from approving a different request.

Revocation requires confirmation and invalidates current authorization before success is
acknowledged. If durable storage fails, terminate access for the current process and return an
explicit persistence failure; do not claim the revocation will survive restart. Repeated
operations are safe and return the resulting state or a clear conflict.

GET `/devices` keeps `pending_revocations` (`id`, `name`) and
`revocation_durability_pending` for unsaved removals. Its separate
`cleanup_pending_revocations` array contains at most one (`id`, `name`) from the retained
ending session whose paired identity has been removed. Cleanup may remain pending even when
storage succeeded; its presence does not imply a durability failure. Names retain their
untrusted display-text treatment. The browser shows a confirmed cleanup retry across page
refreshes using DELETE `/devices/{id}`. If both failures apply, both remain visible. A successful
retry completes cleanup and persistence; the cleanup row disappears, and another DELETE for
the completed removal returns 404.

## Metrics and downloads

A snapshot includes the last ten seconds and whole-session distributions for every stage,
total host time, and input delivery, plus the streaming counters in FR-047. Show the stage with
the largest frame-budget share and flag a full-pipeline p99 above the frame interval. A status
of unavailable/invalid includes its reason; neither zero nor an empty chart means success.

Requested/chosen bitrate are targets; measured encoded bitrate and network throughput have their
own fields, units, and observation intervals. Any clamp remains visible during the session and
in exports. Do not hide it behind measured bitrate.

Only valid positive representable requests are clamped; invalid or absent required targets
fail negotiation without inventing defaults, per [decision D1](implementation-decisions.md#d1-invalid-or-absent-bitrate-versus-valid-out-of-range-bitrate).
Measurement faults expose affected values as unavailable with reasons and coverage, preserving
valid channels and known failures, per [D3](implementation-decisions.md#d3-measurement-errors-and-acceptance).

Whole-session aggregates survive raw sample eviction. Raw records are retained for at most
600 seconds, possibly less at the fixed capacity limit; exports state the actual retained
interval, observed population, missing samples, enabled intervals, and metrics-off gaps.
Turning metrics off stops detailed collection, preserves existing aggregate state, and leaves
the FR-021 protocol measure operational. Re-enabling never makes the off interval look measured.

Download generation uses bounded snapshot/buffer work outside media/input paths. Diagnostics
exclude private keys, credentials, PINs, tokens, and raw pairing secrets. Keep required client
names and version/mode context; log only what supports troubleshooting, with bounded retention.

## Live updates and phone flows

The visible page polls once per second without overlapping requests. State changes render on
arrival; metrics must meet FR-048's update cadence under the supported load. Hidden/closed
pages stop polling. The server has no per-browser update worker when no browser is connected.
Metrics-enabled collection continues independently; no-browser behavior does not disable FR-047.

Controls on the home screen support these completed paths at 360 CSS pixels:

| Operation | Counted app actions | Maximum |
|---|---|---:|
| Enter PIN | Open/focus pairing control, submit PIN (optional device-name typing excluded) | 2 |
| Revoke device | Open paired devices, choose revoke, confirm | 3 |
| Toggle metrics | Change the home-screen metrics switch | 1 |

Typing and keyboard-dismissal taps are excluded; submission still counts when it dismisses the
keyboard. Confirmation remains mandatory for revoke and stop. Verify actual navigation rather
than inferring tap counts from endpoint count. No horizontal scrolling or zooming is needed.
Use labeled controls, visible focus/error states, and layouts that remain usable in current
mobile Safari/Chrome and desktop Chrome/Edge/Firefox/Safari (FR-014).

## Error contract

Errors include a stable code, plain-language explanation, recovery action, and request identifier.
Use 400 for invalid values, 403 for peer/origin/token rejection where an HTTP response is possible,
404 for absent resources, 409 for conflicting session/pairing state, 429 with remaining time for
cooldown, 503 for an unavailable console dependency, and 500 for unexpected or persistence errors.
Refuse out-of-boundary peers before exposing management data; a dropped connection need not emit
HTTP. Never include secrets, raw memory, or internal stack traces in user-visible errors.

Validate phone flows, durability failures, multiple tabs, stale pairing submissions, routed-private
access, denied public/global sources, and metrics toggles during streaming. The no-login policy
does not waive pairing checks on the distinct Moonlight streaming/control endpoints.
