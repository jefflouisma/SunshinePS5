/* Synthetic clocks and delivery evidence only. */
#include "../fakes/platform.h"
#include "metrics/records.h"
#include <stdio.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            return __LINE__;                                                                       \
        }                                                                                          \
    } while (0)
int main(void)
{
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_metrics *m = sp5_metrics_create(9, (sp5_metrics_capacity){2, 2, 2}, &fake.clock);
    CHECK(m != NULL);
    CHECK(sp5_metrics_enable(m, true, 0).status == SP5_OK);
    sp5_input_evidence event = {.session_id = 9,
                                .event_id = 3,
                                .controller_id = 1,
                                .delivery = SP5_INPUT_DELIVERY_CONFIRMED,
                                .delivery_boundary_ref = "synthetic delivery"};
    CHECK(sp5_metrics_stamp(m, &event.host_received).status == SP5_OK);
    CHECK(sp5_fake_advance(&fake, 123).status == SP5_OK);
    CHECK(sp5_metrics_stamp(m, &event.console_delivered).status == SP5_OK);
    CHECK(sp5_metrics_record_input(m, &event, false).reason == SP5_REASON_OWNERSHIP);
    event.session_id = 8;
    CHECK(sp5_metrics_record_input(m, &event, true).reason == SP5_REASON_OWNERSHIP);
    event.session_id = 9;
    CHECK(sp5_metrics_record_input(m, &event, true).status == SP5_OK);
    CHECK(sp5_metrics_worker_step(m, 123, NULL).status == SP5_OK);
    sp5_metrics_view view;
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK);
    CHECK(view.whole[SP5_STAGE_INPUT_DELIVERY].p99.value_us.present);
    CHECK(view.whole[SP5_STAGE_INPUT_DELIVERY].p99.value_us.value == 123);
    sp5_metrics_destroy(m);
    return 0;
}
