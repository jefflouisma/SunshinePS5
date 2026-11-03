/* Synthetic portable contract tests; no console or interoperability evidence. */
#if defined(SP5_PENDING_CORE) && defined(__APPLE__)
#define SP5_CORE_API __attribute__((weak_import))
#elif defined(SP5_PENDING_CORE)
#define SP5_CORE_API __attribute__((weak))
#endif

#include "pairing.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            if (fputs(__FILE__ ": " #condition "\n", stderr) == EOF) {                             \
                return 2;                                                                          \
            }                                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
#ifdef SP5_PENDING_CORE
#define NEED(api)                                                                                  \
    do {                                                                                           \
        if ((api) == NULL) {                                                                       \
            if (fputs("TDD missing implementation: " #api "\n", stderr) == EOF) {                  \
                return 2;                                                                          \
            }                                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
#else
#define NEED(api) ((void)0)
#endif

static sp5_bytes bytes(const char *text)
{
    return (sp5_bytes){(const uint8_t *)text, strlen(text)};
}
/* Public RSA proof generated with OpenSSL (req -x509 -newkey rsa:2048;
 * dgst -sha256 -sign ephemeral-key secret.bin). Private key is scratch-only.
 * Digest oracle: Python hashlib.sha256(challenge + X509 signature + secret).
 * Source algorithm/layout: protocol-notes.md 13.4/13.5, sun nvhttp.cpp 883-913
 * @ d0b4a88 and mqt nvpairingmanager.cpp 296-306 @ 8f2e52e. Synthetic, not a
 * captured official-client exchange and not a claim of TLS interoperability. */
/* Public proof fixture generated with OpenSSL req -x509 -newkey rsa:2048
 * and dgst -sha256 -sign over secret bytes 00..0f; private key was discarded.
 * Response oracle: hashlib.sha256(challenge || X509 signature || secret).
 * Layout/algorithm: protocol-notes.md 13.4/13.5; sun nvhttp.cpp 883-913
 * @ d0b4a88, mqt nvpairingmanager.cpp 296-306 @ 8f2e52e. This synthetic
 * fixture is not a captured official-client exchange or TLS conformance proof. */
static const char certificate[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDPTCCAiWgAwIBAgIUfQfi58Qr3W9VM4QZQc+N60Cbh00wDQYJKoZIhvcNAQEL\n"
    "BQAwLjEsMCoGA1UEAwwjU3ludGhldGljIFQwNDYgcHVibGljIHByb29mIGZpeHR1\n"
    "cmUwHhcNMjYxMDI1MTcyOTMwWhcNMzYxMDIyMTcyOTMwWjAuMSwwKgYDVQQDDCNT\n"
    "eW50aGV0aWMgVDA0NiBwdWJsaWMgcHJvb2YgZml4dHVyZTCCASIwDQYJKoZIhvcN\n"
    "AQEBBQADggEPADCCAQoCggEBAMXpvGXSJot6gZNZP4CXlcqvCUyvmZtfKI7GLSi2\n"
    "fx7ECI+OeaEfoO5AkTGebUH+KQ6cWT/rGHmbZRH5nvlwhT4Xs4iudPNAex8FU+Io\n"
    "n7tX3n0KQlRpK9a8SHyma12HJKlOXUkbIkOFKl+3oRxsOOv/Fkk9dR/SiLBpTIcy\n"
    "J5qhGqdXuJTcQeh6FgbBhbOtVwTHi58VdsuNBTac+3uJjnpDvVrE/JCMRBOLhJeb\n"
    "78i1ZLLWBhg4P9Y7/Wfze5SA25fyZAT0eTobJBbKbWn9YtKCS3X46p6xPoZi/IXv\n"
    "3qSlyYwHFZr/2uysp36ywMNX/CXfE7fPZw8+YoEHiI0geZcCAwEAAaNTMFEwHQYD\n"
    "VR0OBBYEFMn9KR2aw2dr6pBhuZWyKf/dxWYGMB8GA1UdIwQYMBaAFMn9KR2aw2dr\n"
    "6pBhuZWyKf/dxWYGMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQELBQADggEB\n"
    "ALFlY8w1lTazlCxENfvDIS3prZa1s6XvmBEa2RLrBSV9+r3DyThwVrALZAK3gMnH\n"
    "qkqJ7XV3o78sH7eikRioOvWS7/8+7Sp4BerS3Sy1iDrMH5UW53PFboFDA71iSQwI\n"
    "/lRjqYWLup6dxH+iz5tyLYaM5YZppSJuGzYH0SJrYGcaxYs25S70bnXobylCqYE3\n"
    "swmos1FXlVNeF6qyWoaMHGh3Fz51ejF97xnGWcdWmlxsigV/NYFir5W01+ywaywM\n"
    "5BPN80o2PyBxKMULFaY7Vo+YxGj6USbNnpg9cjsG+mTahIwDHT6LXAsKg5jEVkQc\n"
    "Dh5qaEuuVFQlFd2QqwGjfP0=\n"
    "-----END CERTIFICATE-----\n";
static const uint8_t challenge[] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static const uint8_t response[] = {0x40, 0x6d, 0x24, 0xe0, 0x1b, 0x2c, 0xd7, 0x9a, 0x55, 0x01, 0xd5,
                                   0xdf, 0xae, 0xec, 0x19, 0x6a, 0x6e, 0xa9, 0xa3, 0xe3, 0x5f, 0x30,
                                   0xcb, 0x08, 0x30, 0x19, 0xd0, 0xba, 0x7e, 0xd4, 0x6f, 0x57};
static const uint8_t signed_secret[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x2c, 0x31, 0xb6, 0x45, 0xd0, 0xec, 0x30, 0x15, 0x76, 0xed, 0xca, 0xe4, 0x9e, 0xba, 0xef, 0xb1,
    0xa6, 0x2f, 0xb9, 0x04, 0x70, 0xb8, 0x74, 0x2e, 0x73, 0xd3, 0x42, 0xfe, 0xe3, 0x55, 0xa4, 0xa1,
    0x4d, 0x40, 0x03, 0x63, 0xd0, 0xc3, 0xde, 0x8b, 0xf9, 0x61, 0x25, 0xae, 0x20, 0x54, 0x40, 0x48,
    0x9e, 0x0c, 0xbf, 0x16, 0x65, 0x85, 0x74, 0xef, 0xbc, 0x00, 0xf8, 0x81, 0x6b, 0x02, 0x7f, 0xfd,
    0x3e, 0x37, 0xcf, 0xc6, 0x5d, 0x97, 0x8e, 0x17, 0x65, 0xbc, 0xa6, 0x08, 0x29, 0xde, 0x10, 0x1a,
    0xa6, 0xef, 0xc9, 0x47, 0xab, 0x03, 0xdd, 0x87, 0xd4, 0x3c, 0xec, 0xb0, 0x56, 0x9e, 0x2c, 0xc5,
    0x43, 0x02, 0x7d, 0xb7, 0x99, 0x30, 0x68, 0x5c, 0x86, 0x20, 0x47, 0xf8, 0x74, 0x3c, 0x81, 0x5a,
    0x33, 0x10, 0xf9, 0x93, 0x7f, 0x54, 0x51, 0x22, 0xa8, 0x53, 0x15, 0x0e, 0xc8, 0x48, 0x4b, 0xe0,
    0xa4, 0x99, 0x0b, 0x46, 0x4e, 0xed, 0x65, 0x8e, 0x3d, 0x69, 0x4f, 0x97, 0x8b, 0xd1, 0xa8, 0x31,
    0x8d, 0xce, 0x9c, 0xde, 0x0b, 0x2a, 0x37, 0xf6, 0xc8, 0x80, 0xc3, 0xc4, 0xf2, 0x58, 0x5b, 0xfd,
    0xe4, 0xbd, 0xc9, 0x36, 0xa0, 0xc2, 0x28, 0x22, 0xf1, 0x4b, 0xc0, 0x4d, 0x21, 0x31, 0x2f, 0xd4,
    0x86, 0x57, 0x90, 0x10, 0xaa, 0x95, 0x12, 0x39, 0xb9, 0x39, 0x2e, 0x77, 0xd8, 0x47, 0xf2, 0x6f,
    0xb3, 0xea, 0x94, 0xe1, 0x86, 0x0f, 0xb6, 0x7c, 0xd4, 0xc3, 0x08, 0x17, 0x08, 0x64, 0x30, 0xed,
    0x1d, 0xba, 0xef, 0x95, 0x62, 0x7f, 0x48, 0x27, 0x34, 0x76, 0x52, 0x9d, 0x33, 0xcd, 0x31, 0x0f,
    0x2e, 0xe5, 0xa4, 0x30, 0x86, 0x98, 0x73, 0xdf, 0xd3, 0xe3, 0x89, 0x19, 0x01, 0x73, 0x1f, 0xc2,
    0xaf, 0x47, 0x15, 0x4d, 0xa0, 0x1c, 0xbd, 0x71, 0x5e, 0x2b, 0x7b, 0xe5, 0x0d, 0x48, 0x45, 0x57};

static int pin_key(void)
{
    NEED(sp5_pairing_pin_key);
    /* Independent hashlib SHA256 oracle, notes 13.4, salt 00..0f / PIN 1234. */
    const uint8_t salt[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const uint8_t expected[16] = {0xba, 0xd0, 0xb4, 0xf7, 0xca, 0xe0, 0x8e, 0xb7,
                                  0xc1, 0xb5, 0xac, 0xc7, 0x63, 0xa8, 0xed, 0x25};
    uint8_t output[16];
    sp5_buffer key = {output, sizeof(output), 0};
    CHECK(sp5_pairing_pin_key((sp5_bytes){salt, sizeof(salt)}, bytes("1234"), &key).status ==
          SP5_OK);
    CHECK(key.size == 16 && memcmp(output, expected, 16) == 0);
    CHECK(sp5_pairing_pin_key((sp5_bytes){salt, sizeof(salt)}, bytes("0000"), &key).status ==
          SP5_OK);
    CHECK(memcmp(output, expected, 16) != 0);
    const char *invalid[] = {"", "123", "12345", "12x4", "-123"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(
            sp5_pairing_pin_key((sp5_bytes){salt, sizeof(salt)}, bytes(invalid[i]), &key).status ==
            SP5_ERROR);
    }
    CHECK(sp5_pairing_pin_key((sp5_bytes){salt, 15}, bytes("1234"), &key).status == SP5_ERROR);
    key.capacity = 15;
    CHECK(sp5_pairing_pin_key((sp5_bytes){salt, 16}, bytes("1234"), &key).status == SP5_ERROR);
    return 0;
}

static int proof(void)
{
    NEED(sp5_pairing_verify_client);
    sp5_bytes cert = bytes(certificate);
    sp5_bytes server = {challenge, sizeof(challenge)};
    sp5_bytes expected = {response, sizeof(response)};
    sp5_bytes signed_value = {signed_secret, sizeof(signed_secret)};
    CHECK(sp5_pairing_verify_client(cert, server, expected, signed_value).status == SP5_OK);
    uint8_t altered[sizeof(signed_secret)];
    memcpy(altered, signed_secret, sizeof(altered));
    altered[16] ^= 1; /* RSA signature mismatch */
    CHECK(sp5_pairing_verify_client(cert, server, expected, (sp5_bytes){altered, sizeof(altered)})
              .status == SP5_ERROR);
    memcpy(altered, signed_secret, sizeof(altered));
    altered[0] ^= 1; /* secret/digest mismatch */
    CHECK(sp5_pairing_verify_client(cert, server, expected, (sp5_bytes){altered, sizeof(altered)})
              .status == SP5_ERROR);
    uint8_t wrong_pin_response[sizeof(response)];
    memcpy(wrong_pin_response, response, sizeof(response));
    wrong_pin_response[0] ^= 1;
    CHECK(sp5_pairing_verify_client(cert, server,
                                    (sp5_bytes){wrong_pin_response, sizeof(wrong_pin_response)},
                                    signed_value)
              .status == SP5_ERROR);
    CHECK(
        sp5_pairing_verify_client(cert, server, expected, (sp5_bytes){signed_secret, 16}).status ==
        SP5_ERROR);
    CHECK(sp5_pairing_verify_client(cert, server, (sp5_bytes){response, 31}, signed_value).status ==
          SP5_ERROR);
    CHECK(sp5_pairing_verify_client(bytes("not a certificate"), server, expected, signed_value)
              .status == SP5_ERROR);
    uint8_t changed_challenge[sizeof(challenge)];
    memcpy(changed_challenge, challenge, sizeof(challenge));
    changed_challenge[15] ^= 1;
    CHECK(sp5_pairing_verify_client(cert, (sp5_bytes){changed_challenge, sizeof(changed_challenge)},
                                    expected, signed_value)
              .status == SP5_ERROR);
    return 0;
}

/* T054: only one pending, exact five-minute boundary, stale/terminal IDs. */
static int expiry(void)
{
    NEED(sp5_pairing_create);
    NEED(sp5_pairing_destroy);
    NEED(sp5_pairing_begin);
    NEED(sp5_pairing_finish);
    NEED(sp5_pairing_read);
    sp5_pairing *pairing = sp5_pairing_create();
    CHECK(pairing != NULL);
    uint64_t first = 0;
    uint64_t second = 0;
    CHECK(sp5_pairing_begin(pairing, 7, &first).status == SP5_OK && first != 0);
    CHECK(sp5_pairing_read(pairing, 7).expires_us == UINT64_C(300000007));
    CHECK(sp5_pairing_begin(pairing, 8, &second).status == SP5_ERROR);
    CHECK(sp5_pairing_read(pairing, 8).request_id == first);
    CHECK(sp5_pairing_read(pairing, UINT64_C(300000006)).state == SP5_PAIR_PENDING);
    CHECK(sp5_pairing_finish(pairing, first, UINT64_C(300000007), true).status == SP5_ERROR);
    CHECK(sp5_pairing_read(pairing, UINT64_C(300000007)).state == SP5_PAIR_EXPIRED);
    CHECK(sp5_pairing_begin(pairing, UINT64_C(300000008), &second).status == SP5_OK &&
          second != first);
    CHECK(sp5_pairing_finish(pairing, first, UINT64_C(300000009), true).status == SP5_ERROR);
    CHECK(sp5_pairing_read(pairing, UINT64_C(300000009)).request_id == second);
    CHECK(sp5_pairing_read(pairing, UINT64_C(300000009)).state == SP5_PAIR_PENDING);
    CHECK(sp5_pairing_finish(pairing, second, UINT64_C(300000010), true).status == SP5_OK);
    CHECK(sp5_pairing_read(pairing, UINT64_C(300000010)).state == SP5_PAIR_COMPLETED);
    CHECK(sp5_pairing_finish(pairing, second, UINT64_C(300000011), false).status == SP5_ERROR);
    CHECK(sp5_pairing_read(pairing, UINT64_C(300000011)).state == SP5_PAIR_COMPLETED);
    sp5_pairing_destroy(pairing);
    return 0;
}

static int cooldown(void)
{
    NEED(sp5_pairing_create);
    NEED(sp5_pairing_destroy);
    NEED(sp5_pairing_begin);
    NEED(sp5_pairing_finish);
    NEED(sp5_pairing_read);
    sp5_pairing *pairing = sp5_pairing_create();
    CHECK(pairing != NULL);
    uint64_t request = 0;
    uint64_t previous = 0;
    for (unsigned failure = 1; failure <= 5; ++failure) {
        CHECK(sp5_pairing_begin(pairing, (uint64_t)failure * 10, &request).status == SP5_OK);
        CHECK(request != previous);
        CHECK(sp5_pairing_finish(pairing, request, (uint64_t)failure * 10 + 1, false).status ==
              SP5_ERROR);
        CHECK(sp5_pairing_read(pairing, (uint64_t)failure * 10 + 1).failures == failure);
        CHECK(sp5_pairing_read(pairing, (uint64_t)failure * 10 + 1).state == SP5_PAIR_FAILED);
        previous = request;
    }
    CHECK(sp5_pairing_read(pairing, 51).cooldown_until_us == UINT64_C(60000051));
    CHECK(sp5_pairing_begin(pairing, UINT64_C(60000050), &request).status == SP5_ERROR);
    CHECK(sp5_pairing_finish(pairing, previous, 52, true).status == SP5_ERROR);
    CHECK(sp5_pairing_begin(pairing, UINT64_C(60000051), &request).status == SP5_OK);
    CHECK(sp5_pairing_finish(pairing, request, UINT64_C(60000052), true).status == SP5_OK);
    CHECK(sp5_pairing_read(pairing, UINT64_C(60000052)).failures == 0);
    CHECK(sp5_pairing_begin(pairing, UINT64_C(60000053), &request).status == SP5_OK);
    CHECK(sp5_pairing_finish(pairing, request, UINT64_C(60000054), false).status == SP5_ERROR);
    CHECK(sp5_pairing_read(pairing, UINT64_C(60000054)).failures == 1);
    sp5_pairing_destroy(pairing);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    if (strcmp(argv[1], "pin_key") == 0) {
        return pin_key();
    }
    if (strcmp(argv[1], "proof") == 0) {
        return proof();
    }
    if (strcmp(argv[1], "expiry") == 0) {
        return expiry();
    }
    if (strcmp(argv[1], "cooldown") == 0) {
        return cooldown();
    }
    return 2;
}
