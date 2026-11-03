/* Internal nonshipping input cleanup seam; callback returns are not delivery evidence. */
#ifndef R3_INPUT_PROBE_H
#define R3_INPUT_PROBE_H

#include "input_system.h"

enum { R3_INPUT_CONTROLLER_CAPACITY = 4 };
typedef struct {
    int (*neutral)(void *, unsigned, int32_t, NeutralReport *);
    int (*remove)(void *, unsigned, int32_t);
    void *context;
} InputCleanupApi;

bool input_cleanup_handles(const InputCleanupApi *, int32_t[R3_INPUT_CONTROLLER_CAPACITY]);

/* Only the opt-in neutral-observation diagnostic may delay handle removal.
 * A stop request interrupts the wait; errors never prevent later removal. */
typedef struct {
    bool (*now_us)(void *, uint64_t *);
    int (*wait_tick)(void *);
    bool (*stopping)(void *);
    void *context;
} InputObservationApi;

enum {
    R3_OBSERVATION_ERROR = -1,
    R3_OBSERVATION_SKIPPED,
    R3_OBSERVATION_COMPLETE,
    R3_OBSERVATION_STOPPED
};
int input_observe_neutral(ProbeMode, bool normal_completion, int neutral_rc,
                          const InputObservationApi *);

#endif
