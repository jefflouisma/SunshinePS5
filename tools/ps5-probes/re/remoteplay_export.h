/* Nonshipping Remote Play export layouts and simulated-memory test seam. */
#ifndef R3_REMOTEPLAY_EXPORT_H
#define R3_REMOTEPLAY_EXPORT_H
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
/* Factual SDK ABI prefix through vaddrbase; no foreign implementation copied. */
struct module_prefix {
    uint64_t next, path, unknown0[2];
    uint32_t refcount, padding;
    uint64_t handle, mapbase, mapsize, textsize, database, datasize;
    uint64_t unknown1, unknown1size, entry, unknown2, vaddrbase;
};
struct module_object {
    struct module_prefix prefix;
    uint32_t tlsindex, padding;
    uint64_t tlsinit, tlsinitsize, tlssize, tlsoffset, tlsalign, pltgot, unknown3[6];
    uint64_t init, fini, eh_frame_hdr, eh_frame_hdr_size, eh_frame, eh_frame_size;
    int32_t status, flags;
    uint64_t unknown4[5], dynsec, unknown5[6];
};
struct dynamic_prefix {
    uint64_t next, previous, sysvec;
    uint32_t refcount, padding;
    uint64_t size, symtab, symtabsize, strtab, strtabsize, pltrela, pltrelasize, rela, relasize;
};
_Static_assert(sizeof(struct module_prefix) == 128, "SDK module prefix");
_Static_assert(sizeof(struct module_object) == 384, "SDK module object");
_Static_assert(offsetof(struct module_object, dynsec) == 328, "SDK dynamic metadata offset");
_Static_assert(offsetof(struct dynamic_prefix, symtab) == 40, "SDK symbol table offset");

struct r3_firmware_layout {
    uint32_t firmware;
    size_t dynlib_head;
};
#ifdef R3_RE_TEST
#define R3_RE_LOCAL
/* Bounded-output constants shared with the implementation for tests. */
enum { R3_RE_MAX_IMAGE_BYTES = 16 * 1024 * 1024, R3_RE_WINDOW_SECONDS = 120 };
extern const struct r3_firmware_layout firmware_layout;
extern uint64_t bytes_written;
extern time_t deadline;
extern char directory[128];
extern volatile sig_atomic_t cancelled;
void request_stop(int);
bool read_user(pid_t, uint64_t, void *, size_t);
bool read_user_partial(pid_t, uint64_t, void *, size_t, size_t *);
bool export_image_segments(pid_t, const char *, uint64_t, uint64_t);
bool export_region(pid_t, const char *, const char *, uint64_t, uint64_t, bool);
bool selected_module(const char *);
bool selected_process(const char *);
bool export_process(pid_t);
intptr_t kernel_get_proc(pid_t);
int kernel_copyout(intptr_t, void *, size_t);
#else
#define R3_RE_LOCAL static
#endif
#endif
