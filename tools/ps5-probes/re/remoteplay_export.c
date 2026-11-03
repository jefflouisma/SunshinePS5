/* Nonshipping T041 static interoperability research. No media calls or patches.
 * ABI/layout source: ps5-payload-dev/sdk a7685ce8e97ce1f8a4f38253a41d54241c9124f1
 * crt/{kernel.h,kernel.c,kernel_procio.c}, target/include/sys/{sysctl,user}.h.
 * Only code and linker symbol/string tables are exported, never process heaps.
 * Stock SDK startup still writes process credentials/syscall bounds before main.
 * Build does not authorize execution. Outputs stay private, outside the repo.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef R3_RE_TEST
#include <sys/sysctl.h>
#include <sys/user.h>
#endif
#include <time.h>
#include <unistd.h>

#ifndef R3_RE_TEST
#include <ps5/kernel.h>
#endif

#include "remoteplay_export.h"

/* Firmware data from pinned SDK crt/kernel.c:kernel_dynlib_obj, 13.60 only. */
R3_RE_LOCAL const struct r3_firmware_layout firmware_layout = {UINT32_C(0x13600000), 0x3e8};

/* These are existing SDK routines/data, declared in its internal crt/kernel.h.
 * Page-table lookup reads entries; this probe never changes a mapping. */
extern unsigned long KERNEL_ADDRESS_DMAP_BASE;
extern int kernel_proc_getpaddr(int, unsigned long, unsigned long *, unsigned long *);

enum {
    MAX_NODES = 512,
    MAX_PROCESSES_BYTES = 2 * 1024 * 1024,
    CHUNK = 16384,
    MAX_OUTPUT_BYTES = 128 * 1024 * 1024,
    MAX_IMAGE_BYTES = 16 * 1024 * 1024,
    WINDOW_SECONDS = 120
};
static unsigned char buffer[CHUNK];
R3_RE_LOCAL uint64_t bytes_written;
R3_RE_LOCAL time_t deadline;
R3_RE_LOCAL char directory[128];
R3_RE_LOCAL volatile sig_atomic_t cancelled;

R3_RE_LOCAL void request_stop(int signum)
{
    cancelled = signum;
}

static bool within_window(void)
{
    time_t now = time(NULL);
    return !cancelled && now != (time_t)-1 && now <= deadline;
}

R3_RE_LOCAL bool read_user_partial(pid_t pid, uint64_t address, void *destination, size_t size,
                                   size_t *progress)
{
    size_t done = 0;
    bool ok = address <= UINT64_MAX - size;
    while (ok && done < size && within_window()) {
        unsigned long physical = 0, available = 0;
        if (kernel_proc_getpaddr(pid, address + done, &physical, &available) != 0 || !available ||
            physical > UINT64_MAX - KERNEL_ADDRESS_DMAP_BASE) {
            ok = false;
            break;
        }
        size_t take = available < size - done ? available : size - done;
        if (kernel_copyout((intptr_t)(KERNEL_ADDRESS_DMAP_BASE + physical),
                           (unsigned char *)destination + done, take) != 0) {
            ok = false;
            break;
        }
        done += take;
    }
    if (progress != NULL)
        *progress = done;
    return ok && done == size;
}

#ifdef R3_RE_TEST
R3_RE_LOCAL bool read_user(pid_t pid, uint64_t address, void *destination, size_t size)
{
    return read_user_partial(pid, address, destination, size, NULL);
}
#endif

/* Best-effort gap-skipping dump of a live mapped module image: 13.60 maps
 * text and data with unmapped pages between them. Walks the image in 4 KiB
 * steps, keeps every readable run, and prints an R3_RE_SEGMENT manifest line
 * per run (image offsets, concatenated bytes in run order). Skipped gaps are
 * recorded, never guessed. */
