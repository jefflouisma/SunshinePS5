#include "protocol/pairing_state.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            if (fputs(__FILE__ ": " #x "\n", stderr) == EOF) {                                     \
                return 2;                                                                          \
            }                                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int metadata_and_clock(void)
{
    CHECK(sp5_pairing_begin(NULL, 0, NULL).status == SP5_ERROR);
    sp5_pairing *pairing = sp5_pairing_create();
    CHECK(pairing != NULL);
    uint64_t id = 0;
    uint8_t hostile[] = "<script>untrusted</script>";
    CHECK(sp5_pairing_begin_request(pairing, 100, (sp5_optional_u64){true, 9000},
                                    (sp5_bytes){hostile, sizeof(hostile) - 1}, &id)
              .status == SP5_OK);
    CHECK(id != 0);
    hostile[0] = '!';
    sp5_pair_view view = sp5_pairing_read(pairing, 101);
    CHECK(view.started_us == 100 && view.expires_us == 300000100);
    CHECK(view.started_wall_unix_us.present && view.started_wall_unix_us.value == 9000);
    CHECK(view.client_name_size == sizeof(hostile) - 1 && view.client_name[0] == '<');
    CHECK(sp5_pairing_finish(pairing, id, 99, true).reason == SP5_REASON_CLOCK);
    CHECK(sp5_pairing_read(pairing, 102).state == SP5_PAIR_PENDING);
    CHECK(sp5_pairing_finish(pairing, id, 103, false).status == SP5_ERROR);
    CHECK(sp5_pairing_read(pairing, 103).failures == 1);
    uint64_t next = 0;
    CHECK(sp5_pairing_begin(pairing, 104, &next).status == SP5_OK && next != id);
    CHECK(sp5_pairing_finish(pairing, id, 105, true).status == SP5_ERROR);
    CHECK(sp5_pairing_read(pairing, 105).failures == 1 &&
          sp5_pairing_read(pairing, 105).state == SP5_PAIR_PENDING);
    CHECK(sp5_pairing_finish(pairing, next, 106, true).status == SP5_OK);
    CHECK(sp5_pairing_read(pairing, 106).failures == 0);
    CHECK(sp5_pairing_begin(pairing, 105, &next).reason == SP5_REASON_CLOCK);
    uint8_t oversized[SP5_PAIR_CLIENT_NAME_CAPACITY + 1] = {0};
    CHECK(sp5_pairing_begin_request(pairing, 107, (sp5_optional_u64){0},
                                    (sp5_bytes){oversized, sizeof(oversized)}, &next)
              .reason == SP5_REASON_CAPACITY);
    CHECK(sp5_pairing_read(pairing, 107).state == SP5_PAIR_COMPLETED);
    CHECK(
        sp5_pairing_begin_request(pairing, 108, (sp5_optional_u64){0}, (sp5_bytes){NULL, 1}, &next)
            .status == SP5_ERROR);
    CHECK(sp5_pairing_begin(pairing, UINT64_MAX, &next).reason == SP5_REASON_OVERFLOW);
    sp5_pairing_destroy(pairing);
    return 0;
}
static int repeated_cooldown(void)
{
    sp5_pairing *pairing = sp5_pairing_create();
    CHECK(pairing != NULL);
    uint64_t id = 0;
    for (unsigned i = 0; i < 5; ++i) {
        CHECK(sp5_pairing_begin(pairing, (uint64_t)2 * i, &id).status == SP5_OK);
        CHECK(sp5_pairing_finish(pairing, id, (uint64_t)2 * i + 1, false).status == SP5_ERROR);
    }
    CHECK(sp5_pairing_read(pairing, 9).cooldown_until_us == 60000009);
    CHECK(sp5_pairing_begin(pairing, 60000008, &id).reason == SP5_REASON_TIMEOUT);
    CHECK(sp5_pairing_begin(pairing, 60000009, &id).status == SP5_OK);
    CHECK(sp5_pairing_finish(pairing, id, 60000010, false).status == SP5_ERROR);
    CHECK(sp5_pairing_read(pairing, 60000010).failures == 6);
    CHECK(sp5_pairing_read(pairing, 60000010).cooldown_until_us == 120000010);
    CHECK(sp5_pairing_begin(pairing, 120000009, &id).status == SP5_ERROR);
    CHECK(sp5_pairing_begin(pairing, 120000010, &id).status == SP5_OK);
    CHECK(sp5_pairing_finish(pairing, id, 120000011, true).status == SP5_OK);
    CHECK(sp5_pairing_read(pairing, 120000011).failures == 0);
    CHECK(sp5_pairing_read(pairing, 120000011).cooldown_until_us == 0);
    sp5_pairing_destroy(pairing);
    return 0;
}
int main(void)
{
    int result = metadata_and_clock();
    return result == 0 ? repeated_cooldown() : result;
}
