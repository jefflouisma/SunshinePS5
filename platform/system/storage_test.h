#ifndef SUNSHINE_POSIX_STORAGE_TEST_H
#define SUNSHINE_POSIX_STORAGE_TEST_H

#include "settings/host.h"

#include <sys/types.h>

#ifdef SP5_STORAGE_TEST
/* Per-instance replacement syscall seam, compiled only into the test binary.
 * Callbacks follow syscall return/errno semantics. A close failure may occur
 * after the descriptor is consumed; neither production nor tests retry close. */
typedef struct {
    void *context;
    ssize_t (*write_bytes)(void *context, int fd, const void *bytes, size_t size);
    int (*sync_file)(void *context, int fd, bool directory);
    int (*close_file)(void *context, int fd);
    int (*rename_file)(void *context, int directory, const char *from, const char *to);
} sp5_posix_replace_test_ops;

void sp5_posix_storage_test_replace_ops(sp5_posix_storage *storage,
                                        const sp5_posix_replace_test_ops *ops);
#endif

#endif
