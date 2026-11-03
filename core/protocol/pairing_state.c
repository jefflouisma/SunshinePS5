#include "pairing_state.h"

#include <limits.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>

struct sp5_pairing {
    sp5_pair_view view;
    uint64_t next_id;
    uint64_t last_us;
    bool cooldown_overflow;
};

sp5_pairing *sp5_pairing_create(void)
{
    sp5_pairing *pairing = calloc(1, sizeof(*pairing));
    if (pairing != NULL) {
        /* Random starting point also separates browser forms from previous
         * manager lifetimes; bounded monotonic IDs never wrap in a lifetime. */
        if (RAND_bytes_ex(NULL, (unsigned char *)&pairing->next_id, sizeof(pairing->next_id), 0) !=
                1 ||
            pairing->next_id == 0 || pairing->next_id == UINT64_MAX) {
            free(pairing);
            return NULL;
        }
        pairing->view.state = SP5_PAIR_EXPIRED;
    }
    return pairing;
}

void sp5_pairing_destroy(sp5_pairing *pairing)
{
    free(pairing);
}

static bool advance(sp5_pairing *pairing, uint64_t now_us)
{
    if (now_us < pairing->last_us) {
        return false;
    }
    pairing->last_us = now_us;
    if (pairing->view.state == SP5_PAIR_PENDING && now_us >= pairing->view.expires_us) {
        pairing->view.state = SP5_PAIR_EXPIRED;
    }
    return true;
}

sp5_result sp5_pairing_begin_request(sp5_pairing *pairing, uint64_t now_us,
                                     sp5_optional_u64 wall_unix_us, sp5_bytes client_name,
                                     uint64_t *request_id)
{
    if (pairing == NULL || request_id == NULL ||
        (client_name.data == NULL && client_name.size != 0)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (client_name.size > sizeof(pairing->view.client_name)) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    if (!advance(pairing, now_us)) {
        return sp5_failure(SP5_REASON_CLOCK);
    }
    if (pairing->view.state == SP5_PAIR_PENDING) {
        return sp5_failure(SP5_REASON_CONFLICT);
    }
    if (pairing->cooldown_overflow || now_us < pairing->view.cooldown_until_us) {
        return sp5_failure(SP5_REASON_TIMEOUT);
    }
    if (now_us > UINT64_MAX - SP5_PAIR_EXPIRY_US || pairing->next_id == UINT64_MAX) {
        return sp5_failure(SP5_REASON_OVERFLOW);
    }
    sp5_pair_view next = {.request_id = pairing->next_id++,
                          .started_us = now_us,
                          .started_wall_unix_us =
                              wall_unix_us.present ? wall_unix_us : (sp5_optional_u64){0},
                          .expires_us = now_us + SP5_PAIR_EXPIRY_US,
                          .cooldown_until_us = pairing->view.cooldown_until_us,
                          .failures = pairing->view.failures,
                          .state = SP5_PAIR_PENDING,
                          .client_name_size = client_name.size};
    if (client_name.size != 0) {
        memcpy(next.client_name, client_name.data, client_name.size);
    }
    pairing->view = next;
    *request_id = next.request_id;
    return sp5_success();
}

sp5_result sp5_pairing_begin(sp5_pairing *pairing, uint64_t now_us, uint64_t *request_id)
{
    return sp5_pairing_begin_request(pairing, now_us, (sp5_optional_u64){0}, (sp5_bytes){0},
                                     request_id);
}

sp5_result sp5_pairing_finish(sp5_pairing *pairing, uint64_t request_id, uint64_t now_us,
                              bool verified_proof)
{
    const struct {
        uint64_t id, observed_us;
    } submission = {request_id, now_us};

    if (pairing == NULL || submission.id == 0 || submission.id != pairing->view.request_id ||
        pairing->view.state != SP5_PAIR_PENDING) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (!advance(pairing, submission.observed_us)) {
        return sp5_failure(SP5_REASON_CLOCK);
    }
    if (pairing->view.state == SP5_PAIR_EXPIRED) {
        return sp5_failure(SP5_REASON_TIMEOUT);
    }
    if (verified_proof) {
        pairing->view.state = SP5_PAIR_COMPLETED;
        pairing->view.failures = 0;
        pairing->view.cooldown_until_us = 0;
        return sp5_success();
    }
    pairing->view.state = SP5_PAIR_FAILED;
    if (pairing->view.failures < UINT_MAX) {
        ++pairing->view.failures;
    }
    if (pairing->view.failures >= 5) {
        if (submission.observed_us > UINT64_MAX - SP5_PAIR_COOLDOWN_US) {
            pairing->cooldown_overflow = true;
            pairing->view.cooldown_until_us = UINT64_MAX;
            return sp5_failure(SP5_REASON_OVERFLOW);
        }
        pairing->view.cooldown_until_us = submission.observed_us + SP5_PAIR_COOLDOWN_US;
    }
    return sp5_failure(SP5_REASON_OWNERSHIP);
}

sp5_result sp5_pairing_cancel(sp5_pairing *pairing, uint64_t request_id, uint64_t now_us)
{
    const struct {
        uint64_t id, observed_us;
    } submission = {request_id, now_us};

    if (pairing == NULL || submission.id == 0 || submission.id != pairing->view.request_id ||
        pairing->view.state != SP5_PAIR_PENDING) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (!advance(pairing, submission.observed_us)) {
        return sp5_failure(SP5_REASON_CLOCK);
    }
    if (pairing->view.state == SP5_PAIR_EXPIRED) {
        return sp5_failure(SP5_REASON_TIMEOUT);
    }
    /* Terminal like any failure; the consecutive-failure counter is unchanged. */
    pairing->view.state = SP5_PAIR_FAILED;
    return sp5_success();
}

sp5_pair_view sp5_pairing_read(sp5_pairing *pairing, uint64_t now_us)
{
    if (pairing == NULL) {
        return (sp5_pair_view){.state = SP5_PAIR_EXPIRED};
    }
    /* A backwards observation cannot rewind expiry or unlock a cooldown. */
    if (!advance(pairing, now_us)) {
        return pairing->view;
    }
    return pairing->view;
}
