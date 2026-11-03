/* Nonshipping T050 diagnostic. Links the store and POSIX adapter with optional
 * tracing and the project-local target syscall error translation.
 * SDK a7685ce8 target sys/{stat,file}.h, fcntl.h, unistd.h; OpenSSL 3.5.9
 * public PEM/X509/EVP interfaces. No console services, patches or network I/O.
 * Stock payload CRT bootstrap still applies. All checks survive NDEBUG.
 */
#include "../../platform/system/storage_syscalls.h"
#include "settings/devices.h"
#include "settings/storage_diagnostic.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(SP5_STORAGE_DIAGNOSTICS) && !defined(T050_HOST)
#include <sys/syscall.h>
#endif
#include <unistd.h>

static _Noreturn void requirement_failed(int line)
{
    /* Fixed scalar diagnostic; the target libc does not provide Annex K fprintf_s.
     * Failure remains fatal even when the diagnostic stream cannot be written. */
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    (void)fprintf(stderr, "T050 fail line=%d errno=%d\n", line, errno);
    exit(1);
}

#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            requirement_failed(__LINE__);                                                          \
        }                                                                                          \
    } while (0)

static const uint8_t client[] = "T050 synthetic registry certificate; not TLS proof";
static const char *files[] = {"identity.bin", "settings.bin", "devices.bin"};
static const char *children[] = {"state",
                                 "corrupt-identity",
                                 "truncated-identity",
                                 "missing-identity",
                                 "corrupt-settings",
                                 "unreadable-registry",
                                 "identity-permissions",
                                 "identity-symlink",
                                 "identity-multilink",
                                 "directory-permissions"};
static const char *cleanup_files[] = {"identity.bin", "settings.bin", "devices.bin",
                                      "identity-decoy.bin"};
#ifdef SP5_STORAGE_DIAGNOSTICS
void sp5_storage_trace(const char *stage, int file, bool begin, int64_t result, sp5_reason reason,
                       int system_error, unsigned long crypto_error)
{
    static unsigned count;
    if (count < 256) {
        printf("T050 trace stage=%s event=%s file=%d result=%" PRId64
               " reason=%s errno=%d openssl=%lu\n",
               stage, begin ? "begin" : "end", file, result, sp5_reason_name(reason), system_error,
               crypto_error);
    } else if (count == 256) {
        puts("T050 trace_limit=256 truncated=1");
    }
    if (count <= 256)
        ++count;
}
#endif

#ifdef SP5_STORAGE_DIAGNOSTIC_TEST
#ifndef T050_HOST
#error "Fault injection is forbidden in target probe builds"
#endif
bool sp5_storage_fault(const char *stage)
{
    static bool used;
    static unsigned link_faults;
    const char *requested = getenv("T050_FAULT_STAGE");
    unsigned link_bit = strcmp(stage, "recovery.symlink_unsupported") == 0    ? 1U
                        : strcmp(stage, "recovery.hardlink_unsupported") == 0 ? 2U
                                                                              : 0U;
    if (requested != NULL && strcmp(requested, "recovery.links_unsupported") == 0 &&
        link_bit != 0 && !(link_faults & link_bit)) {
        link_faults |= link_bit;
        printf("T050 fault stage=%s injected=1\n", stage);
        errno = EIO;
        return true;
    }
    if (!used && requested != NULL && strcmp(requested, stage) == 0) {
        used = true;
        printf("T050 fault stage=%s injected=1\n", stage);
        errno = EIO;
        return true;
    }
    return false;
}
#endif
static sp5_bytes client_bytes(void)
{
    return (sp5_bytes){client, sizeof(client) - 1};
}

static void join(char out[512], const char *directory, const char *leaf)
{
    int n = snprintf(out, 512, "%s/%s", directory, leaf);
    REQUIRE(n > 0 && n < 512);
}

/* No arbitrary path, separators, traversal, empty suffix or unbounded run ID. */
static int valid_path(const char *path)
{
#ifdef T050_HOST
#ifdef __APPLE__
    const char *prefix = "/private/tmp/t050-";
#else
    const char *prefix = "/tmp/t050-";
#endif
#else
    const char *prefix = "/data/ps5-dev/t050-";
#endif
    size_t n = strlen(prefix);
    if (strncmp(path, prefix, n) != 0 || strlen(path + n) == 0 || strlen(path + n) > 64)
        return 0;
    for (const char *p = path + n; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '-'))
            return 0;
    return 1;
}

