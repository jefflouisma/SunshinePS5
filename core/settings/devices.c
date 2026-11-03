#include "devices.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

enum { DEVICE_RECORD_CAPACITY = 40000 };
struct sp5_devices {
    sp5_storage_ops storage;
    sp5_device records[SP5_DEVICE_CAPACITY];
    size_t slots[SP5_DEVICE_CAPACITY];
    /* Separate stable allocation: transactional record copies must never copy
     * atomics while input readers are inspecting their generation. */
    atomic_uint_fast64_t *generations;
    size_t count;
    bool repairing_required;
    bool dirty;
    sp5_device_revocation pending[SP5_DEVICE_CAPACITY];
    size_t pending_count;
};

sp5_devices *sp5_devices_create(const sp5_storage_ops *storage)
{
    if (storage == NULL || storage->read == NULL || storage->replace == NULL) {
        return NULL;
    }
    sp5_devices *devices = calloc(1, sizeof(*devices));
    if (devices != NULL) {
        devices->storage = *storage;
        devices->generations = calloc(SP5_DEVICE_CAPACITY, sizeof(*devices->generations));
        if (devices->generations == NULL) {
            free(devices);
            return NULL;
        }
        for (size_t i = 0; i < SP5_DEVICE_CAPACITY; ++i) {
            atomic_init(&devices->generations[i], 0);
        }
        if (!atomic_is_lock_free(&devices->generations[0])) {
            sp5_devices_destroy(devices);
            return NULL;
        }
    }
    return devices;
}

void sp5_devices_destroy(sp5_devices *devices)
{
    if (devices != NULL) {
        free(devices->generations);
        free(devices);
    }
}

static void invalidate(atomic_uint_fast64_t *generation)
{
    uint_fast64_t value = atomic_load_explicit(generation, memory_order_relaxed);
    if ((value & 1) != 0) {
        atomic_store_explicit(generation, value + 1, memory_order_release);
    }
}

static bool publish(atomic_uint_fast64_t *generation)
{
    uint_fast64_t value = atomic_load_explicit(generation, memory_order_relaxed);
    if ((value & 1) != 0 || value >= UINT_FAST64_MAX - 1) {
        return false;
    }
    atomic_store_explicit(generation, value + 1, memory_order_release);
    return true;
}

static void sp5_devices_clear(sp5_devices *devices)
{
    if (devices != NULL) {
        for (size_t i = 0; i < SP5_DEVICE_CAPACITY; ++i) {
            invalidate(&devices->generations[i]);
        }
        memset(devices->records, 0, sizeof(devices->records));
        devices->count = 0;
        devices->repairing_required = false;
        devices->dirty = false;
        memset(devices->pending, 0, sizeof(devices->pending));
        devices->pending_count = 0;
    }
}

/* Every successful persist writes the complete current set, so it also makes
 * any earlier failed revocation durable. */
static void durable(sp5_devices *devices)
{
    devices->dirty = false;
    memset(devices->pending, 0, sizeof(devices->pending));
    devices->pending_count = 0;
}

static bool valid_certificate(sp5_bytes certificate)
{
    return certificate.data != NULL && certificate.size > 0 &&
           certificate.size <= SP5_CERTIFICATE_CAPACITY;
}

static size_t find(const sp5_devices *devices, sp5_bytes certificate)
{
    if (devices != NULL && valid_certificate(certificate)) {
        for (size_t i = 0; i < devices->count; ++i) {
            const sp5_device *device = &devices->records[i];
            if (device->certificate_size == certificate.size &&
                CRYPTO_memcmp(device->certificate, certificate.data, certificate.size) == 0) {
                return i;
            }
        }
    }
    return SP5_DEVICE_CAPACITY;
}

static bool identify(sp5_device *device)
{
    uint8_t digest[32];
    size_t size = 0;
    if (EVP_Q_digest(NULL, "SHA256", NULL, device->certificate, device->certificate_size, digest,
                     &size) != 1 ||
        size != sizeof(digest)) {
        return false;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        device->id[2 * i] = hex[digest[i] >> 4];
        device->id[2 * i + 1] = hex[digest[i] & 15];
    }
    device->id[64] = 0;
    return true;
}

