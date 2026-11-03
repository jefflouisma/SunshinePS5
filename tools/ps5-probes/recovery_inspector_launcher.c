/* Source-pinned launcher boundary for the T050 recovery inspector. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RECOVERY_INSPECTOR_FIRMWARE UINT32_C(0x13600007)
#define RECOVERY_INSPECTOR_REFUSED 78
#define RECOVERY_INSPECTOR_INTERNAL_ERROR 70

typedef int (*recovery_inspector_observe_fn)(void);

static int write_text(FILE *output, const char *text)
{
    size_t length = strlen(text);
    return fwrite(text, 1, length, output) == length ? 0 : -1;
}

static int write_hex32(FILE *output, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    char encoded[10] = {'0', 'x'};
    for (size_t index = 0; index < 8; ++index) {
        unsigned shift = (unsigned)(7 - index) * 4;
        encoded[index + 2] = digits[(value >> shift) & UINT32_C(0xf)];
    }
    return fwrite(encoded, 1, sizeof(encoded), output) == sizeof(encoded) ? 0 : -1;
}

int recovery_inspector_launcher_run(uint32_t firmware, recovery_inspector_observe_fn observe,
                                    FILE *output)
{
    if (output == NULL)
        return RECOVERY_INSPECTOR_INTERNAL_ERROR;

    if (firmware != RECOVERY_INSPECTOR_FIRMWARE) {
        if (write_text(output, "{\"schema_version\":1,\"complete\":false,"
                               "\"error\":\"unsupported_firmware\","
                               "\"expected_firmware\":\"") ||
            write_hex32(output, RECOVERY_INSPECTOR_FIRMWARE) ||
            write_text(output, "\",\"observed_firmware\":\"") || write_hex32(output, firmware) ||
            write_text(output, "\"}\n") || fflush(output) != 0)
            return RECOVERY_INSPECTOR_INTERNAL_ERROR;
        return RECOVERY_INSPECTOR_REFUSED;
    }

    if (observe == NULL) {
        if (write_text(output, "{\"schema_version\":1,\"complete\":false,"
                               "\"error\":\"observer_unavailable\"}\n") ||
            fflush(output) != 0)
            return RECOVERY_INSPECTOR_INTERNAL_ERROR;
        return RECOVERY_INSPECTOR_REFUSED;
    }

    return observe();
}

#ifndef RECOVERY_INSPECTOR_LAUNCHER_TEST
#include <ps5/kernel.h>

int recovery_inspector_observe(void);

int main(void)
{
    /*
     * The pinned SDK CRT initializes libc/runtime linking and performs its
     * process-local credential/syscall bootstrap before reaching this main.
     * Building this ELF does not establish safe deployability or hardware
     * validation; both still require independent review and device evidence.
     */
    return recovery_inspector_launcher_run(kernel_get_fw_version(), recovery_inspector_observe,
                                           stdout);
}
#endif