R3_RE_LOCAL bool export_image_segments(pid_t pid, const char *module, uint64_t mapbase,
                                       uint64_t mapsize)
{
    enum { PAGE = 4096 };
    if (!within_window() || !mapbase || !mapsize || mapsize > (uint64_t)MAX_IMAGE_BYTES ||
        mapsize > UINT64_MAX - mapbase) {
        printf("R3_RE_REFUSED module=%s region=segs reason=region_bounds\n", module);
        return false;
    }
    char path[256];
    int length = snprintf(path, sizeof(path), "%s/%ld-%s-segs.bin", directory, (long)pid, module);
    if (length < 0 || (size_t)length >= sizeof(path))
        return false;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return false;
    bool ok = true;
    uint64_t cursor = 0, written = 0, runs = 0, skipped = 0;
    bool in_run = false;
    uint64_t run_start = 0;
    while (cursor < mapsize && within_window()) {
        uint64_t left = mapsize - cursor;
        size_t want = left < PAGE ? (size_t)left : PAGE;
        size_t made = 0;
        if (read_user_partial(pid, mapbase + cursor, buffer, want, &made) && made == want) {
            if (!in_run) {
                in_run = true;
                run_start = cursor;
            }
            size_t sent = 0;
            while (sent < want) {
                ssize_t rc = write(fd, buffer + sent, want - sent);
                if (rc <= 0) {
                    ok = false;
                    break;
                }
                sent += (size_t)rc;
            }
            if (!ok)
                break;
            written += want;
            cursor += want;
        } else {
            if (in_run) {
                ++runs;
                printf("R3_RE_SEGMENT module=%s start=0x%" PRIx64 " length=0x%" PRIx64 "\n", module,
                       run_start, cursor - run_start);
                in_run = false;
            }
            if (made > 0) { /* partial page: keep what was readable */
                size_t sent = 0;
                while (sent < made) {
                    ssize_t rc = write(fd, buffer + sent, made - sent);
                    if (rc <= 0) {
                        ok = false;
                        break;
                    }
                    sent += (size_t)rc;
                }
                if (!ok)
                    break;
                written += made;
            }
            skipped += want - made;
            cursor += want;
        }
    }
    if (in_run) {
        ++runs;
        printf("R3_RE_SEGMENT module=%s start=0x%" PRIx64 " length=0x%" PRIx64 "\n", module,
               run_start, cursor - run_start);
    }
    if (close(fd) != 0)
        ok = false;
    if (written == 0 || bytes_written > MAX_OUTPUT_BYTES - written) {
        ok = false;
        if (unlink(path) != 0)
            printf("R3_RE_CLEANUP path=%s unlink_errno=%d\n", path, errno);
    } else {
        bytes_written += written;
    }
    printf("R3_RE_SEGS pid=%ld module=%s mapbase=0x%" PRIx64 " mapsize=0x%" PRIx64 " runs=%" PRIu64
           " written=%" PRIu64 " skipped=%" PRIu64 " status=%s path=%s\n",
           (long)pid, module, mapbase, mapsize, runs, written, skipped,
           ok && cursor == mapsize ? "exported" : "partial", path);
    return ok && cursor == mapsize && written > 0;
}

R3_RE_LOCAL bool export_region(pid_t pid, const char *module, const char *kind, uint64_t address,
                               uint64_t size, bool kernel_memory)
{
    /* Per-kind caps: full mapped images 16 MiB, linker tables 4 MiB, raw
     * module objects 512 B. Total output stays within MAX_OUTPUT_BYTES. */
    const uint64_t cap = strcmp(kind, "text") == 0 || strcmp(kind, "image") == 0
                             ? (uint64_t)MAX_IMAGE_BYTES
                             : (strcmp(kind, "obj") == 0 ? 512 : 4 * 1024 * 1024);
    if (!within_window() || !address || !size || size > cap || address > UINT64_MAX - size ||
        bytes_written > MAX_OUTPUT_BYTES - size) {
        printf("R3_RE_REFUSED module=%s region=%s reason=region_bounds\n", module, kind);
        return false;
    }
    char path[256];
    int length =
        snprintf(path, sizeof(path), "%s/%ld-%s-%s.bin", directory, (long)pid, module, kind);
    if (length < 0 || (size_t)length >= sizeof(path))
        return false;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return false;
    bool ok = true;
    uint64_t done = 0;
    while (done < size && within_window()) {
        size_t take = size - done < sizeof(buffer) ? (size_t)(size - done) : sizeof(buffer);
        size_t made = 0;
        bool read_ok;
        if (kernel_memory) {
            read_ok = kernel_copyout((intptr_t)(address + done), buffer, take) == 0;
            made = read_ok ? take : 0;
        } else {
            read_ok = read_user_partial(pid, address + done, buffer, take, &made);
        }
        if (made > 0) {
            size_t sent = 0;
            while (sent < made) {
                ssize_t rc = write(fd, buffer + sent, made - sent);
                if (rc <= 0) {
                    ok = false;
                    break;
                }
                sent += (size_t)rc;
            }
            if (!ok)
                break;
            done += made;
            bytes_written += made;
        }
        if (!read_ok)
            break;
    }
    ok = ok && done == size;
    if (close(fd) != 0)
        ok = false;
    /* Text dumps stop at the first unmapped page (the live text/data gap
     * observed on 13.60). A nonzero partial text is exactly the readable
     * prefix and is KEPT, clearly marked partial; every other kind keeps the
     * delete-on-failure guarantee. */
    bool partial_text = !ok && done > 0 && strcmp(kind, "text") == 0;
    if (!ok && !partial_text && unlink(path) != 0) {
        printf("R3_RE_CLEANUP path=%s unlink_errno=%d\n", path, errno);
    }
    printf("R3_RE_REGION pid=%ld module=%s region=%s address=0x%" PRIx64 " bytes=%" PRIu64
           " status=%s path=%s\n",
           (long)pid, module, kind, address, done,
           ok ? "exported" : (partial_text ? "partial" : "failed"), path);
    return ok;
}

