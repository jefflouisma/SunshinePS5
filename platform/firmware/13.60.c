#include "firmware/13.60.h"

#include "runtime.h"

#if defined(__PROSPERO__)
#include <ps5/kernel.h>
#endif

/* One source/rollback row per change the adapters can make. No address or
 * symbol offset is invented: the stock SDK CRT writes are external, the
 * ResourceArbitrator selector is a typed source-level operation, and this
 * adapter installs zero privileged hooks on the supported build.
 * SDK pin: ps5-payload-dev/sdk a7685ce8e97ce1f8a4f38253a41d54241c9124f1. */
static const sp5_fw_operation operations[] = {
    {.operation = "stock-sdk-crt-bootstrap",
     .source_ref = "ps5-payload-dev/sdk@a7685ce8 crt/crt.c -> crt/patch.c",
     .rollback_ref = "SDK-owned process credential/attribute/syscall-bound writes; "
                     "process exit; reversal unverified",
     .rollback = SP5_FW_ROLLBACK_NOT_REVERSED,
     .adapter_owned = false,
     .privileged_hook = false},
    {.operation = "resource-arbitrator-selector-0xb4",
     .source_ref = "tools/ps5-probes/capture_encode.c resource_start/resource_cleanup; "
                   "firmware-13.60 ResourceArbitrator caller 0x41c231a0",
     .rollback_ref = "request STOP, unregister, then bounded selector-removal queries",
     .rollback = SP5_FW_ROLLBACK_OWNED,
     .adapter_owned = true,
     .privileged_hook = false},
    {.operation = "avcap2-video-session",
     .source_ref = "tools/ps5-probes/capture_encode.c video_start/video_cleanup; "
                   "Remote Play caller 0x41c284c0, Avcap2 NIDs in that probe",
     .rollback_ref = "Avcap2 Stop, Close, Terminate in that order",
     .rollback = SP5_FW_ROLLBACK_OWNED,
     .adapter_owned = true,
     .privileged_hook = false},
};

static const sp5_fw_profile profile = {
    .exact_version = SP5_FIRMWARE_13_60_EXACT,
    .label = "13.60 (13.600.007)",
    .operations = operations,
    .operation_count = sizeof(operations) / sizeof(operations[0]),
    .privileged_hook_count = 0,
    .source_ref = "specs/001-moonlight-streaming-host/research-r3-findings.md"
                  "#t041-required-evidence-ledger",
};

const sp5_fw_profile *sp5_fw_13_60_profile(void)
{
    return &profile;
}

bool sp5_fw_13_60_supported(uint32_t raw)
{
    return raw == (uint32_t)SP5_FIRMWARE_13_60_EXACT;
}

sp5_result sp5_fw_13_60_check(uint32_t raw)
{
    /* Any other value, including another 13.60 patch build, is refused. No
     * offset, symbol or hook from 13.600.007 is assumed for a different build. */
    return sp5_fw_13_60_supported(raw) ? sp5_success() : sp5_failure(SP5_REASON_UNSUPPORTED);
}

uint32_t sp5_fw_13_60_read(void)
{
#if defined(__PROSPERO__)
    return kernel_get_fw_version();
#elif defined(SP5_FW_13_60_HOST_VERSION)
    return (uint32_t)(SP5_FW_13_60_HOST_VERSION);
#else
    return 0;
#endif
}

bool sp5_ps5_firmware_supported(void)
{
    return sp5_fw_13_60_supported(sp5_fw_13_60_read());
}

void sp5_fw_13_60_provenance(bool observed_exact, sp5_platform_provenance *provenance)
{
    if (provenance == NULL)
        return;
    *provenance = (sp5_platform_provenance){
        .synthetic = false,
        .implementation_ref = "platform/firmware/13.60.c (provisional; identity table only)",
        .firmware = profile.label,
        .capability_evidence_ref = profile.source_ref,
        .validity = observed_exact ? SP5_VALIDITY_VALID : SP5_VALIDITY_INVALID,
        .reason = observed_exact ? SP5_REASON_NONE : SP5_REASON_UNVERIFIED};
}
