#ifndef SUNSHINE_PLATFORM_PRIVILEGED_ACCESS_H
#define SUNSHINE_PLATFORM_PRIVILEGED_ACCESS_H

#include "platform.h"

/* Minimal privileged-operation boundary for the provisional PS5 system adapter.
 *
 * Only R3-established reversible operations belong here. The retained probes
 * install no privileged hooks; the only startup writes are the stock SDK CRT's
 * process credential/attribute/syscall-bound changes. Those are SDK-owned and
 * are NOT claimed to be reversed. This module therefore owns zero hooks:
 * prepare is a checked no-op and rollback is an idempotent no-op.
 * Do not add guessed kernel addresses, symbols or field offsets here. */

typedef struct {
    bool probed;
    bool prepared;
    bool firmware_exact;
    uint32_t firmware_raw;
    size_t hooks_installed;
    sp5_reason failure;
} sp5_priv_access;

void sp5_priv_access_init(sp5_priv_access *access);

/* Probe exact firmware 0x13600007; unsupported builds are refused. */
sp5_result sp5_priv_probe(sp5_priv_access *access, sp5_platform_provenance *provenance);
/* Injectable variant for bounded host/console smoke checks. */
sp5_result sp5_priv_probe_raw(sp5_priv_access *access, uint32_t raw,
                              sp5_platform_provenance *provenance);

/* Failed prepare unwinds its own partial changes. Here there are none. */
sp5_result sp5_priv_prepare(sp5_priv_access *access);
/* Idempotent, applies only owned changes. Here there are none. */
sp5_result sp5_priv_rollback(sp5_priv_access *access);

bool sp5_priv_prepared(const sp5_priv_access *access);

/* Direct sp5_system_ops-shaped entry points for the future system adapter. */
sp5_result sp5_priv_system_probe(void *context, sp5_platform_provenance *provenance);
sp5_result sp5_priv_system_prepare(void *context);
sp5_result sp5_priv_system_rollback(void *context);

#endif
