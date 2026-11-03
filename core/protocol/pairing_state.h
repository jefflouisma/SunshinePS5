#ifndef SUNSHINE_PROTOCOL_PAIRING_STATE_H
#define SUNSHINE_PROTOCOL_PAIRING_STATE_H

#include "pairing.h"

/* Serialized management owner only; no allocation after setup. Client name is
 * bounded untrusted display bytes, copied exactly and escaped by the renderer;
 * it never identifies/authorizes a device. Empty metadata is permitted. */
enum { SP5_PAIR_CLIENT_NAME_CAPACITY = 256 };
#define SP5_PAIR_EXPIRY_US UINT64_C(300000000)
#define SP5_PAIR_COOLDOWN_US UINT64_C(60000000)
sp5_result sp5_pairing_begin_request(sp5_pairing *pairing, uint64_t now_us,
                                     sp5_optional_u64 wall_unix_us, sp5_bytes client_name,
                                     uint64_t *request_id);
/* Ends the pending request as failed WITHOUT counting a failed PIN entry
 * (FR-010): host-side faults and client aborts before the PIN key is used.
 * Same identity/clock/expiry rules as sp5_pairing_finish. */
sp5_result sp5_pairing_cancel(sp5_pairing *pairing, uint64_t request_id, uint64_t now_us);

#endif