R3_RE_LOCAL bool selected_module(const char *name)
{
#ifdef R3_RE_LOAD_RECORDING
    static const char *const names[] = {"libSceVideoRecording.sprx"};
#elif defined(R3_RE_RESOURCE_EXPORT)
    /* The saved daemon GOT resolves the selector query to ResourceArbitrator.
     * This separate build reads only the two admitted research libraries. */
    static const char *const names[] = {"libSceResourceArbitrator.sprx",
                                        "libSceVideoRecording.sprx"};
#else
    static const char *const names[] = {
        "eboot.bin",           "SceRemotePlay.elf",     "SceAvCapture.elf",
        "SceSysAvControl.elf", "libSceRemoteplay.sprx", "libSceRemotePlayClientIpc.sprx",
        "libSceAvcap2.sprx",   "libSceAvcap2mvr.sprx",  "libSceVenc.sprx",
        "libSceIpmi.sprx"};
#endif
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (strcmp(name, names[i]) == 0)
            return true;
    return false;
}

#if !defined(R3_RE_LOAD_RECORDING) || defined(R3_RE_TEST)
R3_RE_LOCAL bool selected_process(const char *name)
{
#ifdef R3_RE_RESOURCE_EXPORT
    return strcmp(name, "SceAvCapture") == 0 || strcmp(name, "SceRemotePlay") == 0 ||
           strcmp(name, "SceShellCore") == 0;
#else
    return strcmp(name, "SceRemotePlay") == 0 || strcmp(name, "SceAvCapture") == 0 ||
           strcmp(name, "SceSysAvControl.elf") == 0;
#endif
}
#endif

