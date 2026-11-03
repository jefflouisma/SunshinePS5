#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*recovery_inspector_observe_fn)(void);

int recovery_inspector_launcher_run(uint32_t firmware, recovery_inspector_observe_fn observe,
                                    FILE *output);

#define CHECK(value)                                                                               \
    do {                                                                                           \
        if (!(value))                                                                              \
            return check_failed(#value);                                                           \
    } while (0)

static unsigned calls;

static int check_failed(const char *expression)
{
    if (fputs("check failed: ", stderr) == EOF || fputs(expression, stderr) == EOF ||
        fputc('\n', stderr) == EOF)
        return 1;
    return 1;
}

static int observe_success(void)
{
    ++calls;
    return 0;
}

static int observe_distinct_failure(void)
{
    ++calls;
    return 37;
}

struct expected {
    int result;
    unsigned calls;
    const char *output;
};

static int run_case(uint32_t firmware, recovery_inspector_observe_fn observe,
                    struct expected expected)
{
    char buffer[256] = {0};
    FILE *output = tmpfile();
    CHECK(output != NULL);
    calls = 0;
    CHECK(recovery_inspector_launcher_run(firmware, observe, output) == expected.result);
    CHECK(calls == expected.calls);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    size_t length = fread(buffer, 1, sizeof(buffer) - 1, output);
    CHECK(!ferror(output));
    CHECK(length == strlen(expected.output));
    CHECK(memcmp(buffer, expected.output, length) == 0);
    CHECK(fclose(output) == 0);
    return 0;
}

int main(void)
{
    CHECK(run_case(UINT32_C(0x13600007), observe_success,
                   (struct expected){.result = 0, .calls = 1, .output = ""}) == 0);
    CHECK(run_case(UINT32_C(0x13600006), observe_success,
                   (struct expected){.result = 78,
                                     .calls = 0,
                                     .output = "{\"schema_version\":1,\"complete\":false,"
                                               "\"error\":\"unsupported_firmware\","
                                               "\"expected_firmware\":\"0x13600007\","
                                               "\"observed_firmware\":\"0x13600006\"}\n"}) == 0);
    CHECK(run_case(UINT32_C(0x13600007), NULL,
                   (struct expected){.result = 78,
                                     .calls = 0,
                                     .output = "{\"schema_version\":1,\"complete\":false,"
                                               "\"error\":\"observer_unavailable\"}\n"}) == 0);
    CHECK(run_case(UINT32_C(0x13600007), observe_distinct_failure,
                   (struct expected){.result = 37, .calls = 1, .output = ""}) == 0);
    CHECK(recovery_inspector_launcher_run(UINT32_C(0x13600007), observe_success, NULL) == 70);
    CHECK(calls == 1);
    puts("recovery inspector launcher: PASS");
    return 0;
}
