#ifndef SUNSHINE_TESTS_FAKES_PAIRING_CLIENT_H
#define SUNSHINE_TESTS_FAKES_PAIRING_CLIENT_H
/* SYNTHETIC Gen7 client side for portable unit tests and the loopback browser
 * fixture. It drives the real core pair flow in process with maintained
 * OpenSSL primitives (protocol-notes.md §§13.2-13.6). It is never an official
 * Moonlight client, a PS5 observation or reference-host evidence. */
#include "protocol/pairing_flow.h"
#include <openssl/evp.h>
#include <openssl/x509.h>

typedef struct {
    EVP_PKEY *key;
    X509 *certificate;
    uint8_t certificate_pem[SP5_CERTIFICATE_CAPACITY];
    size_t certificate_size;
    uint8_t salt[16];
    uint64_t request_id;
} sp5_fake_pair_client;

/* Writes a fresh RSA-2048 private key PEM followed by its self-signed
 * certificate PEM (the POSIX storage identity layout). Test setup only. */
sp5_result sp5_fake_pair_identity(sp5_buffer *identity);
/* identity holds one private key PEM and one certificate PEM, in any order. */
sp5_result sp5_fake_pair_client_open(sp5_fake_pair_client *client, sp5_bytes identity);
void sp5_fake_pair_client_close(sp5_fake_pair_client *client);
/* getservercert with a percent-encoded untrusted display name. *code receives
 * the host's HTTP code; *deferred is true while the host waits for a PIN. */
sp5_result sp5_fake_pair_client_begin(sp5_fake_pair_client *client, sp5_pair_flow *flow,
                                      sp5_pairing *state, sp5_bytes client_name, uint64_t now_us,
                                      unsigned *code, bool *deferred);
/* WOULD_BLOCK until a PIN was entered on the host. Otherwise completes the
 * remaining exchange with the PIN this client displays; *paired reports the
 * host's final paired flag (false for a mismatched PIN, expiry or failure). */
sp5_result sp5_fake_pair_client_prove(sp5_fake_pair_client *client, sp5_pair_flow *flow,
                                      sp5_bytes pin, uint64_t now_us, sp5_optional_u64 wall_us,
                                      bool *paired);
#endif
