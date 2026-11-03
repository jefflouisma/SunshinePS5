/* Simulated memory/filesystem tests; no console or firmware capability evidence. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Test operations and checks must execute in optimized NDEBUG builds too. */
#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            (void)fputs(__FILE__ ": check failed: " #expression "\n", stderr);                     \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

static unsigned char kernel_bytes[8192], user_bytes[8192];
unsigned long KERNEL_ADDRESS_DMAP_BASE = UINT64_C(0xffff800000000000);
intptr_t kernel_get_proc(pid_t pid)
{
    return pid == 85 ? 64 : 0;
}
int kernel_copyout(intptr_t address, void *out, size_t size)
{
    uint64_t a = (uint64_t)address;
    if (a >= KERNEL_ADDRESS_DMAP_BASE) {
        a -= KERNEL_ADDRESS_DMAP_BASE;
        if (a > sizeof(user_bytes) || size > sizeof(user_bytes) - a)
            return -1;
        memcpy(out, user_bytes + a, size);
    } else {
        if (a > sizeof(kernel_bytes) || size > sizeof(kernel_bytes) - a)
            return -1;
        memcpy(out, kernel_bytes + a, size);
    }
    return 0;
}
int kernel_proc_getpaddr(int pid, unsigned long address, unsigned long *page_physical,
                         unsigned long *page_available)
{
    CHECK(page_physical != page_available);
    if (pid != 85 || address < 0x400000 || address >= 0x402000)
        return -1;
    *page_physical = address - 0x400000;
    *page_available = 4096 - address % 4096;
    return 0;
}

#include "../../tools/ps5-probes/re/remoteplay_export.h"

