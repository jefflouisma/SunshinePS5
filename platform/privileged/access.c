#include "privileged/access.h"
#include "firmware/13.60.h"

#include "runtime.h"

#include <string.h>

// NOLINTBEGIN(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
void sp5_priv_access_init(sp5_priv_access *access)
{
    if (access == NULL)
        return;
    memset(access, 0, sizeof(*access));
    access->failure = SP5_REASON_NONE;
}

sp5_result sp5_priv_probe_raw(sp5_priv_access *access, uint32_t raw,
                              sp5_platform_provenance *provenance)
{
    if (access == NULL || provenance == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    *provenance = (sp5_platform_provenance){0};
    access->firmware_raw = raw;
    access->firmware_exact = sp5_fw_13_60_supported(raw);
    access->probed = true;
    access->hooks_installed = 0;
    sp5_result result = sp5_fw_13_60_check(raw);
    if (result.status != SP5_OK) {
        access->failure = result.reason;
        sp5_fw_13_60_provenance(false, provenance);
        provenance->reason = result.reason;
        return result;
    }
    /* The validity here covers the exact firmware identity observation only.
     * Media/system capability remains provisional; capability_evidence_ref
     * points at the retained R3/T041 record, and no hook is installed. */
    sp5_fw_13_60_provenance(true, provenance);
    access->failure = SP5_REASON_NONE;
    return sp5_success();
}

sp5_result sp5_priv_probe(sp5_priv_access *access, sp5_platform_provenance *provenance)
{
    return sp5_priv_probe_raw(access, sp5_fw_13_60_read(), provenance);
}

sp5_result sp5_priv_prepare(sp5_priv_access *access)
{
    if (access == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (!access->probed)
        return sp5_failure(SP5_REASON_UNVERIFIED);
    if (!access->firmware_exact)
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    if (access->prepared)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    /* No R3-established privileged hook is installed by these adapters. The
     * stock SDK CRT baseline is external and already applied before main. */
    access->hooks_installed = 0;
    access->prepared = true;
    access->failure = SP5_REASON_NONE;
    return sp5_success();
}

sp5_result sp5_priv_rollback(sp5_priv_access *access)
{
    if (access == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (!access->prepared)
        return sp5_success(); /* Idempotent; no owned hook was installed. */
    /* Nothing adapter-owned to reverse. Never touch the SDK CRT baseline or the
     * foreground game. A future hook must record its own reversal here. */
    access->hooks_installed = 0;
    access->prepared = false;
    access->failure = SP5_REASON_NONE;
    return sp5_success();
}

bool sp5_priv_prepared(const sp5_priv_access *access)
{
    return access != NULL && access->prepared;
}

sp5_result sp5_priv_system_probe(void *context, sp5_platform_provenance *provenance)
{
    return sp5_priv_probe((sp5_priv_access *)context, provenance);
}

sp5_result sp5_priv_system_prepare(void *context)
{
    return sp5_priv_prepare((sp5_priv_access *)context);
}

sp5_result sp5_priv_system_rollback(void *context)
{
    return sp5_priv_rollback((sp5_priv_access *)context);
}

// NOLINTEND(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
