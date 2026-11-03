#ifndef SUNSHINE_WEB_SERVER_H
#define SUNSHINE_WEB_SERVER_H
#include "json.h"
#include "protocol/pairing_flow.h"
#include "session/lifecycle.h"

enum { SP5_WEB_BODY_LIMIT = 2048, SP5_WEB_RESPONSE_LIMIT = 32768, SP5_WEB_PAGE_CAPACITY = 16 };
typedef struct {
    unsigned status;
    const char *content_type;
    sp5_buffer body;
    bool stopping;
    sp5_result (*stream)(void *context, const sp5_metrics_sink *sink);
    void *stream_context;
    const char *download_filename;
    /* Set by sp5_web_dispatch before routing; extension errors report it. */
    uint64_t request_id;
} sp5_web_response;
typedef struct {
    void *context;
    /* Invoked after peer/Host/Origin validation. Return false for an unknown
     * path. JSON/download rendering must fit response.body's fixed capacity. */
    bool (*route)(void *context, const sp5_request *request, uint64_t now_us,
                  sp5_web_response *response);
    /* Requested value has already persisted. Apply asynchronously off media/
     * input owners; status callback reports effective state until completion. */
    void (*settings_requested)(void *context, bool enabled, uint64_t now_us);
    bool (*metrics_effective)(void *context);
} sp5_web_extensions;
typedef struct {
    sp5_store *store;
    sp5_host *host;
    sp5_session *session;
    /* Adapter coordinates the independent input owner and quiesces media leases
     * before dispatching revoke/stop cleanup. Fixture has no concurrent producers. */
    sp5_lifecycle *lifecycle;
    sp5_pairing *pairing;
    sp5_pair_flow *pair_flow;
    /* Exact served authority (including non-default port), no trailing slash.
     * Dispatch serves the embedded allowlisted assets (web/assets.h) for GET
     * after the same peer/Host/Origin checks; adapters only frame responses. */
    const char *authority;
    bool https;
    sp5_web_extensions extensions;
} sp5_web_config;
typedef struct sp5_web sp5_web;
/* Setup-only allocation. Serialized bounded management owner; dependencies
 * outlive server. Socket adapter supplies actual peer/interface, never headers.
 * Dispatch never runs on input/media owners. No per-browser worker exists. */
sp5_web *sp5_web_create(const sp5_web_config *config);
void sp5_web_destroy(sp5_web *web);
sp5_result sp5_web_dispatch(sp5_web *web, const char *peer, bool on_link, sp5_bytes http,
                            uint64_t now_us, sp5_web_response *response);
/* Adapter sends the complete stopping acknowledgement before calling finish.
 * Quiesce media producers first; retry cleanup errors without losing ownership.
 * Only the adapter exits/removes listeners after successful cleanup. */
sp5_result sp5_web_finish_stop(sp5_web *web);
bool sp5_web_error(sp5_web_response *response, unsigned status, const char *code,
                   const char *message, const char *recovery, uint64_t request_id);
#endif