static void put_u64(uint8_t *output, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) {
        output[i] = (uint8_t)(value >> (56 - 8 * i));
    }
}

static uint64_t get_u64(const uint8_t *input)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value = (value << 8) | input[i];
    }
    return value;
}

static sp5_result persist(sp5_devices *devices)
{
    uint8_t raw[DEVICE_RECORD_CAPACITY];
    /* Pack supports overlap, leaving room for its version/length prefix. */
    uint8_t *payload = raw + 12;
    size_t used = 1;
    payload[0] = (uint8_t)devices->count;
    for (size_t i = 0; i < devices->count; ++i) {
        const sp5_device *device = &devices->records[i];
        size_t name_size = strlen(device->name);
        size_t required = device->certificate_size + name_size + 20;
        if (used > sizeof(raw) - SP5_RECORD_OVERHEAD ||
            required > sizeof(raw) - SP5_RECORD_OVERHEAD - used) {
            return sp5_failure(SP5_REASON_CAPACITY);
        }
        payload[used++] = (uint8_t)(device->certificate_size >> 8);
        payload[used++] = (uint8_t)device->certificate_size;
        memcpy(payload + used, device->certificate, device->certificate_size);
        used += device->certificate_size;
        payload[used++] = (uint8_t)name_size;
        memcpy(payload + used, device->name, name_size);
        used += name_size;
        put_u64(payload + used, device->paired_at);
        used += 8;
        payload[used++] = device->last_session_at.present ? 1 : 0;
        put_u64(payload + used, device->last_session_at.value);
        used += 8;
    }
    sp5_buffer record = {raw, sizeof(raw), 0};
    sp5_result result = sp5_record_pack(SP5_STORE_DEVICES, (sp5_bytes){payload, used}, &record);
    if (result.status == SP5_OK) {
        result = devices->storage.replace(devices->storage.context, SP5_STORE_DEVICES,
                                          (sp5_bytes){record.data, record.size});
    }
    return result;
}

static bool decode(sp5_devices *devices, sp5_bytes payload)
{
    if (payload.size < 1 || payload.data[0] > SP5_DEVICE_CAPACITY) {
        return false;
    }
    size_t used = 1;
    size_t count = payload.data[0];
    for (size_t i = 0; i < count; ++i) {
        if (payload.size - used < 2) {
            return false;
        }
        sp5_device *device = &devices->records[i];
        device->certificate_size = ((size_t)payload.data[used] << 8) | payload.data[used + 1];
        used += 2;
        if (device->certificate_size == 0 || device->certificate_size > SP5_CERTIFICATE_CAPACITY ||
            device->certificate_size >= payload.size - used) {
            return false;
        }
        memcpy(device->certificate, payload.data + used, device->certificate_size);
        used += device->certificate_size;
        size_t length = payload.data[used++];
        if (length > payload.size - used || payload.size - used - length < 17 ||
            !sp5_name_valid((sp5_bytes){payload.data + used, length})) {
            return false;
        }
        memcpy(device->name, payload.data + used, length);
        device->name[length] = 0;
        used += length;
        device->paired_at = get_u64(payload.data + used);
        used += 8;
        if (payload.data[used] > 1) {
            return false;
        }
        device->last_session_at.present = payload.data[used++] != 0;
        device->last_session_at.value = get_u64(payload.data + used);
        used += 8;
        if ((!device->last_session_at.present && device->last_session_at.value != 0) ||
            (device->last_session_at.present &&
             device->last_session_at.value < device->paired_at) ||
            !identify(device)) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(device->id, devices->records[j].id) == 0) {
                return false;
            }
        }
    }
    if (used != payload.size) {
        return false;
    }
    devices->count = count;
    return true;
}

