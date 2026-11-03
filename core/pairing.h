#ifndef SUNSHINE_CORE_PAIRING_H
#define SUNSHINE_CORE_PAIRING_H

#include "protocol.h"

typedef struct sp5_pairing sp5_pairing;
typedef enum {
    SP5_PAIR_PENDING,
    SP5_PAIR_COMPLETED,
    SP5_PAIR_FAILED,
    SP5_PAIR_EXPIRED
} sp5_pair_state;
typedef struct {
    uint64_t request_id;
    uint64_t expires_us;
    uint64_t cooldown_until_us;
    unsigned failures;
    sp5_pair_state state;
    uint64_t started_us;
    sp5_optional_u64 started_wall_unix_us;
    uint8_t client_name[256];
    size_t client_name_size;
} sp5_pair_view;
/* Absolute times are synthetic monotonic microseconds in tests. A failed proof
 * is terminal; counters belong to the pairing manager, across request IDs. */
SP5_CORE_API sp5_pairing *sp5_pairing_create(void);
SP5_CORE_API void sp5_pairing_destroy(sp5_pairing *pairing);
SP5_CORE_API sp5_result sp5_pairing_begin(sp5_pairing *pairing, uint64_t now_us,
                                          uint64_t *request_id);
SP5_CORE_API sp5_result sp5_pairing_finish(sp5_pairing *pairing, uint64_t request_id,
                                           uint64_t now_us, bool verified_proof);
SP5_CORE_API sp5_pair_view sp5_pairing_read(sp5_pairing *pairing, uint64_t now_us);
/* Protocol generation 7: first 16 bytes of SHA256(salt16 || four ASCII digits).
 * This is protocol composition through maintained crypto, not a new primitive. */
SP5_CORE_API sp5_result sp5_pairing_pin_key(sp5_bytes salt, sp5_bytes pin, sp5_buffer *key);
/* Host-side final client proof, protocol-notes.md 13.4/13.5: recompute SHA256
 * (server challenge || X509 certificate signature || client secret), compare
 * the decrypted response, and verify RSA-SHA256 over the 16-byte client secret.
 * Maintained crypto must parse/verify; certificate and signed-secret are bounded.
 * This function alone does not authorize or persist a client. */
SP5_CORE_API sp5_result sp5_pairing_verify_client(sp5_bytes certificate_pem,
                                                  sp5_bytes server_challenge,
                                                  sp5_bytes decrypted_response,
                                                  sp5_bytes signed_client_secret);

#endif
