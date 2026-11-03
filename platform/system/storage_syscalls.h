#ifndef SUNSHINE_STORAGE_SYSCALLS_H
#define SUNSHINE_STORAGE_SYSCALLS_H

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* Host-testable translation of the target ABI. Carry is authoritative: a
 * positive success (including fd 2) must never be mistaken for an errno. */
static inline int64_t sp5_storage_syscall_result(int64_t value, bool carry)
{
    if (carry) {
        errno = value > 0 && value <= INT_MAX ? (int)value : EIO;
        return -1;
    }
    return value;
}

#ifdef __PROSPERO__
#include <sys/syscall.h>
_Static_assert(sizeof(long) == 8, "Reviewed x86-64 syscall ABI");
_Static_assert(SYS_openat == 499 && SYS_fstatat == 493 && SYS_renameat == 501 &&
                   SYS_unlinkat == 503 && SYS_mkdirat == 496 && SYS_link == 9 && SYS_symlink == 57,
               "Reviewed public SDK syscall constants");

/* SDK a7685ce8 stubs expose raw RAX/CF. Keep this project-local and never
 * override SDK/libc symbols. Capture CF before C or another instruction can
 * alter it. errno expands to the platform's thread-local __error() location. */
__attribute__((noinline)) static int64_t
sp5_storage_kernel_call4(long number, long first, long second, long third, long fourth)
{
    int64_t value;
    unsigned char carry;
    register long arg4 __asm__("r10") = fourth;
    __asm__ volatile("syscall\n\tsetc %1"
                     : "=a"(value), "=qm"(carry)
                     : "a"(number), "D"(first), "S"(second), "d"(third), "r"(arg4)
                     : "rcx", "r11", "cc", "memory");
    return sp5_storage_syscall_result(value, carry);
}
#endif

static inline int sp5_storage_openat(int directory, const char *path, int flags, mode_t mode)
{
#ifdef __PROSPERO__
    return (int)sp5_storage_kernel_call4(SYS_openat, directory, (long)path, flags, mode);
#else
    return openat(directory, path, flags, mode);
#endif
}

static inline int sp5_storage_fstatat(int directory, const char *path, struct stat *info, int flags)
{
#ifdef __PROSPERO__
    return (int)sp5_storage_kernel_call4(SYS_fstatat, directory, (long)path, (long)info, flags);
#else
    return fstatat(directory, path, info, flags);
#endif
}

static inline int sp5_storage_renameat(int from_directory, const char *from, int to_directory,
                                       const char *to)
{
#ifdef __PROSPERO__
    return (int)sp5_storage_kernel_call4(SYS_renameat, from_directory, (long)from, to_directory,
                                         (long)to);
#else
    return renameat(from_directory, from, to_directory, to);
#endif
}

static inline int sp5_storage_unlinkat(int directory, const char *path, int flags)
{
#ifdef __PROSPERO__
    return (int)sp5_storage_kernel_call4(SYS_unlinkat, directory, (long)path, flags, 0);
#else
    return unlinkat(directory, path, flags);
#endif
}

/* Probe fixture setup also uses these raw SDK stubs. No production adapter
 * directory creation, link creation or arbitrary syscall API is introduced. */
static inline int sp5_storage_mkdirat(int directory, const char *path, mode_t mode)
{
#ifdef __PROSPERO__
    return (int)sp5_storage_kernel_call4(SYS_mkdirat, directory, (long)path, mode, 0);
#else
    return mkdirat(directory, path, mode);
#endif
}

static inline int sp5_storage_link(const char *from, const char *to)
{
#ifdef __PROSPERO__
    return (int)sp5_storage_kernel_call4(SYS_link, (long)from, (long)to, 0, 0);
#else
    return link(from, to);
#endif
}

static inline int sp5_storage_symlink(const char *target, const char *path)
{
#ifdef __PROSPERO__
    return (int)sp5_storage_kernel_call4(SYS_symlink, (long)target, (long)path, 0, 0);
#else
    return symlink(target, path);
#endif
}

#endif
