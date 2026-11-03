#ifndef SUNSHINE_WEB_ASSETS_H
#define SUNSHINE_WEB_ASSETS_H
#include "server.h"

/* Immutable copies of the allowlisted webapp sources, generated at build time
 * by cmake/embed-webapp.cmake. The console serves only these bytes: no file
 * system lookup, CDN or network fetch is involved (FR-013, offline use). */
typedef struct {
    const char *path;
    const char *content_type;
    const uint8_t *data;
    size_t size;
} sp5_web_asset;
extern const sp5_web_asset sp5_web_assets[];
extern const size_t sp5_web_asset_count;
/* Exact match of a request target ("/" selects /index.html); NULL otherwise.
 * sp5_web_dispatch consults this only for GET after peer/Host/Origin checks,
 * and never for /api/ targets. */
const sp5_web_asset *sp5_web_asset_find(sp5_bytes target);
/* Shared response head for every adapter: status line, Content-Type and either
 * Content-Length (body) or chunked attachment framing (stream), plus no-store,
 * nosniff, the restrictive CSP (frame-ancestors 'none'), no-referrer and a
 * same-origin resource policy. No cross-origin allowance is ever emitted.
 * Rejects header-injecting content types and unsafe download names. */
sp5_result sp5_web_format_head(const sp5_web_response *response, sp5_buffer *head);
#endif