R3_RE_LOCAL bool export_process(pid_t pid)
{
    intptr_t process = kernel_get_proc(pid);
    uint64_t head = 0, node = 0;
    if (!process ||
        kernel_copyout(process + (intptr_t)firmware_layout.dynlib_head, &head, sizeof(head)) != 0 ||
        !head || kernel_copyout((intptr_t)head, &node, sizeof(node)) != 0)
        return false;
    uint64_t seen[MAX_NODES];
    size_t count = 0, selected = 0;
    bool ok = true;
    while (node && count < MAX_NODES && within_window()) {
        for (size_t i = 0; i < count; ++i)
            if (seen[i] == node)
                return false;
        seen[count++] = node;
        struct module_object object;
        if (kernel_copyout((intptr_t)node, &object, sizeof(object)) != 0)
            return false;
        if (object.prefix.path) {
            char path[256] = {0};
            if (kernel_copyout((intptr_t)object.prefix.path, path, sizeof(path)) != 0 ||
                memchr(path, '\0', sizeof(path)) == NULL)
                return false;
            const char *name = strrchr(path, '/');
            name = name ? name + 1 : path;
            printf("R3_RE_MODULE pid=%ld name=%s handle=%" PRIu64 " base=0x%" PRIx64
                   " word38=0x%" PRIx64 " word40=0x%" PRIx64 " selected=%s\n",
                   (long)pid, name, object.prefix.handle, object.prefix.mapbase,
                   object.prefix.mapsize, object.prefix.textsize,
                   selected_module(name) ? "yes" : "no");
            if (selected_module(name)) {
                ++selected;
                /* The 2026-10-25 run showed the live 13.60 dynlib_obj layout
                 * diverges from the SDK struct between 0x38 and 0x148: the
                 * SDK textsize offset (0x40) holds a kernel pointer. So this
                 * dumps the raw 384-byte module object first, then the whole
                 * mapped image bounded by the word at 0x38 when it is sane;
                 * both raw words are printed above for local verification. */
                if (!export_region(pid, name, "obj", (uint64_t)node, sizeof(object), true))
                    ok = false;
                if (object.prefix.mapbase && object.prefix.mapsize &&
                    object.prefix.mapsize <= (uint64_t)MAX_IMAGE_BYTES) {
                    /* Gap-skipping segment walk: on 13.60 the mapped image has
                     * unmapped pages between text and data; every readable run
                     * is kept with a printed R3_RE_SEGMENT manifest (see
                     * export_image_segments). */
                    if (!export_image_segments(pid, name, object.prefix.mapbase,
                                               object.prefix.mapsize))
                        ok = false;
                } else {
                    printf(
                        "R3_RE_REFUSED module=%s region=segs reason=image_bounds word38=0x%" PRIx64
                        "\n",
                        name, object.prefix.mapsize);
                    ok = false;
                }
                if (object.dynsec) {
                    struct dynamic_prefix dynamic;
                    if (kernel_copyout((intptr_t)object.dynsec, &dynamic, sizeof(dynamic)) != 0)
                        return false;
                    if (dynamic.symtabsize % 24 != 0 ||
                        !export_region(pid, name, "symbols", dynamic.symtab, dynamic.symtabsize,
                                       true) ||
                        !export_region(pid, name, "strings", dynamic.strtab, dynamic.strtabsize,
                                       true))
                        ok = false;
                    if (dynamic.pltrelasize &&
                        (dynamic.pltrelasize % 24 != 0 ||
                         !export_region(pid, name, "pltrela", dynamic.pltrela, dynamic.pltrelasize,
                                        true)))
                        ok = false;
                    if (dynamic.relasize &&
                        (dynamic.relasize % 24 != 0 ||
                         !export_region(pid, name, "rela", dynamic.rela, dynamic.relasize, true)))
                        ok = false;
                } else
                    ok = false;
            }
        }
        node = object.prefix.next;
    }
    printf("R3_RE_PROCESS pid=%ld nodes=%zu selected=%zu snapshot=live_not_atomic\n", (long)pid,
           count, selected);
    return ok && !node && selected > 0;
}

#ifndef R3_RE_TEST
#ifdef R3_RE_LOAD_RECORDING
/* SDK lifecycle ABI from the pinned crt/rtld_sprx.c, matching capture_encode.c. */
extern int sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t, void *, int *);
extern int sceKernelStopUnloadModule(int, size_t, const void *, uint32_t, void *, int *);

/* Acquisition only: the SDK loader interface is already used by the media
 * probe. No VideoRecording export is resolved or invoked. Module constructors
 * and stop/unload effects still need explicit execution approval. */
static int export_loaded_recording(void)
{
    const char *const names[] = {"libSceIpmi.sprx", "libSceVideoRecording.sprx"};
    const char *const paths[] = {"/system/common/lib/libSceIpmi.sprx",
                                 "/system/common/lib/libSceVideoRecording.sprx"};
    int modules[2] = {-1, -1};
    bool owned[2] = {false, false};
    bool ok = true;
    for (size_t i = 0; i < 2; ++i) {
        uint32_t existing = 0;
        int rc = kernel_dynlib_handle(getpid(), names[i], &existing);
        if (rc == 0 && existing != 0 && existing <= INT32_MAX) {
            modules[i] = (int)existing;
            printf("R3_RE_LOAD name=%s ownership=preexisting\n", names[i]);
        } else {
            int result = 0;
            modules[i] = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &result);
            owned[i] = modules[i] >= 0;
            printf("R3_RE_LOAD name=%s rc=0x%08x result=0x%08x\n", names[i], (unsigned)modules[i],
                   (unsigned)result);
            if (!owned[i] || result != 0) {
                ok = false;
                break;
            }
        }
    }
    if (ok)
        ok = export_process(getpid());
    for (int i = 1; i >= 0; --i) {
        if (!owned[i])
            continue;
        int result = 0;
        int rc = sceKernelStopUnloadModule(modules[i], 0, NULL, 0, NULL, &result);
        printf("R3_RE_UNLOAD name=%s rc=0x%08x result=0x%08x\n", names[i], (unsigned)rc,
               (unsigned)result);
        if (rc != 0 || result != 0) {
            ok = false;
            break;
        }
    }
    printf("R3_RE_RESULT selected_processes=1 output_bytes=%" PRIu64
           " directory=%s status=%s media_capability=unverified\n",
           bytes_written, directory, ok ? "exported" : "incomplete");
    return fflush(stdout) == EOF || ferror(stdout) || !ok ? 1 : 2;
}
#endif

