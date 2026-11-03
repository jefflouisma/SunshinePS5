#include "host.h"

#include <string.h>

bool sp5_name_valid(sp5_bytes name)
{
    if (name.data == NULL || name.size == 0 || name.size >= SP5_NAME_CAPACITY) {
        return false;
    }
    for (size_t i = 0; i < name.size;) {
        uint32_t scalar = name.data[i++];
        unsigned continuation = 0;
        uint32_t minimum = 0;
        if (scalar >= 0xc2 && scalar <= 0xdf) {
            scalar &= 0x1f;
            continuation = 1;
            minimum = 0x80;
        } else if (scalar >= 0xe0 && scalar <= 0xef) {
            scalar &= 0x0f;
            continuation = 2;
            minimum = 0x800;
        } else if (scalar >= 0xf0 && scalar <= 0xf4) {
            scalar &= 7;
            continuation = 3;
            minimum = 0x10000;
        } else if (scalar >= 0x80) {
            return false;
        }
        if (continuation > name.size - i) {
            return false;
        }
        for (unsigned j = 0; j < continuation; ++j) {
            uint8_t next = name.data[i++];
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff) ||
            scalar < 0x20 || (scalar >= 0x7f && scalar <= 0x9f)) {
            return false;
        }
    }
    return true;
}

const char *sp5_name_requirement(void)
{
    return "Name is invalid or too long. Use 1-63 Unicode scalars and at most 63 UTF-8 bytes, "
           "without control characters.";
}

sp5_settings sp5_settings_defaults(void)
{
    return (sp5_settings){.host_name = "SunshinePS5", .metrics_enabled = false};
}

bool sp5_settings_valid(const sp5_settings *settings)
{
    if (settings == NULL) {
        return false;
    }
    const char *end = memchr(settings->host_name, 0, sizeof(settings->host_name));
    return end != NULL && sp5_name_valid((sp5_bytes){(const uint8_t *)settings->host_name,
                                                     (size_t)(end - settings->host_name)});
}

static bool copy_text(char *output, size_t capacity, const char *text)
{
    if (text == NULL) {
        return false;
    }
    size_t length = 0;
    while (length < capacity && text[length] != 0) {
        if ((unsigned char)text[length] < 0x20 || (unsigned char)text[length] == 0x7f) {
            return false;
        }
        ++length;
    }
    if (length == 0 || length == capacity) {
        return false;
    }
    memcpy(output, text, length + 1);
    return true;
}

sp5_result sp5_host_init(sp5_host *host, const char *version, const char *firmware,
                         const char *observed_address)
{
    if (host == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *host = (sp5_host){.settings = sp5_settings_defaults(), .state = SP5_HOST_STARTING};
    if (!copy_text(host->version, sizeof(host->version), version) ||
        !copy_text(host->firmware_version, sizeof(host->firmware_version), firmware) ||
        !copy_text(host->address, sizeof(host->address), observed_address)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (strcmp(host->firmware_version, "13.60") != 0) {
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    }
    /* This build has no validated console capture/encode modes. SDK symbols and
     * probe formats cannot populate this production capability list. */
    return sp5_success();
}

sp5_result sp5_host_settings(sp5_host *host, const sp5_settings *settings)
{
    if (host == NULL || !sp5_settings_valid(settings)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    host->settings = *settings;
    return sp5_success();
}

sp5_result sp5_host_transition(sp5_host *host, sp5_host_state state)
{
    if (host == NULL || state < SP5_HOST_STARTING || state > SP5_HOST_STOPPING ||
        state < host->state) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (state == SP5_HOST_RUNNING && strcmp(host->firmware_version, "13.60") != 0) {
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    }
    host->state = state;
    return sp5_success();
}

sp5_result sp5_host_refresh(sp5_host *host, const sp5_store *store)
{
    if (host == NULL || store == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    host->identity = sp5_store_identity(store);
    if (host->identity.size == 0) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    host->settings = sp5_store_settings(store);
    return sp5_success();
}
