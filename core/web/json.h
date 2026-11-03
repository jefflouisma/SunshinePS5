#ifndef SUNSHINE_WEB_JSON_H
#define SUNSHINE_WEB_JSON_H
#include "runtime.h"

/* Bounded management-only JSON. Flat mutation objects deliberately reject
 * unknown/duplicate keys, nesting, floats and truncated or invalid UTF-8. */
enum { SP5_JSON_FIELDS = 6, SP5_JSON_STRING = 256 };
typedef enum { SP5_JSON_TEXT, SP5_JSON_BOOL, SP5_JSON_UINT } sp5_json_type;
typedef struct {
    char key[32], text[SP5_JSON_STRING];
    size_t size;
    sp5_json_type type;
    bool boolean;
    uint64_t number;
} sp5_json_field;
typedef struct {
    sp5_json_field fields[SP5_JSON_FIELDS];
    size_t count;
} sp5_json_object;
bool sp5_json_parse(sp5_bytes input, sp5_json_object *object);
const sp5_json_field *sp5_json_get(const sp5_json_object *object, const char *key);
bool sp5_json_keys(const sp5_json_object *object, const char *const *keys, size_t count);
/* Appends to caller storage; false leaves an unusable response that dispatch
 * turns into a capacity error. Never publish partially rendered JSON. */
bool sp5_json_raw(sp5_buffer *out, const char *text);
bool sp5_json_string(sp5_buffer *out, sp5_bytes text);
bool sp5_json_u64(sp5_buffer *out, uint64_t value);
#endif
