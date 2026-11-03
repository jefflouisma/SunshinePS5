#include "assets.h"
#include <stdio.h>
#include <string.h>

const sp5_web_asset *sp5_web_asset_find(sp5_bytes target)
{
    static const char index_path[] = "/index.html";
    if (target.size == 1 && target.data != NULL && target.data[0] == '/')
        target = (sp5_bytes){(const uint8_t *)index_path, sizeof(index_path) - 1};
    if (target.data == NULL || target.size < 2 || target.data[0] != '/')
        return NULL;
    for (size_t i = 0; i < sp5_web_asset_count; ++i)
        if (strlen(sp5_web_assets[i].path) == target.size &&
            memcmp(sp5_web_assets[i].path, target.data, target.size) == 0)
            return &sp5_web_assets[i];
    return NULL;
}

static const char *reason(unsigned status)
{
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 429:
        return "Too Many Requests";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Response";
    }
}

static bool header_safe(const char *value, bool filename)
{
    if (value == NULL || value[0] == 0)
        return false;
    for (const char *p = value; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '.' || c == '-' || c == '_';
        if (filename ? !plain : (c < 32 || c >= 127))
            return false;
    }
    return true;
}

sp5_result sp5_web_format_head(const sp5_web_response *r, sp5_buffer *head)
{
    if (r == NULL || head == NULL || head->data == NULL || r->status < 100 || r->status > 599 ||
        !header_safe(r->content_type, false) ||
        (r->stream != NULL && !header_safe(r->download_filename, true)))
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    char framing[192];
    int n = r->stream != NULL
                ? snprintf(framing, sizeof(framing),
                           "Transfer-Encoding: chunked\r\nContent-Disposition: attachment; "
                           "filename=\"%s\"\r\n",
                           r->download_filename)
                : snprintf(framing, sizeof(framing), "Content-Length: %zu\r\n", r->body.size);
    if (n < 0 || (size_t)n >= sizeof(framing))
        return sp5_failure(SP5_REASON_CAPACITY);
    char text[1024];
    n = snprintf(text, sizeof(text),
                 "HTTP/1.1 %u %s\r\nContent-Type: %s\r\n%sCache-Control: no-store\r\n"
                 "X-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'self'; "
                 "script-src 'self'; style-src 'self'; object-src 'none'; base-uri 'none'; "
                 "frame-ancestors 'none'\r\nReferrer-Policy: no-referrer\r\n"
                 "Cross-Origin-Resource-Policy: same-origin\r\nConnection: close\r\n\r\n",
                 r->status, reason(r->status), r->content_type, framing);
    if (n < 0 || (size_t)n >= sizeof(text))
        return sp5_failure(SP5_REASON_CAPACITY);
    head->size = 0;
    return sp5_buffer_append(head, (sp5_bytes){(const uint8_t *)text, (size_t)n});
}