sp5_result sp5_devices_load(sp5_devices *devices)
{
    if (devices == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (devices->dirty) {
        /* A failed revocation must stay effective for this process. Retry the
         * durable removal instead of reloading the old authorization set. */
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    sp5_devices_clear(devices);
    uint8_t raw[DEVICE_RECORD_CAPACITY];
    sp5_buffer record = {raw, sizeof(raw), 0};
    sp5_result result = devices->storage.read(devices->storage.context, SP5_STORE_DEVICES, &record);
    if (result.status != SP5_OK && result.reason == SP5_REASON_WOULD_BLOCK) {
        return sp5_success();
    }
    sp5_bytes payload;
    if (result.status == SP5_OK && record.size <= record.capacity &&
        sp5_record_unpack(SP5_STORE_DEVICES, (sp5_bytes){raw, record.size}, &payload).status ==
            SP5_OK &&
        decode(devices, payload)) {
        for (size_t i = 0; i < devices->count; ++i) {
            devices->slots[i] = i;
            if (!publish(&devices->generations[i])) {
                sp5_devices_clear(devices);
                devices->repairing_required = true;
                return sp5_failure(SP5_REASON_CAPACITY);
            }
        }
        return sp5_success();
    }
    sp5_devices_clear(devices);
    devices->repairing_required = true;
    return sp5_failure(result.status == SP5_OK ? SP5_REASON_INVALID_ARGUMENT : result.reason);
}

size_t sp5_devices_count(const sp5_devices *devices)
{
    return devices != NULL ? devices->count : 0;
}

bool sp5_devices_repairing_required(const sp5_devices *devices)
{
    return devices != NULL && devices->repairing_required;
}

const char *sp5_devices_explanation(const sp5_devices *devices)
{
    return sp5_devices_repairing_required(devices)
               ? "Paired-device storage could not be read; no devices are authorized. Pair again."
               : "";
}

const sp5_device *sp5_devices_at(const sp5_devices *devices, size_t index)
{
    return devices != NULL && index < devices->count ? &devices->records[index] : NULL;
}

bool sp5_devices_authorized(const sp5_devices *devices, sp5_bytes certificate)
{
    return find(devices, certificate) != SP5_DEVICE_CAPACITY;
}

sp5_device_lease sp5_devices_lease(const sp5_devices *devices, sp5_bytes certificate)
{
    size_t index = find(devices, certificate);
    if (index == SP5_DEVICE_CAPACITY) {
        return (sp5_device_lease){0};
    }
    const atomic_uint_fast64_t *generation = &devices->generations[devices->slots[index]];
    return (sp5_device_lease){generation, atomic_load_explicit(generation, memory_order_acquire)};
}

bool sp5_device_lease_valid(sp5_device_lease lease)
{
    return lease.generation != NULL && (lease.expected & 1) != 0 &&
           atomic_load_explicit(lease.generation, memory_order_acquire) == lease.expected;
}

static size_t available_slot(const sp5_devices *devices)
{
    for (size_t i = 0; i < SP5_DEVICE_CAPACITY; ++i) {
        uint_fast64_t value = atomic_load_explicit(&devices->generations[i], memory_order_relaxed);
        if ((value & 1) == 0 && value < UINT_FAST64_MAX - 1) {
            return i;
        }
    }
    return SP5_DEVICE_CAPACITY;
}

sp5_result sp5_devices_pair(sp5_devices *devices, sp5_bytes certificate, sp5_bytes name,
                            uint64_t paired_at)
{
    /* The same owner-approved scalar/octet limit applies to host and device names. */
    if (devices == NULL || !valid_certificate(certificate) || !sp5_name_valid(name)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (find(devices, certificate) != SP5_DEVICE_CAPACITY) {
        return sp5_success();
    }
    size_t slot = available_slot(devices);
    if (devices->count == SP5_DEVICE_CAPACITY || slot == SP5_DEVICE_CAPACITY) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    sp5_devices candidate = *devices;
    sp5_device *device = &candidate.records[candidate.count];
    *device = (sp5_device){.certificate_size = certificate.size, .paired_at = paired_at};
    memcpy(device->certificate, certificate.data, certificate.size);
    memcpy(device->name, name.data, name.size);
    if (!identify(device)) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    for (size_t i = 0; i < devices->count; ++i) {
        if (strcmp(device->id, devices->records[i].id) == 0) {
            return sp5_failure(SP5_REASON_CONFLICT);
        }
    }
    candidate.slots[candidate.count] = slot;
    ++candidate.count;
    sp5_result result = persist(&candidate);
    if (result.status == SP5_OK) {
        durable(&candidate);
        candidate.repairing_required = false;
        *devices = candidate;
        if (!publish(&devices->generations[slot])) {
            sp5_devices_clear(devices);
            return sp5_failure(SP5_REASON_CAPACITY);
        }
    }
    return result;
}

sp5_result sp5_devices_rename(sp5_devices *devices, sp5_device_name update)
{
    sp5_bytes certificate = update.certificate;
    sp5_bytes name = update.name;
    size_t index = find(devices, certificate);
    if (index == SP5_DEVICE_CAPACITY || !sp5_name_valid(name)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    sp5_devices candidate = *devices;
    memset(candidate.records[index].name, 0, sizeof(candidate.records[index].name));
    memcpy(candidate.records[index].name, name.data, name.size);
    sp5_result result = persist(&candidate);
    if (result.status == SP5_OK) {
        durable(&candidate);
        *devices = candidate;
    }
    return result;
}

sp5_result sp5_devices_session_ended(sp5_devices *devices, sp5_bytes certificate, uint64_t ended_at)
{
    size_t index = find(devices, certificate);
    if (index == SP5_DEVICE_CAPACITY || ended_at < devices->records[index].paired_at) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    sp5_devices candidate = *devices;
    candidate.records[index].last_session_at = (sp5_optional_u64){true, ended_at};
    sp5_result result = persist(&candidate);
    if (result.status == SP5_OK) {
        durable(&candidate);
        *devices = candidate;
    }
    return result;
}

sp5_result sp5_devices_revoke(sp5_devices *devices, sp5_bytes certificate)
{
    if (devices == NULL || !valid_certificate(certificate)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    size_t index = find(devices, certificate);
    if (index != SP5_DEVICE_CAPACITY) {
        /* This release store precedes disk I/O and acknowledgement. An acquire
         * check linearizes each input admission against this same atomic slot;
         * no reader touches mutable records or waits for the management owner. */
        invalidate(&devices->generations[devices->slots[index]]);
        if (devices->pending_count < SP5_DEVICE_CAPACITY) {
            sp5_device_revocation *entry = &devices->pending[devices->pending_count++];
            memcpy(entry->id, devices->records[index].id, sizeof(entry->id));
            memcpy(entry->name, devices->records[index].name, sizeof(entry->name));
        }
        memmove(&devices->slots[index], &devices->slots[index + 1],
                (devices->count - index - 1) * sizeof(devices->slots[0]));
        memmove(&devices->records[index], &devices->records[index + 1],
                (devices->count - index - 1) * sizeof(sp5_device));
        --devices->count;
        memset(&devices->records[devices->count], 0, sizeof(sp5_device));
        devices->dirty = true;
    }
    return sp5_devices_persist_pending(devices);
}

bool sp5_devices_durability_pending(const sp5_devices *devices)
{
    return devices != NULL && devices->dirty;
}

size_t sp5_devices_pending_count(const sp5_devices *devices)
{
    return devices != NULL && devices->dirty ? devices->pending_count : 0;
}

const sp5_device_revocation *sp5_devices_pending_at(const sp5_devices *devices, size_t index)
{
    return index < sp5_devices_pending_count(devices) ? &devices->pending[index] : NULL;
}

sp5_result sp5_devices_persist_pending(sp5_devices *devices)
{
    if (devices == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (!devices->dirty) {
        return sp5_success();
    }
    sp5_result result = persist(devices);
    if (result.status == SP5_OK) {
        durable(devices);
    }
    return result;
}
