#include "protocol/fec.h"
#include "protocol/video.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            if (fputs(__FILE__ ": " #c "\n", stderr) == EOF)                                       \
                return 2;                                                                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static size_t allocations;
static void *allocate(size_t n, const char *file, int line)
{
    (void)file;
    (void)line;
    ++allocations;
    return malloc(n);
}
static void *reallocate(void *p, size_t n, const char *file, int line)
{
    (void)file;
    (void)line;
    ++allocations;
    return realloc(p, n);
}
static void release(void *p, const char *file, int line)
{
    (void)file;
    (void)line;
    free(p);
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int vectors(void)
{
    /* nanors a9b1636f9c98dbfdd0daeaf6982ccd24d1c30f29 rs.c:111-139,
     * GF polynomial 0x11d; oracle independently compiled for client fec.rs.
     * This is synthetic erasure-code evidence, not a console recording. */
    uint8_t shards[5][16] = {{1, 0x34, 0, 2, 2, 0, 0, 0, 0, 0, 1, 0x65, 0x11, 0x22, 0x33, 0x44},
                             {0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0,
                              0x10, 0x20, 0x30, 0x40},
                             {0xab, 0xcd},
                             {0},
                             {0}};
    const uint8_t expected[2][16] = {
        {0x5e, 0x4c, 0x2d, 0x79, 0x76, 0x66, 0x69, 0x44, 0x4b, 0x5a, 0xdb, 0xbc, 0x7d, 0xfa, 0x87,
         0xe9},
        {0xd5, 0x4f, 0xb5, 0xb1, 0x37, 0x55, 0xd3, 0x66, 0xe0, 0x77, 5, 0x23, 7, 0x0e, 9, 0x1c}};
    uint8_t *p[5] = {shards[0], shards[1], shards[2], shards[3], shards[4]};
    sp5_fec fec;
    sp5_fec_init(&fec);
    CHECK(sp5_fec_encode(&fec, p, 3, 2, 16).status == SP5_OK);
    CHECK(memcmp(shards[3], expected, sizeof(expected)) == 0);
    CHECK(sp5_fec_encode(&fec, p, 0, 2, 16).status == SP5_ERROR);
    CHECK(sp5_fec_encode(&fec, p, 255, 1, 16).status == SP5_ERROR);
    CHECK(sp5_fec_encode(&fec, p, 3, 2, SIZE_MAX).status == SP5_ERROR);
    p[4] = p[0];
    CHECK(sp5_fec_encode(&fec, p, 3, 2, 16).status == SP5_ERROR);
    return 0;
}
static int packetization(void)
{
    /* Wire/header constants: protocol-notes §§5-7, Sunshine d0b4a88
     * stream.cpp:1588-1779; mcc 41cedb8 Video.h:12-48. */
    sp5_video_config c = {
        .packet_size = 64, .fec_percent = 20, .minimum_parity = 2, .first_packet_index = 65535};
    sp5_video *v = sp5_video_create(&c);
    CHECK(v != NULL);
    uint8_t data[16384];
    for (size_t i = 0; i < sizeof(data); ++i)
        data[i] = (uint8_t)i;
    sp5_video_frame_input f = {.bitstream = {data, 9},
                               .frame_index = 7,
                               .rtp_timestamp = 9000,
                               .key_frame = true,
                               .processing_us = {true, 1234}};
    CHECK(sp5_video_begin(v, &f).status == SP5_OK);
    CHECK(sp5_video_begin(v, &f).reason == SP5_REASON_OWNERSHIP);
    uint8_t bytes[SP5_VIDEO_WIRE_LIMIT];
    sp5_buffer wire = {bytes, 1, 99};
    CHECK(sp5_video_next(v, &wire).reason == SP5_REASON_CAPACITY && wire.size == 0);
    wire.capacity = sizeof(bytes);
    CHECK(sp5_video_next(v, &wire).status == SP5_OK && wire.size == 80);
    CHECK(bytes[0] == 0x90 && bytes[1] == 0 && bytes[2] == 255 && bytes[3] == 255);
    CHECK(bytes[4] == 0 && bytes[5] == 0 && bytes[6] == 0x23 && bytes[7] == 0x28);
    CHECK(le32(bytes + 16) == 0x00ffff00 && le32(bytes + 20) == 7);
    CHECK(bytes[24] == 7 && bytes[26] == 0x10 && bytes[27] == 0);
    CHECK(le32(bytes + 28) == ((1U << 22) | (200U << 4)));
    const uint8_t header[8] = {1, 12, 0, 2, 17, 0, 0, 0};
    CHECK(memcmp(bytes + 32, header, 8) == 0 && memcmp(bytes + 40, data, 9) == 0);
    for (size_t i = 49; i < 80; ++i)
        CHECK(bytes[i] == 0);
    CHECK(sp5_video_next(v, &wire).status == SP5_OK && bytes[2] == 0 && bytes[3] == 0);
    CHECK((le32(bytes + 28) >> 12 & 1023) == 1);
    CHECK(sp5_video_next(v, &wire).status == SP5_OK);
    CHECK(sp5_video_next(v, &wire).reason == SP5_REASON_WOULD_BLOCK);
    /* Multi-block with SOF/EOF per block and contiguous RTP, payload ordering. */
    f.bitstream.size = sizeof(data);
    f.repeated = true;
    CHECK(sp5_video_begin(v, &f).status == SP5_OK);
    size_t payload_offset = 0, packets = 0, blocks = 0;
    uint16_t sequence = 2;
    while (sp5_video_next(v, &wire).status == SP5_OK) {
        CHECK(bytes[2] == (uint8_t)(sequence >> 8) && bytes[3] == (uint8_t)sequence);
        ++sequence;
        ++packets;
        uint32_t info = le32(bytes + 28);
        size_t d = info >> 22, index = (info >> 12) & 1023;
        CHECK(d > 0 && d + (d * ((info >> 4) & 255) + 99) / 100 <= 255);
        if (index >= d)
            continue;
        CHECK(bytes[24] == (uint8_t)(1 | (index == 0 ? 4 : 0) | (index + 1 == d ? 2 : 0)));
        if (index == 0)
            ++blocks;
        for (size_t i = 32; i < 80; ++i, ++payload_offset) {
            if (payload_offset < 8) {
                if (payload_offset == 1 || payload_offset == 2)
                    CHECK(bytes[i] == 0);
            } else if (payload_offset - 8 < sizeof(data))
                CHECK(bytes[i] == data[payload_offset - 8]);
            else
                CHECK(bytes[i] == 0);
        }
    }
    CHECK(blocks == 2 && packets > 340 && payload_offset >= sizeof(data) + 8);
    f.bitstream.size = SIZE_MAX;
    CHECK(sp5_video_begin(v, &f).status == SP5_ERROR);
    f.bitstream.size = 9;
    CHECK(sp5_video_begin(v, &f).status == SP5_OK);
    sp5_video_cancel(v);
    CHECK(sp5_video_next(v, &wire).reason == SP5_REASON_WOULD_BLOCK);
    sp5_video_destroy(v);
    c.packet_size = 63;
    CHECK(sp5_video_create(&c) == NULL);
    CHECK(sp5_video_latency((sp5_optional_u64){true, 49}, false) == 0);
    CHECK(sp5_video_latency((sp5_optional_u64){true, 50}, false) == 1);
    CHECK(sp5_video_latency((sp5_optional_u64){true, 6553449}, false) == 65534);
    CHECK(sp5_video_latency((sp5_optional_u64){true, 6553450}, false) == 65535);
    CHECK(sp5_video_latency((sp5_optional_u64){true, UINT64_MAX}, false) == 65535);
    CHECK(sp5_video_latency((sp5_optional_u64){true, UINT64_MAX}, true) == 0);
    CHECK(sp5_video_latency((sp5_optional_u64){0}, false) == 0);
    return 0;
}
static int encryption(void)
{
    sp5_video_config c = {
        .packet_size = 64, .fec_percent = 20, .minimum_parity = 2, .encrypted = true};
    memset(c.key, 0x42, 16);
    sp5_video *v = sp5_video_create(&c);
    CHECK(v != NULL);
    uint8_t data[3] = {1, 2, 3}, bytes[SP5_VIDEO_WIRE_LIMIT], plain[SP5_VIDEO_WIRE_LIMIT];
    sp5_buffer wire = {bytes, sizeof(bytes), 0};
    sp5_video_frame_input f = {.bitstream = {data, 3}, .frame_index = 42};
    size_t before = allocations;
    CHECK(sp5_video_begin(v, &f).status == SP5_OK);
    CHECK(sp5_video_next(v, &wire).status == SP5_OK);
    CHECK(allocations == before); /* Includes the very first frame/shard. */
    CHECK(wire.size == 112 && le32(bytes + 12) == 42 && bytes[11] == 'V');
    /* Wire facts: notes §§5/6 @ sun d0b4a88 and mcc 41cedb8. Independent
     * oracle: tools/moonlight-mcp crypto.rs, locked Rust aes-gcm 0.10.3,
     * key 42x16, IV 00x11|'V', frame 42, payload {1,2,3}, 200% minimum FEC. */
    const uint8_t expected_tag[16] = {0x63, 0xcd, 0xc9, 0x62, 0x88, 0x4f, 0x24, 0x89,
                                      0x5f, 0xc2, 0xed, 0xee, 0x3f, 0x40, 0x9c, 0x2f};
    const uint8_t expected_cipher[80] = {
        0x34, 0xb1, 0x23, 0xff, 0x9e, 0xda, 0x80, 0xf1, 0x82, 0xfa, 0x95, 0xe8, 0x40, 0xbb,
        0x4e, 0xbd, 0xdb, 0x18, 0xd5, 0x23, 0xc6, 0xb3, 0x26, 0x38, 0x31, 0x7c, 0xbf, 0xb3,
        0x6a, 0x1d, 0x43, 0x54, 0x73, 0x3e, 0x5e, 0x18, 0x9a, 0xd7, 0xe7, 0xd7, 0x0b, 0x0d,
        0x1e, 0x96, 0xd5, 0xdc, 0x87, 0xb3, 0xec, 0xf4, 0x78, 0x82, 0xe6, 0x6a, 0x71, 0x1d,
        0x34, 0x5c, 0x1d, 0x91, 0xd1, 0x02, 0x27, 0x55, 0x6e, 0xb8, 0x48, 0xc7, 0x5c, 0x2c,
        0x54, 0x47, 0xff, 0x95, 0x1e, 0x5c, 0x58, 0x59, 0x64, 0x34};
    CHECK(memcmp(bytes + 16, expected_tag, sizeof(expected_tag)) == 0);
    CHECK(memcmp(bytes + 32, expected_cipher, sizeof(expected_cipher)) == 0);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    CHECK(ctx != NULL);
    int n = 0, tail = 0;
    CHECK(EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, c.key, bytes) == 1);
    CHECK(EVP_DecryptUpdate(ctx, plain, &n, bytes + 32, 80) == 1);
    CHECK(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, bytes + 16) == 1);
    CHECK(EVP_DecryptFinal_ex(ctx, plain + n, &tail) == 1 && n + tail == 80);
    CHECK(plain[0] == 0x90 && le32(plain + 20) == 42 && memcmp(plain + 40, data, 3) == 0);
    EVP_CIPHER_CTX_free(ctx);
    for (unsigned i = 1; i < 3; ++i) {
        before = allocations;
        CHECK(sp5_video_next(v, &wire).status == SP5_OK);
        CHECK(allocations == before && bytes[0] == i && bytes[11] == 'V');
    }
    CHECK(sp5_video_begin(v, &f).status == SP5_OK);
    CHECK(sp5_video_next(v, &wire).status == SP5_OK && bytes[0] == 3);
    sp5_video_cancel(v);
    CHECK(sp5_video_begin(v, &f).status == SP5_OK);
    CHECK(sp5_video_next(v, &wire).status == SP5_OK && bytes[0] == 4);
    sp5_video_destroy(v);
    return 0;
}
int main(void)
{
    CHECK(CRYPTO_set_mem_functions(allocate, reallocate, release) == 1);
    CHECK(vectors() == 0);
    CHECK(packetization() == 0);
    CHECK(encryption() == 0);
    return 0;
}
