/* T110 synthetic harness: RTSP framing, SDP negotiation against labeled fixture
 * modes, and the stateful session exchange in plain and encrypted launches.
 * No socket, console adapter or real client; the GCM key is a fixed test key.
 *
 * Raw input: stateless parse/negotiate, plus one plain exchange.
 * "SP5S" input: [u8 variant] then up to RECORD_LIMIT records
 * [u16 length LE][u8 flags][request]. Variant bit0 selects the encrypted
 * launch, where each record is sealed in the documented envelope
 * (protocol-notes.md §3) unless flags bit0 sends it raw; bit1 reuses the
 * previous sequence (replay); bit2 corrupts the tag; bit3 lets the lifecycle
 * owner activate the session after this record once ANNOUNCE configured it. */
#include "protocol/rtsp.h"
#include "harness.h"
#include "protocol/negotiation.h"
#include <openssl/evp.h>
#include <string.h>

enum { RECORD_LIMIT = 16, INPUT_LIMIT = 65536, RESPONSE_LIMIT = SP5_RTSP_LIMIT + 64 };
static const uint8_t test_key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                     0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

static sp5_result missing(void *context, sp5_store_file file, sp5_buffer *output)
{
    (void)context;
    (void)file;
    (void)output;
    return sp5_failure(SP5_REASON_WOULD_BLOCK);
}
static sp5_result replace(void *context, sp5_store_file file, sp5_bytes contents)
{
    (void)context;
    (void)file;
    (void)contents;
    return sp5_success();
}

static sp5_host fixture_host(void)
{
    /* Labeled synthetic fixture modes; never a console capability claim. */
    return (sp5_host){.supported_mode_count = 2,
                      .supported_modes = {{1280, 720, 60, "h264", SP5_SDR, 100, 1000},
                                          {1920, 1080, 60, "hevc", SP5_HDR_PQ_BT2020, 200, 2000}}};
}

static void stateless(sp5_bytes wire)
{
    sp5_request request;
    sp5_bitrate bitrate;
    sp5_negotiated chosen;
    sp5_host host = fixture_host();
    if (sp5_sdp_negotiate(wire, &host, &chosen).status == SP5_OK) {
        fuzz_require(chosen.mode_index < host.supported_mode_count);
    }
    (void)sp5_negotiate_bitrate(wire, 100, 1000, &bitrate);
    if (sp5_rtsp_parse(wire, SP5_RTSP_LIMIT, &request).status == SP5_OK) {
        fuzz_span(wire, request.method);
        fuzz_span(wire, request.target);
        fuzz_span(wire, request.body);
        if (sp5_sdp_negotiate(request.body, &host, &chosen).status == SP5_OK) {
            fuzz_require(chosen.mode_index < host.supported_mode_count);
            const sp5_host_mode *mode = &host.supported_modes[chosen.mode_index];
            fuzz_require(chosen.bitrate.chosen_kbps >= mode->minimum_bitrate_kbps &&
                         chosen.bitrate.chosen_kbps <= mode->maximum_bitrate_kbps);
        }
        (void)sp5_negotiate_bitrate(request.body, 100, 1000, &bitrate);
    }
}

typedef struct {
    sp5_devices *devices;
    sp5_authority *authority;
    sp5_session *session;
    sp5_host host;
    sp5_rtsp *rtsp;
    bool encrypted, activated, played;
    uint32_t sequence, accepted;
    uint64_t now;
} exchange_state;

static void open_exchange(exchange_state *state, bool encrypted)
{
    memset(state, 0, sizeof(*state));
    state->encrypted = encrypted;
    state->now = 100;
    sp5_storage_ops storage = {.read = missing, .replace = replace};
    state->devices = sp5_devices_create(&storage);
    fuzz_require(state->devices != NULL);
    state->authority = sp5_authority_create(state->devices);
    fuzz_require(state->authority != NULL);
    state->session = sp5_session_create(state->authority);
    fuzz_require(state->session != NULL);
    static const uint8_t identity[] = "synthetic-rtsp-fuzz-client";
    sp5_bytes certificate = {identity, sizeof(identity) - 1};
    fuzz_require(sp5_authority_pair(state->authority, certificate).status == SP5_OK);
    fuzz_require(sp5_session_admit(state->session, certificate, 1).status == SP5_OK);
    state->host = fixture_host();
    sp5_rtsp_config config = {.host = &state->host,
                              .devices = state->devices,
                              .session = state->session,
                              .verified_certificate = certificate,
                              .peer = "10.1.2.3",
                              .audio_port = 48000,
                              .video_port = 47998,
                              .control_port = 47999,
                              .launch = {.session_id = 1,
                                         .width = 1280,
                                         .height = 720,
                                         .fps = 60,
                                         .control_connect_data = 42,
                                         .encrypted_rtsp = encrypted}};
    memcpy(config.launch.key, test_key, sizeof(test_key));
    state->rtsp = sp5_rtsp_create(&config);
    fuzz_require(state->rtsp != NULL);
}

static void close_exchange(exchange_state *state)
{
    sp5_rtsp_destroy(state->rtsp);
    sp5_session_destroy(state->session);
    sp5_authority_destroy(state->authority);
    sp5_devices_destroy(state->devices);
}

