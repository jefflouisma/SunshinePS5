#include "../../platform/system/storage_syscalls.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

static _Noreturn void check_failed(int line)
{
    /* Fixed scalar diagnostic; the target libc does not provide Annex K fprintf_s.
     * Failure remains fatal even when the diagnostic stream cannot be written. */
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    (void)fprintf(stderr, "storage syscall check failed line=%d\n", line);
    exit(1);
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            check_failed(__LINE__);                                                                \
        }                                                                                          \
    } while (0)

static atomic_uint ready;
static atomic_bool release_threads;
typedef struct {
    int error;
    bool passed;
} thread_case;

static void *translation_thread(void *opaque)
{
    thread_case *test = opaque;
    int64_t result = sp5_storage_syscall_result(test->error, 1);
    atomic_fetch_add_explicit(&ready, 1, memory_order_release);
    while (!atomic_load_explicit(&release_threads, memory_order_acquire)) {
    }
    test->passed = result == -1 && errno == test->error;
    return NULL;
}

int main(void)
{
    const int errors[] = {EPERM, ENOENT, EBADF, EINTR, EACCES, EEXIST, EXDEV, ENOSPC, EMFILE};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        errno = 0;
        CHECK(sp5_storage_syscall_result(errors[i], 1) == -1 && errno == errors[i]);
    }
    const int64_t successes[] = {0, 2, 19, INT_MAX};
    for (size_t i = 0; i < sizeof(successes) / sizeof(successes[0]); ++i) {
        errno = EBUSY;
        CHECK(sp5_storage_syscall_result(successes[i], 0) == successes[i] && errno == EBUSY);
    }
    const int64_t malformed_errors[] = {0, -1, INT64_MAX};
    for (size_t i = 0; i < sizeof(malformed_errors) / sizeof(malformed_errors[0]); ++i) {
        errno = 0;
        CHECK(sp5_storage_syscall_result(malformed_errors[i], 1) == -1 && errno == EIO);
    }
    thread_case cases[] = {{ENOENT, false}, {EACCES, false}};
    pthread_t threads[2];
    atomic_init(&ready, 0);
    atomic_init(&release_threads, false);
    for (unsigned i = 0; i < 2; ++i)
        CHECK(pthread_create(&threads[i], NULL, translation_thread, &cases[i]) == 0);
    while (atomic_load_explicit(&ready, memory_order_acquire) != 2) {
    }
    errno = EBUSY;
    atomic_store_explicit(&release_threads, true, memory_order_release);
    for (unsigned i = 0; i < 2; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0 && cases[i].passed);
    CHECK(errno == EBUSY);
    puts("T050_SYSCALL carry_errors=9 carry_clear=4 malformed=3 thread_local_errno=1 pass=1");
    return 0;
}
