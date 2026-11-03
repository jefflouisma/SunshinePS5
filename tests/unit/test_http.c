/* Synthetic portable HTTP/TLS contracts; no console evidence. */
#include "protocol/http.h"

#include <openssl/pem.h>
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

static sp5_bytes bytes(const char *text)
{
    return (sp5_bytes){(const uint8_t *)text, strlen(text)};
}

static bool contains(sp5_buffer value, const char *text)
{
    size_t size = strlen(text);
    for (size_t i = 0; i <= value.size && size <= value.size - i; ++i) {
        if (memcmp(value.data + i, text, size) == 0) {
            return true;
        }
    }
    return false;
}

static int parsing(void)
{
    sp5_request request;
    const char *invalid[] = {
        "GET / HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",
        "GET / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        "GET / HTTP/1.1\r\nBad Header: value\r\n\r\n",
        "GET / HTTP/1.1\r\n folded: value\r\n\r\n",
        "GET / HTTP/1.1\r\nX: a\nb\r\n\r\n",
        "GET / HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n",
        "GET / HTTP/2.0\r\n\r\n",
        "GET http://elsewhere/ HTTP/1.1\r\n\r\n",
        "GET /%zz HTTP/1.1\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(sp5_http_parse(bytes(invalid[i]), SP5_HTTP_LIMIT, &request).status == SP5_ERROR);
        CHECK(request.method.data == NULL && request.target.data == NULL);
    }
    CHECK(sp5_http_parse(bytes("POST /pair HTTP/1.0\r\ncontent-length: 3\r\n\r\na\0b"),
                         SP5_HTTP_LIMIT, &request)
              .status == SP5_ERROR);
    const char binary[] = "POST /pair HTTP/1.0\r\ncontent-length: 3\r\n\r\na\0b";
    CHECK(sp5_http_parse((sp5_bytes){(const uint8_t *)binary, sizeof(binary) - 1}, SP5_HTTP_LIMIT,
                         &request)
              .status == SP5_OK);
    CHECK(request.body.size == 3 && request.body.data[1] == 0);
    return 0;
}

static int query_and_xml(void)
{
    uint8_t data[256];
    sp5_buffer output = {data, sizeof(data), 0};
    sp5_request request = {.target = bytes("/pair?uniqueid=a%26b&devicename=A+%3CB%3E")};
    bool present = false;
    CHECK(sp5_http_query(&request, "uniqueid", &output, &present).status == SP5_OK && present);
    CHECK(output.size == 3 && memcmp(data, "a&b", 3) == 0);
    CHECK(sp5_http_query(&request, "absent", &output, &present).status == SP5_OK && !present);
    const char *invalid[] = {"/pair?uniqueid=a&unique%69d=b",
                             "/pair?a=%00",
                             "/pair?a=%q1",
                             "/pair?a=%0d%0a",
                             "/pair?=empty",
                             "/pair?a=1&&b=2"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        request.target = bytes(invalid[i]);
        CHECK(sp5_http_query(&request, "uniqueid", &output, &present).status == SP5_ERROR);
    }
    sp5_http_response response = {.code = 200, .fields = {data, sizeof(data), 0}};
    CHECK(sp5_http_field(&response, "hostname", bytes("A<&\"'B")).status == SP5_OK);
    CHECK(contains(response.fields, "A&lt;&amp;&quot;&apos;B"));
    uint8_t wire_data[512];
    sp5_buffer wire = {wire_data, sizeof(wire_data), 0};
    CHECK(sp5_http_render(&response, &wire).status == SP5_OK);
    CHECK(contains(wire, "HTTP/1.1 200 OK\r\n"));
    CHECK(contains(wire, "<root status_code=\"200\">"));
    wire.capacity = 8;
    CHECK(sp5_http_render(&response, &wire).status == SP5_ERROR && wire.size == 0);
    return 0;
}

typedef struct {
    uint8_t data[40000];
    size_t size;
} files;

static sp5_result read_file(void *context, sp5_store_file file, sp5_buffer *output)
{
    files *storage = context;
    if (file != SP5_STORE_DEVICES || storage->size == 0) {
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    }
    return sp5_buffer_append(output, (sp5_bytes){storage->data, storage->size});
}

static sp5_result write_file(void *context, sp5_store_file file, sp5_bytes value)
{
    files *storage = context;
    if (file != SP5_STORE_DEVICES || value.size > sizeof(storage->data)) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    memcpy(storage->data, value.data, value.size);
    storage->size = value.size;
    return sp5_success();
}

