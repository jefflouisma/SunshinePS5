#ifndef SUNSHINE_WEB_INTERNAL_H
#define SUNSHINE_WEB_INTERNAL_H
#include "server.h"
typedef struct {
    char token[65];
    uint64_t expires_us, issued;
} sp5_web_page;
struct sp5_web {
    sp5_web_config config;
    char authority[192], origin[208];
    sp5_web_page pages[SP5_WEB_PAGE_CAPACITY];
    uint64_t request_id, now_us, pages_issued;
};
bool sp5_web_status(sp5_web *web, uint64_t now_us, sp5_web_response *response);
bool sp5_web_devices(sp5_web *web, const sp5_request *request, const sp5_json_object *body,
                     sp5_web_response *response);
bool sp5_web_settings(sp5_web *web, const sp5_json_object *body, uint64_t now_us,
                      sp5_web_response *response);
bool sp5_web_stop(sp5_web *web, const sp5_json_object *body, sp5_web_response *response);
bool sp5_web_equal(sp5_bytes value, const char *text);
bool sp5_web_text(sp5_buffer *out, const char *text);
bool sp5_web_confirm(const sp5_json_object *body);
#endif
