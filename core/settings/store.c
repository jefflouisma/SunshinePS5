#include "devices.h"
#include "host.h"
#include "storage_diagnostic.h"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

struct sp5_store {
    sp5_storage_ops storage;
    sp5_settings settings;
    sp5_store_view view;
    uint8_t identity[SP5_IDENTITY_CAPACITY];
    size_t identity_size;
    sp5_devices *devices;
};

sp5_result sp5_record_pack(sp5_store_file kind, sp5_bytes payload, sp5_buffer *record)
{
    if (record == NULL || record->data == NULL || payload.data == NULL ||
        kind < SP5_STORE_IDENTITY || kind >= SP5_STORE_FILE_COUNT || payload.size > UINT32_MAX ||
        record->capacity < SP5_RECORD_OVERHEAD ||
        payload.size > record->capacity - SP5_RECORD_OVERHEAD) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    record->size = 0;
    uint8_t *data = record->data;
    memmove(data + 12, payload.data, payload.size);
    static const uint8_t magic[7] = {'S', 'P', '5', 'R', 'E', 'C', '1'};
    memcpy(data, magic, sizeof(magic));
    data[7] = (uint8_t)kind;
    for (unsigned i = 0; i < 4; ++i) {
        data[8 + i] = (uint8_t)(payload.size >> (24 - 8 * i));
    }
    size_t digest_size = 0;
    if (EVP_Q_digest(NULL, "SHA256", NULL, data, payload.size + 12, data + payload.size + 12,
                     &digest_size) != 1 ||
        digest_size != 32) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    record->size = payload.size + SP5_RECORD_OVERHEAD;
    return sp5_success();
}

