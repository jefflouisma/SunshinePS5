#ifndef SUNSHINE_PLATFORM_SYSTEM_PS5_H
#define SUNSHINE_PLATFORM_SYSTEM_PS5_H

#include "platform.h"

#include <stdbool.h>
#include <stddef.h>

/* PROVISIONAL production system adapter for firmware 13.60, not device-qualified.
 *
 * Evidence: specs/001-moonlight-streaming-host/research-r3-findings.md (T040
 * source audit, T042 evidence ledger and the 2026-11-01 state probe) and
 * tools/ps5-probes/r3_state3.c. The console probe established the pointer-form
 * `sceShellCoreUtilGetRemotePlayStatus(int32_t *)` and
 * `sceShellCoreUtilGetSharePlayStatus(int32_t *)` calls, the no-argument
 * `sceRemoteplayIsRemotePlaying()` call and
 * `sceSystemStateMgrGetCurrentState()`. On an idle running console these
 * returned 0, 1000 and idle status -256 respectively. Any other status value is
 * conservatively treated as active. The no-argument SharePlay form crashed and
 * is not used.
 *
 * Probe checks the exact observed build 0x13600007 through
 * sp5_ps5_firmware_supported() and installs nothing. Provenance is VALID only
 * when the state query succeeds; otherwise INVALID/STAGE_UNAVAILABLE.
 * Unsupported firmware returns UNSUPPORTED with INVALID/UNSUPPORTED provenance.
 * Prepare is a bounded no-op after its own exact-firmware check: it records
 * ownership of this context only, with zero console changes, hooks or privileged
 * symbols. Rollback is an idempotent bounded no-op over those owned changes; a
 * context that never probed or prepared refuses with OWNERSHIP. Notify has no
 * sourced console notification API and returns STAGE_UNAVAILABLE for valid
 * arguments instead of guessing one.
 *
 * No allocation, per-call logging, timing read, retry, guessed offset or
 * privileged call occurs. The caller serializes access to one context.
 *
 * The stock SDK CRT startup writes are external to this adapter; they are not
 * installed, owned or claimed as reversed here. See platform/firmware/13.60.c. */

#define SP5_PS5_SYSTEM_IMPLEMENTATION_REF                                                          \
    "platform/system/ps5.c (provisional; exact firmware identity and no-op owned state only)"
#define SP5_PS5_SYSTEM_FIRMWARE "13.60"
#define SP5_PS5_SYSTEM_CAPABILITY_EVIDENCE_REF                                                     \
    "specs/001-moonlight-streaming-host/research-r3-findings.md#t040-source-audit, "               \
    "#t042-required-evidence-ledger and .omo/evidence/t078-state-probe-20261101/REPORT.md"

typedef struct {
    bool probed;
    bool prepared;
    bool firmware_exact;
    /* Number of console changes this context owns. Always zero: no hook, symbol
     * or offset is installed. Kept explicit so rollback ownership stays clear. */
    size_t owned_changes;
    sp5_reason failure;
} sp5_ps5_system;

void sp5_ps5_system_init(sp5_ps5_system *system);
sp5_system_ops sp5_ps5_system_ops(sp5_ps5_system *system);

/* Direct entry points, matching the sp5_system_ops signatures. */
sp5_result sp5_ps5_system_probe(sp5_ps5_system *system, sp5_platform_provenance *provenance);
sp5_result sp5_ps5_system_prepare(sp5_ps5_system *system);
sp5_result sp5_ps5_system_state(sp5_ps5_system *system, sp5_system_state *state);
sp5_result sp5_ps5_system_notify(sp5_ps5_system *system, const char *message, size_t size);
sp5_result sp5_ps5_system_rollback(sp5_ps5_system *system);

bool sp5_ps5_system_prepared(const sp5_ps5_system *system);

#endif
