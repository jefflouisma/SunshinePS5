#include "protocol/rtsp.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            if (fputs(__FILE__ ": " #c "\n", stderr) == EOF)                                       \
                return 2;                                                                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static sp5_bytes bytes(const char *s)
{
    return (sp5_bytes){(const uint8_t *)s, strlen(s)};
}
static const char sdp[] =
    "v=0\r\na=x-nv-video[0].clientViewportWd:1280 \r\na=x-nv-video[0].clientViewportHt:720 "
    "\r\na=x-nv-video[0].maxFPS:60 \r\na=x-ml-video.configuredBitrateKbps:1200 "
    "\r\na=x-nv-vqos[0].bitStreamFormat:0 \r\na=x-nv-video[0].dynamicRangeMode:0 "
    "\r\na=x-nv-general.useReliableUdp:13 \r\na=x-ss-general.encryptionEnabled:1 "
    "\r\na=x-ml-general.featureFlags:3 \r\na=x-nv-video[0].packetSize:1392 "
    "\r\na=x-nv-audio.surround.numChannels:2 \r\na=x-nv-audio.surround.channelMask:3 \r\n";
typedef struct {
    uint8_t data[40000];
    size_t size;
} files;
static sp5_result read_file(void *ctx, sp5_store_file file, sp5_buffer *out)
{
    files *f = ctx;
    if (file != SP5_STORE_DEVICES || f->size == 0)
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    return sp5_buffer_append(out, (sp5_bytes){f->data, f->size});
}
static sp5_result write_file(void *ctx, sp5_store_file file, sp5_bytes data)
{
    files *f = ctx;
    if (file != SP5_STORE_DEVICES || data.size > sizeof(f->data))
        return sp5_failure(SP5_REASON_CAPACITY);
    memcpy(f->data, data.data, data.size);
    f->size = data.size;
    return sp5_success();
}
static sp5_result exchange(sp5_rtsp *r, const char *method, const char *target, unsigned seq,
                           const char *body, sp5_buffer *out)
{
    char wire[SP5_RTSP_LIMIT];
    int n = snprintf(wire, sizeof(wire),
                     "%s %s RTSP/1.0\r\nCSeq: %u\r\nSession: DEADBEEFCAFE\r\nContent-Type: "
                     "application/sdp\r\nContent-Length: %zu\r\n\r\n%s",
                     method, target, seq, strlen(body), body);
    if (n < 0 || (size_t)n >= sizeof(wire))
        return sp5_failure(SP5_REASON_CAPACITY);
    out->size = 0;
    return sp5_rtsp_exchange(r, "10.1.2.3", false, (sp5_bytes){(const uint8_t *)wire, (size_t)n},
                             100, (sp5_optional_u64){0}, out);
}
/* Independent encrypted-RTSP oracle (notes §3): raw frame, EVP GCM and
 * directional nonce. All identities/keys in this unit test are synthetic. */
