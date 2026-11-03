#ifndef SUNSHINE_PROTOCOL_HTTP_H
#define SUNSHINE_PROTOCOL_HTTP_H

#include "session/session.h"

#include <openssl/ssl.h>

/* Management-owner limits, not GameStream wire constants. One request per
 * connection, bounded buffers; TLS and pairing crypto never run on input/media. */
enum { SP5_HTTP_LIMIT = 16384, SP5_HTTP_QUERY_CAPACITY = 32 };
typedef struct {
    unsigned code;
    bool deferred;
    sp5_buffer fields;
} sp5_http_response;
sp5_result sp5_http_query(const sp5_request *request, const char *key, sp5_buffer *value,
                          bool *present);
sp5_result sp5_http_field(sp5_http_response *response, const char *tag, sp5_bytes value);
sp5_result sp5_http_field_hex(sp5_http_response *response, const char *tag, sp5_bytes value);
sp5_result sp5_http_render(const sp5_http_response *response, sp5_buffer *wire);

typedef sp5_result (*sp5_http_pair_handler)(void *context, const sp5_request *request,
                                            sp5_bytes verified_certificate, uint64_t now_us,
                                            sp5_optional_u64 wall_us, sp5_http_response *response);
typedef struct {
    sp5_host *host;
    sp5_devices *devices;
    sp5_session *session;
    sp5_bytes certificate_pem, private_key_pem;
    void *pair_context;
    sp5_http_pair_handler pair;
} sp5_http_config;
typedef struct sp5_http_service sp5_http_service;
/* Setup/teardown only. All dependencies outlive service. TLS callback consults
 * the sole current registry; it accepts no system CA or client-provided ID. */
sp5_http_service *sp5_http_create(const sp5_http_config *config);
void sp5_http_destroy(sp5_http_service *service);
SSL_CTX *sp5_http_tls_context(sp5_http_service *service);
typedef struct {
    uint64_t session_id;
    uint8_t key[16], ping[8];
    uint32_t key_id, control_connect_data;
    uint32_t width, height, fps;
    bool hdr, encrypted_rtsp;
} sp5_http_launch;
sp5_result sp5_http_launch_read(const sp5_http_service *service, sp5_http_launch *launch);
/* peer/on_link originate in the accepted socket. tls is NULL on HTTP, otherwise
 * the completed server SSL object from this service, never a claimed identity.
 * Deferred getservercert keeps its connection in the caller's bounded pool. */
sp5_result sp5_http_route(sp5_http_service *service, const char *peer, bool on_link, SSL *tls,
                          const sp5_request *request, uint64_t now_us, sp5_optional_u64 wall_us,
                          sp5_http_response *response);

#endif