static int identity(uint8_t *certificate, size_t *certificate_size, uint8_t *private_key,
                    size_t *key_size)
{
    EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", 2048);
    X509 *cert = X509_new();
    BIO *cert_bio = BIO_new(BIO_s_mem());
    BIO *key_bio = BIO_new(BIO_s_mem());
    CHECK(key != NULL && cert != NULL && cert_bio != NULL && key_bio != NULL);
    CHECK(X509_set_version(cert, 2) == 1);
    CHECK(ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) == 1);
    CHECK(X509_gmtime_adj(X509_getm_notBefore(cert), -60) != NULL);
    CHECK(X509_gmtime_adj(X509_getm_notAfter(cert), 3600) != NULL);
    CHECK(X509_set_pubkey(cert, key) == 1);
    X509_NAME *name = X509_get_subject_name(cert);
    CHECK(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                     (const unsigned char *)"Synthetic HTTP fixture", -1, -1,
                                     0) == 1);
    CHECK(X509_set_issuer_name(cert, name) == 1 && X509_sign(cert, key, EVP_sha256()) > 0);
    CHECK(PEM_write_bio_X509(cert_bio, cert) == 1);
    CHECK(PEM_write_bio_PrivateKey(key_bio, key, NULL, NULL, 0, NULL, NULL) == 1);
    char *cert_data = NULL;
    char *key_data = NULL;
    long cert_length = BIO_get_mem_data(cert_bio, &cert_data);
    long key_length = BIO_get_mem_data(key_bio, &key_data);
    CHECK(cert_length > 0 && cert_length <= SP5_CERTIFICATE_CAPACITY);
    CHECK(key_length > 0 && key_length <= 4096);
    memcpy(certificate, cert_data, (size_t)cert_length);
    memcpy(private_key, key_data, (size_t)key_length);
    *certificate_size = (size_t)cert_length;
    *key_size = (size_t)key_length;
    BIO_free(cert_bio);
    BIO_free(key_bio);
    X509_free(cert);
    EVP_PKEY_free(key);
    return 0;
}

static bool handshake(SSL *server, SSL *client)
{
    for (unsigned i = 0; i < 30; ++i) {
        int result = SSL_do_handshake(client);
        if (result != 1 && SSL_get_error(client, result) != SSL_ERROR_WANT_READ &&
            SSL_get_error(client, result) != SSL_ERROR_WANT_WRITE) {
            return false;
        }
        result = SSL_do_handshake(server);
        if (result != 1 && SSL_get_error(server, result) != SSL_ERROR_WANT_READ &&
            SSL_get_error(server, result) != SSL_ERROR_WANT_WRITE) {
            return false;
        }
        if (SSL_is_init_finished(server) && SSL_is_init_finished(client)) {
            return true;
        }
    }
    return false;
}

