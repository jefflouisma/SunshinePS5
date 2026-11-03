#include "fec.h"
#include <stdint.h>
#include <string.h>

/* Wire code facts only: nanors a9b1636f9c98dbfdd0daeaf6982ccd24d1c30f29,
 * rs.c:111-139, GF polynomial 285 in deps/obl/gf2_8_tables.h. A systematic
 * Cauchy parity coefficient is inverse((P+column) XOR row). This independent
 * table-based encoder does not incorporate upstream implementation code. */
void sp5_fec_init(sp5_fec *fec)
{
    if (fec == NULL)
        return;
    memset(fec, 0, sizeof(*fec));
    unsigned value = 1;
    for (unsigned i = 0; i < 255; ++i) {
        fec->exponent[i] = (uint8_t)value;
        fec->logarithm[value] = (uint8_t)i;
        value <<= 1;
        if (value & 256U)
            value ^= 0x11dU;
    }
    for (size_t i = 255; i < sizeof(fec->exponent); ++i)
        fec->exponent[i] = fec->exponent[i - 255];
}
static uint8_t multiply(const sp5_fec *fec, uint8_t a, uint8_t b)
{
    return a == 0 || b == 0 ? 0 : fec->exponent[fec->logarithm[a] + fec->logarithm[b]];
}
static sp5_result validate(const sp5_fec *fec, uint8_t *const shards[], size_t data, size_t parity,
                           size_t size)
{
    if (fec == NULL || shards == NULL || data == 0 || data > SP5_FEC_SHARDS ||
        parity > SP5_FEC_SHARDS - data || size == 0 || size > SP5_FEC_SIZE_LIMIT)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    for (size_t i = 0; i < data + parity; ++i) {
        if (shards[i] == NULL || (uintptr_t)shards[i] > UINTPTR_MAX - size)
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        for (size_t j = 0; j < i; ++j) {
            uintptr_t a = (uintptr_t)shards[i], b = (uintptr_t)shards[j];
            if (a < b + size && b < a + size)
                return sp5_failure(SP5_REASON_OWNERSHIP);
        }
    }
    return sp5_success();
}
static void encode_row(const sp5_fec *fec, uint8_t *const shards[], size_t data, size_t row,
                       size_t size, const uint8_t *coefficients)
{
    memset(shards[data + row], 0, size);
    for (size_t column = 0; column < data; ++column)
        for (size_t byte = 0; byte < size; ++byte)
            shards[data + row][byte] ^= multiply(fec, coefficients[column], shards[column][byte]);
}
sp5_result sp5_fec_encode(const sp5_fec *fec, uint8_t *const shards[], size_t data, size_t parity,
                          size_t size)
{
    sp5_result result = validate(fec, shards, data, parity, size);
    if (result.status != SP5_OK)
        return result;
    uint8_t coefficients[SP5_FEC_SHARDS];
    for (size_t row = 0; row < parity; ++row) {
        for (size_t column = 0; column < data; ++column) {
            uint8_t value = (uint8_t)((parity + column) ^ row);
            coefficients[column] = fec->exponent[255 - fec->logarithm[value]];
        }
        encode_row(fec, shards, data, row, size, coefficients);
    }
    return sp5_success();
}
sp5_result sp5_fec_audio(const sp5_fec *fec, uint8_t *const shards[6], size_t size)
{
    /* notes §8, mcc 41cedb8 RtpAudioQueue.c:48-60, Sunshine d0b4a88
     * stream.cpp:1871-1877: audio replaces the default parity matrix. */
    const uint8_t matrix[2][4] = {{0x77, 0x40, 0x38, 0x0e}, {0xc7, 0xa7, 0x0d, 0x6c}};
    sp5_result result = validate(fec, shards, 4, 2, size);
    if (result.status != SP5_OK)
        return result;
    for (size_t row = 0; row < 2; ++row)
        encode_row(fec, shards, 4, row, size, matrix[row]);
    return sp5_success();
}
