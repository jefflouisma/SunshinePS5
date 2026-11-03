# Contract: Diagnostic Unsourced Protection CLEAR

**Feature**: `001-moonlight-streaming-host` | **Version**: 1.0.0 | **Date**: 2026-11-01
**Status**: Diagnostic-only contract. It does not close B04, FR-022, T041, T045, T079 or any
acceptance criterion.

## Purpose

No read-only capture-protection getter was recovered from firmware 13.60 Avcap2/Venc.
The diagnostic console host may nevertheless exercise the real capture/encode/audio/input
path by explicitly assuming `SP5_PROTECTION_CLEAR`. This contract defines that assumption,
its labeling and its limits so a diagnostic run cannot be mistaken for sourced protection.

## Activation

The override is opt-in and never implicit:

- Source builds default to `SP5_DIAGNOSTIC_PROTECTION_CLEAR=0`; `platform/main_stream.c`
  refuses a session with `protection_required`/`UNVERIFIED` unless overridden.
- The reviewed diagnostic artifact is **defined** as built with
  `-DSP5_DIAGNOSTIC_PROTECTION_CLEAR=1`; a source build or an operator may pass
  `--assume-protection-clear`. The source default remains `0`.
- The ready/streaming JSON, the lifecycle policy evidence reference and every retained
  report must carry the exact label
  `diagnostic:--assume-protection-clear`.
- The production pair/web/status host and any future production host must not enable it.

## Runtime semantics

1. Capture opens normally; only after a successful lifecycle start does the host call
   `sp5_ps5_capture_observe_protection(&capture, SP5_PROTECTION_CLEAR,
   "diagnostic:--assume-protection-clear")`.
2. The observation increments the capture source epoch once and records the evidence label.
   If the setter fails, the session ends with `capture_protection` and no frames are sent.
3. The same source state is copied to the audio adapter through
   `sp5_ps5_audio_observe_source`; audio emits samples only under that shared premise.
4. No protection transition monitoring, protected-content detection, queue-flush behavior or
   placeholder/silence proof is claimed. A source that becomes protected mid-run is not detected.

## Preconditions

Before enabling the override the operator must confirm:

- firmware `13.600.007` and the exact reviewed diagnostic artifact hash;
- SDR mode only (HDR remains refused by the adapters);
- a fresh HDMI frame/vision check showing the intended foreground content and no dialog;
- no DRM/protected media is playing, and the operator accepts that this is not provable
  from the host;
- no active session or prior payload owns the capture/input resources.

## Evidence labeling and verdicts

- Every diagnostic report keeps overall `Incomplete`; no SC-003/SC-004/SC-009/SC-014 or
  FR-022 claim may be derived from a CLEAR-assumed run.
- Frame/protection evidence records the label; missing or different labels invalidate the
  diagnostic premise.
- The run may support investigative observations (fps, measured bitrate, Opus packet loss,
  host input receipt, teardown), explicitly separated from acceptance.
- T041/T045 and readiness remain unchanged: sourced protection is still required.

## Failure and abort rules

Abort and retain partial evidence when:

- the build/CLI did not enable the override;
- the capture or audio source observation fails;
- the preflight shows protected content, an unexpected dialog, or a game/UI state outside the
  declared diagnostic;
- capture returns `PROTECTION_UNKNOWN`/`PROTECTED`, or the session cannot start;
- teardown/PID/service/HDMI checks fail.

No retry may silently remove the label or switch to a sourced claim.

## Review requirement

An independent review must verify the exact build flag/CLI behavior, labels, refusal path,
audio propagation and evidence wording before a diagnostic run. This contract is not an
acceptance-profile change and does not modify `spec.md`, `tasks.md`, reviewer checklist
markers or the measurement contract.

## Limits

- Avcap2/Venc expose no readable protection state in the retained evidence.
- The `CLEAR` observation is an operator premise, not a platform observation.
- Protected-content behavior, transition flushing, HDR source metadata and production
  protection remain open (B03/B04).