sp5_result sp5_record_unpack(sp5_store_file kind, sp5_bytes record, sp5_bytes *payload)
{
    if (payload == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *payload = (sp5_bytes){0};
    if (record.data == NULL || record.size < SP5_RECORD_OVERHEAD ||
        memcmp(record.data, "SP5REC1", 7) != 0 || record.data[7] != (uint8_t)kind) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    uint32_t length = 0;
    for (unsigned i = 0; i < 4; ++i) {
        length = (length << 8) | record.data[8 + i];
    }
    uint8_t digest[32];
    size_t digest_size = 0;
    if (length != record.size - SP5_RECORD_OVERHEAD ||
        EVP_Q_digest(NULL, "SHA256", NULL, record.data, record.size - 32, digest, &digest_size) !=
            1 ||
        digest_size != 32 || CRYPTO_memcmp(digest, record.data + record.size - 32, 32) != 0) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *payload = (sp5_bytes){record.data + 12, length};
    return sp5_success();
}

sp5_store *sp5_store_create(const sp5_storage_ops *storage)
{
    if (storage == NULL || storage->read == NULL || storage->replace == NULL ||
        storage->generate_identity == NULL) {
        return NULL;
    }
    sp5_store *store = calloc(1, sizeof(*store));
    if (store != NULL) {
        store->storage = *storage;
        store->settings = sp5_settings_defaults();
        store->devices = sp5_devices_create(storage);
        if (store->devices == NULL) {
            free(store);
            return NULL;
        }
    }
    return store;
}

void sp5_store_destroy(sp5_store *store)
{
    if (store != NULL) {
        sp5_devices_destroy(store->devices);
        OPENSSL_cleanse(store, sizeof(*store));
        free(store);
    }
}

static sp5_result load_identity(sp5_store *store)
{
    uint8_t raw[SP5_IDENTITY_CAPACITY + SP5_RECORD_OVERHEAD];
    sp5_buffer record = {raw, sizeof(raw), 0};
    sp5_result result = store->storage.read(store->storage.context, SP5_STORE_IDENTITY, &record);
    if (result.status != SP5_OK && result.reason == SP5_REASON_WOULD_BLOCK) {
        /* Missing identity with any existing/unreadable installation state is
         * damage, not first run. Never pretend existing pairings survived. */
        uint8_t probe[1];
        for (sp5_store_file file = SP5_STORE_SETTINGS; file < SP5_STORE_FILE_COUNT; ++file) {
            sp5_buffer output = {probe, sizeof(probe), 0};
            sp5_result exists = store->storage.read(store->storage.context, file, &output);
            if (exists.status == SP5_OK || exists.reason != SP5_REASON_WOULD_BLOCK) {
                return sp5_failure(SP5_REASON_OWNERSHIP);
            }
        }
        sp5_buffer identity = {store->identity, sizeof(store->identity), 0};
        result = store->storage.generate_identity(store->storage.context, &identity);
        if (result.status == SP5_OK && identity.size > 0 && identity.size <= identity.capacity) {
            SP5_STORAGE_BEGIN("identity.pack", SP5_STORE_IDENTITY);
            result =
                SP5_STORAGE_OP("identity.pack",
                               sp5_record_pack(SP5_STORE_IDENTITY,
                                               (sp5_bytes){identity.data, identity.size}, &record),
                               sp5_failure(SP5_REASON_PLATFORM));
            SP5_STORAGE_END("identity.pack", SP5_STORE_IDENTITY, result.status, result.reason,
                            ERR_peek_last_error());
            if (result.status == SP5_OK) {
                result = store->storage.replace(store->storage.context, SP5_STORE_IDENTITY,
                                                (sp5_bytes){record.data, record.size});
            }
            if (result.status == SP5_OK) {
                store->identity_size = identity.size;
            }
        } else if (result.status == SP5_OK) {
            result = sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
    } else if (result.status == SP5_OK && record.size <= record.capacity) {
        sp5_bytes payload;
        result = sp5_record_unpack(SP5_STORE_IDENTITY, (sp5_bytes){raw, record.size}, &payload);
        if (result.status == SP5_OK && payload.size > 0 &&
            payload.size <= sizeof(store->identity)) {
            memcpy(store->identity, payload.data, payload.size);
            store->identity_size = payload.size;
        } else {
            result = sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
    } else if (result.status == SP5_OK) {
        result = sp5_failure(SP5_REASON_CAPACITY);
    }
    OPENSSL_cleanse(raw, sizeof(raw));
    return result;
}

sp5_result sp5_store_load(sp5_store *store)
{
    if (store == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    store->view = (sp5_store_view){0};
    store->identity_size = 0;
    OPENSSL_cleanse(store->identity, sizeof(store->identity));
    store->settings = sp5_settings_defaults();
    sp5_result result = load_identity(store);
    if (result.status != SP5_OK) {
        store->view.diagnostic = result.reason;
        store->view.repairing_required = true;
        return result;
    }
    store->view.identity_valid = true;
    uint8_t raw[128];
    sp5_buffer record = {raw, sizeof(raw), 0};
    result = store->storage.read(store->storage.context, SP5_STORE_SETTINGS, &record);
    if (result.status == SP5_OK && record.size <= record.capacity) {
        sp5_bytes payload;
        result = sp5_record_unpack(SP5_STORE_SETTINGS, (sp5_bytes){raw, record.size}, &payload);
        if (result.status == SP5_OK && payload.size >= 2 && payload.size <= 64 &&
            payload.data[0] <= 1 &&
            sp5_name_valid((sp5_bytes){payload.data + 1, payload.size - 1})) {
            memcpy(store->settings.host_name, payload.data + 1, payload.size - 1);
            store->settings.host_name[payload.size - 1] = 0;
            store->settings.metrics_enabled = payload.data[0] != 0;
        } else {
            result = sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
    } else if (result.status == SP5_OK) {
        result = sp5_failure(SP5_REASON_CAPACITY);
    }
    if (result.status != SP5_OK && result.reason != SP5_REASON_WOULD_BLOCK) {
        store->view.settings_recovered = true;
        store->view.diagnostic = result.reason;
    }
    result = sp5_devices_load(store->devices);
    if (result.status != SP5_OK) {
        store->view.repairing_required = true;
        store->view.diagnostic = result.reason;
    }
    return sp5_success();
}

sp5_store_view sp5_store_read(const sp5_store *store)
{
    if (store == NULL) {
        return (sp5_store_view){.diagnostic = SP5_REASON_INVALID_ARGUMENT};
    }
    sp5_store_view view = store->view;
    view.paired_count = view.identity_valid ? sp5_devices_count(store->devices) : 0;
    view.repairing_required =
        !view.identity_valid || sp5_devices_repairing_required(store->devices);
    return view;
}

const char *sp5_store_explanation(const sp5_store *store)
{
    sp5_store_view view = sp5_store_read(store);
    if (store == NULL || !view.identity_valid) {
        return "The host identity could not be read, so no device can pair or stream. Restore "
               "the host's saved files, or reinstall and pair every device again.";
    }
    if (view.settings_recovered && view.repairing_required) {
        return "Saved settings and paired devices were unreadable; defaults are in use and no "
               "devices are authorized. Re-enter the host name and metrics setting, then pair "
               "again.";
    }
    if (view.repairing_required) {
        return sp5_devices_explanation(store->devices);
    }
    if (view.settings_recovered) {
        return "Saved settings were unreadable; defaults are in use. Re-enter the host name and "
               "metrics setting.";
    }
    return "";
}

sp5_bytes sp5_store_identity(const sp5_store *store)
{
    return store != NULL && store->view.identity_valid
               ? (sp5_bytes){store->identity, store->identity_size}
               : (sp5_bytes){0};
}

sp5_settings sp5_store_settings(const sp5_store *store)
{
    return store != NULL ? store->settings : sp5_settings_defaults();
}

sp5_result sp5_store_save_settings(sp5_store *store, const sp5_settings *settings)
{
    if (store == NULL || !store->view.identity_valid || !sp5_settings_valid(settings)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    uint8_t payload[64] = {0};
    size_t length = strlen(settings->host_name);
    payload[0] = settings->metrics_enabled ? 1 : 0;
    memcpy(payload + 1, settings->host_name, length);
    uint8_t raw[128];
    sp5_buffer record = {raw, sizeof(raw), 0};
    sp5_result result =
        sp5_record_pack(SP5_STORE_SETTINGS, (sp5_bytes){payload, length + 1}, &record);
    if (result.status == SP5_OK) {
        result = store->storage.replace(store->storage.context, SP5_STORE_SETTINGS,
                                        (sp5_bytes){record.data, record.size});
    }
    if (result.status == SP5_OK) {
        store->settings = *settings;
        /* Owner-entered settings now replace the recovered defaults. */
        store->view.settings_recovered = false;
    } else {
        store->view.diagnostic = result.reason;
    }
    return result;
}

sp5_result sp5_store_pair(sp5_store *store, sp5_bytes certificate, uint64_t paired_at_unix_us)
{
    if (store == NULL || !store->view.identity_valid) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    if (paired_at_unix_us == 0) {
        return sp5_failure(SP5_REASON_CLOCK);
    }
    return sp5_devices_pair(store->devices, certificate,
                            (sp5_bytes){(const uint8_t *)"Moonlight client", 16},
                            paired_at_unix_us);
}

bool sp5_store_authorized(const sp5_store *store, sp5_bytes certificate)
{
    return store != NULL && store->view.identity_valid &&
           sp5_devices_authorized(store->devices, certificate);
}

sp5_devices *sp5_store_devices(sp5_store *store)
{
    return store != NULL && store->view.identity_valid ? store->devices : NULL;
}