int main(void)
{
    uint32_t firmware = kernel_get_fw_version();
    printf("R3_RE_CONTEXT firmware_raw=0x%08" PRIx32 " exporter_pid=%ld\n", firmware,
           (long)getpid());
    if ((firmware & UINT32_C(0xffff0000)) != firmware_layout.firmware)
        return 1;
    time_t now = time(NULL);
    if (now == (time_t)-1)
        return 1;
    deadline = now + WINDOW_SECONDS;
    struct sigaction action = {0};
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0)
        return 1;
    int length =
        snprintf(directory, sizeof(directory), "/data/ps5-dev/t041-remoteplay-%ld", (long)getpid());
    if (length < 0 || (size_t)length >= sizeof(directory) || mkdir(directory, 0700) != 0)
        return 1;
    puts("R3_RE_BEGIN max_output_bytes=134217728 max_nodes_per_process=512 "
         "planned_seconds=120 hard_kernel_io_deadline=unknown "
         "media_calls=none probe_kernel_writes=none stock_sdk_startup_writes=yes");
    if (fflush(stdout) == EOF)
        return 1;
#ifdef R3_RE_LOAD_RECORDING
    return export_loaded_recording();
#else
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    errno = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0) {
        printf("R3_RE_SYSCTL step=size_query errno=%d\n", errno);
        return 1;
    }
    printf("R3_RE_SYSCTL step=size_query size=%zu kinfo_proc_size=%zu\n", size,
           sizeof(struct kinfo_proc));
    if (size == 0 || size > MAX_PROCESSES_BYTES)
        return 1;
    struct kinfo_proc *processes = malloc(size);
    if (!processes)
        return 1;
    errno = 0;
    if (sysctl(mib, 4, processes, &size, NULL, 0) != 0) {
        printf("R3_RE_SYSCTL step=fill errno=%d\n", errno);
        free(processes);
        return 1;
    }
    /* Records are variable-size: walk by each entry's ki_structsize exactly as
     * the pinned SDK samples/ps/main.c does, never by sizeof(kinfo_proc). */
    size_t selected = 0, walked = 0, entries = 0;
    bool ok = true;
    while (walked + sizeof(struct kinfo_proc) <= size) {
        struct kinfo_proc *entry = (struct kinfo_proc *)((unsigned char *)processes + walked);
        int structsize = entry->ki_structsize;
        if (structsize < (int)sizeof(struct kinfo_proc) || structsize > 8192 ||
            walked + (size_t)structsize > size) {
            printf("R3_RE_SYSCTL step=walk invalid_structsize=%d walked=%zu\n", structsize, walked);
            free(processes);
            return 1;
        }
        ++entries;
        if (memchr(entry->ki_comm, '\0', sizeof(entry->ki_comm)) == NULL) {
            printf("R3_RE_SYSCTL step=walk unterminated_comm entry=%zu\n", entries);
            free(processes);
            return 1;
        }
        if (entries <= 5)
            printf("R3_RE_SYSCTL sample entry=%zu structsize=%d pid=%ld comm=%s\n", entries,
                   structsize, (long)entry->ki_pid, entry->ki_comm);
        const char *name = entry->ki_comm;
        if (selected_process(name)) {
            printf("R3_RE_TARGET pid=%ld name=%s\n", (long)entry->ki_pid, name);
            ++selected;
            if (!export_process(entry->ki_pid))
                ok = false;
        }
        walked += (size_t)structsize;
    }
    printf("R3_RE_SYSCTL step=walk entries=%zu walked=%zu size=%zu\n", entries, walked, size);
    free(processes);
    if (walked != size)
        return 1;
    printf("R3_RE_RESULT selected_processes=%zu output_bytes=%" PRIu64
           " directory=%s status=%s media_capability=unverified\n",
           selected, bytes_written, directory, ok && selected == 3 ? "exported" : "incomplete");
    return fflush(stdout) == EOF || ferror(stdout) || !ok ? 1 : 2;
#endif
}
#endif
