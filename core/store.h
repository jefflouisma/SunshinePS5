#ifndef SUNSHINE_CORE_STORE_H
#define SUNSHINE_CORE_STORE_H

#include "protocol.h"

typedef enum {
    SP5_STORE_IDENTITY,
    SP5_STORE_SETTINGS,
    SP5_STORE_DEVICES,
    SP5_STORE_FILE_COUNT
} sp5_store_file;
/* Setup/storage boundary, never called on input/media producers. Replacement
 * must be atomic and durable (temp write, sync, replace, directory sync), and
 * report failures. Missing read is WOULD_BLOCK; corrupt bytes are not missing.
 * Portable tests use in-memory durable-file fakes, not disk/power-loss evidence. */
typedef struct {
    void *context;
    sp5_result (*read)(void *context, sp5_store_file file, sp5_buffer *output);
    sp5_result (*replace)(void *context, sp5_store_file file, sp5_bytes contents);
    sp5_result (*generate_identity)(void *context, sp5_buffer *output);
} sp5_storage_ops;
typedef struct sp5_store sp5_store;
typedef struct {
    char host_name[64];
    bool metrics_enabled;
} sp5_settings;
typedef struct {
    bool identity_valid;
    bool settings_recovered;
    bool repairing_required;
    sp5_reason diagnostic;
    size_t paired_count;
} sp5_store_view;
SP5_CORE_API sp5_store *sp5_store_create(const sp5_storage_ops *storage);
SP5_CORE_API void sp5_store_destroy(sp5_store *store);
SP5_CORE_API sp5_result sp5_store_load(sp5_store *store);
SP5_CORE_API sp5_store_view sp5_store_read(const sp5_store *store);
/* FR-039 plain-language state of the last load and what the owner can do next;
 * empty when storage is healthy. Static text: no paths, bytes or key material.
 * Saving settings again clears the settings-fallback part. */
SP5_CORE_API const char *sp5_store_explanation(const sp5_store *store);
SP5_CORE_API sp5_bytes sp5_store_identity(const sp5_store *store);
SP5_CORE_API sp5_settings sp5_store_settings(const sp5_store *store);
SP5_CORE_API sp5_result sp5_store_save_settings(sp5_store *store, const sp5_settings *settings);
/* paired_at is caller-supplied wall-clock Unix MICROSECONDS, the unit of every
 * device timestamp (pair flow, session end, web app); zero is refused. */
SP5_CORE_API sp5_result sp5_store_pair(sp5_store *store, sp5_bytes certificate,
                                       uint64_t paired_at_unix_us);
SP5_CORE_API bool sp5_store_authorized(const sp5_store *store, sp5_bytes certificate);

#endif