static int encrypted_options(const sp5_rtsp_config *config)
{
    sp5_rtsp_config encrypted = *config;
    encrypted.launch.encrypted_rtsp = true;
    sp5_rtsp *r = sp5_rtsp_create(&encrypted);
    CHECK(r != NULL);
    EVP_CIPHER_CTX *crypto = EVP_CIPHER_CTX_new();
    CHECK(crypto != NULL);
    uint8_t wire[512] = {0}, output[SP5_RTSP_LIMIT], plaintext[512], iv[12] = {0};
    sp5_buffer out = {output, sizeof(output), 0};
    const char request[] = "OPTIONS x RTSP/1.0\r\nCSeq: 1\r\n\r\n";
    const uint32_t size = sizeof(request) - 1;
    wire[0] = 0x80;
    wire[3] = (uint8_t)size;
    wire[7] = 1;
    iv[0] = 1;
    iv[10] = 'C';
    iv[11] = 'R';
    int n = 0, tail = 0;
    CHECK(EVP_EncryptInit_ex(crypto, EVP_aes_128_gcm(), NULL, encrypted.launch.key, iv) == 1);
    CHECK(EVP_EncryptUpdate(crypto, wire + 24, &n, (const uint8_t *)request, (int)size) == 1);
    CHECK(EVP_EncryptFinal_ex(crypto, wire + 24 + n, &tail) == 1);
    CHECK(EVP_CIPHER_CTX_ctrl(crypto, EVP_CTRL_GCM_GET_TAG, 16, wire + 8) == 1);
    CHECK(sp5_rtsp_exchange(r, "10.1.2.4", false, (sp5_bytes){wire, 24 + size}, 1,
                            (sp5_optional_u64){0}, &out)
              .status == SP5_ERROR);
    CHECK(sp5_rtsp_exchange(r, "10.1.2.3", false, (sp5_bytes){wire, 24 + size}, 1,
                            (sp5_optional_u64){0}, &out)
              .status == SP5_OK);
    CHECK(out.size > 24 && output[0] == 0x80 && output[7] == 1);
    iv[10] = 'H';
    CHECK(EVP_DecryptInit_ex(crypto, EVP_aes_128_gcm(), NULL, encrypted.launch.key, iv) == 1);
    CHECK(EVP_DecryptUpdate(crypto, plaintext, &n, output + 24, (int)(out.size - 24)) == 1);
    CHECK(EVP_CIPHER_CTX_ctrl(crypto, EVP_CTRL_GCM_SET_TAG, 16, output + 8) == 1);
    CHECK(EVP_DecryptFinal_ex(crypto, plaintext + n, &tail) == 1);
    CHECK(memcmp(plaintext, "RTSP/1.0 200", 12) == 0);
    out.size = 0;
    CHECK(sp5_rtsp_exchange(r, "10.1.2.3", false, (sp5_bytes){wire, 24 + size}, 1,
                            (sp5_optional_u64){0}, &out)
                  .status == SP5_ERROR &&
          out.size == 0);
    wire[7] = 2;
    wire[8] ^= 1;
    CHECK(sp5_rtsp_exchange(r, "10.1.2.3", false, (sp5_bytes){wire, 24 + size}, 1,
                            (sp5_optional_u64){0}, &out)
              .status == SP5_ERROR);
    CHECK(sp5_rtsp_exchange(r, "10.1.2.3", false, bytes(request), 1, (sp5_optional_u64){0}, &out)
              .status == SP5_ERROR);
    EVP_CIPHER_CTX_free(crypto);
    sp5_rtsp_destroy(r);
    return 0;
}
int main(void)
{
    sp5_request parsed;
    const char *bad_requests[] = {
        "OPTIONS x RTSP/1.0\r\nCSeq: 1\r\ncontent-length: 0\r\nContent-Length: 0\r\n\r\n",
        "OPTIONS x RTSP/1.0\r\nCSeq: 0\r\n\r\n",
        "OPTIONS x RTSP/1.0\r\nCSeq: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "OPTIONS x RTSP/1.0\r\nCSeq: 1\r\n folded: value\r\n\r\n",
        "OPTIONS x RTSP/1.0\r\nCSeq: 1\r\n\r\nx"};
    for (size_t i = 0; i < sizeof(bad_requests) / sizeof(bad_requests[0]); ++i) {
        CHECK(sp5_rtsp_parse(bytes(bad_requests[i]), SP5_RTSP_LIMIT, &parsed).status == SP5_ERROR);
        CHECK(parsed.method.data == NULL);
    }
    sp5_host host;
    CHECK(sp5_host_init(&host, "fixture", "13.60", "10.1.2.2").status == SP5_OK);
    sp5_negotiated chosen;
    CHECK(sp5_sdp_negotiate(bytes(sdp), &host, &chosen).status == SP5_ERROR);
    host.supported_mode_count = 1;
    host.supported_modes[0] = (sp5_host_mode){1280, 720, 60, "h264", SP5_SDR, 100, 1000};
    CHECK(sp5_sdp_negotiate(bytes(sdp), &host, &chosen).status == SP5_OK);
    CHECK(chosen.mode_index == 0 && chosen.bitrate.requested_kbps == 1200 &&
          chosen.bitrate.chosen_kbps == 1000 && chosen.bitrate.adjustment == SP5_BITRATE_LOWERED);
    /* mcc 41cedb8 SdpGenerator.c:276-304: optional VIDEO/AUDIO are
     * client-selected, not forced by supported/requested advertisements. */
    for (unsigned flags = 1; flags <= 9; flags += 2) {
        char selected[sizeof(sdp)];
        memcpy(selected, sdp, sizeof(sdp));
        char *value = strstr(selected, "encryptionEnabled:1");
        CHECK(value != NULL);
        value[strlen("encryptionEnabled:")] = (char)('0' + flags);
        sp5_result result = sp5_sdp_negotiate(bytes(selected), &host, &chosen);
        CHECK(flags == 9 ? result.status == SP5_ERROR
                         : result.status == SP5_OK && chosen.encryption_flags == flags);
    }
    char legacy_sdp[sizeof(sdp)];
    memcpy(legacy_sdp, sdp, sizeof(sdp));
    char *legacy = strstr(legacy_sdp, "encryptionEnabled:1");
    CHECK(legacy != NULL);
    legacy[strlen("encryptionEnabled:")] = '0';
    CHECK(sp5_sdp_negotiate(bytes(legacy_sdp), &host, &chosen).status == SP5_ERROR);
    char invalid[SP5_SDP_LIMIT];
    int n = snprintf(invalid, sizeof(invalid), "%sa=x-nv-video[0].maxFPS:60\r\n", sdp);
    CHECK(n > 0 && (size_t)n < sizeof(invalid));
    CHECK(sp5_sdp_negotiate(bytes(invalid), &host, &chosen).status == SP5_ERROR);
    memcpy(invalid, sdp, sizeof(sdp));
    char *width = strstr(invalid, "Wd:1280");
    CHECK(width != NULL);
    width[3] = '9';
    CHECK(sp5_sdp_negotiate(bytes(invalid), &host, &chosen).status == SP5_ERROR);
    uint8_t storage[SP5_RTSP_LIMIT];
    sp5_buffer out = {storage, sizeof(storage), 0};
    CHECK(sp5_sdp_describe(&host, &out).status == SP5_OK);
    CHECK(out.size > 0 && memchr(storage, 0, out.size) == NULL);
    CHECK(out.size < sizeof(storage));
    storage[out.size] = 0;
    CHECK(strstr((const char *)storage, "encryptionSupported:7\r\n") != NULL);
    CHECK(strstr((const char *)storage, "encryptionRequested:1\r\n") != NULL);
    CHECK(strstr((const char *)storage, "featureFlags:0\r\n") != NULL);
    host.controller_touch_events = true;
    out.size = 0;
    CHECK(sp5_sdp_describe(&host, &out).status == SP5_OK && out.size < sizeof(storage));
    storage[out.size] = 0;
    CHECK(strstr((const char *)storage, "featureFlags:2\r\n") != NULL);
    host.controller_touch_events = false;
    files f = {0};
    sp5_storage_ops ops = {&f, read_file, write_file, NULL};
    sp5_devices *devices = sp5_devices_create(&ops);
    CHECK(devices != NULL);
    CHECK(sp5_devices_load(devices).status == SP5_OK);
    sp5_authority *authority = sp5_authority_create(devices);
    CHECK(authority != NULL);
    sp5_session *session = sp5_session_create(authority);
    CHECK(session != NULL);
    sp5_bytes cert = bytes("synthetic-rtsp-certificate");
    CHECK(sp5_authority_pair(authority, cert).status == SP5_OK);
    CHECK(sp5_session_admit(session, cert, 1).status == SP5_OK);
    sp5_rtsp_config config = {
        .host = &host,
        .devices = devices,
        .session = session,
        .verified_certificate = cert,
        .peer = "10.1.2.3",
        .audio_port = 48000,
        .video_port = 47998,
        .control_port = 47999,
        .launch = {
            .session_id = 1, .width = 1280, .height = 720, .fps = 60, .control_connect_data = 42}};
    CHECK(encrypted_options(&config) == 0);
    sp5_rtsp *r = sp5_rtsp_create(&config);
    CHECK(r != NULL);
    CHECK(exchange(r, "OPTIONS", "rtsp://10.1.2.2:48010", 1, "", &out).status == SP5_OK);
    CHECK(memcmp(storage, "RTSP/1.0 200", 12) == 0);
    CHECK(exchange(r, "OPTIONS", "x", 1, "", &out).status == SP5_ERROR); /* replay */
    CHECK(exchange(r, "ANNOUNCE", "streamid=control/13/0", 2, sdp, &out).status == SP5_ERROR);
    CHECK(!sp5_session_read(session).mode_configured);
    CHECK(exchange(r, "DESCRIBE", "x", 3, "", &out).status == SP5_OK);
    CHECK(exchange(r, "SETUP", "streamid=audio/0/0", 4, "", &out).status == SP5_OK);
    CHECK(exchange(r, "SETUP", "streamid=video/0/0", 5, "", &out).status == SP5_OK);
    CHECK(exchange(r, "SETUP", "streamid=control/13/0", 6, "", &out).status == SP5_OK);
    CHECK(exchange(r, "ANNOUNCE", "streamid=control/13/0", 7, invalid, &out).status == SP5_ERROR);
    CHECK(!sp5_session_read(session).mode_configured);
    CHECK(exchange(r, "ANNOUNCE", "streamid=control/13/0", 8, sdp, &out).status == SP5_OK);
    CHECK(sp5_rtsp_negotiated(r, &chosen).status == SP5_OK);
    CHECK(sp5_session_read(session).chosen_bitrate_kbps == 1000);
    CHECK(exchange(r, "PLAY", "/", 9, "", &out).status == SP5_ERROR);
    CHECK(sp5_session_activate(session).status == SP5_OK);
    CHECK(exchange(r, "PLAY", "/", 10, "", &out).status == SP5_OK);
    CHECK(sp5_authority_revoke(authority, cert).status == SP5_OK);
    CHECK(exchange(r, "OPTIONS", "x", 11, "", &out).status == SP5_ERROR && out.size == 0);
    sp5_rtsp_destroy(r);
    sp5_session_destroy(session);
    sp5_authority_destroy(authority);
    sp5_devices_destroy(devices);
    return 0;
}
