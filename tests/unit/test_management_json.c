#include "web/json.h"
#include <stdio.h>
#include <string.h>
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            if (fputs(#c "\n", stderr) == EOF)                                                     \
                return 2;                                                                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static sp5_bytes text(const char *s)
{
    return (sp5_bytes){(const uint8_t *)s, strlen(s)};
}
int main(void)
{
    sp5_json_object object;
    const char *bad[] = {"",
                         "[]",
                         "{",
                         "{\"x\":null}",
                         "{\"x\":-1}",
                         "{\"x\":1.0}",
                         "{\"x\":01}",
                         "{\"x\":18446744073709551616}",
                         "{\"x\":true,}",
                         "{\"x\":true,\"x\":false}",
                         "{\"x\":\"\\u0000\"}",
                         "{\"x\":\"\\udc00\"}",
                         "{\"x\":\"\\ud800x\"}",
                         "{\"x\":\"\xc0\xaf\"}",
                         "{\"x\":{}}",
                         "{\"x\":\"bad\ntext\"}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!sp5_json_parse(text(bad[i]), &object));
    CHECK(sp5_json_parse(
        text(" {\"name\":\"A\\uD83C\\uDFAE\",\"value\":18446744073709551615,\"flag\":true} \n"),
        &object));
    CHECK(object.count == 3 && sp5_json_get(&object, "value")->number == UINT64_MAX);
    CHECK(strcmp(sp5_json_get(&object, "name")->text, "A\xf0\x9f\x8e\xae") == 0);
    CHECK(sp5_json_get(&object, "flag")->boolean);
    uint8_t bytes[100];
    sp5_buffer out = {bytes, sizeof(bytes) - 1, 0};
    CHECK(sp5_json_string(&out, text("a\"\\\n\xf0\x9f\x8e\xae")));
    bytes[out.size] = 0;
    CHECK(strcmp((char *)bytes, "\"a\\\"\\\\\\u000a\xf0\x9f\x8e\xae\"") == 0);
    out.size = 0;
    CHECK(sp5_json_string(&out, text("\xc0\xaf")));
    bytes[out.size] = 0;
    CHECK(strcmp((char *)bytes, "\"\\u00c0\\u00af\"") == 0);
    for (size_t capacity = 0; capacity < 6; capacity++) {
        out = (sp5_buffer){bytes, capacity, 0};
        CHECK(!sp5_json_string(&out, text("12345")));
        CHECK(out.size <= capacity);
    }
    return 0;
}
