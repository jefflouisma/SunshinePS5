/* Non-shipping probe: dumps the decrypted, mapped code of a system library
 * loaded in this process (default libScePad.sprx) as hex, plus the addresses
 * of selected exports, for offline disassembly. Read-only: pages are copied
 * with kernel_proc_copyout, which fails on unmapped memory instead of
 * faulting. Build with -lkernel -lScePad (the library must be loaded).
 *
 *   -DMODULE_DUMP_NAME="libScePad.sprx"
 *   -DMODULE_DUMP_BYTES=0x80000   upper bound on the dump size
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>

#ifndef MODULE_DUMP_NAME
#define MODULE_DUMP_NAME "libScePad.sprx"
#endif
#ifndef MODULE_DUMP_BYTES
#define MODULE_DUMP_BYTES 0x80000
#endif

/* Referenced so the linker keeps libScePad loaded in this process. */
int scePadInit(void);

static const char *const exports[] = {
    "scePadVirtualDeviceInsertData",
    "scePadVirtualDeviceAddDevice",
    "scePadVirtualDeviceDeleteDevice",
    "scePadReadState",
    "scePadRead",
    "scePadOpen",
    "scePadSetProcessPrivilege",
    "scePadSetLoginUserNumber",
    "scePadGetHandle",
    "scePadVirtualDeviceGetRemoteSetting",
    "scePadSetVibration",
    "scePadSetLightBar",
    "scePadSetTriggerEffect",
    "scePadSetMotionSensorState",
};

int main(void)
{
    (void)scePadInit;
    pid_t pid = getpid();
    uint32_t handle = 0;
    if (kernel_dynlib_handle(pid, MODULE_DUMP_NAME, &handle) != 0) {
        printf("MODULE_DUMP no handle for %s\n", MODULE_DUMP_NAME);
        return 1;
    }
    intptr_t base = kernel_dynlib_mapbase_addr(pid, handle);
    printf("MODULE_DUMP module=%s handle=%u base=0x%lx\n", MODULE_DUMP_NAME, handle, (long)base);
    for (size_t i = 0; i < sizeof(exports) / sizeof(exports[0]); ++i) {
        intptr_t address = kernel_dynlib_dlsym(pid, handle, exports[i]);
        printf("MODULE_DUMP sym %s 0x%lx off=0x%lx\n", exports[i], (long)address,
               address != 0 ? (long)(address - base) : -1L);
    }
    static uint8_t page[4096];
    size_t dumped = 0;
    for (; dumped < MODULE_DUMP_BYTES; dumped += sizeof(page)) {
        if (kernel_proc_copyout(pid, base + (intptr_t)dumped, page, sizeof(page)) != 0)
            break;
        for (size_t line = 0; line < sizeof(page); line += 32) {
            printf("MD %06zx ", dumped + line);
            for (size_t i = 0; i < 32; ++i)
                printf("%02x", page[line + i]);
            printf("\n");
        }
    }
    printf("MODULE_DUMP bytes=0x%zx\n", dumped);
    return 0;
}
