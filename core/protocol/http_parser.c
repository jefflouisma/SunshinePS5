#include "http.h"

#include <string.h>

static bool token(uint8_t value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') ||
           (value != 0 && strchr("!#$%&'*+-.^_`|~", value) != NULL);
}

static int hex_digit(uint8_t value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool equal_case(sp5_bytes value, const char *text)
{
    if (value.size != strlen(text)) {
        return false;
    }
    for (size_t i = 0; i < value.size; ++i) {
        uint8_t lower = value.data[i];
        if (lower >= 'A' && lower <= 'Z') {
            lower = (uint8_t)(lower + ('a' - 'A'));
        }
        if (lower != (uint8_t)text[i]) {
            return false;
        }
    }
    return true;
}

static bool line(sp5_bytes wire, size_t *offset, sp5_bytes *value)
{
    size_t start = *offset;
    for (size_t i = start; i + 1 < wire.size; ++i) {
        if (wire.data[i] == '\r' && wire.data[i + 1] == '\n') {
            *value = (sp5_bytes){wire.data + start, i - start};
            *offset = i + 2;
            return true;
        }
        if (wire.data[i] == '\n' || wire.data[i] == '\r' || wire.data[i] == 0 ||
            (wire.data[i] < 32 && wire.data[i] != '\t') || wire.data[i] == 127) {
            return false;
        }
    }
    return false;
}

static bool decimal(sp5_bytes value, size_t *number)
{
    if (value.size == 0) {
        return false;
    }
    *number = 0;
    for (size_t i = 0; i < value.size; ++i) {
        uint8_t digit = value.data[i];
        if (digit < '0' || digit > '9' || *number > (SIZE_MAX - (digit - '0')) / 10) {
            return false;
        }
        *number = *number * 10 + (digit - '0');
    }
    return true;
}

sp5_result sp5_http_parse(sp5_bytes wire, size_t limit, sp5_request *request)
{
    if (request == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *request = (sp5_request){0};
    if (wire.data == NULL || wire.size == 0 || wire.size > limit || wire.size > SP5_HTTP_LIMIT) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    size_t offset = 0;
    sp5_bytes first;
    if (!line(wire, &offset, &first)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    size_t method_end = 0;
    while (method_end < first.size && token(first.data[method_end])) {
        ++method_end;
    }
    if (method_end == 0 || method_end >= first.size || first.data[method_end] != ' ') {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    size_t target_start = method_end + 1;
    size_t target_end = target_start;
    while (target_end < first.size && first.data[target_end] != ' ') {
        uint8_t value = first.data[target_end];
        if (value <= 32 || value >= 127 || value == '#' || value == '\\') {
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
        if (value == '%') {
            if (target_end + 2 >= first.size || hex_digit(first.data[target_end + 1]) < 0 ||
                hex_digit(first.data[target_end + 2]) < 0) {
                return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
            }
            target_end += 2;
        }
        ++target_end;
    }
    if (target_start == target_end || first.data[target_start] != '/' ||
        first.size - target_end != 9 ||
        (memcmp(first.data + target_end, " HTTP/1.1", 9) != 0 &&
         memcmp(first.data + target_end, " HTTP/1.0", 9) != 0)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    bool length_present = false;
    size_t content_length = 0;
    unsigned header_count = 0;
    for (;;) {
        sp5_bytes header;
        if (!line(wire, &offset, &header) || ++header_count > 64) {
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
        if (header.size == 0) {
            break;
        }
        size_t colon = 0;
        while (colon < header.size && token(header.data[colon])) {
            ++colon;
        }
        if (colon == 0 || colon >= header.size || header.data[colon] != ':') {
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
        sp5_bytes name = {header.data, colon};
        size_t start = colon + 1;
        size_t end = header.size;
        while (start < end && (header.data[start] == ' ' || header.data[start] == '\t')) {
            ++start;
        }
        while (end > start && (header.data[end - 1] == ' ' || header.data[end - 1] == '\t')) {
            --end;
        }
        if (equal_case(name, "transfer-encoding")) {
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
        if (equal_case(name, "content-length")) {
            if (length_present ||
                !decimal((sp5_bytes){header.data + start, end - start}, &content_length)) {
                return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
            }
            length_present = true;
        }
    }
    if (content_length != wire.size - offset) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *request = (sp5_request){.method = {wire.data, method_end},
                             .target = {first.data + target_start, target_end - target_start},
                             .body = {wire.data + offset, content_length}};
    return sp5_success();
}

static sp5_result decode(sp5_bytes encoded, sp5_buffer *output)
{
    for (size_t i = 0; i < encoded.size; ++i) {
        uint8_t value = encoded.data[i];
        if (value == '%') {
            if (i + 2 >= encoded.size) {
                return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
            }
            int high = hex_digit(encoded.data[i + 1]);
            int low = hex_digit(encoded.data[i + 2]);
            if (high < 0 || low < 0) {
                return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
            }
            value = (uint8_t)((high << 4) | low);
            i += 2;
        } else if (value == '+') {
            value = ' ';
        }
        if (value < 32 || value == 127) {
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
        if (output != NULL && sp5_buffer_append(output, (sp5_bytes){&value, 1}).status != SP5_OK) {
            return sp5_failure(SP5_REASON_CAPACITY);
        }
    }
    return sp5_success();
}

sp5_result sp5_http_query(const sp5_request *request, const char *key, sp5_buffer *value,
                          bool *present)
{
    if (request == NULL || key == NULL || value == NULL || present == NULL || value->data == NULL ||
        request->target.data == NULL || request->target.size > SP5_HTTP_LIMIT) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *present = false;
    value->size = 0;
    sp5_bytes target = request->target;
    const uint8_t *question = memchr(target.data, '?', target.size);
    if (question == NULL) {
        return sp5_success();
    }
    size_t start = (size_t)(question - target.data) + 1;
    uint8_t keys[SP5_HTTP_QUERY_CAPACITY][64];
    size_t lengths[SP5_HTTP_QUERY_CAPACITY];
    size_t count = 0;
    while (start < target.size) {
        size_t end = start;
        while (end < target.size && target.data[end] != '&') {
            ++end;
        }
        const uint8_t *equal = memchr(target.data + start, '=', end - start);
        if (equal == NULL || equal == target.data + start || count == SP5_HTTP_QUERY_CAPACITY) {
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
        sp5_buffer decoded_key = {keys[count], sizeof(keys[count]), 0};
        if (decode((sp5_bytes){target.data + start, (size_t)(equal - target.data) - start},
                   &decoded_key)
                .status != SP5_OK) {
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
        for (size_t i = 0; i < count; ++i) {
            if (lengths[i] == decoded_key.size &&
                memcmp(keys[i], decoded_key.data, decoded_key.size) == 0) {
                return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
            }
        }
        lengths[count++] = decoded_key.size;
        bool match =
            strlen(key) == decoded_key.size && memcmp(key, decoded_key.data, decoded_key.size) == 0;
        sp5_bytes encoded_value = {equal + 1, end - (size_t)(equal + 1 - target.data)};
        sp5_result result = decode(encoded_value, match ? value : NULL);
        if (result.status != SP5_OK) {
            value->size = 0;
            return result;
        }
        *present = *present || match;
        if (end < target.size && end + 1 == target.size) {
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
        start = end + 1;
    }
    return sp5_success();
}
