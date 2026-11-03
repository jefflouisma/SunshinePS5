#ifndef SUNSHINE_PLATFORM_FIRMWARE_13_60_H
#define SUNSHINE_PLATFORM_FIRMWARE_13_60_H

#include "platform.h"

/* Exact firmware identity observed by the retained T041/T042 probes:
 *   live shsrv banner 13.600.007, kernel_get_fw_version() == 0x13600007.
 * The probe's looser mask 0xffff0000 == 0x13600000 is NOT sufficient here:
 * production adapters require the exact observed build value. */
enum { SP5_FIRMWARE_13_60_EXACT = 0x13600007 };

typedef enum {
    /* No adapter-owned write exists, so rollback is a successful no-op. */
    SP5_FW_ROLLBACK_NONE = 0,
    /* Change belongs to the stock SDK CRT and is not reversed by this adapter. */
    SP5_FW_ROLLBACK_NOT_REVERSED,
    /* Adapter owns the change and records the exact cleanup operation below. */
    SP5_FW_ROLLBACK_OWNED
} sp5_fw_rollback_kind;

typedef struct {
    const char *operation;
    const char *source_ref;
    const char *rollback_ref;
    sp5_fw_rollback_kind rollback;
    bool adapter_owned;
    bool privileged_hook;
} sp5_fw_operation;

typedef struct {
    uint32_t exact_version;
    const char *label;
    const sp5_fw_operation *operations;
    size_t operation_count;
    size_t privileged_hook_count;
    const char *source_ref;
} sp5_fw_profile;

const sp5_fw_profile *sp5_fw_13_60_profile(void);
bool sp5_fw_13_60_supported(uint32_t raw);
sp5_result sp5_fw_13_60_check(uint32_t raw);

/* Reads the exact firmware value once through the pinned SDK helper and
 * returns true only for 0x13600007. Convenience for adapters; no cached
 * result and no looser 0x13600000 mask. */
bool sp5_ps5_firmware_supported(void);

/* Reads the pinned SDK firmware helper. Host builds return
 * SP5_FW_13_60_HOST_VERSION (default 0) so the same file compiles off-console. */
uint32_t sp5_fw_13_60_read(void);

/* Provisional identity provenance, not media-capability acceptance. When
 * observed_exact is true the firmware value itself was read; otherwise the
 * record is INVALID/UNVERIFIED. synthetic is always false. */
void sp5_fw_13_60_provenance(bool observed_exact, sp5_platform_provenance *provenance);

#endif