static int routing(void)
{
    files storage = {0};
    sp5_storage_ops ops = {&storage, read_file, write_file, NULL};
    sp5_devices *devices = sp5_devices_create(&ops);
    CHECK(devices != NULL && sp5_devices_load(devices).status == SP5_OK);
    sp5_authority *authority = sp5_authority_create(devices);
    sp5_session *session = sp5_session_create(authority);
    CHECK(authority != NULL && session != NULL);
    uint8_t certificate[SP5_CERTIFICATE_CAPACITY], private_key[4096];
    size_t certificate_size = 0, key_size = 0;
    CHECK(identity(certificate, &certificate_size, private_key, &key_size) == 0);
    sp5_bytes cert = {certificate, certificate_size};
    sp5_host host;
    CHECK(sp5_host_init(&host, "synthetic-test", "13.60", "10.0.0.2").status == SP5_OK);
    CHECK(sp5_host_transition(&host, SP5_HOST_RUNNING).status == SP5_OK);
    sp5_http_service *service =
        sp5_http_create(&(sp5_http_config){.host = &host,
                                           .devices = devices,
                                           .session = session,
                                           .certificate_pem = cert,
                                           .private_key_pem = {private_key, key_size}});
    CHECK(service != NULL);
    uint8_t fields[8192];
    sp5_http_response response = {.fields = {fields, sizeof(fields), 0}};
    sp5_request request = {.method = bytes("GET"), .target = bytes("/serverinfo?uniqueid=spoof")};
    CHECK(sp5_http_route(service, "10.0.0.1", false, NULL, &request, 1, (sp5_optional_u64){0},
                         &response)
              .status == SP5_OK);
    CHECK(response.code == 200 && contains(response.fields, "<PairStatus>0</PairStatus>"));
    CHECK(contains(response.fields, "<ServerCodecModeSupport>0</ServerCodecModeSupport>"));
    request.target = bytes("/applist?uniqueid=spoof");
    CHECK(sp5_http_route(service, "10.0.0.1", false, NULL, &request, 2, (sp5_optional_u64){0},
                         &response)
                  .status == SP5_OK &&
          response.code == 403);
    CHECK(sp5_devices_pair(devices, cert, bytes("Test device"), 1).status == SP5_OK);
    SSL_CTX *client_context = SSL_CTX_new(TLS_client_method());
    CHECK(client_context != NULL);
    BIO *cert_bio = BIO_new_mem_buf(certificate, (int)certificate_size);
    BIO *key_bio = BIO_new_mem_buf(private_key, (int)key_size);
    CHECK(cert_bio != NULL && key_bio != NULL);
    X509 *client_cert = PEM_read_bio_X509(cert_bio, NULL, NULL, NULL);
    EVP_PKEY *client_key = PEM_read_bio_PrivateKey(key_bio, NULL, NULL, NULL);
    CHECK(client_cert != NULL && client_key != NULL);
    CHECK(SSL_CTX_use_certificate(client_context, client_cert) == 1);
    CHECK(SSL_CTX_use_PrivateKey(client_context, client_key) == 1);
    SSL_CTX_set_verify(client_context, SSL_VERIFY_NONE, NULL);
    SSL *server = SSL_new(sp5_http_tls_context(service));
    SSL *client = SSL_new(client_context);
    CHECK(server != NULL && client != NULL);
    SSL *unfinished = SSL_new(sp5_http_tls_context(service));
    CHECK(unfinished != NULL);
    CHECK(sp5_http_route(service, "10.0.0.1", false, unfinished, &request, 3, (sp5_optional_u64){0},
                         &response)
                  .status == SP5_OK &&
          response.code == 403);
    SSL_free(unfinished);
    BIO *server_io = NULL;
    BIO *client_io = NULL;
    CHECK(BIO_new_bio_pair(&server_io, 0, &client_io, 0) == 1);
    SSL_set_bio(server, server_io, server_io);
    SSL_set_bio(client, client_io, client_io);
    SSL_set_accept_state(server);
    SSL_set_connect_state(client);
    CHECK(handshake(server, client));
    CHECK(sp5_http_route(service, "203.0.113.1", false, server, &request, 3, (sp5_optional_u64){0},
                         &response)
                  .status == SP5_OK &&
          response.code == 403);
    CHECK(sp5_http_route(service, "10.0.0.1", false, server, &request, 3, (sp5_optional_u64){0},
                         &response)
                  .status == SP5_OK &&
          response.code == 200);
    CHECK(contains(response.fields, "<AppTitle>PS5</AppTitle>"));
    CHECK(contains(response.fields, "<ID>1</ID>"));
    request.target = bytes("/cancel");
    CHECK(sp5_http_route(service, "10.0.0.1", false, server, &request, 3, (sp5_optional_u64){0},
                         &response)
                  .status == SP5_OK &&
          response.code == 200);
    CHECK(contains(response.fields, "<cancel>1</cancel>"));
    CHECK(sp5_session_read(session).id == 0 && !sp5_session_read(session).has_end_reason);
    const char *bad_launches[] = {
        "/launch?appid=1&appid=1",
        "/launch?appid=2&rikey=00112233445566778899aabbccddeeff&rikeyid=1&localAudioPlayMode=0",
        "/launch?appid=1&rikey=zz112233445566778899aabbccddeeff&rikeyid=1&localAudioPlayMode=0",
        "/launch?appid=1&rikey=00112233445566778899aabbccddeeff&rikeyid=-2147483649&"
        "localAudioPlayMode=0",
        "/launch?appid=1&rikey=00112233445566778899aabbccddeeff&rikeyid=1&localAudioPlayMode=0&"
        "mode=0x720x60",
    };
    for (size_t i = 0; i < sizeof(bad_launches) / sizeof(bad_launches[0]); ++i) {
        request.target = bytes(bad_launches[i]);
        CHECK(sp5_http_route(service, "10.0.0.1", false, server, &request, 3, (sp5_optional_u64){0},
                             &response)
                      .status == SP5_OK &&
              response.code == 400);
        CHECK(sp5_session_read(session).state == SP5_SESSION_ENDED);
    }
    request.target = bytes("/launch?appid=1&rikey=00112233445566778899aabbccddeeff&rikeyid=-1&"
                           "localAudioPlayMode=0&mode=1280x720x60");
    CHECK(sp5_http_route(service, "10.0.0.1", false, server, &request, 4, (sp5_optional_u64){0},
                         &response)
                  .status == SP5_OK &&
          response.code == 200);
    uint64_t session_id = sp5_session_read(session).id;
    CHECK(session_id != 0 && sp5_session_read(session).state == SP5_SESSION_NEGOTIATING);
    CHECK(contains(response.fields, "<gamesession>1</gamesession>"));
    sp5_http_launch launch;
    CHECK(sp5_http_launch_read(service, &launch).status == SP5_OK);
    CHECK(launch.session_id == session_id && launch.key_id == UINT32_MAX && launch.width == 1280);
    CHECK(sp5_http_route(service, "10.0.0.1", false, server, &request, 5, (sp5_optional_u64){0},
                         &response)
                  .status == SP5_OK &&
          response.code == 409);
    CHECK(sp5_session_read(session).id == session_id);
    host.supported_mode_count = 1;
    host.supported_modes[0] = (sp5_host_mode){1280, 720, 60, "h264", SP5_SDR, 100, 1000};
    CHECK(sp5_session_configure(
              session, &host,
              &(sp5_session_parameters){.mode_index = 0, .requested_kbps = 100, .started_us = 4})
              .status == SP5_OK);
    CHECK(sp5_session_activate(session).status == SP5_OK);
    CHECK(sp5_http_route(service, "10.0.0.1", false, server, &request, 5, (sp5_optional_u64){0},
                         &response)
                  .status == SP5_OK &&
          response.code == 409);
    CHECK(sp5_session_read(session).id == session_id &&
          sp5_session_read(session).state == SP5_SESSION_ACTIVE);
    request.target = bytes("/cancel");
    CHECK(sp5_http_route(service, "10.0.0.1", false, server, &request, 6, (sp5_optional_u64){0},
                         &response)
                  .status == SP5_OK &&
          response.code == 200);
    CHECK(sp5_session_read(session).end_reason == SP5_END_CLIENT_QUIT);
    CHECK(sp5_http_launch_read(service, &launch).status == SP5_ERROR);
    request.target = bytes("/resume?rikey=00112233445566778899aabbccddeeff&rikeyid=1&corever=1");
    CHECK(sp5_http_route(service, "10.0.0.1", false, server, &request, 7, (sp5_optional_u64){0},
                         &response)
                  .status == SP5_OK &&
          response.code == 200);
    CHECK(contains(response.fields, "<resume>1</resume>"));
    CHECK(contains(response.fields, "rtspenc://10.0.0.2:48010"));
    CHECK(sp5_session_read(session).id > session_id);
    CHECK(sp5_http_launch_read(service, &launch).status == SP5_OK && launch.encrypted_rtsp);
    request.target = bytes("/cancel");
    CHECK(sp5_http_route(service, "10.0.0.1", false, server, &request, 7, (sp5_optional_u64){0},
                         &response)
                  .status == SP5_OK &&
          response.code == 200);
    CHECK(sp5_devices_revoke(devices, cert).status == SP5_OK);
    request.target = bytes("/applist");
    CHECK(sp5_http_route(service, "10.0.0.1", false, server, &request, 7, (sp5_optional_u64){0},
                         &response)
                  .status == SP5_OK &&
          response.code == 403);
    SSL *revoked_server = SSL_new(sp5_http_tls_context(service));
    SSL *revoked_client = SSL_new(client_context);
    CHECK(revoked_server != NULL && revoked_client != NULL);
    BIO *revoked_server_io = NULL;
    BIO *revoked_client_io = NULL;
    CHECK(BIO_new_bio_pair(&revoked_server_io, 0, &revoked_client_io, 0) == 1);
    SSL_set_bio(revoked_server, revoked_server_io, revoked_server_io);
    SSL_set_bio(revoked_client, revoked_client_io, revoked_client_io);
    SSL_set_accept_state(revoked_server);
    SSL_set_connect_state(revoked_client);
    CHECK(!handshake(revoked_server, revoked_client));
    SSL_free(revoked_client);
    SSL_free(revoked_server);
    SSL_free(client);
    SSL_free(server);
    SSL_CTX_free(client_context);
    X509_free(client_cert);
    EVP_PKEY_free(client_key);
    BIO_free(cert_bio);
    BIO_free(key_bio);
    sp5_http_destroy(service);
    sp5_session_destroy(session);
    sp5_authority_destroy(authority);
    sp5_devices_destroy(devices);
    OPENSSL_cleanse(private_key, sizeof(private_key));
    return 0;
}