int main(void)
{
    const char *temporary_root = getenv("TMPDIR");
    if (!temporary_root)
        temporary_root = "/tmp";
    char temporary[128];
    int temporary_length =
        snprintf(temporary, sizeof(temporary), "%s/t041-export-XXXXXX", temporary_root);
    CHECK(temporary_length > 0 && (size_t)temporary_length < sizeof(temporary));
    char *root = mkdtemp(temporary);
    CHECK(root);
    CHECK(strlen(root) < sizeof(directory));
    memcpy(directory, root, strlen(root) + 1);
    deadline = time(NULL) + 30;
    for (size_t i = 0; i < sizeof(user_bytes); ++i)
        user_bytes[i] = (unsigned char)i;
    unsigned char readback[8192];
    CHECK(read_user(85, 0x400000, readback, sizeof(readback)));
    CHECK(memcmp(readback, user_bytes, sizeof(readback)) == 0);
    CHECK(!read_user(86, 0x400000, readback, 1));
    CHECK(!read_user(85, UINT64_MAX, readback, 2));
    CHECK(!read_user(85, 0x401fff, readback, 2));
#ifdef R3_RE_LOAD_RECORDING
    CHECK(selected_module("libSceVideoRecording.sprx"));
    CHECK(!selected_module("libSceResourceArbitrator.sprx"));
    CHECK(!selected_module("eboot.bin"));
#elif defined(R3_RE_RESOURCE_EXPORT)
    CHECK(selected_module("libSceResourceArbitrator.sprx"));
    CHECK(selected_module("libSceVideoRecording.sprx"));
    CHECK(!selected_module("eboot.bin"));
    CHECK(!selected_module("libSceVenc.sprx"));
#else
    CHECK(selected_module("libSceVenc.sprx"));
    CHECK(!selected_module("libSceResourceArbitrator.sprx"));
#endif
    CHECK(!selected_module("../../secret"));
    CHECK(!selected_module("libSceNpAuth.sprx"));
    CHECK(selected_process("SceAvCapture"));
    CHECK(selected_process("SceRemotePlay"));
    CHECK(!selected_process("payload.elf"));
#ifdef R3_RE_RESOURCE_EXPORT
    CHECK(selected_process("SceShellCore"));
    CHECK(!selected_process("SceSysAvControl.elf"));
#else
    CHECK(!selected_process("SceShellCore"));
    CHECK(selected_process("SceSysAvControl.elf"));
#endif

    CHECK(export_region(85, "fixture", "text", 0x400000, 8192, false));
    CHECK(!export_region(85, "fixture", "text", 0x400000, 8192, false));
    CHECK(bytes_written == 8192);
    CHECK(!export_region(85, "large", "text", 0x400000, UINT64_MAX, false));
    CHECK(!export_region(85, "zero", "text", 0x400000, 0, false));
    /* A text dump that hits an unmapped page keeps its nonzero readable
     * prefix as a marked partial; zero progress is still deleted. */
    CHECK(!export_region(85, "partial", "text", 0x401000, 8192, false));
    char path[256];
    int n = snprintf(path, sizeof(path), "%s/85-partial-text.bin", root);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    FILE *file = fopen(path, "rb");
    CHECK(file && fread(readback, 1, 4096, file) == 4096 && fclose(file) == 0);
    CHECK(memcmp(readback, user_bytes + 4096, 4096) == 0);
    CHECK(unlink(path) == 0);
    /* Raw module-object dumps (kernel memory, 384 bytes, 512-byte cap). */
    memcpy(kernel_bytes + 3000, user_bytes, 384);
    CHECK(export_region(85, "fixture", "obj", 3000, 384, true));
    CHECK(!export_region(85, "fixture", "obj", 3000, 513, true));
    CHECK(!export_region(85, "fixture", "obj", 0, 384, true));
    /* Full mapped-image dumps use the same user-memory path as text and the
     * shared 16 MiB per-region cap. */
    CHECK(export_region(85, "fixture", "image", 0x400000, 8192, false));
    CHECK(!export_region(85, "fixture", "image", 0x400000, (uint64_t)R3_RE_MAX_IMAGE_BYTES + 1,
                         false));
    /* Gap-skipping segment walk: user memory here is one contiguous run
     * (0x400000..0x402000); a larger mapsize skips the unmapped pages and
     * still succeeds, keeping only the readable run. */
    CHECK(export_image_segments(85, "fixture", 0x400000, 8192));
    CHECK(export_image_segments(85, "fixture2", 0x400000, 0x4000));
    n = snprintf(path, sizeof(path), "%s/85-fixture-segs.bin", root);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    file = fopen(path, "rb");
    CHECK(file && fread(readback, 1, 8192, file) == 8192 && fclose(file) == 0);
    CHECK(memcmp(readback, user_bytes, 8192) == 0);
    CHECK(unlink(path) == 0);
    n = snprintf(path, sizeof(path), "%s/85-fixture2-segs.bin", root);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    file = fopen(path, "rb");
    CHECK(file && fread(readback, 1, 8192, file) == 8192 && fclose(file) == 0);
    CHECK(unlink(path) == 0);
    n = snprintf(path, sizeof(path), "%s/85-fixture-text.bin", root);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    file = fopen(path, "rb");
    CHECK(file && fread(readback, 1, sizeof(readback), file) == sizeof(readback));
    CHECK(fclose(file) == 0 && memcmp(readback, user_bytes, sizeof(readback)) == 0);
    CHECK(unlink(path) == 0);
    n = snprintf(path, sizeof(path), "%s/85-fixture-obj.bin", root);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    CHECK(access(path, F_OK) == 0 && unlink(path) == 0);
    n = snprintf(path, sizeof(path), "%s/85-fixture-image.bin", root);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    CHECK(access(path, F_OK) == 0 && unlink(path) == 0);

    uint64_t head = 1200, first = 1400;
    memcpy(kernel_bytes + 64 + firmware_layout.dynlib_head, &head, sizeof(head));
    memcpy(kernel_bytes + head, &first, sizeof(first));
    struct module_object object = {0};
    object.prefix.next = first;
    object.prefix.path = 2200;
    memcpy(kernel_bytes + first, &object, sizeof(object));
    memcpy(kernel_bytes + 2200, "/ignored/module.sprx", sizeof("/ignored/module.sprx"));
    CHECK(!export_process(85)); /* Cycle terminates rather than reading indefinitely. */
    CHECK(!export_process(86));
    request_stop(SIGTERM);
    CHECK(!read_user(85, 0x400000, readback, 1));
    cancelled = 0;
    deadline = time(NULL) - 1;
    CHECK(!read_user(85, 0x400000, readback, 1));
    CHECK(!export_process(85));
    CHECK(rmdir(root) == 0);
    puts("T041 Remote Play export bounds/cleanup tests passed; hardware evidence absent");
    return 0;
}
