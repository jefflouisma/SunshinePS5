#ifndef SUNSHINE_SETTINGS_HOST_H
#define SUNSHINE_SETTINGS_HOST_H

#include "store.h"

enum { SP5_NAME_CAPACITY = 64, SP5_MODE_CAPACITY = 16, SP5_IDENTITY_CAPACITY = 8192 };
typedef enum { SP5_HOST_STARTING, SP5_HOST_RUNNING, SP5_HOST_STOPPING } sp5_host_state;
typedef struct {
    uint32_t width, height, fps;
    char codec[24];
    sp5_dynamic_range dynamic_range;
    uint32_t minimum_bitrate_kbps, maximum_bitrate_kbps;
} sp5_host_mode;
typedef struct {
    sp5_settings settings;
    char address[128];
    char version[64];
    char firmware_version[64];
    sp5_host_state state;
    sp5_bytes identity;
    sp5_host_mode supported_modes[SP5_MODE_CAPACITY];
    size_t supported_mode_count;
    /* Explicit adapter capability: mcc 41cedb8 Limelight.h:1016-1019,
     * LI_FF_CONTROLLER_TOUCH_EVENTS also gates controller motion. Defaults
     * false; portable parser support alone proves no console capability. */
    bool controller_touch_events;
    /* Optional explicit protocol encryption capabilities, notes §3:
     * CONTROL_V2=1, VIDEO=2, AUDIO=4. Zero preserves the default 7. */
    uint32_t encryption_supported;
} sp5_host;

/* Names are valid UTF-8, 1-63 scalars AND <=63 octets for DNS-SD. Reject
 * Unicode C0/C1 controls, malformed encoding, embedded NUL and truncation.
 * All settings/registry operations run on the serialized management owner;
 * they must never be called by a media/input producer. */
bool sp5_name_valid(sp5_bytes name);
const char *sp5_name_requirement(void);
sp5_settings sp5_settings_defaults(void);
bool sp5_settings_valid(const sp5_settings *settings);
sp5_result sp5_host_init(sp5_host *host, const char *version, const char *firmware,
                         const char *observed_address);
sp5_result sp5_host_settings(sp5_host *host, const sp5_settings *settings);
sp5_result sp5_host_transition(sp5_host *host, sp5_host_state state);
/* Refresh settings and borrowed identity together before producing status or
 * discovery snapshots; the store must outlive this management-owner snapshot. */
sp5_result sp5_host_refresh(sp5_host *host, const sp5_store *store);

/* POSIX development-machine adapter only; not a console key-protection claim.
 * Caller durably provisions an existing directory owned by the effective user
 * with mode 0700. Only one
 * management owner may use a directory. No keys appear in diagnostics. */
typedef struct sp5_posix_storage sp5_posix_storage;
sp5_posix_storage *sp5_posix_storage_open(const char *directory);
sp5_storage_ops sp5_posix_storage_operations(sp5_posix_storage *storage);
sp5_result sp5_posix_storage_close(sp5_posix_storage *storage);

/* Versioned bounded persistence envelope shared by store and registry. Digest
 * detects damage; file permissions, not this digest, protect against tampering. */
enum { SP5_RECORD_OVERHEAD = 44 };
sp5_result sp5_record_pack(sp5_store_file kind, sp5_bytes payload, sp5_buffer *record);
sp5_result sp5_record_unpack(sp5_store_file kind, sp5_bytes record, sp5_bytes *payload);

#endif
