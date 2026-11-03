#include "opus_encoder.h"
#include "runtime.h"
#include <opus.h>
#include <stdlib.h>
#include <string.h>

struct sp5_opus_encoder {
    OpusEncoder *encoder;
    uint32_t sample_rate;
    uint8_t channels;
    uint32_t frame_samples;
};

sp5_opus_encoder *sp5_opus_encoder_create(uint32_t sample_rate, uint8_t channels,
                                          uint32_t bitrate_bps, uint32_t duration_ms,
                                          sp5_result *result)
{
    if (result != NULL)
        *result = sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (sample_rate != 48000 || channels != 2 || bitrate_bps == 0 ||
        (duration_ms != 5 && duration_ms != 10)) {
        if (result != NULL)
            *result = sp5_failure(SP5_REASON_UNSUPPORTED);
        return NULL;
    }
    sp5_opus_encoder *encoder = calloc(1, sizeof(*encoder));
    if (encoder == NULL) {
        if (result != NULL)
            *result = sp5_failure(SP5_REASON_CAPACITY);
        return NULL;
    }
    int error = OPUS_OK;
    encoder->encoder =
        opus_encoder_create((opus_int32)sample_rate, (int)channels, OPUS_APPLICATION_AUDIO, &error);
    if (encoder->encoder == NULL || error != OPUS_OK) {
        free(encoder);
        if (result != NULL)
            *result = sp5_failure(SP5_REASON_PLATFORM);
        return NULL;
    }
    /* 5 ms packets are the host's smallest declared duration. CBR keeps the
     * diagnostic bitrate bounded and predictable. */
    if (opus_encoder_ctl(encoder->encoder, OPUS_SET_BITRATE((opus_int32)bitrate_bps)) != OPUS_OK ||
        opus_encoder_ctl(encoder->encoder, OPUS_SET_VBR(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder->encoder, OPUS_SET_DTX(0)) != OPUS_OK) {
        opus_encoder_destroy(encoder->encoder);
        free(encoder);
        if (result != NULL)
            *result = sp5_failure(SP5_REASON_PLATFORM);
        return NULL;
    }
    encoder->sample_rate = sample_rate;
    encoder->channels = channels;
    encoder->frame_samples = sample_rate * duration_ms / 1000;
    if (result != NULL)
        *result = sp5_success();
    return encoder;
}

void sp5_opus_encoder_destroy(sp5_opus_encoder *encoder)
{
    if (encoder == NULL)
        return;
    if (encoder->encoder != NULL)
        opus_encoder_destroy(encoder->encoder);
    free(encoder);
}

sp5_result sp5_opus_encoder_encode_float(sp5_opus_encoder *encoder, const float *pcm,
                                         size_t capacity, sp5_buffer *output)
{
    if (encoder == NULL || encoder->encoder == NULL || pcm == NULL || output == NULL ||
        output->data == NULL || capacity == 0)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    int encoded = opus_encode_float(encoder->encoder, pcm, (int)encoder->frame_samples,
                                    output->data, (opus_int32)capacity);
    if (encoded < 0)
        return sp5_failure(SP5_REASON_PLATFORM);
    output->size = (size_t)encoded;
    return sp5_success();
}

uint32_t sp5_opus_encoder_frame_samples(const sp5_opus_encoder *encoder)
{
    return encoder == NULL ? 0 : encoder->frame_samples;
}