typedef struct {
    unsigned calls;
    char target[128];
} pair_probe;

static sp5_result record_pair(void *context, const sp5_request *request,
                              sp5_bytes verified_certificate, uint64_t now_us,
                              sp5_optional_u64 wall_us, sp5_http_response *response)
{
    (void)verified_certificate;
    (void)now_us;
    (void)wall_us;
    pair_probe *probe = context;
    if (request->target.size >= sizeof(probe->target)) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    memcpy(probe->target, request->target.data, request->target.size);
    probe->target[request->target.size] = 0;
    ++probe->calls;
    response->code = 200;
    return sp5_success();
}

/* T056: Moonlight's plain-HTTP GET /unpair reaches the pairing owner after the
 * peer boundary, not the unauthorized 403 (protocol-notes.md:1036-1037). */
static int unpair_route(void)
{
    files storage = {0};
    sp5_storage_ops ops = {&storage, read_file, write_file, NULL};
    sp5_devices *devices = sp5_devices_create(&ops);
    CHECK(devices != NULL && sp5_devices_load(devices).status == SP5_OK);
    sp5_authority *authority = sp5_authority_create(devices);
    sp5_session *session = sp5_session_create(authority);
    CHECK(authority != NULL && session != NULL);
    uint8_t certificate[SP5_CERTIFICATE_CAPACITY], private_key[4096];
    size_t certificate_size = 0, key_size = 0;
    CHECK(identity(certificate, &certificate_size, private_key, &key_size) == 0);
    sp5_host host;
    CHECK(sp5_host_init(&host, "synthetic-test", "13.60", "10.0.0.2").status == SP5_OK);
    pair_probe probe = {0};
    sp5_http_service *service =
        sp5_http_create(&(sp5_http_config){.host = &host,
                                           .devices = devices,
                                           .session = session,
                                           .certificate_pem = {certificate, certificate_size},
                                           .private_key_pem = {private_key, key_size},
                                           .pair_context = &probe,
                                           .pair = record_pair});
    CHECK(service != NULL);
    uint8_t fields[256];
    sp5_http_response response = {.fields = {fields, sizeof(fields), 0}};
    const char unpair[] = "/unpair?uniqueid=0123456789ABCDEF&uuid=00112233";
    sp5_request request = {.method = bytes("GET"), .target = bytes(unpair)};
    CHECK(sp5_http_route(service, "10.0.0.1", false, NULL, &request, 1, (sp5_optional_u64){0},
                         &response)
              .status == SP5_OK);
    CHECK(response.code == 200 && probe.calls == 1 && strcmp(probe.target, unpair) == 0);
    CHECK(sp5_http_route(service, "203.0.113.1", false, NULL, &request, 2, (sp5_optional_u64){0},
                         &response)
                  .status == SP5_OK &&
          response.code == 403 && probe.calls == 1);
    request.method = bytes("POST");
    CHECK(sp5_http_route(service, "10.0.0.1", false, NULL, &request, 3, (sp5_optional_u64){0},
                         &response)
                  .status == SP5_OK &&
          response.code == 405 && probe.calls == 1);
    request.method = bytes("GET");
    request.target = bytes("/unpaired?uniqueid=0123456789ABCDEF");
    CHECK(sp5_http_route(service, "10.0.0.1", false, NULL, &request, 4, (sp5_optional_u64){0},
                         &response)
                  .status == SP5_OK &&
          response.code == 403 && probe.calls == 1);
    sp5_http_destroy(service);
    sp5_session_destroy(session);
    sp5_authority_destroy(authority);
    sp5_devices_destroy(devices);
    OPENSSL_cleanse(private_key, sizeof(private_key));
    return 0;
}

int main(void)
{
    CHECK(parsing() == 0);
    CHECK(query_and_xml() == 0);
    CHECK(routing() == 0);
    CHECK(unpair_route() == 0);
    return 0;
}
