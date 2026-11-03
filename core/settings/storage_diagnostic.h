#ifndef SUNSHINE_STORAGE_DIAGNOSTIC_H
#define SUNSHINE_STORAGE_DIAGNOSTIC_H

#include "store.h"

/* Opt-in, nonshipping diagnostics. No paths, contents, keys, certificates or
 * pointers cross this boundary; callers supply only literal stage names and
 * public result codes. Ordinary builds emit nothing and evaluate no trace args. */
#ifdef SP5_STORAGE_DIAGNOSTICS
#include <errno.h>
void sp5_storage_trace(const char *stage, int file, bool begin, int64_t result, sp5_reason reason,
                       int system_error, unsigned long crypto_error);
#define SP5_STORAGE_BEGIN(stage, file)                                                             \
    do {                                                                                           \
        int saved_errno = errno;                                                                   \
        sp5_storage_trace(stage, file, true, 0, SP5_REASON_NONE, 0, 0);                            \
        errno = saved_errno;                                                                       \
    } while (0)
#define SP5_STORAGE_END(stage, file, result, reason, crypto_error)                                 \
    do {                                                                                           \
        int saved_errno = errno;                                                                   \
        sp5_storage_trace(stage, file, false, result, reason, saved_errno, crypto_error);          \
        errno = saved_errno;                                                                       \
    } while (0)
#else
#define SP5_STORAGE_BEGIN(stage, file) ((void)0)
#define SP5_STORAGE_END(stage, file, result, reason, crypto_error) ((void)0)
#endif

/* Test builds alone can replace one operation with its normal failure value.
 * This hook is absent from the target payload and ordinary production builds. */
#ifdef SP5_STORAGE_DIAGNOSTIC_TEST
#ifndef SP5_STORAGE_DIAGNOSTICS
#error "Diagnostic fault injection requires diagnostic tracing"
#endif
bool sp5_storage_fault(const char *stage);
#define SP5_STORAGE_OP(stage, expression, failure)                                                 \
    (sp5_storage_fault(stage) ? (failure) : (expression))
#else
#define SP5_STORAGE_OP(stage, expression, failure) (expression)
#endif

#endif
