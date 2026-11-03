#include "rtsp.h"

#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    sp5_bytes name, value;
} header;
typedef struct {
    sp5_request request;
    header headers[64];
    size_t count;
} message;
static bool equal(sp5_bytes s, const char *text)
{
    return s.size == strlen(text) && memcmp(s.data, text, s.size) == 0;
}
static bool equal_case(sp5_bytes s, const char *text)
{
    if (s.size != strlen(text))
        return false;
    for (size_t i = 0; i < s.size; ++i) {
        uint8_t c = s.data[i];
        if (c >= 'A' && c <= 'Z')
            c = (uint8_t)(c + 'a' - 'A');
        if (c != (uint8_t)text[i])
            return false;
    }
    return true;
}
static bool line(sp5_bytes wire, size_t *offset, sp5_bytes *out)
{
    size_t start = *offset;
    for (size_t i = start; i + 1 < wire.size; ++i) {
        if (wire.data[i] == '\r' && wire.data[i + 1] == '\n') {
            *out = (sp5_bytes){wire.data + start, i - start};
            *offset = i + 2;
            return true;
        }
        if (wire.data[i] < 32 || wire.data[i] == 127)
            return false;
    }
    return false;
}
static bool decimal(sp5_bytes s, uint32_t *out)
{
    uint32_t n = 0;
    if (s.size == 0)
        return false;
    for (size_t i = 0; i < s.size; ++i) {
        uint8_t c = s.data[i];
        if (c < '0' || c > '9' || n > (UINT32_MAX - (c - '0')) / 10U)
            return false;
        n = n * 10U + (c - '0');
    }
    *out = n;
    return true;
}
static sp5_bytes field(const message *m, const char *name)
{
    for (size_t i = 0; i < m->count; ++i)
        if (equal_case(m->headers[i].name, name))
            return m->headers[i].value;
    return (sp5_bytes){0};
}
static sp5_result parse(sp5_bytes wire, size_t limit, message *m)
{
    *m = (message){0};
    if (wire.data == NULL || wire.size == 0 || wire.size > limit || wire.size > SP5_RTSP_LIMIT)
        return sp5_failure(SP5_REASON_CAPACITY);
    size_t offset = 0;
    sp5_bytes first;
    if (!line(wire, &offset, &first))
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    size_t method = 0;
    while (method < first.size && first.data[method] >= 'A' && first.data[method] <= 'Z')
        ++method;
    if (method == 0 || method >= first.size || first.data[method] != ' ')
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    size_t target = method + 1, end = target;
    while (end < first.size && first.data[end] != ' ')
        ++end;
    if (end == target || end >= first.size ||
        !equal((sp5_bytes){first.data + end + 1, first.size - end - 1}, "RTSP/1.0"))
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    m->request.method = (sp5_bytes){first.data, method};
    m->request.target = (sp5_bytes){first.data + target, end - target};
    for (;;) {
        sp5_bytes h;
        if (!line(wire, &offset, &h))
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        if (h.size == 0)
            break;
        size_t colon = 0;
        while (colon < h.size && h.data[colon] != ':') {
            uint8_t c = h.data[colon];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-'))
                return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
            ++colon;
        }
        if (colon == 0 || colon == h.size || m->count == 64)
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        size_t start = colon + 1;
        while (start < h.size && h.data[start] == ' ')
            ++start;
        size_t finish = h.size;
        while (finish > start && h.data[finish - 1] == ' ')
            --finish;
        sp5_bytes name = {h.data, colon};
        for (size_t i = 0; i < m->count; ++i) {
            sp5_bytes other = m->headers[i].name;
            if (other.size == name.size) {
                bool match = true;
                for (size_t j = 0; j < name.size; ++j) {
                    uint8_t a = name.data[j], b = other.data[j];
                    if (a >= 'A' && a <= 'Z')
                        a = (uint8_t)(a + 32);
                    if (b >= 'A' && b <= 'Z')
                        b = (uint8_t)(b + 32);
                    if (a != b)
                        match = false;
                }
                if (match)
                    return sp5_failure(SP5_REASON_CONFLICT);
            }
        }
        m->headers[m->count++] = (header){name, {h.data + start, finish - start}};
    }
    uint32_t length = 0;
    sp5_bytes len = field(m, "content-length");
    if (!decimal(field(m, "cseq"), &m->request.cseq) || m->request.cseq == 0 ||
        (len.data != NULL && !decimal(len, &length)) || length != wire.size - offset ||
        field(m, "transfer-encoding").data != NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    m->request.body = (sp5_bytes){wire.data + offset, length};
    return sp5_success();
}
sp5_result sp5_rtsp_parse(sp5_bytes wire, size_t limit, sp5_request *request)
{
    if (request == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    *request = (sp5_request){0};
    message m;
    sp5_result result = parse(wire, limit, &m);
    if (result.status == SP5_OK)
        *request = m.request;
    return result;
}

struct sp5_rtsp {
    sp5_rtsp_config config;
    sp5_device_lease lease;
    uint8_t certificate[SP5_CERTIFICATE_CAPACITY];
    char peer[128];
    uint32_t cseq, rx_sequence, tx_sequence;
    unsigned setup_mask;
    bool described, announced, playing;
    sp5_negotiated chosen;
    EVP_CIPHER_CTX *encrypt, *decrypt;
    uint8_t plain[SP5_RTSP_LIMIT], reply[SP5_RTSP_LIMIT];
};

sp5_rtsp *sp5_rtsp_create(const sp5_rtsp_config *config)
{
    if (config == NULL || config->host == NULL || config->devices == NULL ||
        config->session == NULL || config->peer == NULL || strlen(config->peer) >= 128 ||
        !sp5_peer_permitted(config->peer, config->on_link) ||
        config->verified_certificate.data == NULL ||
        config->verified_certificate.size > SP5_CERTIFICATE_CAPACITY || config->audio_port == 0 ||
        config->video_port == 0 || config->control_port == 0)
        return NULL;
    sp5_session_view view = sp5_session_read(config->session);
    if (view.id != config->launch.session_id || view.state != SP5_SESSION_NEGOTIATING)
        return NULL;
    bool owns = false;
    for (size_t i = 0; i < sp5_devices_count(config->devices); ++i) {
        const sp5_device *d = sp5_devices_at(config->devices, i);
        if (d != NULL && strcmp(view.device_id, d->id) == 0 &&
            d->certificate_size == config->verified_certificate.size &&
            CRYPTO_memcmp(d->certificate, config->verified_certificate.data, d->certificate_size) ==
                0)
            owns = true;
    }
    if (!owns)
        return NULL;
    sp5_rtsp *r = calloc(1, sizeof(*r));
    if (r == NULL)
        return NULL;
    r->config = *config;
    memcpy(r->certificate, config->verified_certificate.data, config->verified_certificate.size);
    r->config.verified_certificate = (sp5_bytes){r->certificate, config->verified_certificate.size};
    memcpy(r->peer, config->peer, strlen(config->peer) + 1);
    r->config.peer = r->peer;
    r->lease = sp5_devices_lease(config->devices, r->config.verified_certificate);
    r->encrypt = EVP_CIPHER_CTX_new();
    r->decrypt = EVP_CIPHER_CTX_new();
    if (r->encrypt == NULL || r->decrypt == NULL) {
        sp5_rtsp_destroy(r);
        return NULL;
    }
    return r;
}
void sp5_rtsp_destroy(sp5_rtsp *r)
{
    if (r != NULL) {
        EVP_CIPHER_CTX_free(r->encrypt);
        EVP_CIPHER_CTX_free(r->decrypt);
        OPENSSL_cleanse(r, sizeof(*r));
        free(r);
    }
}
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put_be32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)(n >> 24);
    p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)n;
}
static void nonce(uint8_t iv[12], uint32_t sequence, bool host)
{
    memset(iv, 0, 12);
    for (unsigned i = 0; i < 4; ++i)
        iv[i] = (uint8_t)(sequence >> (8 * i));
    iv[10] = host ? 'H' : 'C';
    iv[11] = 'R';
}
static sp5_result decrypt_request(sp5_rtsp *r, sp5_bytes wire, sp5_bytes *plain)
{
    /* protocol-notes.md §3; mcc 41cedb8 RtspConnection.c:93–185;
     * Sunshine d0b4a88 rtsp.cpp:165–231. Full-message GCM, no AAD. */
    if (wire.size < 24 || wire.size - 24 > SP5_RTSP_LIMIT ||
        be32(wire.data) != (UINT32_C(0x80000000) | (uint32_t)(wire.size - 24)))
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    uint32_t seq = be32(wire.data + 4);
    if (seq <= r->rx_sequence)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    uint8_t iv[12];
    nonce(iv, seq, false);
    int n = 0, tail = 0;
    if (EVP_DecryptInit_ex(r->decrypt, EVP_aes_128_gcm(), NULL, r->config.launch.key, iv) != 1 ||
        EVP_DecryptUpdate(r->decrypt, r->plain, &n, wire.data + 24, (int)(wire.size - 24)) != 1 ||
        EVP_CIPHER_CTX_ctrl(r->decrypt, EVP_CTRL_GCM_SET_TAG, 16, (void *)(wire.data + 8)) != 1 ||
        EVP_DecryptFinal_ex(r->decrypt, r->plain + n, &tail) != 1) {
        OPENSSL_cleanse(r->plain, sizeof(r->plain));
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    r->rx_sequence = seq;
    *plain = (sp5_bytes){r->plain, (size_t)(n + tail)};
    return sp5_success();
}
static sp5_result respond(sp5_rtsp *r, sp5_buffer *out, sp5_bytes response)
{
    if (!r->config.launch.encrypted_rtsp)
        return sp5_buffer_append(out, response);
    if (r->tx_sequence == UINT32_MAX || out->capacity - out->size < response.size + 24)
        return sp5_failure(SP5_REASON_CAPACITY);
    uint8_t *p = out->data + out->size;
    uint32_t seq = ++r->tx_sequence;
    uint8_t iv[12];
    nonce(iv, seq, true);
    put_be32(p, UINT32_C(0x80000000) | (uint32_t)response.size);
    put_be32(p + 4, seq);
    int n = 0, tail = 0;
    if (EVP_EncryptInit_ex(r->encrypt, EVP_aes_128_gcm(), NULL, r->config.launch.key, iv) != 1 ||
        EVP_EncryptUpdate(r->encrypt, p + 24, &n, response.data, (int)response.size) != 1 ||
        EVP_EncryptFinal_ex(r->encrypt, p + 24 + n, &tail) != 1 ||
        EVP_CIPHER_CTX_ctrl(r->encrypt, EVP_CTRL_GCM_GET_TAG, 16, p + 8) != 1)
        return sp5_failure(SP5_REASON_PLATFORM);
    out->size += 24 + (size_t)(n + tail);
    return sp5_success();
}
static sp5_result render(sp5_rtsp *r, sp5_buffer *out, unsigned code, const char *extra,
                         sp5_bytes body)
{
    const char *status = code == 200   ? "OK"
                         : code == 401 ? "Unauthorized"
                         : code == 455 ? "Method Not Valid in This State"
                         : code == 461 ? "Unsupported Transport"
                                       : "Bad Request";
    int n = snprintf((char *)r->reply, sizeof(r->reply),
                     "RTSP/1.0 %u %s\r\nCSeq: %u\r\n%sContent-Length: %zu\r\n\r\n", code, status,
                     r->cseq, extra, body.size);
    if (n < 0 || (size_t)n >= sizeof(r->reply) || body.size > sizeof(r->reply) - (size_t)n)
        return sp5_failure(SP5_REASON_CAPACITY);
    if (body.size != 0) {
        memcpy(r->reply + n, body.data, body.size);
    }
    sp5_result result = respond(r, out, (sp5_bytes){r->reply, (size_t)n + body.size});
    return result.status == SP5_OK && code != 200 ? sp5_failure(SP5_REASON_INVALID_ARGUMENT)
                                                  : result;
}
sp5_result sp5_rtsp_exchange(sp5_rtsp *r, const char *peer, bool on_link, sp5_bytes wire,
                             uint64_t now_us, sp5_optional_u64 wall_us, sp5_buffer *out)
{
    if (r == NULL || peer == NULL || wire.data == NULL || out == NULL || out->data == NULL ||
        out->size > out->capacity)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    /* Bound the authenticated launch, not a claimed RTSP Session string. */
    sp5_session_view view = sp5_session_read(r->config.session);
    if (!sp5_peer_permitted(peer, on_link) || strcmp(peer, r->peer) != 0 ||
        !sp5_device_lease_valid(r->lease) || view.id != r->config.launch.session_id ||
        view.state >= SP5_SESSION_ENDING)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    /* Reserve a complete bounded response before mutating negotiation state. */
    if (out->capacity - out->size < SP5_RTSP_LIMIT)
        return sp5_failure(SP5_REASON_CAPACITY);
    sp5_bytes plain = wire;
    if (r->config.launch.encrypted_rtsp) {
        sp5_result result = decrypt_request(r, wire, &plain);
        if (result.status != SP5_OK)
            return result;
    }
    message m;
    sp5_result result = parse(plain, SP5_RTSP_LIMIT, &m);
    if (result.status != SP5_OK)
        return result;
    if (m.request.cseq <= r->cseq)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    r->cseq = m.request.cseq;
    const sp5_request *request = &m.request;
    if (equal(request->method, "OPTIONS"))
        return render(r, out, 200, "", (sp5_bytes){0});
    if (equal(request->method, "DESCRIBE")) {
        uint8_t data[2048];
        sp5_buffer body = {data, sizeof(data), 0};
        result = sp5_sdp_describe(r->config.host, &body);
        if (result.status != SP5_OK)
            return result;
        result =
            render(r, out, 200, "Content-Type: application/sdp\r\n", (sp5_bytes){data, body.size});
        if (result.status == SP5_OK)
            r->described = true;
        return result;
    }
    if (equal(request->method, "SETUP")) {
        if (!r->described || r->announced)
            return render(r, out, 455, "", (sp5_bytes){0});
        unsigned bit = 0;
        uint16_t port = 0;
        if (equal(request->target, "streamid=audio/0/0")) {
            bit = 1;
            port = r->config.audio_port;
        }
        if (equal(request->target, "streamid=video/0/0")) {
            bit = 2;
            port = r->config.video_port;
        }
        if (equal(request->target, "streamid=control/13/0")) {
            bit = 4;
            port = r->config.control_port;
        }
        if (r->setup_mask != 0 && !equal(field(&m, "session"), "DEADBEEFCAFE")) {
            return render(r, out, 455, "", (sp5_bytes){0});
        }
        if (bit == 0 || (r->setup_mask & bit) != 0)
            return render(r, out, 461, "", (sp5_bytes){0});
        char extra[256];
        char ping[17];
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < 8; ++i) {
            ping[i * 2] = hex[r->config.launch.ping[i] >> 4];
            ping[i * 2 + 1] = hex[r->config.launch.ping[i] & 15];
        }
        ping[16] = 0;
        /* §3 SETUP: Sunshine rtsp.cpp:956–1013. Session is correlation
         * syntax only; ping and ENet connect-data carry real launch tokens. */
        int n = bit == 4 ? snprintf(extra, sizeof(extra),
                                    "Session: DEADBEEFCAFE;timeout = 90\r\nTransport: "
                                    "server_port=%u\r\nX-SS-Connect-Data: %u\r\n",
                                    port, r->config.launch.control_connect_data)
                         : snprintf(extra, sizeof(extra),
                                    "Session: DEADBEEFCAFE;timeout = 90\r\nTransport: "
                                    "server_port=%u\r\nX-SS-Ping-Payload: %s\r\n",
                                    port, ping);
        if (n < 0 || (size_t)n >= sizeof(extra))
            return sp5_failure(SP5_REASON_CAPACITY);
        result = render(r, out, 200, extra, (sp5_bytes){0});
        if (result.status == SP5_OK)
            r->setup_mask |= bit;
        return result;
    }
    if (!equal(field(&m, "session"), "DEADBEEFCAFE"))
        return render(r, out, 455, "", (sp5_bytes){0});
    if (equal(request->method, "ANNOUNCE")) {
        if (r->setup_mask != 7 || r->announced ||
            !equal(request->target, "streamid=control/13/0") ||
            !equal_case(field(&m, "content-type"), "application/sdp"))
            return render(r, out, 455, "", (sp5_bytes){0});
        sp5_negotiated next;
        result = sp5_sdp_negotiate(request->body, r->config.host, &next);
        if (result.status != SP5_OK)
            return render(r, out, 400, "", (sp5_bytes){0});
        sp5_bytes hostname = field(&m, "host");
        next.high_quality_audio = hostname.size != 0;
        for (size_t i = 0; i + 7 <= hostname.size; ++i) {
            if (memcmp(hostname.data + i, "0.0.0.0", 7) == 0)
                next.high_quality_audio = false;
        }
        const sp5_host_mode *mode = &r->config.host->supported_modes[next.mode_index];
        if (mode->width != r->config.launch.width || mode->height != r->config.launch.height ||
            mode->fps != r->config.launch.fps ||
            (!r->config.launch.hdr && mode->dynamic_range != SP5_SDR))
            return render(r, out, 400, "", (sp5_bytes){0});
        sp5_session_parameters parameters = {next.mode_index, next.bitrate.requested_kbps, now_us,
                                             wall_us};
        result = sp5_session_configure(r->config.session, r->config.host, &parameters);
        if (result.status != SP5_OK)
            return result;
        r->chosen = next;
        r->announced = true;
        return render(r, out, 200, "Session: DEADBEEFCAFE\r\n", (sp5_bytes){0});
    }
    if (equal(request->method, "PLAY")) {
        if (!r->announced || view.state != SP5_SESSION_ACTIVE ||
            !sp5_session_authorized(r->config.session, r->config.verified_certificate))
            return render(r, out, 455, "", (sp5_bytes){0});
        sp5_result played = render(r, out, 200, "Session: DEADBEEFCAFE\r\n", (sp5_bytes){0});
        if (played.status == SP5_OK)
            r->playing = true;
        return played;
    }
    if (equal(request->method, "TEARDOWN")) {
        result = sp5_session_begin_end(r->config.session, SP5_END_CLIENT_QUIT, SP5_REASON_NONE);
        return result.status == SP5_OK ? render(r, out, 200, "", (sp5_bytes){0}) : result;
    }
    return render(r, out, 400, "", (sp5_bytes){0});
}
sp5_result sp5_rtsp_negotiated(const sp5_rtsp *r, sp5_negotiated *chosen)
{
    if (r == NULL || chosen == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    *chosen = (sp5_negotiated){0};
    if (!r->announced || !sp5_device_lease_valid(r->lease))
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    *chosen = r->chosen;
    return sp5_success();
}

bool sp5_rtsp_playing(const sp5_rtsp *service)
{
    return service != NULL && service->playing;
}
