/* Non-shipping probe: lists every thread on the console with its process,
 * run state and kernel wait message, to locate a hung payload's blocked call.
 * Read-only (sysctl kern.proc.all with KERN_PROC_INC_THREAD).
 *
 *   -DPROC_THREADS_MATCH="text"  only processes whose command contains text
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>

int main(void)
{
    /* Firmware support varies; use the first listing the kernel accepts. */
    static const int variants[] = {KERN_PROC_PROC | KERN_PROC_INC_THREAD,
                                   KERN_PROC_ALL | KERN_PROC_INC_THREAD, KERN_PROC_PROC,
                                   KERN_PROC_ALL};
    int mib[3] = {CTL_KERN, KERN_PROC, 0};
    size_t size = 0;
    size_t chosen = 0;
    for (; chosen < sizeof(variants) / sizeof(variants[0]); ++chosen) {
        mib[2] = variants[chosen];
        size = 0;
        if (sysctl(mib, 3, NULL, &size, NULL, 0) == 0 && size != 0)
            break;
        printf("PROC_THREADS variant=0x%x unavailable\n", variants[chosen]);
    }
    if (chosen == sizeof(variants) / sizeof(variants[0]))
        return 1;
    printf("PROC_THREADS variant=0x%x\n", mib[2]);
    size += size / 4;
    struct kinfo_proc *procs = malloc(size);
    if (procs == NULL || sysctl(mib, 3, procs, &size, NULL, 0) != 0) {
        perror("PROC_THREADS sysctl");
        return 1;
    }
    /* 13.60 reports a larger record than the SDK header (Sony appends
     * fields); step by the kernel's size and read the FreeBSD prefix. */
    int stride = size >= sizeof(int) ? procs[0].ki_structsize : 0;
    if (stride < (int)sizeof(struct kinfo_proc)) {
        printf("PROC_THREADS record %d bytes is smaller than the header %zu\n", stride,
               sizeof(struct kinfo_proc));
        return 2;
    }
    size_t count = size / (size_t)stride;
    printf("PROC_THREADS entries=%zu stride=%d header=%zu\n", count, stride,
           sizeof(struct kinfo_proc));
    for (size_t i = 0; i < count; ++i) {
        const struct kinfo_proc *p =
            (const struct kinfo_proc *)(const void *)((const char *)procs + i * (size_t)stride);
#ifdef PROC_THREADS_MATCH
        if (strstr(p->ki_comm, PROC_THREADS_MATCH) == NULL)
            continue;
#endif
        printf("PROC_THREADS pid=%d tid=%d comm=%s td=%s stat=%d wmesg=%s threads=%d\n",
               (int)p->ki_pid, (int)p->ki_tid, p->ki_comm, p->ki_tdname, (int)p->ki_stat,
               p->ki_wmesg, (int)p->ki_numthreads);
#ifdef PROC_THREADS_MATCH
        /* Per-process thread list, where the firmware allows it. */
        int tmib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID | KERN_PROC_INC_THREAD, p->ki_pid};
        size_t tsize = 0;
        if (sysctl(tmib, 4, NULL, &tsize, NULL, 0) != 0 || tsize == 0) {
            printf("PROC_THREADS pid=%d threads unavailable\n", (int)p->ki_pid);
            continue;
        }
        tsize += tsize / 2;
        char *threads = malloc(tsize);
        if (threads != NULL && sysctl(tmib, 4, threads, &tsize, NULL, 0) == 0) {
            for (size_t t = 0; t + (size_t)stride <= tsize; t += (size_t)stride) {
                const struct kinfo_proc *td =
                    (const struct kinfo_proc *)(const void *)(threads + t);
                printf("PROC_THREADS   tid=%d td=%s stat=%d wmesg=%s\n", (int)td->ki_tid,
                       td->ki_tdname, (int)td->ki_stat, td->ki_wmesg);
            }
        }
        free(threads);
#endif
    }
    free(procs);
    return 0;
}
