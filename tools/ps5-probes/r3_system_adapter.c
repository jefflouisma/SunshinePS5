/* Nonshipping check of the production T075 system adapter on firmware 13.60. */
#include "firmware/13.60.h"
#include "system/ps5.h"
#include <stdio.h>
int main(void)
{
    sp5_ps5_system system;
    sp5_platform_provenance provenance;
    sp5_system_state state;
    sp5_ps5_system_init(&system);
    sp5_result probe = sp5_ps5_system_probe(&system, &provenance);
    sp5_result st = sp5_ps5_system_state(&system, &state);
    printf("R3_SYS probe_status=%d validity=%d reason=%d synthetic=%d\n", probe.status,
           (int)provenance.validity, (int)provenance.reason, (int)provenance.synthetic);
    printf("R3_SYS state_status=%d validity=%d reason=%d remote=%d share=%d rest=%d\n", st.status,
           (int)state.validity, (int)state.reason, (int)state.remote_play, (int)state.share_play,
           (int)state.stopping_or_rest);
    sp5_result prep = sp5_ps5_system_prepare(&system);
    sp5_result roll = sp5_ps5_system_rollback(&system);
    printf("R3_SYS prepare=%d rollback=%d prepared=%d\n", prep.status, roll.status,
           (int)sp5_ps5_system_prepared(&system));
    fflush(stdout);
    return probe.status == SP5_OK && st.status == SP5_OK && state.validity == SP5_VALIDITY_VALID &&
                   prep.status == SP5_OK && roll.status == SP5_OK
               ? 0
               : 1;
}
