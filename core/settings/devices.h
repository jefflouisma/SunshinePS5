#ifndef SUNSHINE_SETTINGS_DEVICES_H
#define SUNSHINE_SETTINGS_DEVICES_H

#include "host.h"

#include <stdatomic.h>

enum { SP5_DEVICE_CAPACITY = 16, SP5_CERTIFICATE_CAPACITY = 2048 };
typedef struct {
    char id[65];
    uint8_t certificate[SP5_CERTIFICATE_CAPACITY];
    size_t certificate_size;
    char name[SP5_NAME_CAPACITY];
    uint64_t paired_at;
    sp5_optional_u64 last_session_at;
} sp5_device;
typedef struct sp5_devices sp5_devices;
typedef struct {
    sp5_bytes certificate;
    sp5_bytes name;
} sp5_device_name;

/* Setup-only allocation; thereafter fixed storage, serialized management owner.
 * Certificate spans are the canonical bytes from VERIFIED pairing/TLS, never
 * client IDs/names. This layer does not replace certificate/proof verification.
 * ID is lowercase SHA-256 of those certificate bytes; names never authorize.
 * Revocation removes access in memory even when durability fails. The caller
 * invalidates admission/input and triggers input-first session teardown before
 * acknowledgement (T053/T059/T067); disk I/O cannot run on the input thread. */
sp5_devices *sp5_devices_create(const sp5_storage_ops *storage);
void sp5_devices_destroy(sp5_devices *devices);
sp5_result sp5_devices_load(sp5_devices *devices);
size_t sp5_devices_count(const sp5_devices *devices);
bool sp5_devices_repairing_required(const sp5_devices *devices);
const char *sp5_devices_explanation(const sp5_devices *devices);
const sp5_device *sp5_devices_at(const sp5_devices *devices, size_t index);
bool sp5_devices_authorized(const sp5_devices *devices, sp5_bytes certificate);
sp5_result sp5_devices_pair(sp5_devices *devices, sp5_bytes certificate, sp5_bytes name,
                            uint64_t paired_at);
sp5_result sp5_devices_rename(sp5_devices *devices, sp5_device_name update);
sp5_result sp5_devices_session_ended(sp5_devices *devices, sp5_bytes certificate,
                                     uint64_t ended_at);
sp5_result sp5_devices_revoke(sp5_devices *devices, sp5_bytes certificate);

/* A revocation whose durable write failed stays effective for this process, but
 * until a later registry write succeeds a restart would restore the access of
 * every device listed here. Entries carry only the public id and display name,
 * never key material; any successful persist (retry, pair, rename, session end)
 * clears them. Bounded: only devices from the last durable set can be pending. */
typedef struct {
    char id[65];
    char name[SP5_NAME_CAPACITY];
} sp5_device_revocation;
bool sp5_devices_durability_pending(const sp5_devices *devices);
size_t sp5_devices_pending_count(const sp5_devices *devices);
const sp5_device_revocation *sp5_devices_pending_at(const sp5_devices *devices, size_t index);
/* Retries the durable write when pending; success when nothing is pending. */
sp5_result sp5_devices_persist_pending(sp5_devices *devices);

/* Management owner acquires a lease only after verified certificate lookup.
 * Input/control readers use ONLY this immutable lease, never registry records.
 * Release stores invalidate before persistence; acquire loads reject revoked,
 * re-paired and reloaded generations. The registry outlives all leases/readers. */
typedef struct {
    const atomic_uint_fast64_t *generation;
    uint_fast64_t expected;
} sp5_device_lease;
sp5_device_lease sp5_devices_lease(const sp5_devices *devices, sp5_bytes certificate);
bool sp5_device_lease_valid(sp5_device_lease lease);

/* Store-owned registry lifetime; permits session/control authority to consult
 * the same current paired set instead of copying stale authorization state. */
sp5_devices *sp5_store_devices(sp5_store *store);

#endif
