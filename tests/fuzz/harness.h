#ifndef SUNSHINE_FUZZ_HARNESS_H
#define SUNSHINE_FUZZ_HARNESS_H
#include "runtime.h"
#include <stdint.h>
#include <stdlib.h>

/* These local harnesses never open a socket or invoke a console adapter. */
static inline void fuzz_require(bool condition)
{
    if (!condition) {
        abort();
    }
}
static inline void fuzz_span(sp5_bytes whole, sp5_bytes part)
{
    if (part.size == 0) {
        return;
    }
    uintptr_t start = (uintptr_t)whole.data, at = (uintptr_t)part.data;
    fuzz_require(at >= start && at - start <= whole.size &&
                 part.size <= whole.size - (size_t)(at - start));
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
#endif
