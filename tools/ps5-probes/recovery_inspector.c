/* Read-only T050 observation core; launcher and full pinned SDK CRT are separate. */
#include "../../platform/system/storage_syscalls.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef RECOVERY_INSPECTOR_HOST
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec
#include <CommonCrypto/CommonDigest.h>
#define SHA256_CTX CC_SHA256_CTX
#define SHA256_Init CC_SHA256_Init
#define SHA256_Update CC_SHA256_Update
#define SHA256_Final CC_SHA256_Final
#else
#include <openssl/sha.h>
#endif

#define ROOT "/data/ps5-dev/t050-20261031-codex6"
#define LIMIT 128
#define MAX_BYTES (1024 * 1024)
#define MAX_TOTAL_BYTES (4 * MAX_BYTES)
struct observation {
    char path[256];
    struct stat st;
    char hash[65];
};
struct inspection {
    struct observation entries[LIMIT];
    size_t count, total_bytes;
};

static int same(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode &&
           a->st_uid == b->st_uid && a->st_gid == b->st_gid && a->st_nlink == b->st_nlink &&
           a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
static int safe_name(const char *s)
{
    if (!*s || !strcmp(s, ".") || !strcmp(s, ".."))
        return 0;
    for (; *s; ++s)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              *s == '.' || *s == '-' || *s == '_'))
            return 0;
    return 1;
}
static int walk(struct inspection *out, int fd, const char *path, unsigned depth)
{
    struct stat before, after;
    if (depth > 3 || out->count == LIMIT || fstat(fd, &before))
        return -1;
    if (out->count && before.st_dev != out->entries[0].st.st_dev)
        return -1;
    if (!S_ISDIR(before.st_mode) && !S_ISREG(before.st_mode))
        return -1;
    if (S_ISREG(before.st_mode) &&
        (before.st_nlink != 1 || before.st_size < 0 || before.st_size > MAX_BYTES))
        return -1;
    for (size_t i = 0; i < out->count; ++i)
        if (out->entries[i].st.st_dev == before.st_dev &&
            out->entries[i].st.st_ino == before.st_ino)
            return -1;
    struct observation *item = &out->entries[out->count++];
    if (snprintf(item->path, sizeof(item->path), "%s", path) >= (int)sizeof(item->path))
        return -1;
    item->st = before;
    if (S_ISREG(before.st_mode)) {
        SHA256_CTX sha;
        unsigned char buffer[4096], digest[32];
        size_t total = 0;
        SHA256_Init(&sha);
        for (;;) {
            ssize_t n = read(fd, buffer, sizeof(buffer));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (!n)
                break;
            total += (size_t)n;
            out->total_bytes += (size_t)n;
            if (total > MAX_BYTES || out->total_bytes > MAX_TOTAL_BYTES)
                return -1;
            SHA256_Update(&sha, buffer, (unsigned)n);
        }
        if (total != (size_t)before.st_size)
            return -1;
        SHA256_Final(digest, &sha);
        for (unsigned i = 0; i < 32; ++i)
            snprintf(item->hash + 2 * i, 3, "%02x", digest[i]);
    } else {
        int copy = dup(fd);
        if (copy < 0)
            return -1;
        DIR *dir = fdopendir(copy);
        if (!dir) {
            close(copy);
            return -1;
        }
        int result = 0;
        for (;;) {
            errno = 0;
            struct dirent *entry = readdir(dir);
            if (!entry) {
                if (errno)
                    result = -1;
                break;
            }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            struct stat named, opened;
            char child[256];
            int n = snprintf(child, sizeof(child), "%s%s%s", strcmp(path, ".") ? path : "",
                             strcmp(path, ".") ? "/" : "", entry->d_name);
            if (!safe_name(entry->d_name) || n < 0 || n >= (int)sizeof(child) ||
                sp5_storage_fstatat(fd, entry->d_name, &named, AT_SYMLINK_NOFOLLOW) ||
                (!S_ISREG(named.st_mode) && !S_ISDIR(named.st_mode))) {
                result = -1;
                break;
            }
            int next = sp5_storage_openat(
                fd, entry->d_name,
                O_RDONLY | O_NOFOLLOW | O_NONBLOCK | (S_ISDIR(named.st_mode) ? O_DIRECTORY : 0), 0);
            if (next < 0) {
                result = -1;
                break;
            }
            result = fstat(next, &opened) || !same(&named, &opened)
                         ? -1
                         : walk(out, next, child, depth + 1);
            if (sp5_storage_fstatat(fd, entry->d_name, &opened, AT_SYMLINK_NOFOLLOW) ||
                !same(&named, &opened))
                result = -1;
            if (close(next))
                result = -1;
            if (result)
                break;
        }
        if (closedir(dir))
            result = -1;
        if (result)
            return -1;
    }
    return fstat(fd, &after) || !same(&before, &after) ? -1 : 0;
}
/* Keep every path binding alive through traversal and final revalidation. */
struct root_chain {
    int fds[32];
    struct stat identities[32];
    char names[32][256];
    size_t count;
};
static int identity(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode &&
           a->st_uid == b->st_uid && a->st_gid == b->st_gid;
}
static int close_chain(struct root_chain *chain)
{
    int result = 0;
    /* Consume ownership even on errors; never retry an uncertain close. */
    for (size_t i = 0; i < chain->count; ++i) {
        int fd = chain->fds[i];
        chain->fds[i] = -1;
        if (close(fd))
            result = -1;
    }
    chain->count = 0;
    return result;
}
static int open_chain(const char *path, struct root_chain *chain)
{
    char copy[512];
    if (*path != '/' || strlen(path) >= sizeof(copy))
        return -1;
    memcpy(copy, path + 1, strlen(path));
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    chain->fds[chain->count++] = fd;
    if (fstat(fd, &chain->identities[0]))
        return -1;
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!safe_name(part) || strlen(part) >= sizeof(chain->names[0]) || chain->count == 32)
            return -1;
        struct stat named;
        if (sp5_storage_fstatat(fd, part, &named, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(named.st_mode))
            return -1;
        int next = sp5_storage_openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
        if (next < 0)
            return -1;
        size_t index = chain->count++;
        chain->fds[index] = next;
        memcpy(chain->names[index], part, strlen(part) + 1);
        if (fstat(next, &chain->identities[index]) || !identity(&named, &chain->identities[index]))
            return -1;
        fd = next;
    }
    return 0;
}
static int validate_chain(const struct root_chain *chain)
{
    for (size_t i = 0; i < chain->count; ++i) {
        struct stat current;
        if (fstat(chain->fds[i], &current) || !identity(&current, &chain->identities[i]))
            return -1;
        if (i && (sp5_storage_fstatat(chain->fds[i - 1], chain->names[i], &current,
                                      AT_SYMLINK_NOFOLLOW) ||
                  !identity(&current, &chain->identities[i])))
            return -1;
    }
    return 0;
}
static int collect(const char *path, struct inspection *out)
{
    struct root_chain chain = {0};
    int result = open_chain(path, &chain);
    if (!result)
        result = walk(out, chain.fds[chain.count - 1], ".", 0);
    if (!result)
        result = validate_chain(&chain);
    if (close_chain(&chain))
        result = -1;
    return result;
}
static int inspect(const char *path)
{
    struct inspection out = {0};
    struct timeval boot = {0};
    size_t length = sizeof(boot);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    int boot_ok = !sysctl(mib, 2, &boot, &length, NULL, 0) && length == sizeof(boot) &&
                  boot.tv_sec > 0 && boot.tv_usec >= 0 && boot.tv_usec < 1000000;
    int ok = !collect(path, &out);
    if (!ok) {
        puts("{\"complete\":false,\"error\":\"inventory_refused\"}");
        return 1;
    }
    printf("{\"schema_version\":1,\"complete\":true,\"no_follow\":true,\"atomic_snapshot\":false,"
           "\"firmware_validated\":false,\"boot_time\":");
    if (boot_ok)
        printf("{\"seconds\":%jd,\"microseconds\":%jd,\"source\":\"CTL_KERN/KERN_BOOTTIME\"}",
               (intmax_t)boot.tv_sec, (intmax_t)boot.tv_usec);
    else
        printf("null");
    printf(",\"process_start_identities\":null,\"no_active_game\":null,\"no_writer\":null,"
           "\"entries\":[");
    for (size_t i = 0; i < out.count; ++i) {
        const struct observation *p = &out.entries[i];
        printf("%s{\"path\":\"%s\",\"kind\":\"%s\",\"device\":%ju,\"inode\":%ju,\"uid\":%ju,"
               "\"mode\":%u,\"links\":%ju,\"size\":%jd,\"sha256\":",
               i ? "," : "", p->path, S_ISDIR(p->st.st_mode) ? "directory" : "file",
               (uintmax_t)p->st.st_dev, (uintmax_t)p->st.st_ino, (uintmax_t)p->st.st_uid,
               (unsigned)(p->st.st_mode & 07777), (uintmax_t)p->st.st_nlink,
               (intmax_t)p->st.st_size);
        if (*p->hash)
            printf("\"%s\"}", p->hash);
        else
            printf("null}");
    }
    puts("]}");
    return ferror(stdout) ? 1 : 0;
}
#ifdef RECOVERY_INSPECTOR_HOST
int main(int argc, char **argv)
{
    return argc == 2 ? inspect(argv[1]) : 64;
}
#else
/*
 * The source-pinned launcher calls this after its exact-version gate. Its
 * reviewed SDK CRT performs process-local credential/syscall bootstrap;
 * deployability and hardware behavior still require separate review.
 */
int recovery_inspector_observe(void)
{
    return inspect(ROOT);
}
#endif
