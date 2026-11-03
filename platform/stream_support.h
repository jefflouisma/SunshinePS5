#ifndef SUNSHINE_PLATFORM_STREAM_SUPPORT_H
#define SUNSHINE_PLATFORM_STREAM_SUPPORT_H

/* Target-sysroot compatibility shim for the diagnostic streaming build only.
 *
 * The pinned ps5-payload-sdk sysroot maps _POSIX_C_SOURCE=200809L to
 * __ISO_C_VISIBLE=1999, so its <stddef.h> omits max_align_t even under
 * -std=c11. core/protocol/control.c uses max_align_t for its fixed ENet pool
 * alignment. Supply the same sysroot type without changing portable core. */

#include <stddef.h>

#if !defined(__cplusplus) && !defined(__CLANG_MAX_ALIGN_T_DEFINED)
typedef __max_align_t max_align_t;
#endif

#endif