/* Resolve every directory component with NOFOLLOW, including the parent. */
static int directory_fd(const char *path)
{
    char copy[512];
    REQUIRE(strlen(path) < sizeof(copy) && path[0] == '/');
    strcpy(copy, path);
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    char *save = NULL;
    for (char *p = strtok_r(copy + 1, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        int next = sp5_storage_openat(fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
        REQUIRE(next >= 0);
        REQUIRE(close(fd) == 0);
        fd = next;
    }
    return fd;
}

#ifdef SP5_STORAGE_DIAGNOSTICS
enum { PREFLIGHT_FDS = 256 };
typedef struct {
    bool present[PREFLIGHT_FDS];
    bool valid;
} descriptor_inventory;
typedef struct {
    int64_t rax;
    unsigned char carry;
} raw_syscall_result;

/* Do not use the suspect openat wrapper to obtain the preflight's parent.
 * Open each absolute prefix with NOFOLLOW under the serialized-run contract. */
static int preflight_parent(const char *path)
{
    char prefix[512] = "/";
    size_t length = strlen(path);
    REQUIRE(length < sizeof(prefix) && path[0] == '/');
    int held = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    REQUIRE(held >= 0);
    for (size_t i = 1; i <= length; ++i) {
        prefix[i] = path[i];
        if (path[i] == '/' || path[i] == 0) {
            prefix[i] = 0;
            int next = open(prefix, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat info;
            REQUIRE(next >= 0 && fstat(next, &info) == 0 && S_ISDIR(info.st_mode));
            REQUIRE(close(held) == 0);
            held = next;
            prefix[i] = path[i];
        }
    }
    return held;
}

/* No descriptor is closed based solely on a nonnegative wrapper return. */
static descriptor_inventory descriptor_snapshot(void)
{
    descriptor_inventory inventory = {.valid = true};
    for (int fd = 0; fd < PREFLIGHT_FDS; ++fd) {
        errno = 0;
        int flags = fcntl(fd, F_GETFD);
        if (flags >= 0 && (flags & ~FD_CLOEXEC) == 0)
            inventory.present[fd] = true;
        else if (flags != -1 || errno != EBADF)
            inventory.valid = false;
    }
    return inventory;
}

#ifndef T050_HOST
_Static_assert(SYS_openat == 499 && SYS_fstatat == 493, "Reviewed SDK syscall numbers");
/* Diagnostic only: capture CF before any instruction can alter flags. This is
 * never an adapter replacement or a generic production syscall interface. */
__attribute__((noinline)) static raw_syscall_result
preflight_raw_syscall(long number, long first, const char *second, long third, long fourth)
{
    raw_syscall_result result;
    register long arg4 __asm__("r10") = fourth;
    __asm__ volatile("syscall\n\tsetc %1"
                     : "=a"(result.rax), "=qm"(result.carry)
                     : "a"(number), "D"(first), "S"(second), "d"(third), "r"(arg4)
                     : "rcx", "r11", "memory", "cc");
    return result;
}
#endif

static raw_syscall_result raw_missing(int directory, const char *relative, int flags,
                                      struct stat *info)
{
#ifdef T050_HOST
    int value = info != NULL ? fstatat(directory, relative, info, AT_SYMLINK_NOFOLLOW)
                             : openat(directory, relative, flags);
    raw_syscall_result result = {value < 0 ? errno : value, value < 0};
#ifdef SP5_STORAGE_DIAGNOSTIC_TEST
    if (info == NULL && sp5_storage_fault("preflight.raw_openat_cf"))
        result.carry = 0;
#endif
    return result;
#else
    return info != NULL ? preflight_raw_syscall(SYS_fstatat, directory, relative, (long)info,
                                                AT_SYMLINK_NOFOLLOW)
                        : preflight_raw_syscall(SYS_openat, directory, relative, flags, 0);
#endif
}

static bool preflight_open(int directory, const char *relative, const char *absolute,
                           unsigned variant)
{
    static const char *names[] = {"openat",
                                  "openat_without_nonblock",
                                  "absolute_open",
                                  "raw_openat",
                                  "raw_openat_without_nonblock",
                                  "fixed_openat",
                                  "fixed_openat_without_nonblock"};
    int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC;
    if (variant != 1 && variant != 4 && variant != 6)
        flags |= O_NONBLOCK;
    descriptor_inventory before = descriptor_snapshot();
    errno = 0;
    int64_t value;
    int carry = -1;
    if (variant == 3 || variant == 4) {
        raw_syscall_result raw = raw_missing(directory, relative, flags, NULL);
        value = raw.rax;
        carry = raw.carry;
    } else {
#ifdef SP5_STORAGE_DIAGNOSTIC_TEST
        if (variant == 0 && sp5_storage_fault("preflight.new_descriptor"))
            value = open("/dev/null", O_RDONLY | O_CLOEXEC);
        else
#endif
            value = SP5_STORAGE_OP(names[variant],
                                   variant >= 5 ? sp5_storage_openat(directory, relative, flags, 0)
                                   : variant == 2 ? open(absolute, flags)
                                                  : openat(directory, relative, flags),
                                   STDERR_FILENO);
    }
    int call_errno = errno;
    descriptor_inventory after = descriptor_snapshot();
    unsigned added = 0;
    bool lost = false;
    for (int fd = 0; fd < PREFLIGHT_FDS; ++fd) {
        added += !before.present[fd] && after.present[fd];
        lost |= before.present[fd] && !after.present[fd];
    }
    bool candidate = carry <= 0 && value >= 0 && value < PREFLIGHT_FDS;
    bool existing = candidate && before.present[value];
    struct stat info = {0};
    int stat_result = -2, stat_errno = 0;
    if (candidate) {
        errno = 0;
        stat_result = fstat((int)value, &info);
        stat_errno = errno;
    }
    bool owned = candidate && before.valid && after.valid && !existing && after.present[value] &&
                 added == 1 && !lost && stat_result == 0;
    int closed = -2, close_errno = 0;
    if (owned) {
        errno = 0;
        closed = close((int)value);
        close_errno = errno;
    }
    printf(
        "T050 preflight op=%s flags=%d rax=%" PRId64
        " cf=%d errno=%d inventory_valid=%d added=%u lost=%d existing=%d fstat=%d"
        " fstat_errno=%d type=%u mode=%u owned=%d close_called=%d close_result=%d close_errno=%d\n",
        names[variant], flags, value, carry, call_errno, before.valid && after.valid, added, lost,
        existing, stat_result, stat_errno, stat_result == 0 ? (unsigned)(info.st_mode & S_IFMT) : 0,
        stat_result == 0 ? (unsigned)(info.st_mode & 0777) : 0, owned, owned, closed, close_errno);
    /* Legacy/raw results are observations, never authorization. All operations
     * must preserve descriptor safety; only the fixed wrapper proves absence. */
    return before.valid && after.valid && added == 0 && !lost &&
           (variant < 5 || (value == -1 && call_errno == ENOENT));
}

/* Before mkdir, store load, settings/registry access or key generation. Only
 * inspect the existing parent and the same definitely absent identity path. */
static bool missing_file_preflight(const char *path)
{
    char parent[512], relative[512], absolute[512];
    REQUIRE(strlen(path) < sizeof(parent));
    strcpy(parent, path);
    char *leaf = strrchr(parent, '/');
    REQUIRE(leaf != NULL && leaf[1] != 0);
    *leaf++ = 0;
    join(relative, leaf, "state/identity.bin");
    join(absolute, path, "state/identity.bin");
    int directory = preflight_parent(parent);
    DIR *listing = fdopendir(dup(directory));
    REQUIRE(listing != NULL);
    bool absent = true;
    unsigned entries = 0;
    errno = 0;
    for (struct dirent *entry = readdir(listing); entry; entry = readdir(listing)) {
        if (++entries > 4096 || strcmp(entry->d_name, leaf) == 0) {
            absent = false;
            break;
        }
    }
    absent &= errno == 0;
    REQUIRE(closedir(listing) == 0);
    printf("T050 preflight root_absent=%d enumeration_bound=4096\n", absent);
    if (!absent) {
        REQUIRE(close(directory) == 0);
        return false;
    }
    struct stat info = {0};
    errno = 0;
    int result = SP5_STORAGE_OP("preflight.fstatat",
                                fstatat(directory, relative, &info, AT_SYMLINK_NOFOLLOW), -1);
    int call_errno = errno;
    printf("T050 preflight op=fstatat rax=%d cf=-1 errno=%d\n", result, call_errno);
    errno = 0;
    raw_syscall_result raw = raw_missing(directory, relative, 0, &info);
    printf("T050 preflight op=raw_fstatat rax=%" PRId64 " cf=%u errno=%d\n", raw.rax, raw.carry,
           errno);
    errno = 0;
    result =
        SP5_STORAGE_OP("preflight.fixed_fstatat",
                       sp5_storage_fstatat(directory, relative, &info, AT_SYMLINK_NOFOLLOW), -1);
    call_errno = errno;
    printf("T050 preflight op=fixed_fstatat rax=%d cf=-1 errno=%d\n", result, call_errno);
    bool passed = result == -1 && call_errno == ENOENT;
    for (unsigned variant = 0; variant < 7; ++variant)
        passed &= preflight_open(directory, relative, absolute, variant);
    REQUIRE(close(directory) == 0);
    return passed;
}
#endif

static void secure_directory(const char *path)
{
    int fd = directory_fd(path);
    struct stat st;
    REQUIRE(fstat(fd, &st) == 0 && st.st_uid == geteuid() && (st.st_mode & 0777) == 0700);
    REQUIRE(close(fd) == 0);
}

static void create_directory(const char *path)
{
    char parent[512];
    REQUIRE(strlen(path) < sizeof(parent));
    strcpy(parent, path);
    char *leaf = strrchr(parent, '/');
    REQUIRE(leaf != NULL && leaf[1] != 0);
    *leaf++ = 0;
    int fd = directory_fd(parent);
    REQUIRE(sp5_storage_mkdirat(fd, leaf, 0700) == 0); /* EEXIST always refuses. */
    REQUIRE(fsync(fd) == 0 && close(fd) == 0);
    secure_directory(path);
}

/* Inventory is complete before the first deletion. A file symlink is unlinked
 * itself; no file contents are opened. All directories stay open during cleanup.
 * Caller serializes runs; this is not a hostile concurrent-writer protocol. */
typedef enum { INVENTORY_FILES, INVENTORY_DIRECTORIES, INVENTORY_SINGLE_LINK_FILES } inventory_kind;
typedef struct {
    unsigned expected;
    unsigned alternative;
    inventory_kind kind;
} inventory_policy;

static unsigned inventory(int fd, const char *const *names, size_t count, inventory_policy policy)
{
    DIR *dir = fdopendir(dup(fd));
    REQUIRE(dir != NULL);
    unsigned seen = 0;
    errno = 0;
    for (struct dirent *entry = readdir(dir); entry; entry = readdir(dir)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        size_t index = 0;
        while (index < count && strcmp(entry->d_name, names[index]) != 0)
            ++index;
        REQUIRE(index < count && ((policy.expected | policy.alternative) & (1U << index)) &&
                !(seen & (1U << index)));
        struct stat st;
        REQUIRE(sp5_storage_fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                st.st_uid == geteuid());
        REQUIRE(policy.kind == INVENTORY_DIRECTORIES ? S_ISDIR(st.st_mode)
                : policy.kind == INVENTORY_SINGLE_LINK_FILES
                    ? S_ISREG(st.st_mode) && st.st_nlink == 1
                    : (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)));
        seen |= 1U << index;
        errno = 0;
    }
    REQUIRE(errno == 0 &&
            (seen == policy.expected || (policy.alternative != 0 && seen == policy.alternative)) &&
            closedir(dir) == 0);
    return seen;
}

static void same_directory(int parent, const char *name, int opened)
{
    struct stat entry, held;
    REQUIRE(fstat(opened, &held) == 0 &&
            sp5_storage_fstatat(parent, name, &entry, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISDIR(entry.st_mode) && entry.st_dev == held.st_dev && entry.st_ino == held.st_ino);
}

static void cleanup(const char *path, unsigned partial)
{
    if (partial != 0) {
        REQUIRE(partial == 4 || partial == 5);
#if defined(T050_HOST) && defined(__APPLE__)
        REQUIRE(strcmp(path, partial == 4 ? "/private/tmp/t050-20261031-codex4"
                                          : "/private/tmp/t050-20261031-codex5") == 0);
#elif defined(T050_HOST)
        REQUIRE(strcmp(path, partial == 4 ? "/tmp/t050-20261031-codex4"
                                          : "/tmp/t050-20261031-codex5") == 0);
#else
        REQUIRE(strcmp(path, partial == 4 ? "/data/ps5-dev/t050-20261031-codex4"
                                          : "/data/ps5-dev/t050-20261031-codex5") == 0);
#endif
    }
    char parent[512];
    REQUIRE(strlen(path) < sizeof(parent));
    strcpy(parent, path);
    char *leaf = strrchr(parent, '/');
    REQUIRE(leaf != NULL && leaf[1] != 0);
    *leaf++ = 0;
    int parent_fd = directory_fd(parent);
    int root_fd =
        sp5_storage_openat(parent_fd, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
    REQUIRE(root_fd >= 0);
    struct stat st;
    REQUIRE(fstat(root_fd, &st) == 0 && st.st_uid == geteuid() && (st.st_mode & 0777) == 0700);
    enum { CHILD_COUNT = sizeof(children) / sizeof(children[0]) };
    int child_fds[CHILD_COUNT];
    unsigned expected[CHILD_COUNT];
    size_t child_count = partial == 4 ? 8 : partial == 5 ? 9 : CHILD_COUNT;
    inventory(
        root_fd, children, CHILD_COUNT,
        (inventory_policy){.expected = (1U << child_count) - 1, .kind = INVENTORY_DIRECTORIES});
    for (size_t i = 0; i < child_count; ++i) {
        child_fds[i] = sp5_storage_openat(root_fd, children[i],
                                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
        REQUIRE(child_fds[i] >= 0 && fstat(child_fds[i], &st) == 0 && st.st_uid == geteuid() &&
                (st.st_mode & 0777) == 0700);
        expected[i] = i == 3 ? 6U : i == 7 || i == 8 ? 15U : 7U;
        if (partial == 4 && i == 7)
            expected[i] = 14U; /* settings + devices + decoy; identity absent. */
        if (partial == 5)
            expected[i] = i == 3 ? 6U : 7U;
        expected[i] = inventory(
            child_fds[i], cleanup_files, 4,
            (inventory_policy){.expected = expected[i],
                               .alternative = partial == 0 && (i == 7 || i == 8) ? 7U : 0U,
                               .kind = partial ? INVENTORY_SINGLE_LINK_FILES : INVENTORY_FILES});
        if (partial == 0 && i == 7) {
            REQUIRE(sp5_storage_fstatat(child_fds[i], "identity.bin", &st, AT_SYMLINK_NOFOLLOW) ==
                    0);
            REQUIRE(expected[i] == 15 ? S_ISLNK(st.st_mode)
                                      : S_ISREG(st.st_mode) && st.st_nlink == 1 &&
                                            st.st_uid == geteuid() && (st.st_mode & 0777) == 0600);
        }
        if (partial == 0 && i == 8) {
            REQUIRE(sp5_storage_fstatat(child_fds[i], "identity.bin", &st, AT_SYMLINK_NOFOLLOW) ==
                    0);
            REQUIRE(S_ISREG(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & 0777) == 0600 &&
                    st.st_nlink == (expected[i] == 15 ? 2 : 1));
            if (expected[i] == 15) {
                struct stat decoy;
                REQUIRE(sp5_storage_fstatat(child_fds[i], "identity-decoy.bin", &decoy,
                                            AT_SYMLINK_NOFOLLOW) == 0);
                REQUIRE(S_ISREG(decoy.st_mode) && decoy.st_uid == geteuid() &&
                        (decoy.st_mode & 0777) == 0600 && decoy.st_nlink == 2 &&
                        decoy.st_dev == st.st_dev && decoy.st_ino == st.st_ino);
            }
        }
    }
    for (size_t i = 0; i < child_count; ++i) {
        same_directory(root_fd, children[i], child_fds[i]);
        for (size_t j = 0; j < 4; ++j)
            if (expected[i] & (1U << j))
                REQUIRE(sp5_storage_unlinkat(child_fds[i], cleanup_files[j], 0) == 0);
        REQUIRE(fsync(child_fds[i]) == 0);
        same_directory(root_fd, children[i], child_fds[i]);
        REQUIRE(sp5_storage_unlinkat(root_fd, children[i], AT_REMOVEDIR) == 0 &&
                close(child_fds[i]) == 0);
    }
    REQUIRE(fsync(root_fd) == 0);
    same_directory(parent_fd, leaf, root_fd);
    REQUIRE(sp5_storage_unlinkat(parent_fd, leaf, AT_REMOVEDIR) == 0 && close(root_fd) == 0);
    REQUIRE(fsync(parent_fd) == 0 && close(parent_fd) == 0);
    printf("T050 mode=%s pid=%ld removed=1 pass=1\n",
           partial == 4   ? "cleanup-partial-codex4"
           : partial == 5 ? "cleanup-partial-codex5"
                          : "cleanup",
           (long)getpid());
}

static void audit_files(const char *path)
{
    secure_directory(path);
    DIR *dir = opendir(path);
    REQUIRE(dir != NULL);
    unsigned seen = 0;
    errno = 0;
    for (struct dirent *e = readdir(dir); e; e = readdir(dir)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        int index = -1;
        for (int i = 0; i < 3; ++i)
            if (strcmp(e->d_name, files[i]) == 0)
                index = i;
        REQUIRE(index >= 0); /* Includes all abandoned .new-* records. */
        struct stat st;
        REQUIRE(sp5_storage_fstatat(dirfd(dir), e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0);
        REQUIRE(S_ISREG(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & 0777) == 0600 &&
                st.st_nlink == 1);
        seen |= 1U << index;
        errno = 0;
    }
    REQUIRE(errno == 0 && seen == 7 && closedir(dir) == 0);
}

static void fingerprint(sp5_store *store, char out[65])
{
    sp5_bytes identity = sp5_store_identity(store);
    REQUIRE(identity.size > 0 && identity.size <= SP5_IDENTITY_CAPACITY);
    BIO *bio = BIO_new_mem_buf(identity.data, (int)identity.size);
    REQUIRE(bio != NULL);
    EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    REQUIRE(key != NULL && cert != NULL && X509_check_private_key(cert, key) == 1);
    EVP_PKEY *public_key = X509_get_pubkey(cert);
    REQUIRE(public_key != NULL && X509_verify(cert, public_key) == 1);
    uint8_t digest[32];
    unsigned size = 0;
    REQUIRE(X509_digest(cert, EVP_sha256(), digest, &size) == 1 && size == 32);
    for (size_t i = 0; i < size; ++i)
        REQUIRE(snprintf(out + i * 2, 3, "%02x", digest[i]) == 2);
    EVP_PKEY_free(public_key);
    X509_free(cert);
    EVP_PKEY_free(key);
    BIO_free(bio);
}

static sp5_store *open_store(const char *path, sp5_posix_storage **disk)
{
    *disk = sp5_posix_storage_open(path);
    REQUIRE(*disk != NULL);
    sp5_storage_ops ops = sp5_posix_storage_operations(*disk);
    sp5_store *store = sp5_store_create(&ops);
    REQUIRE(store != NULL);
    return store;
}

static void close_store(sp5_store *store, sp5_posix_storage *disk)
{
    sp5_store_destroy(store);
    REQUIRE(sp5_posix_storage_close(disk).status == SP5_OK);
}

static void clone_records(sp5_posix_storage *source, const char *destination)
{
    create_directory(destination);
    sp5_posix_storage *disk = sp5_posix_storage_open(destination);
    REQUIRE(disk != NULL);
    sp5_storage_ops from = sp5_posix_storage_operations(source);
    sp5_storage_ops to = sp5_posix_storage_operations(disk);
    uint8_t bytes[40000];
    for (int i = 0; i < SP5_STORE_FILE_COUNT; ++i) {
        sp5_buffer buffer = {.data = bytes, .capacity = sizeof(bytes)};
        REQUIRE(from.read(from.context, (sp5_store_file)i, &buffer).status == SP5_OK);
        REQUIRE(to.replace(to.context, (sp5_store_file)i, (sp5_bytes){bytes, buffer.size}).status ==
                SP5_OK);
        OPENSSL_cleanse(bytes, sizeof(bytes));
    }
    REQUIRE(sp5_posix_storage_close(disk).status == SP5_OK);
}

static void recovery(const char *root, sp5_posix_storage *source, const char *expected)
{
    const char *const *cases = children + 1;
    for (size_t i = 0; i < sizeof(children) / sizeof(children[0]) - 1; ++i) {
        char path[512], file[512], extra[512];
        join(path, root, cases[i]);
        clone_records(source, path);
        join(file, path, files[i == 3 ? 1 : i == 4 ? 2 : 0]);
        if (i == 0 || i == 3) {
            int fd = open(file, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
            REQUIRE(fd >= 0 && pwrite(fd, "!", 1, 0) == 1 && fsync(fd) == 0 && close(fd) == 0);
        } else if (i == 1) {
            REQUIRE(truncate(file, 8) == 0);
        } else if (i == 2) {
            REQUIRE(unlink(file) == 0);
        } else if (i == 4 || i == 5) {
            REQUIRE(chmod(file, 0000) == 0);
        } else if (i == 6 || i == 7) {
            join(extra, path, "identity-decoy.bin");
            if (i == 6) {
                REQUIRE(rename(file, extra) == 0);
                bool allow_unsupported = false;
#ifdef __PROSPERO__
                _Static_assert(ENOTSUP == 45, "Reviewed target unsupported errno");
                allow_unsupported = true;
#endif
                int linked;
#ifdef SP5_STORAGE_DIAGNOSTIC_TEST
                if (sp5_storage_fault("recovery.symlink_unsupported")) {
                    allow_unsupported = true;
                    errno = ENOTSUP;
                    linked = -1;
                } else
#endif
                    linked = SP5_STORAGE_OP("recovery.symlink",
                                            sp5_storage_symlink("identity-decoy.bin", file), -1);
                int link_errno = errno;
                if (linked != 0 && allow_unsupported && link_errno == ENOTSUP) {
                    int directory = directory_fd(path);
                    inventory(
                        directory, cleanup_files, 4,
                        (inventory_policy){.expected = 14U, .kind = INVENTORY_SINGLE_LINK_FILES});
                    REQUIRE(sp5_storage_renameat(directory, "identity-decoy.bin", directory,
                                                 "identity.bin") == 0);
                    REQUIRE(fsync(directory) == 0 && close(directory) == 0);
                    audit_files(path);
                    sp5_posix_storage *restored_disk;
                    sp5_store *restored = open_store(path, &restored_disk);
                    REQUIRE(sp5_store_load(restored).status == SP5_OK);
                    char restored_fingerprint[65];
                    fingerprint(restored, restored_fingerprint);
                    REQUIRE(strcmp(expected, restored_fingerprint) == 0 &&
                            sp5_store_read(restored).paired_count == 1 &&
                            sp5_store_authorized(restored, client_bytes()));
                    close_store(restored, restored_disk);
                    printf("T050 recovery=identity-symlink unsupported=1 errno=%d restored=1 "
                           "skipped=1\n",
                           link_errno);
                    continue;
                }
                REQUIRE(linked == 0);
            } else {
                bool allow_unsupported = false;
#ifdef __PROSPERO__
                allow_unsupported = true;
#endif
                int linked;
#ifdef SP5_STORAGE_DIAGNOSTIC_TEST
                if (sp5_storage_fault("recovery.hardlink_unsupported")) {
                    allow_unsupported = true;
                    errno = ENOTSUP;
                    linked = -1;
                } else
#endif
                    linked = SP5_STORAGE_OP("recovery.hardlink", sp5_storage_link(file, extra), -1);
                int link_errno = errno;
                if (linked != 0 && allow_unsupported && link_errno == ENOTSUP) {
                    audit_files(path);
                    int directory = directory_fd(path);
                    REQUIRE(fsync(directory) == 0 && close(directory) == 0);
                    sp5_posix_storage *unchanged_disk;
                    sp5_store *unchanged = open_store(path, &unchanged_disk);
                    REQUIRE(sp5_store_load(unchanged).status == SP5_OK);
                    char unchanged_fingerprint[65];
                    fingerprint(unchanged, unchanged_fingerprint);
                    REQUIRE(strcmp(expected, unchanged_fingerprint) == 0 &&
                            sp5_store_read(unchanged).paired_count == 1 &&
                            sp5_store_authorized(unchanged, client_bytes()));
                    close_store(unchanged, unchanged_disk);
                    printf("T050 recovery=identity-multilink unsupported=1 errno=%d secure=1 "
                           "skipped=1\n",
                           link_errno);
                    continue;
                }
                REQUIRE(linked == 0);
            }
        } else {
            REQUIRE(chmod(path, 0755) == 0);
            REQUIRE(sp5_posix_storage_open(path) == NULL);
            REQUIRE(chmod(path, 0700) == 0);
            printf("T050 recovery=%s refused=1\n", cases[i]);
            continue;
        }
        sp5_posix_storage *disk;
        sp5_store *store = open_store(path, &disk);
        sp5_result result = sp5_store_load(store);
        sp5_store_view view = sp5_store_read(store);
        REQUIRE(view.diagnostic != SP5_REASON_NONE && strlen(sp5_store_explanation(store)) > 0);
        if (i == 3 || i == 4) {
            REQUIRE(result.status == SP5_OK && view.identity_valid);
            char actual[65];
            fingerprint(store, actual);
            REQUIRE(strcmp(expected, actual) == 0);
            if (i == 3) {
                sp5_settings defaults = sp5_settings_defaults(),
                             settings = sp5_store_settings(store);
                REQUIRE(view.settings_recovered && !settings.metrics_enabled &&
                        strcmp(defaults.host_name, settings.host_name) == 0);
                REQUIRE(sp5_store_authorized(store, client_bytes()));
            } else {
                REQUIRE(view.repairing_required && view.paired_count == 0 &&
                        !sp5_store_authorized(store, client_bytes()));
            }
        } else {
            REQUIRE(result.status != SP5_OK && !view.identity_valid && view.repairing_required);
            REQUIRE(sp5_store_identity(store).size == 0 && view.paired_count == 0);
            REQUIRE(!sp5_store_authorized(store, client_bytes()) &&
                    sp5_store_pair(store, client_bytes(), 1).status != SP5_OK);
            if (i == 2) {
                struct stat st;
                REQUIRE(lstat(file, &st) != 0 && errno == ENOENT);
            }
        }
        printf("T050 recovery=%s identity=%d paired=%zu diagnostic=%s explanation=%s\n", cases[i],
               view.identity_valid, view.paired_count, sp5_reason_name(view.diagnostic),
               sp5_store_explanation(store));
        close_store(store, disk);
    }
}

int main(int argc, char **argv)
{
    REQUIRE(setvbuf(stdout, NULL, _IONBF, 0) == 0);
#ifdef T050_DIRECTORY
    (void)argc;
    (void)argv;
    const char *mode = T050_MODE, *path = T050_DIRECTORY;
#ifdef T050_FINGERPRINT
    const char *expected = T050_FINGERPRINT;
#else
    const char *expected = "";
#endif
#else
    REQUIRE(argc == 3 || argc == 4);
    const char *mode = argv[1], *path = argv[2], *expected = argc == 4 ? argv[3] : "";
#endif
    REQUIRE(valid_path(path));
    unsigned partial = strcmp(mode, "cleanup-partial-codex4") == 0   ? 4
                       : strcmp(mode, "cleanup-partial-codex5") == 0 ? 5
                                                                     : 0;
    if (strcmp(mode, "cleanup") == 0 || partial != 0) {
        REQUIRE(expected[0] == 0);
        cleanup(path, partial);
        return 0;
    }
    int init = strcmp(mode, "init") == 0;
    REQUIRE(init || strcmp(mode, "verify") == 0);
    REQUIRE(init ? expected[0] == 0 : strlen(expected) == 64);
    if (!init)
        for (size_t i = 0; i < 64; ++i)
            REQUIRE((expected[i] >= '0' && expected[i] <= '9') ||
                    (expected[i] >= 'a' && expected[i] <= 'f'));
#ifdef SP5_STORAGE_DIAGNOSTICS
    if (init) {
        bool passed = missing_file_preflight(path);
        printf("T050 preflight verdict=%s paths_created=0 store_loaded=0\n",
               passed ? "pass" : "fail");
        REQUIRE(passed);
    }
#endif
    umask(0077);
    if (init)
        create_directory(path);
    else
        secure_directory(path);
    char state[512];
    join(state, path, "state");
    if (init)
        create_directory(state);
    else
        audit_files(state); /* Never bootstrap verify. */
    sp5_posix_storage *disk;
    sp5_store *store = open_store(state, &disk);
    sp5_result loaded = sp5_store_load(store);
    printf("T050 load stage=initial status=%d reason=%s\n", loaded.status,
           sp5_reason_name(loaded.reason));
    REQUIRE(loaded.status == SP5_OK);
    char actual[65];
    fingerprint(store, actual);
    if (!init)
        REQUIRE(strcmp(actual, expected) == 0);
    sp5_store_view view = sp5_store_read(store);
    REQUIRE(view.identity_valid && !view.repairing_required && !view.settings_recovered);
    sp5_settings settings = sp5_store_settings(store);
    if (init) {
        REQUIRE(view.paired_count == 0);
        strcpy(settings.host_name, "T050 stored name");
        settings.metrics_enabled = true;
        REQUIRE(sp5_store_save_settings(store, &settings).status == SP5_OK);
        REQUIRE(sp5_store_pair(store, client_bytes(), 1793404800000000ULL).status == SP5_OK);
    } else {
        REQUIRE(strcmp(settings.host_name, "T050 stored name") == 0 && settings.metrics_enabled);
        REQUIRE(view.paired_count == 1 && sp5_store_authorized(store, client_bytes()));
        strcpy(settings.host_name, "T050 replacement name");
        settings.metrics_enabled = false;
        REQUIRE(sp5_store_save_settings(store, &settings).status == SP5_OK);
    }
    REQUIRE(sp5_store_read(store).paired_count == 1 && sp5_store_authorized(store, client_bytes()));
    audit_files(state);
    if (init)
        recovery(path, disk, actual);
    close_store(store, disk);
    /* Reopen after the replacement, in addition to the caller's fresh process. */
    store = open_store(state, &disk);
    REQUIRE(sp5_store_load(store).status == SP5_OK);
    settings = sp5_store_settings(store);
    REQUIRE(settings.metrics_enabled == (init != 0) &&
            strcmp(settings.host_name, init ? "T050 stored name" : "T050 replacement name") == 0);
    char reopened[65];
    fingerprint(store, reopened);
    REQUIRE(strcmp(actual, reopened) == 0 && sp5_store_authorized(store, client_bytes()));
    close_store(store, disk);
    audit_files(state);
    printf("T050 mode=%s pid=%ld euid=%ld sha256=%s paired=1 metrics=%d pass=1\n", mode,
           (long)getpid(), (long)geteuid(), actual, init);
    return 0;
}
