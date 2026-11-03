#include "ps5.h"

#include "firmware/13.60.h"
#include "runtime.h"

/* Remote Play, Share Play and system-state queries are now sourced from the
 * 2026-11-01 console probe (tools/ps5-probes/r3_state3.c):
 *   sceRemoteplayIsRemotePlaying() -> 0 on an idle console;
 *   sceSystemStateMgrGetCurrentState() -> 1000 (running) on the same console;
 *   sceShellCoreUtilGetRemotePlayStatus(int32_t *) -> rc 0, idle value -256;
 *   sceShellCoreUtilGetSharePlayStatus(int32_t *) -> rc 0, idle value -256.
 * The pointer forms are required; the no-argument form crashed. `-256`
 * (0xFFFFFF00) is the observed idle value, so any other value is conservatively
 * treated as active. A state fetch is VALID only when every call succeeds;
 * otherwise it is INVALID/STAGE_UNAVAILABLE. Notification still has no sourced
 * API and returns STAGE_UNAVAILABLE. prepare/rollback own no console change. */

#if defined(__PROSPERO__)
int sceRemoteplayIsRemotePlaying(void);
int sceSystemStateMgrGetCurrentState(void);
int sceShellCoreUtilGetRemotePlayStatus(int32_t *status);
int sceShellCoreUtilGetSharePlayStatus(int32_t *status);
#endif

#define SP5_PS5_IDLE_STATUS (-256)

static void fill_provenance(sp5_reason reason, sp5_validity validity,
                            sp5_platform_provenance *provenance)
{
    *provenance =
        (sp5_platform_provenance){.synthetic = false,
                                  .implementation_ref = SP5_PS5_SYSTEM_IMPLEMENTATION_REF,
                                  .firmware = SP5_PS5_SYSTEM_FIRMWARE,
                                  .capability_evidence_ref = SP5_PS5_SYSTEM_CAPABILITY_EVIDENCE_REF,
                                  .validity = validity,
                                  .reason = reason};
}

void sp5_ps5_system_init(sp5_ps5_system *system)
{
    if (system == NULL)
        return;
    *system = (sp5_ps5_system){0};
    system->failure = SP5_REASON_NONE;
}

sp5_result sp5_ps5_system_probe(sp5_ps5_system *system, sp5_platform_provenance *provenance)
{
    if (system == NULL || provenance == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    *provenance = (sp5_platform_provenance){0};
    const bool exact = sp5_ps5_firmware_supported();
    system->probed = true;
    system->firmware_exact = exact;
    if (!exact) {
        /* Every other build, including another 13.60 patch build, is refused
         * before any adapter state can be installed. */
        system->failure = SP5_REASON_UNSUPPORTED;
        fill_provenance(SP5_REASON_UNSUPPORTED, SP5_VALIDITY_INVALID, provenance);
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    }
    sp5_system_state state;
    if (sp5_ps5_system_state(system, &state).status == SP5_OK &&
        state.validity == SP5_VALIDITY_VALID) {
        system->failure = SP5_REASON_NONE;
        fill_provenance(SP5_REASON_NONE, SP5_VALIDITY_VALID, provenance);
        return sp5_success();
    }
    system->failure = SP5_REASON_STAGE_UNAVAILABLE;
    fill_provenance(SP5_REASON_STAGE_UNAVAILABLE, SP5_VALIDITY_INVALID, provenance);
    return sp5_success();
}

sp5_result sp5_ps5_system_prepare(sp5_ps5_system *system)
{
    if (system == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (system->prepared)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    /* Re-check at the preparation boundary rather than trusting a cached
     * observation. This helper reads the pinned SDK firmware context once. */
    system->probed = true;
    system->firmware_exact = sp5_ps5_firmware_supported();
    if (!system->firmware_exact) {
        system->owned_changes = 0;
        system->failure = SP5_REASON_UNSUPPORTED;
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    }
    /* No hook, privileged symbol, offset or console write is owned. The
     * stock SDK CRT baseline is external and is not claimed or reversed here. */
    system->owned_changes = 0;
    system->prepared = true;
    system->failure = SP5_REASON_NONE;
    return sp5_success();
}

sp5_result sp5_ps5_system_state(sp5_ps5_system *system, sp5_system_state *state)
{
    if (system == NULL || state == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    *state = (sp5_system_state){0};
#if defined(__PROSPERO__)
    int playing = sceRemoteplayIsRemotePlaying();
    int32_t remote_status = SP5_PS5_IDLE_STATUS;
    int32_t share_status = SP5_PS5_IDLE_STATUS;
    int remote_rc = sceShellCoreUtilGetRemotePlayStatus(&remote_status);
    int share_rc = sceShellCoreUtilGetSharePlayStatus(&share_status);
    int current = sceSystemStateMgrGetCurrentState();
    if (playing < 0 || remote_rc != 0 || share_rc != 0 || current < 0) {
        *state = (sp5_system_state){.validity = SP5_VALIDITY_INVALID,
                                    .reason = SP5_REASON_STAGE_UNAVAILABLE};
        return sp5_success();
    }
    *state = (sp5_system_state){.remote_play = playing != 0 || remote_status != SP5_PS5_IDLE_STATUS,
                                .share_play = share_status != SP5_PS5_IDLE_STATUS,
                                .stopping_or_rest = current != 1000,
                                .validity = SP5_VALIDITY_VALID,
                                .reason = SP5_REASON_NONE};
    return sp5_success();
#else
    *state = (sp5_system_state){.validity = SP5_VALIDITY_INVALID,
                                .reason = SP5_REASON_STAGE_UNAVAILABLE};
    return sp5_success();
#endif
}

sp5_result sp5_ps5_system_notify(sp5_ps5_system *system, const char *message, size_t size)
{
    if (system == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (message == NULL && size != 0)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    /* No sourced console notification API exists. The message is borrowed only
     * for validation; nothing is copied, stored, allocated or displayed. */
    return sp5_failure(SP5_REASON_STAGE_UNAVAILABLE);
}

sp5_result sp5_ps5_system_rollback(sp5_ps5_system *system)
{
    if (system == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    /* A context that never probed or prepared owns nothing; refuse rather than
     * silently claiming a rollback for state this adapter never acquired. */
    if (!system->probed)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    if (!system->prepared)
        return sp5_success(); /* Idempotent after success; nothing owned now. */
    system->owned_changes = 0;
    system->prepared = false;
    system->failure = SP5_REASON_NONE;
    return sp5_success();
}

bool sp5_ps5_system_prepared(const sp5_ps5_system *system)
{
    return system != NULL && system->prepared;
}

/* The portable vtable takes void *context; these thin wrappers preserve the
 * typed direct entry points without performing any additional work. */
static sp5_result system_op_probe(void *context, sp5_platform_provenance *provenance)
{
    return sp5_ps5_system_probe(context, provenance);
}

static sp5_result system_op_prepare(void *context)
{
    return sp5_ps5_system_prepare(context);
}

static sp5_result system_op_state(void *context, sp5_system_state *state)
{
    return sp5_ps5_system_state(context, state);
}

static sp5_result system_op_notify(void *context, const char *message, size_t size)
{
    return sp5_ps5_system_notify(context, message, size);
}

static sp5_result system_op_rollback(void *context)
{
    return sp5_ps5_system_rollback(context);
}

sp5_system_ops sp5_ps5_system_ops(sp5_ps5_system *system)
{
    return (sp5_system_ops){.context = system,
                            .probe = system_op_probe,
                            .prepare = system_op_prepare,
                            .state = system_op_state,
                            .notify = system_op_notify,
                            .rollback = system_op_rollback};
}
