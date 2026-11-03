#include "json.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
static bool utf8(sp5_bytes text);

bool sp5_json_raw(sp5_buffer *out, const char *text)
{
    return text != NULL &&
           sp5_buffer_append(out, (sp5_bytes){(const uint8_t *)text, strlen(text)}).status ==
               SP5_OK;
}
bool sp5_json_u64(sp5_buffer *out, uint64_t value)
{
    char number[32];
    int n = snprintf(number, sizeof(number), "%" PRIu64, value);
    return n > 0 && (size_t)n < sizeof(number) && sp5_json_raw(out, number);
}
bool sp5_json_string(sp5_buffer *out, sp5_bytes text)
{
    if (text.size && !text.data)
        return false;
    /* Pairing metadata is borrowed untrusted bytes, unlike validated names.
     * Preserve valid Unicode; encode malformed bytes as escaped byte values so
     * the entire status document remains valid JSON. */
    bool valid = utf8(text);
    if (!sp5_json_raw(out, "\""))
        return false;
    for (size_t i = 0; i < text.size; i++) {
        uint8_t c = text.data[i];
        if (c == '"' || c == '\\') {
            uint8_t escaped[2] = {'\\', c};
            if (sp5_buffer_append(out, (sp5_bytes){escaped, 2}).status != SP5_OK)
                return false;
        } else if (c < 32 || c == 127 || (!valid && c >= 128)) {
            char escaped[7];
            if (snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)c) != 6 ||
                !sp5_json_raw(out, escaped))
                return false;
        } else if (sp5_buffer_append(out, (sp5_bytes){&c, 1}).status != SP5_OK)
            return false;
    }
    return sp5_json_raw(out, "\"");
}
static void space(sp5_bytes in, size_t *at)
{
    while (*at < in.size && (in.data[*at] == ' ' || in.data[*at] == '\t' || in.data[*at] == '\r' ||
                             in.data[*at] == '\n'))
        ++*at;
}
static int hex(uint8_t c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static bool unit(sp5_bytes in, size_t *at, uint32_t *value)
{
    *value = 0;
    if (in.size - *at < 4)
        return false;
    for (unsigned i = 0; i < 4; i++) {
        int h = hex(in.data[(*at)++]);
        if (h < 0)
            return false;
        *value = (*value << 4) | (unsigned)h;
    }
    return true;
}
static bool utf8(sp5_bytes s)
{
    for (size_t i = 0; i < s.size;) {
        uint32_t v = s.data[i++], minimum = 0;
        unsigned more = 0;
        if (v < 128)
            continue;
        if (v >= 0xc2 && v <= 0xdf) {
            v &= 31;
            more = 1;
            minimum = 128;
        } else if (v >= 0xe0 && v <= 0xef) {
            v &= 15;
            more = 2;
            minimum = 2048;
        } else if (v >= 0xf0 && v <= 0xf4) {
            v &= 7;
            more = 3;
            minimum = 65536;
        } else
            return false;
        if (more > s.size - i)
            return false;
        while (more--) {
            uint8_t c = s.data[i++];
            if ((c & 0xc0) != 0x80)
                return false;
            v = (v << 6) | (c & 63);
        }
        if (v < minimum || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
            return false;
    }
    return true;
}
static bool string(sp5_bytes in, size_t *at, char *out, size_t capacity, size_t *length)
{
    if (*at >= in.size || in.data[*at] != '"')
        return false;
    ++*at;
    size_t used = 0;
    while (*at < in.size) {
        uint8_t c = in.data[(*at)++];
        if (c == '"') {
            out[used] = 0;
            *length = used;
            return utf8((sp5_bytes){(const uint8_t *)out, used});
        }
        if (c < 32)
            return false;
        uint8_t bytes[4] = {c};
        size_t count = 1;
        if (c == '\\') {
            if (*at == in.size)
                return false;
            c = in.data[(*at)++];
            if (c == 'u') {
                uint32_t u = 0;
                if (!unit(in, at, &u))
                    return false;
                if (u >= 0xd800 && u <= 0xdbff) {
                    if (in.size - *at < 6 || in.data[*at] != '\\' || in.data[*at + 1] != 'u')
                        return false;
                    *at += 2;
                    uint32_t low = 0;
                    if (!unit(in, at, &low) || low < 0xdc00 || low > 0xdfff)
                        return false;
                    u = 0x10000 + ((u - 0xd800) << 10) + (low - 0xdc00);
                } else if (u >= 0xdc00 && u <= 0xdfff)
                    return false;
                if (u == 0)
                    return false;
                if (u < 128)
                    bytes[0] = (uint8_t)u;
                else if (u < 2048) {
                    count = 2;
                    bytes[0] = (uint8_t)(0xc0 | (u >> 6));
                    bytes[1] = (uint8_t)(0x80 | (u & 63));
                } else if (u < 65536) {
                    count = 3;
                    bytes[0] = (uint8_t)(0xe0 | (u >> 12));
                    bytes[1] = (uint8_t)(0x80 | ((u >> 6) & 63));
                    bytes[2] = (uint8_t)(0x80 | (u & 63));
                } else {
                    count = 4;
                    bytes[0] = (uint8_t)(0xf0 | (u >> 18));
                    bytes[1] = (uint8_t)(0x80 | ((u >> 12) & 63));
                    bytes[2] = (uint8_t)(0x80 | ((u >> 6) & 63));
                    bytes[3] = (uint8_t)(0x80 | (u & 63));
                }
            } else {
                switch (c) {
                case '"':
                case '\\':
                case '/':
                    bytes[0] = c;
                    break;
                case 'b':
                    bytes[0] = 8;
                    break;
                case 'f':
                    bytes[0] = 12;
                    break;
                case 'n':
                    bytes[0] = 10;
                    break;
                case 'r':
                    bytes[0] = 13;
                    break;
                case 't':
                    bytes[0] = 9;
                    break;
                default:
                    return false;
                }
            }
        }
        if (count >= capacity - used)
            return false;
        memcpy(out + used, bytes, count);
        used += count;
    }
    return false;
}
const sp5_json_field *sp5_json_get(const sp5_json_object *o, const char *key)
{
    for (size_t i = 0; i < o->count; i++)
        if (strcmp(o->fields[i].key, key) == 0)
            return &o->fields[i];
    return NULL;
}
bool sp5_json_keys(const sp5_json_object *o, const char *const *keys, size_t count)
{
    for (size_t i = 0; i < o->count; i++) {
        bool found = false;
        for (size_t j = 0; j < count; j++)
            found = found || strcmp(o->fields[i].key, keys[j]) == 0;
        if (!found)
            return false;
    }
    return true;
}
bool sp5_json_parse(sp5_bytes in, sp5_json_object *o)
{
    if (!o || !in.data)
        return false;
    memset(o, 0, sizeof(*o));
    size_t at = 0;
    space(in, &at);
    if (at == in.size || in.data[at] != '{')
        return false;
    ++at;
    space(in, &at);
    if (at < in.size && in.data[at] == '}') {
        at++;
        space(in, &at);
        return at == in.size;
    }
    while (at < in.size && o->count < SP5_JSON_FIELDS) {
        sp5_json_field *f = &o->fields[o->count];
        size_t key_size = 0;
        if (!string(in, &at, f->key, sizeof(f->key), &key_size) || key_size == 0 ||
            sp5_json_get(o, f->key))
            return false;
        space(in, &at);
        if (at == in.size || in.data[at] != ':')
            return false;
        ++at;
        space(in, &at);
        if (at == in.size)
            return false;
        if (in.data[at] == '"') {
            f->type = SP5_JSON_TEXT;
            if (!string(in, &at, f->text, sizeof(f->text), &f->size))
                return false;
        } else if (in.size - at >= 4 && memcmp(in.data + at, "true", 4) == 0) {
            f->type = SP5_JSON_BOOL;
            f->boolean = true;
            at += 4;
        } else if (in.size - at >= 5 && memcmp(in.data + at, "false", 5) == 0) {
            f->type = SP5_JSON_BOOL;
            at += 5;
        } else {
            f->type = SP5_JSON_UINT;
            size_t start = at;
            while (at < in.size && in.data[at] >= '0' && in.data[at] <= '9') {
                unsigned d = in.data[at++] - '0';
                if (f->number > (UINT64_MAX - d) / 10)
                    return false;
                f->number = f->number * 10 + d;
            }
            if (at == start || (at - start > 1 && in.data[start] == '0'))
                return false;
        }
        ++o->count;
        space(in, &at);
        if (at == in.size)
            return false;
        if (in.data[at] == '}') {
            at++;
            space(in, &at);
            return at == in.size;
        }
        if (in.data[at++] != ',')
            return false;
        space(in, &at);
    }
    return false;
}
