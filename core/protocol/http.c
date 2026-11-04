#include "http.h"

#include <openssl/pem.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Wire spellings/default ports/version/capability bits: protocol-notes.md §§1,
 * 2, 13; sun nvhttp.h:38-53, nvhttp.cpp:1243-1572 @ d0b4a88;
 * mcc Limelight.h:505-522 @ 41cedb8. App ID 1 is this host's stable local ID. */
struct sp5_http_service {
    sp5_http_config config;
    SSL_CTX *tls;
    char unique_id[65];
    uint64_t next_session_id;
    sp5_http_launch launch;
    sp5_device_lease launch_lease;
};

#define TRY(expression)                                                                            \
    do {                                                                                           \
        sp5_result try_result = (expression);                                                      \
        if (try_result.status != SP5_OK) {                                                         \
            return try_result;                                                                     \
        }                                                                                          \
    } while (0)

static sp5_bytes text(const char *value)
{
    return (sp5_bytes){(const uint8_t *)value, strlen(value)};
}

static bool equal(sp5_bytes value, const char *literal)
{
    return value.size == strlen(literal) && memcmp(value.data, literal, value.size) == 0;
}

static sp5_result append(sp5_buffer *output, const char *value)
{
    return sp5_buffer_append(output, text(value));
}

sp5_result sp5_http_field(sp5_http_response *response, const char *tag, sp5_bytes value)
{
    if (response == NULL || tag == NULL || tag[0] == 0 || (value.data == NULL && value.size != 0)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    for (size_t i = 0; tag[i] != 0; ++i) {
        if (!((tag[i] >= 'a' && tag[i] <= 'z') || (tag[i] >= 'A' && tag[i] <= 'Z') ||
              (i > 0 && tag[i] >= '0' && tag[i] <= '9'))) {
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
    }
    size_t saved = response->fields.size;
    sp5_result result = append(&response->fields, "<");
    if (result.status == SP5_OK) {
        result = append(&response->fields, tag);
    }
    if (result.status == SP5_OK) {
        result = append(&response->fields, ">");
    }
    for (size_t i = 0; i < value.size && result.status == SP5_OK; ++i) {
        switch (value.data[i]) {
        case '&':
            result = append(&response->fields, "&amp;");
            break;
        case '<':
            result = append(&response->fields, "&lt;");
            break;
        case '>':
            result = append(&response->fields, "&gt;");
            break;
        case '\"':
            result = append(&response->fields, "&quot;");
            break;
        case '\'':
            result = append(&response->fields, "&apos;");
            break;
        default:
            if (value.data[i] < 32 || value.data[i] == 127) {
                result = sp5_failure(SP5_REASON_INVALID_ARGUMENT);
            } else {
                result = sp5_buffer_append(&response->fields, (sp5_bytes){value.data + i, 1});
            }
        }
    }
    if (result.status == SP5_OK) {
        result = append(&response->fields, "</");
    }
    if (result.status == SP5_OK) {
        result = append(&response->fields, tag);
    }
    if (result.status == SP5_OK) {
        result = append(&response->fields, ">");
    }
    if (result.status != SP5_OK) {
        response->fields.size = saved;
    }
    return result;
}

sp5_result sp5_http_field_hex(sp5_http_response *response, const char *tag, sp5_bytes value)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t encoded[SP5_CERTIFICATE_CAPACITY * 2];
    if ((value.data == NULL && value.size != 0) || value.size > sizeof(encoded) / 2) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    for (size_t i = 0; i < value.size; ++i) {
        encoded[2 * i] = (uint8_t)hex[value.data[i] >> 4];
        encoded[2 * i + 1] = (uint8_t)hex[value.data[i] & 15];
    }
    return sp5_http_field(response, tag, (sp5_bytes){encoded, value.size * 2});
}

sp5_result sp5_http_render(const sp5_http_response *response, sp5_buffer *wire)
{
    if (wire == NULL || response == NULL || response->fields.size > response->fields.capacity ||
        response->fields.data == NULL || response->code < 100 || response->code > 599) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    wire->size = 0;
    if (response->deferred) {
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    }
    const char *message = response->code != 200 ? response->status_message : NULL;
    for (const char *at = message; at != NULL && *at != 0; ++at) {
        if (*at < 0x20 || *at > 0x7e || strchr("<>&\"'", *at) != NULL) {
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
    }
    char opening[320];
    int opening_size =
        message == NULL
            ? snprintf(opening, sizeof(opening),
                       "<?xml version=\"1.0\" encoding=\"utf-8\"?><root status_code=\"%u\">",
                       response->code)
            : snprintf(opening, sizeof(opening),
                       "<?xml version=\"1.0\" encoding=\"utf-8\"?><root status_code=\"%u\" "
                       "status_message=\"%s\">",
                       response->code, message);
    if (opening_size < 0 || (size_t)opening_size >= sizeof(opening) ||
        response->fields.size > SP5_HTTP_LIMIT) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    size_t body_size = (size_t)opening_size + response->fields.size + strlen("</root>");
    char headers[192];
    /* GameStream carries protocol errors in root.status_code (notes §13.5).
     * Close-delimited ownership also avoids pipelining pending pairing requests. */
    int header_size = snprintf(headers, sizeof(headers),
                               "HTTP/1.1 200 OK\r\nContent-Type: application/xml; charset=utf-8\r\n"
                               "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                               body_size);
    if (header_size < 0 || (size_t)header_size >= sizeof(headers) ||
        (size_t)header_size > wire->capacity || body_size > wire->capacity - (size_t)header_size) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    TRY(append(wire, headers));
    TRY(append(wire, opening));
    TRY(sp5_buffer_append(wire, (sp5_bytes){response->fields.data, response->fields.size}));
    return append(wire, "</root>");
}

static sp5_result canonical_certificate(X509 *certificate, sp5_buffer *output)
{
    BIO *bio = BIO_new(BIO_s_mem());
    if (certificate == NULL || bio == NULL) {
        BIO_free(bio);
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    sp5_result result = sp5_failure(SP5_REASON_CAPACITY);
    char *data = NULL;
    if (PEM_write_bio_X509(bio, certificate) == 1) {
        long size = BIO_get_mem_data(bio, &data);
        if (size > 0 && size <= SP5_CERTIFICATE_CAPACITY) {
            result = sp5_buffer_append(output, (sp5_bytes){(const uint8_t *)data, (size_t)size});
        }
    }
    BIO_free(bio);
    return result;
}

static int verify_peer(int preverified, X509_STORE_CTX *store)
{
    (void)preverified;
    SSL *ssl = X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx());
    sp5_http_service *service = ssl == NULL ? NULL : SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl));
    if (service == NULL || X509_STORE_CTX_get_error_depth(store) != 0) {
        return 0;
    }
    uint8_t data[SP5_CERTIFICATE_CAPACITY];
    sp5_buffer certificate = {data, sizeof(data), 0};
    bool authorized =
        canonical_certificate(X509_STORE_CTX_get0_cert(store), &certificate).status == SP5_OK &&
        sp5_devices_authorized(service->config.devices,
                               (sp5_bytes){certificate.data, certificate.size});
    if (authorized) {
        X509_STORE_CTX_set_error(store, X509_V_OK);
    }
    return authorized ? 1 : 0;
}

sp5_http_service *sp5_http_create(const sp5_http_config *config)
{
    if (config == NULL || config->host == NULL || config->devices == NULL ||
        config->session == NULL || config->certificate_pem.data == NULL ||
        config->private_key_pem.data == NULL || config->certificate_pem.size == 0 ||
        config->certificate_pem.size > SP5_CERTIFICATE_CAPACITY ||
        config->private_key_pem.size == 0 || config->private_key_pem.size > SP5_IDENTITY_CAPACITY) {
        return NULL;
    }
    sp5_http_service *service = calloc(1, sizeof(*service));
    if (service == NULL) {
        return NULL;
    }
    service->config = *config;
    service->next_session_id = 1;
    service->tls = SSL_CTX_new(TLS_server_method());
    BIO *cert_bio =
        BIO_new_mem_buf(config->certificate_pem.data, (int)config->certificate_pem.size);
    BIO *key_bio = BIO_new_mem_buf(config->private_key_pem.data, (int)config->private_key_pem.size);
    X509 *cert = cert_bio == NULL ? NULL : PEM_read_bio_X509(cert_bio, NULL, NULL, NULL);
    EVP_PKEY *key = key_bio == NULL ? NULL : PEM_read_bio_PrivateKey(key_bio, NULL, NULL, NULL);
    bool valid = service->tls != NULL && cert != NULL && key != NULL &&
                 SSL_CTX_set_min_proto_version(service->tls, TLS1_2_VERSION) == 1 &&
                 SSL_CTX_use_certificate(service->tls, cert) == 1 &&
                 SSL_CTX_use_PrivateKey(service->tls, key) == 1 &&
                 SSL_CTX_check_private_key(service->tls) == 1;
    uint8_t digest[32];
    size_t digest_size = 0;
    if (valid) {
        valid = EVP_Q_digest(NULL, "SHA256", NULL, config->certificate_pem.data,
                             config->certificate_pem.size, digest, &digest_size) == 1 &&
                digest_size == sizeof(digest);
    }
    BIO_free(cert_bio);
    BIO_free(key_bio);
    X509_free(cert);
    EVP_PKEY_free(key);
    if (!valid) {
        sp5_http_destroy(service);
        return NULL;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        service->unique_id[i * 2] = hex[digest[i] >> 4];
        service->unique_id[i * 2 + 1] = hex[digest[i] & 15];
    }
    SSL_CTX_set_app_data(service->tls, service);
    SSL_CTX_set_verify(service->tls, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       verify_peer);
    SSL_CTX_set_verify_depth(service->tls, 0);
    /* Tickets/resumption must not outlive a registry authorization decision. */
    SSL_CTX_set_session_cache_mode(service->tls, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_options(service->tls, SSL_OP_NO_TICKET);
    if (SSL_CTX_set_num_tickets(service->tls, 0) != 1) {
        sp5_http_destroy(service);
        return NULL;
    }
    return service;
}

void sp5_http_destroy(sp5_http_service *service)
{
    if (service != NULL) {
        SSL_CTX_free(service->tls);
        OPENSSL_cleanse(service, sizeof(*service));
        free(service);
    }
}

SSL_CTX *sp5_http_tls_context(sp5_http_service *service)
{
    return service == NULL ? NULL : service->tls;
}

sp5_result sp5_http_launch_read(const sp5_http_service *service, sp5_http_launch *launch)
{
    if (service == NULL || launch == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *launch = (sp5_http_launch){0};
    sp5_session_view view = sp5_session_read(service->config.session);
    if (view.id != service->launch.session_id || view.state >= SP5_SESSION_ENDING ||
        !sp5_device_lease_valid(service->launch_lease)) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    *launch = service->launch;
    return sp5_success();
}

static sp5_result number(sp5_http_response *response, const char *tag, uint64_t value)
{
    char digits[24];
    int size = snprintf(digits, sizeof(digits), "%llu", (unsigned long long)value);
    if (size < 0 || (size_t)size >= sizeof(digits)) {
        return sp5_failure(SP5_REASON_OVERFLOW);
    }
    return sp5_http_field(response, tag, text(digits));
}

static sp5_result server_info(sp5_http_service *service, bool authorized,
                              sp5_http_response *response)
{
    const sp5_host *host = service->config.host;
    sp5_session_view session = sp5_session_read(service->config.session);
    bool busy = session.id != 0 && session.state < SP5_SESSION_ENDED;
    uint32_t codecs = 0;
    uint64_t max_hevc_luma = 0;
    if (host->supported_mode_count > SP5_MODE_CAPACITY) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    for (size_t i = 0; i < host->supported_mode_count; ++i) {
        const sp5_host_mode *mode = &host->supported_modes[i];
        if (strcmp(mode->codec, "h264") == 0) {
            codecs |= 0x00000001;
        } else if (strcmp(mode->codec, "hevc") == 0 || strcmp(mode->codec, "hevc-main10") == 0) {
            codecs |= 0x00000100;
            if (mode->dynamic_range == SP5_HDR_PQ_BT2020) {
                codecs |= 0x00000200;
            }
            uint64_t pixels = (uint64_t)mode->width * mode->height;
            if (pixels > max_hevc_luma) {
                max_hevc_luma = pixels;
            }
        }
    }
    TRY(sp5_http_field(response, "hostname", text(host->settings.host_name)));
    TRY(sp5_http_field(response, "appversion", text("7.1.431.-1")));
    TRY(sp5_http_field(response, "GfeVersion", text("3.23.0.74")));
    TRY(sp5_http_field(response, "uniqueid", text(service->unique_id)));
    TRY(number(response, "HttpsPort", 47984));
    TRY(number(response, "ExternalPort", 47989));
    TRY(sp5_http_field(response, "LocalIP", text(host->address)));
    TRY(number(response, "ServerCodecModeSupport", codecs));
    TRY(number(response, "MaxLumaPixelsHEVC", max_hevc_luma));
    TRY(number(response, "PairStatus", authorized ? 1 : 0));
    TRY(number(response, "currentgame", busy ? 1 : 0));
    return sp5_http_field(response, "state",
                          text(busy ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE"));
}

static sp5_result query_text(const sp5_request *request, const char *key, uint8_t *data,
                             size_t capacity, bool required, sp5_bytes *value)
{
    sp5_buffer output = {data, capacity, 0};
    bool present = false;
    TRY(sp5_http_query(request, key, &output, &present));
    if (required && (!present || output.size == 0)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *value = (sp5_bytes){data, output.size};
    return sp5_success();
}

static bool unsigned_decimal(sp5_bytes value, uint32_t *number_value)
{
    if (value.size == 0) {
        return false;
    }
    *number_value = 0;
    for (size_t i = 0; i < value.size; ++i) {
        uint8_t digit = value.data[i];
        if (digit < '0' || digit > '9' || *number_value > (UINT32_MAX - (digit - '0')) / 10) {
            return false;
        }
        *number_value = *number_value * 10 + digit - '0';
    }
    return true;
}

static bool parse_key(sp5_bytes value, uint8_t key[16])
{
    if (value.size != 32) {
        return false;
    }
    for (size_t i = 0; i < 16; ++i) {
        unsigned byte = 0;
        for (size_t j = 0; j < 2; ++j) {
            uint8_t digit = value.data[i * 2 + j];
            unsigned half;
            if (digit >= '0' && digit <= '9') {
                half = digit - '0';
            } else if (digit >= 'a' && digit <= 'f') {
                half = digit - 'a' + 10;
            } else if (digit >= 'A' && digit <= 'F') {
                half = digit - 'A' + 10;
            } else {
                return false;
            }
            byte = (byte << 4) | half;
        }
        key[i] = (uint8_t)byte;
    }
    return true;
}

static sp5_result launch_parameters(const sp5_request *request, bool resume,
                                    sp5_http_launch *launch)
{
    uint8_t data[128];
    sp5_bytes value = {0};
    TRY(query_text(request, "appid", data, sizeof(data), !resume, &value));
    if (value.size != 0 && !equal(value, "1")) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    TRY(query_text(request, "rikey", data, sizeof(data), true, &value));
    if (!parse_key(value, launch->key)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    TRY(query_text(request, "rikeyid", data, sizeof(data), true, &value));
    bool negative = value.size != 0 && value.data[0] == '-';
    if (negative) {
        ++value.data;
        --value.size;
    }
    if (!unsigned_decimal(value, &launch->key_id) ||
        (negative && launch->key_id > UINT32_C(2147483648))) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (negative) {
        launch->key_id = UINT32_C(0) - launch->key_id;
    }
    TRY(query_text(request, "localAudioPlayMode", data, sizeof(data), !resume, &value));
    if (value.size != 0 && !equal(value, "0") && !equal(value, "1")) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    TRY(query_text(request, "hdrMode", data, sizeof(data), false, &value));
    if (value.size != 0 && !equal(value, "0") && !equal(value, "1")) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    launch->hdr = equal(value, "1");
    TRY(query_text(request, "corever", data, sizeof(data), false, &value));
    uint32_t core_version = 0;
    if (value.size != 0 && !unsigned_decimal(value, &core_version)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    launch->encrypted_rtsp = core_version >= 1;
    TRY(query_text(request, "surroundAudioInfo", data, sizeof(data), false, &value));
    launch->audio_channels = 2;
    if (value.size != 0) {
        uint32_t surround = 0;
        if (!unsigned_decimal(value, &surround)) {
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
        launch->audio_channels = surround & 0xffffU; /* channelMask << 16 | channelCount */
    }
    TRY(query_text(request, "mode", data, sizeof(data), false, &value));
    if (value.size != 0) {
        uint32_t *parts[] = {&launch->width, &launch->height, &launch->fps};
        size_t start = 0;
        for (size_t i = 0; i < 3; ++i) {
            size_t end = start;
            while (end < value.size && value.data[end] != 'x') {
                ++end;
            }
            if (!unsigned_decimal((sp5_bytes){value.data + start, end - start}, parts[i]) ||
                *parts[i] == 0 || (i < 2 && end == value.size) || (i == 2 && end != value.size)) {
                return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
            }
            start = end + 1;
        }
    }
    return sp5_success();
}

static bool offered_mode(const sp5_host *host, const sp5_http_launch *launch)
{
    for (size_t i = 0; i < host->supported_mode_count && i < SP5_MODE_CAPACITY; ++i) {
        const sp5_host_mode *mode = &host->supported_modes[i];
        if (mode->width == launch->width && mode->height == launch->height &&
            mode->fps == launch->fps) {
            return true;
        }
    }
    return false;
}

static sp5_result start_session(sp5_http_service *service, const sp5_request *request,
                                sp5_bytes certificate, bool resume, sp5_http_response *response)
{
    sp5_session_view current = sp5_session_read(service->config.session);
    if (current.id != 0 && current.state != SP5_SESSION_ENDED) {
        response->code = 409;
        return sp5_success();
    }
    sp5_http_launch next = {0};
    if (launch_parameters(request, resume, &next).status != SP5_OK) {
        OPENSSL_cleanse(&next, sizeof(next));
        response->code = 400;
        return sp5_success();
    }
    /* Refuse what RTSP negotiation would refuse later, while Moonlight can
     * still show the reason instead of a generic handshake failure. */
    const char *refusal = NULL;
    if (next.audio_channels != 2) {
        refusal = "This host streams stereo audio only. Set audio to Stereo in Moonlight settings.";
    } else if (next.width != 0 && !offered_mode(service->config.host, &next)) {
        refusal = "This host streams 1080p or 720p at 60 or 30 FPS. Change the resolution or "
                  "frame rate in Moonlight settings.";
    }
    if (refusal != NULL) {
        OPENSSL_cleanse(&next, sizeof(next));
        response->code = 400;
        response->status_message = refusal;
        return sp5_success();
    }
    if (service->next_session_id == UINT64_MAX || RAND_bytes(next.ping, sizeof(next.ping)) != 1 ||
        RAND_bytes((uint8_t *)&next.control_connect_data, sizeof(next.control_connect_data)) != 1) {
        OPENSSL_cleanse(&next, sizeof(next));
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    char url[192];
    const char *address = service->config.host->address;
    bool ipv6 = strchr(address, ':') != NULL;
    int size =
        snprintf(url, sizeof(url), "%s%s%s%s:48010", next.encrypted_rtsp ? "rtspenc://" : "rtsp://",
                 ipv6 ? "[" : "", address, ipv6 ? "]" : "");
    if (size < 0 || (size_t)size >= sizeof(url)) {
        OPENSSL_cleanse(&next, sizeof(next));
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    sp5_result result = sp5_http_field(response, "sessionUrl0", text(url));
    if (result.status == SP5_OK) {
        result = number(response, resume ? "resume" : "gamesession", 1);
    }
    if (result.status != SP5_OK) {
        OPENSSL_cleanse(&next, sizeof(next));
        return result;
    }
    next.session_id = service->next_session_id;
    result = sp5_session_admit(service->config.session, certificate, next.session_id);
    if (result.status == SP5_OK) {
        service->launch = next;
        service->launch_lease = sp5_devices_lease(service->config.devices, certificate);
        ++service->next_session_id;
    } else {
        response->fields.size = 0;
        response->code = 409;
    }
    OPENSSL_cleanse(&next, sizeof(next));
    return sp5_success();
}

static bool owns_session(sp5_http_service *service, sp5_bytes certificate)
{
    sp5_session_view session = sp5_session_read(service->config.session);
    size_t count = sp5_devices_count(service->config.devices);
    for (size_t i = 0; i < count; ++i) {
        const sp5_device *device = sp5_devices_at(service->config.devices, i);
        if (device != NULL && certificate.size == device->certificate_size &&
            CRYPTO_memcmp(certificate.data, device->certificate, certificate.size) == 0 &&
            strcmp(session.device_id, device->id) == 0) {
            return true;
        }
    }
    return false;
}

sp5_result sp5_http_route(sp5_http_service *service, const char *peer, bool on_link, SSL *tls,
                          const sp5_request *request, uint64_t now_us, sp5_optional_u64 wall_us,
                          sp5_http_response *response)
{
    if (service == NULL || request == NULL || response == NULL || request->method.data == NULL ||
        request->target.data == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    response->fields.size = 0;
    response->deferred = false;
    response->code = 200;
    response->status_message = NULL;
    if (!sp5_peer_permitted(peer, on_link)) {
        response->code = 403;
        return sp5_success();
    }
    if (!equal(request->method, "GET") || request->body.size != 0) {
        response->code = 405;
        return sp5_success();
    }
    uint8_t ignored[1];
    bool present = false;
    sp5_buffer validation = {ignored, sizeof(ignored), 0};
    if (sp5_http_query(request, "", &validation, &present).status != SP5_OK) {
        response->code = 400;
        return sp5_success();
    }
    uint8_t cert_data[SP5_CERTIFICATE_CAPACITY];
    sp5_buffer certificate = {cert_data, sizeof(cert_data), 0};
    bool authorized = false;
    if (tls != NULL) {
        if (SSL_get_SSL_CTX(tls) != service->tls || !SSL_is_server(tls) ||
            !SSL_is_init_finished(tls) || SSL_get_verify_result(tls) != X509_V_OK ||
            canonical_certificate(SSL_get0_peer_certificate(tls), &certificate).status != SP5_OK) {
            response->code = 403;
            return sp5_success();
        }
        authorized = sp5_devices_authorized(service->config.devices,
                                            (sp5_bytes){certificate.data, certificate.size});
        if (!authorized) {
            response->code = 403;
            return sp5_success();
        }
    }
    size_t path_size = 0;
    while (path_size < request->target.size && request->target.data[path_size] != '?') {
        ++path_size;
    }
    sp5_bytes path = {request->target.data, path_size};
    if (equal(path, "/serverinfo")) {
        return server_info(service, authorized, response);
    }
    /* Moonlight sends /unpair over HTTP after any pairing failure, including a
     * wrong PIN it detected before step 4 (protocol-notes.md:1036-1037). Only
     * the pairing owner may end the matching pending request; no registry change. */
    if (equal(path, "/pair") || equal(path, "/unpair")) {
        if (service->config.pair == NULL) {
            response->code = 503;
            return sp5_success();
        }
        return service->config.pair(service->config.pair_context, request,
                                    (sp5_bytes){certificate.data, certificate.size}, now_us,
                                    wall_us, response);
    }
    if (!authorized) {
        response->code = 403;
        return sp5_success();
    }
    if (equal(path, "/applist")) {
        bool hdr = false;
        const sp5_host *host = service->config.host;
        if (host->supported_mode_count > SP5_MODE_CAPACITY) {
            return sp5_failure(SP5_REASON_CAPACITY);
        }
        for (size_t i = 0; i < host->supported_mode_count; ++i) {
            hdr = hdr || host->supported_modes[i].dynamic_range == SP5_HDR_PQ_BT2020;
        }
        TRY(append(&response->fields, "<App>"));
        TRY(sp5_http_field(response, "AppTitle", text("PS5")));
        TRY(number(response, "ID", 1));
        TRY(number(response, "IsHdrSupported", hdr ? 1 : 0));
        return append(&response->fields, "</App>");
    }
    if (service->config.host->state != SP5_HOST_RUNNING) {
        response->code = 503;
        return sp5_success();
    }
    if (equal(path, "/launch") || equal(path, "/resume")) {
        return start_session(service, request, (sp5_bytes){certificate.data, certificate.size},
                             equal(path, "/resume"), response);
    }
    if (equal(path, "/cancel")) {
        sp5_session_view session = sp5_session_read(service->config.session);
        if (session.id != 0 && session.state != SP5_SESSION_ENDED &&
            !owns_session(service, (sp5_bytes){certificate.data, certificate.size})) {
            response->code = 403;
            return sp5_success();
        }
        TRY(number(response, "cancel", 1));
        if (session.id != 0) {
            TRY(sp5_session_end(service->config.session, SP5_END_CLIENT_QUIT));
        }
        OPENSSL_cleanse(&service->launch, sizeof(service->launch));
        return sp5_success();
    }
    response->code = 404;
    return sp5_success();
}
