#ifndef SUNSHINE_CORE_CONTROLLER_H
#define SUNSHINE_CORE_CONTROLLER_H

#include "protocol/control.h"

typedef struct sp5_controller_input sp5_controller_input;
/* Caller authenticates before entry. Manager owns four logical IDs in connection
 * order, mapping wire IDs independently, never silently replacing a live pad.
 * Canonical buttons retain the sourced Moonlight mask; production mapping is
 * the platform adapter's obligation. Canonical triggers are u16 [0,65535],
 * preserving proportion; signed stick values retain wire polarity until the
 * platform's sourced convention is known. Receipt precedes parse/auth upstream. */
SP5_CORE_API sp5_controller_input *sp5_controller_create(uint64_t session_id,
                                                         const sp5_input_ops *ops);
SP5_CORE_API void sp5_controller_destroy(sp5_controller_input *input);
SP5_CORE_API sp5_result sp5_controller_receive(sp5_controller_input *input, sp5_bytes packet,
                                               bool metrics, sp5_input_evidence *event);
SP5_CORE_API sp5_result sp5_controller_close(sp5_controller_input *input);

/* Production setup binds immutable session authority and certificate. Receive,
 * add/remove, delivery and feedback recheck authorization without media waits.
 * The unbound constructor above is a trusted adapter/unit-test entrypoint. */
sp5_controller_input *sp5_controller_create_bound(sp5_session *session,
                                                  sp5_bytes verified_certificate,
                                                  const sp5_input_ops *ops);
/* Setup before activation: reserves the manager using an admitted lease;
 * receive still requires ACTIVE authorization at every delivery. */
sp5_controller_input *sp5_controller_create_reserved(sp5_session *session,
                                                     sp5_bytes verified_certificate,
                                                     const sp5_input_ops *ops);
/* Validate transport channel/reliability and the input envelope before receive.
 * The caller records host_received on its actual clock before this call. */
sp5_result sp5_controller_receive_control(sp5_controller_input *input,
                                          const sp5_control_event *packet, bool metrics,
                                          sp5_input_evidence *event);
/* Poll at most one feedback event and enqueue reliable rumble. Queue pressure
 * retains one pending item for retry; no network send or media wait occurs. */
sp5_result sp5_controller_forward_feedback(sp5_controller_input *input, sp5_control *control);
/* Explicit rate in Hz (zero disables), motion type 1 accel / 2 gyro. Hosts
 * request motion only when the client's arrival advertises that capability. */
typedef struct {
    uint32_t controller_id;
    uint16_t rate_hz;
    uint8_t motion_type;
} sp5_controller_motion_config;
sp5_result sp5_controller_motion_request(sp5_controller_input *input, sp5_control *control,
                                         sp5_controller_motion_config config);

#endif
