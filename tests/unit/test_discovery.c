#include "protocol/discovery.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            if (fputs(__FILE__ ": " #c "\n", stderr) == EOF) {                                     \
                return 2;                                                                          \
            }                                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
typedef struct {
    unsigned published, withdrawn;
    bool fail;
    sp5_discovery_record record;
} fixture;
static sp5_result publish(void *context, const sp5_discovery_record *record)
{
    fixture *f = context;
    ++f->published;
    f->record = *record;
    return f->fail ? sp5_failure(SP5_REASON_PLATFORM) : sp5_success();
}
static sp5_result withdraw(void *context)
{
    fixture *f = context;
    ++f->withdrawn;
    return sp5_success();
}
static sp5_bytes bytes(const char *s)
{
    return (sp5_bytes){(const uint8_t *)s, strlen(s)};
}
int main(void)
{
    fixture f = {0};
    sp5_discovery d;
    sp5_host h;
    CHECK(sp5_host_init(&h, "fixture", "13.60", "10.1.2.3").status == SP5_OK);
    CHECK(sp5_discovery_init(&d, (sp5_discovery_ops){&f, publish, withdraw}).status == SP5_OK);
    CHECK(sp5_discovery_refresh(&d, &h, 4, 47989).status == SP5_OK);
    CHECK(f.published == 1 && strcmp(f.record.name, "SunshinePS5") == 0);
    CHECK(f.record.interface_index == 4 && f.record.port == 47989);
    CHECK(sp5_discovery_refresh(&d, &h, 4, 47989).status == SP5_OK && f.published == 1);
    sp5_settings settings = sp5_settings_defaults();
    memcpy(settings.host_name, "Living room", 12);
    CHECK(sp5_host_settings(&h, &settings).status == SP5_OK);
    CHECK(sp5_discovery_refresh(&d, &h, 4, 47989).status == SP5_OK);
    CHECK(f.published == 2 && f.withdrawn == 1 && strcmp(f.record.name, "Living room") == 0);
    CHECK(sp5_discovery_refresh(&d, &h, 0, 47989).status == SP5_ERROR);
    CHECK(f.published == 2 && d.published);
    memcpy(settings.host_name, "Other room", 11);
    CHECK(sp5_host_settings(&h, &settings).status == SP5_OK);
    f.fail = true;
    CHECK(sp5_discovery_refresh(&d, &h, 4, 47989).status == SP5_ERROR && !d.published);
    f.fail = false;
    CHECK(sp5_discovery_refresh(&d, &h, 4, 47989).status == SP5_OK);
    CHECK(sp5_discovery_stop(&d).status == SP5_OK);
    CHECK(sp5_discovery_stop(&d).status == SP5_OK);
    CHECK(sp5_host_transition(&h, SP5_HOST_STOPPING).status == SP5_OK);
    CHECK(sp5_discovery_refresh(&d, &h, 4, 47989).status == SP5_OK && !d.published);
    sp5_manual_endpoint ep;
    CHECK(sp5_discovery_manual(bytes("10.77.1.3"), false, &ep).status == SP5_OK);
    CHECK(strcmp(ep.address, "10.77.1.3") == 0 && ep.port == 47989);
    CHECK(sp5_discovery_manual(bytes("172.30.9.1:48089"), false, &ep).status == SP5_OK &&
          ep.port == 48089);
    CHECK(sp5_discovery_manual(bytes("[fd12::1]:48089"), false, &ep).status == SP5_OK &&
          ep.port == 48089);
    CHECK(sp5_discovery_manual(bytes("fd12::1"), false, &ep).status == SP5_OK);
    CHECK(sp5_discovery_manual(bytes("fe80::1"), true, &ep).status == SP5_OK);
    const char *bad[] = {
        "fe80::1",     "8.8.8.8",  "127.0.0.1",       "10.0.0.1:0",  "10.0.0.1:65536",
        "10.0.0.1:-1", "[fd12::1", "http://10.0.0.1", "10.0.0.1:1x", ""};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(sp5_discovery_manual(bytes(bad[i]), false, &ep).status == SP5_ERROR);
        CHECK(ep.address[0] == 0 && ep.port == 0);
    }
    return 0;
}