/* Independent envelope: be32(0x80000000|len), be32(seq), 16-byte tag, GCM
 * ciphertext; nonce = le32(seq) || 0 ... || 'C' 'R' (client to host). */
static size_t seal(sp5_bytes plain, uint32_t sequence, uint8_t *wire)
{
    uint8_t iv[12] = {0};
    for (unsigned i = 0; i < 4; ++i) {
        iv[i] = (uint8_t)(sequence >> (8 * i));
    }
    iv[10] = 'C';
    iv[11] = 'R';
    uint32_t length = UINT32_C(0x80000000) | (uint32_t)plain.size;
    for (unsigned i = 0; i < 4; ++i) {
        wire[i] = (uint8_t)(length >> (24 - 8 * i));
        wire[4 + i] = (uint8_t)(sequence >> (24 - 8 * i));
    }
    EVP_CIPHER_CTX *cipher = EVP_CIPHER_CTX_new();
    fuzz_require(cipher != NULL);
    int written = 0;
    int final = 0;
    fuzz_require(EVP_EncryptInit_ex(cipher, EVP_aes_128_gcm(), NULL, test_key, iv) == 1);
    fuzz_require(EVP_EncryptUpdate(cipher, wire + 24, &written, plain.data, (int)plain.size) == 1);
    fuzz_require(EVP_EncryptFinal_ex(cipher, wire + 24 + written, &final) == 1);
    fuzz_require(EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_GET_TAG, 16, wire + 8) == 1);
    EVP_CIPHER_CTX_free(cipher);
    fuzz_require((size_t)written + (size_t) final == plain.size);
    return plain.size + 24;
}

static bool starts(sp5_bytes value, const char *prefix)
{
    size_t size = strlen(prefix);
    return value.size >= size && memcmp(value.data, prefix, size) == 0;
}

static void exchange(exchange_state *state, sp5_bytes request, uint8_t flags)
{
    static uint8_t sealed[SP5_RTSP_LIMIT + 24];
    static uint8_t output[RESPONSE_LIMIT];
    sp5_buffer response = {output, sizeof(output), 0};
    sp5_bytes wire = request;
    uint32_t sequence = 0;
    bool authentic = false;
    if (state->encrypted && (flags & 1) == 0 && request.size <= SP5_RTSP_LIMIT) {
        sequence = (flags & 2) != 0 && state->sequence != 0 ? state->sequence : ++state->sequence;
        wire = (sp5_bytes){sealed, seal(request, sequence, sealed)};
        if ((flags & 4) != 0) {
            sealed[8] ^= 1;
        } else {
            authentic = true;
        }
    }
    sp5_result result = sp5_rtsp_exchange(state->rtsp, "10.1.2.3", false, wire, ++state->now,
                                          (sp5_optional_u64){0}, &response);
    fuzz_require(response.size <= response.capacity);
    if (state->encrypted) {
        /* Unsealed, tampered or replayed requests are never processed. */
        fuzz_require(result.status != SP5_OK || (authentic && sequence > state->accepted));
        if (result.status == SP5_OK ||
            (authentic && sequence > state->accepted && result.reason != SP5_REASON_OWNERSHIP)) {
            state->accepted = sequence;
        }
        if (response.size != 0) {
            fuzz_require(response.size >= 24 && output[0] == 0x80);
        }
    }
    if (result.status == SP5_OK && starts(request, "PLAY ")) {
        state->played = true;
    }
    if ((flags & 8) != 0 && !state->activated) {
        sp5_session_view view = sp5_session_read(state->session);
        if (view.mode_configured && view.state == SP5_SESSION_NEGOTIATING) {
            fuzz_require(sp5_session_activate(state->session).status == SP5_OK);
            state->activated = true;
        }
    }
    /* PLAY succeeds only for a session ANNOUNCE configured and the owner activated. */
    if (sp5_rtsp_playing(state->rtsp)) {
        sp5_session_view view = sp5_session_read(state->session);
        fuzz_require(state->played && state->activated && view.mode_configured);
        sp5_negotiated chosen;
        fuzz_require(sp5_rtsp_negotiated(state->rtsp, &chosen).status == SP5_OK);
        fuzz_require(chosen.mode_index < state->host.supported_mode_count);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > INPUT_LIMIT) {
        return 0;
    }
    sp5_bytes wire = {data, size};
    exchange_state state;
    if (size >= 5 && memcmp(data, "SP5S", 4) == 0) {
        open_exchange(&state, (data[4] & 1) != 0);
        size_t at = 5;
        for (unsigned count = 0; count < RECORD_LIMIT && size - at >= 3; ++count) {
            size_t length = (size_t)data[at] | ((size_t)data[at + 1] << 8);
            uint8_t flags = data[at + 2];
            at += 3;
            if (length > size - at) {
                length = size - at;
            }
            sp5_bytes request = {data + at, length};
            fuzz_span(wire, request);
            if (length <= SP5_RTSP_LIMIT + 24) {
                exchange(&state, request, flags);
            }
            at += length;
        }
        close_exchange(&state);
        return 0;
    }
    if (size > SP5_RTSP_LIMIT + 1U) {
        return 0;
    }
    stateless(wire);
    open_exchange(&state, false);
    exchange(&state, wire, 0);
    close_exchange(&state);
    return 0;
}
