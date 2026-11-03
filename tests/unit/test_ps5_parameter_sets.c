/* Portable checks for the Avcap2 configuration-header parser. The packet is
 * synthetic; this proves the transcription of
 * tools/ps5-probes/capture_encode.c:581-620, never console behavior. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../platform/capture/ps5.h"

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            (void)fputs(__FILE__ ": check failed: " #expression "\n", stderr);                     \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

_Static_assert(SP5_PS5_CAPTURE_PARAMETER_BYTES >= 3 * (4 + 128),
               "parameter buffer holds every sourced slot");

static void store32(uint8_t *bytes, size_t offset, uint32_t value)
{
    memcpy(bytes + offset, &value, sizeof(value));
}

static void store64(uint8_t *bytes, size_t offset, uint64_t value)
{
    memcpy(bytes + offset, &value, sizeof(value));
}

static void packet_init(uint8_t *packet, uint32_t codec)
{
    memset(packet, 0, SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES);
    store64(packet, 0, SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES);
    store32(packet, 8, codec);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void packet_slot(uint8_t *packet, size_t length_offset, size_t payload_offset,
                        const uint8_t *payload, size_t size)
{
    store64(packet, length_offset, size);
    memcpy(packet + payload_offset, payload, size);
}

static void expect_invalid(const uint8_t *packet, size_t size, uint32_t codec)
{
    uint8_t sets[SP5_PS5_CAPTURE_PARAMETER_BYTES];
    memset(sets, 0xaa, sizeof(sets));
    CHECK(sp5_ps5_parameter_sets_parse(sets, sizeof(sets), packet, size, codec) == 0);
    for (size_t i = 0; i < sizeof(sets); ++i)
        CHECK(sets[i] == 0xaa);
}

static void test_avc(void)
{
    uint8_t packet[SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES];
    packet_init(packet, 0);
    static const uint8_t sps[] = {0x67, 0x42, 0x00, 0x1f};
    static const uint8_t pps[] = {0x68, 0xce, 0x3c};
    packet_slot(packet, 0x10, 0x18, sps, sizeof(sps));
    packet_slot(packet, 0x98, 0xa0, pps, sizeof(pps));
    /* AVC ignores the HEVC-only third slot exactly as the probe does. */
    packet_slot(packet, 0x120, 0x128, (const uint8_t[]){0x40, 0x01}, 2);

    uint8_t sets[SP5_PS5_CAPTURE_PARAMETER_BYTES] = {0};
    size_t used = sp5_ps5_parameter_sets_parse(sets, sizeof(sets), packet, sizeof(packet), 0);
    static const uint8_t expected[] = {0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1f,
                                       0, 0, 0, 1, 0x68, 0xce, 0x3c};
    CHECK(used == sizeof(expected));
    CHECK(memcmp(sets, expected, sizeof(expected)) == 0);

    /* Exact capacity succeeds; one byte short fails without writing. */
    used = sp5_ps5_parameter_sets_parse(sets, sizeof(expected), packet, sizeof(packet), 0);
    CHECK(used == sizeof(expected));
    memset(sets, 0xaa, sizeof(sets));
    CHECK(sp5_ps5_parameter_sets_parse(sets, sizeof(expected) - 1, packet, sizeof(packet), 0) == 0);
    CHECK(sets[0] == 0xaa);
}

static void test_hevc(void)
{
    uint8_t packet[SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES];
    packet_init(packet, 1);
    static const uint8_t vps[] = {0x40, 0x01};
    static const uint8_t sps[] = {0x42, 0x01, 0x02};
    static const uint8_t pps[] = {0x44, 0x01};
    /* Probe slot mapping: VPS 0x120, SPS 0x10, PPS 0x98; output VPS/SPS/PPS. */
    packet_slot(packet, 0x120, 0x128, vps, sizeof(vps));
    packet_slot(packet, 0x10, 0x18, sps, sizeof(sps));
    packet_slot(packet, 0x98, 0xa0, pps, sizeof(pps));

    uint8_t sets[SP5_PS5_CAPTURE_PARAMETER_BYTES] = {0};
    size_t used = sp5_ps5_parameter_sets_parse(sets, sizeof(sets), packet, sizeof(packet), 1);
    static const uint8_t expected[] = {0,    0,    0,    1, 0x40, 0x01, 0, 0,    0,   1,
                                       0x42, 0x01, 0x02, 0, 0,    0,    1, 0x44, 0x01};
    CHECK(used == sizeof(expected));
    CHECK(memcmp(sets, expected, sizeof(expected)) == 0);
}

static void test_rejections(void)
{
    uint8_t packet[SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES];
    packet_init(packet, 0);
    static const uint8_t sps[] = {0x67, 0x42};
    static const uint8_t pps[] = {0x68, 0xce};
    packet_slot(packet, 0x10, 0x18, sps, sizeof(sps));
    packet_slot(packet, 0x98, 0xa0, pps, sizeof(pps));

    expect_invalid(NULL, SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES, 0);
    expect_invalid(packet, SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES - 1, 0);
    expect_invalid(packet, SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES, 2);

    packet_init(packet, 0);
    packet_slot(packet, 0x10, 0x18, sps, sizeof(sps));
    packet_slot(packet, 0x98, 0xa0, pps, sizeof(pps));
    store64(packet, 0, SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES - 1);
    expect_invalid(packet, SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES, 0);

    packet_init(packet, 1);
    packet_slot(packet, 0x10, 0x18, sps, sizeof(sps));
    packet_slot(packet, 0x98, 0xa0, pps, sizeof(pps));
    expect_invalid(packet, SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES, 0);

    packet_init(packet, 0);
    packet_slot(packet, 0x10, 0x18, (const uint8_t[]){0x68, 0x42}, 2);
    packet_slot(packet, 0x98, 0xa0, pps, sizeof(pps));
    expect_invalid(packet, SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES, 0);

    packet_init(packet, 0);
    packet_slot(packet, 0x10, 0x18, sps, sizeof(sps));
    packet_slot(packet, 0x98, 0xa0, pps, 0);
    expect_invalid(packet, SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES, 0);

    packet_init(packet, 0);
    packet_slot(packet, 0x10, 0x18, sps, sizeof(sps));
    packet_slot(packet, 0x98, 0xa0, pps, sizeof(pps));
    store64(packet, 0x98, 129);
    expect_invalid(packet, SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES, 0);

    packet_init(packet, 1);
    packet_slot(packet, 0x120, 0x128, (const uint8_t[]){0x40}, 1);
    packet_slot(packet, 0x10, 0x18, (const uint8_t[]){0x42, 0x01}, 2);
    packet_slot(packet, 0x98, 0xa0, (const uint8_t[]){0x44, 0x01}, 2);
    expect_invalid(packet, SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES, 1);

    uint8_t sets[SP5_PS5_CAPTURE_PARAMETER_BYTES] = {0};
    CHECK(sp5_ps5_parameter_sets_parse(NULL, sizeof(sets), packet,
                                       SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES, 1) == 0);
    CHECK(sp5_ps5_parameter_sets_parse(sets, 0, packet, SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES,
                                       1) == 0);

    packet_init(packet, 1);
    packet_slot(packet, 0x120, 0x128, (const uint8_t[]){0x42, 0x01}, 2);
    packet_slot(packet, 0x10, 0x18, (const uint8_t[]){0x42, 0x01}, 2);
    packet_slot(packet, 0x98, 0xa0, (const uint8_t[]){0x44, 0x01}, 2);
    expect_invalid(packet, SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES, 1);
}

int main(void)
{
    test_avc();
    test_hevc();
    test_rejections();
    return EXIT_SUCCESS;
}
