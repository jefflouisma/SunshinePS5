/* Nonshipping T041 probe tests: simulated service responses, never hardware evidence. */
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../tools/ps5-probes/media_probe.h"

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            (void)fputs(__FILE__ ": check failed: " #expression "\n", stderr);                     \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

struct module_fixture {
    char events[32];
    size_t count;
    int fail_handle, rc, result;
    bool fail_begin, fail_complete, interrupt;
};
static struct module_fixture module_fixture;
static struct media_modules interrupted_modules;
static jmp_buf module_jump;

static void module_event(struct module_fixture *fixture, char event)
{
    CHECK(fixture->count + 1 < sizeof(fixture->events));
    fixture->events[fixture->count++] = event;
    fixture->events[fixture->count] = '\0';
}

static int fake_module_unload(void *context, int handle, int *result)
{
    struct module_fixture *fixture = context;
    CHECK(handle >= 0 && handle < R3_MEDIA_MODULE_CAPACITY);
    module_event(fixture, (char)('a' + handle));
    if (fixture->interrupt)
        longjmp(module_jump, 1);
    *result = handle == fixture->fail_handle ? fixture->result : 0;
    return handle == fixture->fail_handle ? fixture->rc : 0;
}

static bool fake_module_observe(void *context, const struct media_module *module)
{
    struct module_fixture *fixture = context;
    CHECK(module->owned && module->attempted && module->name != NULL);
    module_event(fixture, (char)((module->completed ? '0' : 'A') + module->handle));
    if (module->completed) {
        CHECK(module->rc == (module->handle == fixture->fail_handle ? fixture->rc : 0));
        CHECK(module->result == (module->handle == fixture->fail_handle ? fixture->result : 0));
    }
    return !(module->completed ? fixture->fail_complete : fixture->fail_begin);
}

static const struct media_module_api module_api = {fake_module_unload, fake_module_observe,
                                                   &module_fixture};

static struct media_modules owned_modules(size_t count)
{
    struct media_modules modules = {.count = R3_MEDIA_MODULE_CAPACITY};
    for (size_t i = 0; i < modules.count; ++i)
        modules.entries[i] =
            (struct media_module){.name = "module", .handle = (int)i, .owned = i < count};
    module_fixture = (struct module_fixture){.fail_handle = -1};
    return modules;
}

static void module_tests(void)
{
    const char *expected[] = {"", "Aa0", "Bb1Aa0", "Cc2Bb1Aa0"};
    for (size_t count = 0; count <= R3_MEDIA_MODULE_CAPACITY; ++count) {
        struct media_modules modules = owned_modules(count);
        CHECK(media_modules_cleanup(&module_api, &modules, true));
        CHECK(strcmp(module_fixture.events, expected[count]) == 0);
        CHECK(media_modules_cleanup(&module_api, &modules, true));
        CHECK(strcmp(module_fixture.events, expected[count]) == 0);
        for (size_t i = 0; i < modules.count; ++i) {
            CHECK(modules.entries[i].attempted == (i < count));
            CHECK(modules.entries[i].completed == (i < count));
        }
    }
    struct media_modules modules = owned_modules(3);
    modules.entries[1].owned = false;
    CHECK(media_modules_cleanup(&module_api, &modules, true));
    CHECK(strcmp(module_fixture.events, "Cc2Aa0") == 0);
    CHECK(!modules.entries[1].attempted);

    for (int failure = 0; failure < R3_MEDIA_MODULE_CAPACITY; ++failure) {
        for (unsigned kind = 0; kind < 2; ++kind) {
            modules = owned_modules(3);
            module_fixture.fail_handle = failure;
            module_fixture.rc = kind == 0 ? -17 : 0;
            module_fixture.result = kind == 1 ? -23 : 0;
            CHECK(!media_modules_cleanup(&module_api, &modules, true));
            CHECK(modules.failed && modules.entries[failure].completed);
            CHECK(module_fixture.count == (size_t)(3 - failure) * 3);
            for (int i = 0; i < failure; ++i)
                CHECK(!modules.entries[i].attempted);
            size_t count = module_fixture.count;
            CHECK(!media_modules_cleanup(&module_api, &modules, true));
            CHECK(count == module_fixture.count);
        }
    }
    modules = owned_modules(3);
    CHECK(!media_modules_cleanup(&module_api, &modules, false));
    CHECK(modules.failed && module_fixture.count == 0);
    CHECK(!media_modules_cleanup(&module_api, &modules, true));
    CHECK(module_fixture.count == 0);

    for (unsigned phase = 0; phase < 2; ++phase) {
        modules = owned_modules(3);
        module_fixture.fail_begin = phase == 0;
        module_fixture.fail_complete = phase == 1;
        CHECK(!media_modules_cleanup(&module_api, &modules, true));
        CHECK(strcmp(module_fixture.events, phase == 0 ? "C" : "Cc2") == 0);
        CHECK(modules.entries[2].attempted && modules.entries[2].completed == (phase == 1));
        CHECK(!modules.entries[1].attempted && !modules.entries[0].attempted);
        size_t count = module_fixture.count;
        CHECK(!media_modules_cleanup(&module_api, &modules, true));
        CHECK(count == module_fixture.count);
    }

    /* Escape a fake blocked call without a completion return. Static state avoids
     * indeterminate automatic values after longjmp. No real platform call occurs. */
    interrupted_modules = owned_modules(3);
    module_fixture.interrupt = true;
    if (setjmp(module_jump) == 0) {
        (void)media_modules_cleanup(&module_api, &interrupted_modules, true);
        CHECK(false);
    }
    CHECK(strcmp(module_fixture.events, "Cc") == 0);
    CHECK(interrupted_modules.entries[2].attempted && !interrupted_modules.entries[2].completed);
    CHECK(!media_modules_cleanup(&module_api, &interrupted_modules, true));
    CHECK(interrupted_modules.failed && strcmp(module_fixture.events, "Cc") == 0);
    CHECK(!interrupted_modules.entries[1].attempted && !interrupted_modules.entries[0].attempted);

    modules = owned_modules(3);
    modules.count = R3_MEDIA_MODULE_CAPACITY + 1;
    CHECK(!media_modules_cleanup(&module_api, &modules, true));
    CHECK(modules.failed && module_fixture.count == 0);
    puts("module cleanup: partial ownership, reverse order, failures, events, interruption passed");
}

static char calls[512];
static size_t call_count;
static int fail_at, read_result;
static bool malformed_sample, unexpected_handle;

static int called(char name)
{
    CHECK(call_count + 1 < sizeof(calls));
    calls[call_count++] = name;
    calls[call_count] = '\0';
    return fail_at == name ? -1 : 0;
}

static int fake_initialize(void)
{
    return called('I');
}
static int fake_terminate(void)
{
    return called('T');
}
static int fake_open(void **handle, const void *request, uint8_t kind)
{
    const struct audio_request *p = request;
    CHECK(kind == 0 && *handle == NULL);
    CHECK(p->size == 0x38 && p->unknown08 == 0x18 && p->source == 2);
    CHECK(p->mode == 0 && p->unknown1c == UINT32_MAX);
    CHECK(p->unknown10 == 0 && p->unknown20 == 0 && p->bitrate == 0);
    CHECK(p->reserved2c == 0 && p->unknown30 == 0);
    int rc = called('O');
    if (rc == 0 || unexpected_handle)
        *handle = calls;
    return rc;
}
static int fake_start(void *handle)
{
    CHECK(handle == calls);
    return called('S');
}
static int fake_stop(void *handle)
{
    CHECK(handle == calls);
    return called('P');
}
static int fake_close(void *handle)
{
    CHECK(handle == calls);
    return called('C');
}
static int fake_read(void *capture_handle, void *capture_buffer, size_t size, void *metadata,
                     uint32_t flags)
{
    CHECK(capture_handle != capture_buffer);
    CHECK(capture_handle == calls && size == 8192 && flags == 1);
    unsigned char expected[16] = {0x10};
    CHECK(memcmp(metadata, expected, sizeof(expected)) == 0);
    float *samples = capture_buffer;
    samples[0] = malformed_sample ? NAN : 0.25f;
    samples[1] = -0.5f;
    return read_result;
}

static const struct audio_api api = {fake_initialize, fake_terminate, fake_open, fake_start,
                                     fake_stop,       fake_close,     fake_read};

static bool resource_available, automatic_resource_availability;
static void fake_resource_callback(int32_t resource_id, int32_t operation, const void *opaque)
{
    CHECK(resource_id == R3_VIDEO_RESOURCE_ID && operation == 1 && opaque == NULL);
}
static int fake_resource_register(int32_t resource_id, resource_callback callback, void *context)
{
    CHECK(resource_id == R3_VIDEO_RESOURCE_ID && callback && context);
    callback(resource_id, 1, NULL);
    return called('G');
}
static int fake_resource_unregister(int32_t resource_id)
{
    CHECK(resource_id == R3_VIDEO_RESOURCE_ID);
    return called('U');
}
static int fake_resource_request(int32_t resource_id, const char *name, void *context, int32_t pid)
{
    CHECK(resource_id == R3_VIDEO_RESOURCE_ID && context && pid == 123);
    CHECK(strcmp(name, "RUNNING") == 0 || strcmp(name, "STOP") == 0);
    bool running = strcmp(name, "RUNNING") == 0;
    int rc = called(running ? 'R' : 'E');
    if (rc == 0 && automatic_resource_availability)
        resource_available = running;
    return rc;
}
static int fake_resource_query(int32_t *selectors)
{
    for (size_t i = 0; i < R3_RESOURCE_SELECTORS; ++i)
        selectors[i] = -1;
    if (resource_available)
        selectors[31] = R3_VIDEO_RESOURCE_ID;
    return called('Q');
}
static const struct resource_api resource_api = {fake_resource_register, fake_resource_unregister,
                                                 fake_resource_request, fake_resource_query};

struct removal_fixture {
    unsigned waits, remove_after, interrupt_after;
};
static bool fake_removal_wait(void *context)
{
    struct removal_fixture *fixture = context;
    ++fixture->waits;
    if (fixture->waits == fixture->remove_after)
        resource_available = false;
    return fixture->waits != fixture->interrupt_after;
}

static void resource_removal_tests(void)
{
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        struct resource_state state = {0};
        struct removal_fixture fixture = {0};
        call_count = 0;
        fail_at = scenario == 3 ? 'Q' : 0;
        resource_available = scenario != 0;
        fixture.remove_after = scenario == 1 ? R3_RESOURCE_CLEANUP_QUERIES - 1 : 0;
        fixture.interrupt_after = scenario == 4 ? 1 : 0;
        bool expected = scenario < 2;
        CHECK(resource_wait_removed(&resource_api, &state, fake_removal_wait, &fixture) ==
              expected);
        CHECK(state.cleanup_failed == !expected);
        CHECK(state.queries == (scenario == 1 || scenario == 2 ? R3_RESOURCE_CLEANUP_QUERIES : 1));
        CHECK(fixture.waits == (scenario == 1 || scenario == 2 ? R3_RESOURCE_CLEANUP_QUERIES - 1
                                : scenario == 4                ? 1
                                                               : 0));
        if (!expected) {
            uint32_t queries = state.queries;
            CHECK(!resource_wait_removed(&resource_api, &state, fake_removal_wait, &fixture));
            CHECK(queries == state.queries);
            struct media_modules modules = owned_modules(3);
            CHECK(!media_modules_cleanup(&module_api, &modules, !state.cleanup_failed));
            CHECK(module_fixture.count == 0);
        }
    }
    puts("selector removal: immediate/final success, exhaustion, query failure, interruption "
         "passed");
}

static void resource_tests(void)
{
    const int failures[] = {'G', 'R', 'Q', 'E', 'U', '\0'};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        memset(calls, 0, sizeof(calls));
        call_count = 0;
        fail_at = failures[i];
        resource_available = false;
        struct resource_state state = {.pid = 123};
        bool started = resource_start(&resource_api, &state, fake_resource_callback);
        CHECK(started == (fail_at != 'G' && fail_at != 'R'));
        if (started) {
            CHECK(resource_query(&resource_api, &state) == (fail_at != 'Q'));
            CHECK(!state.available);
            if (fail_at != 'Q') {
                resource_available = true;
                CHECK(resource_query(&resource_api, &state) && state.available);
                CHECK(state.selectors[31] == R3_VIDEO_RESOURCE_ID && state.queries == 2);
            }
        }
        bool cleanup_ok = fail_at != 'E' && fail_at != 'U';
        CHECK(resource_cleanup(&resource_api, &state) == cleanup_ok);
        CHECK(!state.registered && !state.requested);
        if (fail_at == 'G')
            CHECK(strcmp(calls, "G") == 0);
        else
            CHECK(strstr(calls, "EU") != NULL);
        size_t completed = call_count;
        CHECK(resource_cleanup(&resource_api, &state) == cleanup_ok && completed == call_count);
    }
}

static bool malformed_video;
static uint32_t fake_mode_flags;
static unsigned fake_capture_state_mode, fake_capture_state_busy_remaining;
static uint32_t word_at(const unsigned char *bytes, size_t offset)
{
    uint32_t value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}
static uint64_t qword_at(const unsigned char *bytes, size_t offset)
{
    uint64_t value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}
static int fake_video_query_headers(const void *request, void *output)
{
    CHECK(request != output);
    const unsigned char *header = request;
    unsigned char *packet = output;
    CHECK(qword_at(header, 0) == 0x28 && word_at(header, 0xc) == 2);
    CHECK(word_at(header, 0x18) == 0 && word_at(header, 0x1c) == 0);
    CHECK(qword_at(packet, 0) == 0x1a8);
    int rc = called('H');
    if (rc != 0)
        return rc;
    uint64_t length = 2;
    CHECK(media_copy(packet + 0x10, 0x1a8 - 0x10, &length, sizeof(length)));
    CHECK(media_copy(packet + 0x98, 0x1a8 - 0x98, &length, sizeof(length)));
    packet[0x18] = 0x67;
    packet[0x19] = 0x64;
    packet[0xa0] = 0x68;
    packet[0xa1] = 0xce;
    return 0;
}
static unsigned busy_opens;
static int fake_video_query_mode_flags(uint32_t *flags)
{
    CHECK(flags != NULL);
    int rc = called('F');
    if (rc == 0)
        *flags = fake_mode_flags;
    return rc;
}
static int fake_video_query_capture_state(void *output)
{
    CHECK(output != NULL);
    int rc = called('V');
    if (rc == 0) {
        unsigned char *bytes = output;
        memset(bytes, 0, R3_VIDEO_CAPTURE_STATE_BYTES);
        bytes[0x14] = fake_capture_state_busy_remaining != 0;
        if (fake_capture_state_busy_remaining != 0)
            --fake_capture_state_busy_remaining;
        bytes[0x17] = (unsigned char)fake_capture_state_mode;
    }
    return rc;
}
static int fake_video_open(void **handle, const void *request, uint8_t kind)
{
    const unsigned char *p = request;
    CHECK(kind == 0 && *handle == NULL);
    CHECK(resource_available);
    CHECK(qword_at(p, 0x00) == 0x2e8 && qword_at(p, 0x08) == 0x28);
    CHECK(word_at(p, 0x10) == 0 && word_at(p, 0x14) == 2);
    CHECK(word_at(p, 0x18) == 3 && word_at(p, 0x1c) == 0);
    CHECK(word_at(p, 0x28) == R3_VIDEO_RESOURCE_ID);
    CHECK(word_at(p, 0x2a8) == fake_mode_flags);
    uint32_t expected_mode = (fake_mode_flags & 1U) != 0   ? 6U
                             : (fake_mode_flags & 2U) != 0 ? 8U
                                                           : 2U;
    CHECK(word_at(p, 0x2ac) == expected_mode);
    CHECK(p[0x2bc] == (unsigned char)(fake_capture_state_mode ^ 1U));
    CHECK((word_at(p, 0x1a8) == 0 || word_at(p, 0x1a8) == 3) && qword_at(p, 0x1c0) == 0 &&
          qword_at(p, 0x1c8) == 0);
    int rc = called('O');
    if (word_at(p, 0x1a8) == 3)
        return (int)UINT32_C(0x81950001);
    if (busy_opens != 0) {
        --busy_opens;
        return (int)UINT32_C(0x81950008);
    }
    if (rc == 0 || unexpected_handle)
        *handle = calls;
    return rc;
}
static int fake_video_read(void *capture_handle, void *capture_buffer, size_t size, void *metadata,
                           uint32_t flags)
{
    CHECK(capture_handle != capture_buffer);
    CHECK(capture_handle == calls && size == R3_VIDEO_BUFFER_BYTES && flags == 0);
    unsigned char expected[112] = {0x70};
    CHECK(memcmp(metadata, expected, sizeof(expected)) == 0);
    if (read_result > 0 && (size_t)read_result <= R3_VIDEO_BUFFER_BYTES) {
        unsigned char frame[5] = {0, 0, 0, 1, 0x65};
        if ((size_t)read_result < sizeof(frame))
            return read_result;
        memcpy(capture_buffer, frame, sizeof(frame));
        if (malformed_video)
            memset(capture_buffer, 0, sizeof(frame));
        uint32_t result = (uint32_t)read_result;
        memcpy((unsigned char *)metadata + 0x5c, &result, sizeof(result));
    }
    return read_result;
}

static void reset(void)
{
    memset(calls, 0, sizeof(calls));
    call_count = 0;
    fail_at = 0;
    read_result = 8;
    malformed_sample = unexpected_handle = false;
    malformed_video = false;
    busy_opens = 0;
    fake_mode_flags = 0;
    fake_capture_state_mode = 0;
    fake_capture_state_busy_remaining = 0;
}

static void parameter_tests(void)
{
    /* Syntax-only fixtures exercise bounds; these are never console evidence. */
    static const unsigned char avc[] = {0, 0,    1,    0x67, 0x42, 0x80, 0x1f, 0,    0,    0,
                                        1, 0x68, 0xce, 0x06, 0,    0,    1,    0x65, 0xaa, 0xbb};
    struct video_parameters parameters = {0};
    CHECK(video_collect_parameters(&parameters, avc, sizeof(avc), 0));
    CHECK(parameters.count == 2 && parameters.used == 14);
    CHECK(parameters.type[0] == 7 && parameters.type[1] == 8);
    CHECK(parameters.length[0] == 7 && parameters.length[1] == 7);
    CHECK(video_collect_parameters(&parameters, avc, sizeof(avc), 0));
    CHECK(parameters.count == 2 && parameters.used == 14);
    static const unsigned char hevc[] = {0, 0, 0,    1, 0x40, 1, 0x80, 0, 0,    1, 0x42, 1, 0x80, 0,
                                         0, 1, 0x44, 1, 0x80, 0, 0,    1, 0x26, 1, 0xaa};
    memset(&parameters, 0, sizeof(parameters));
    CHECK(video_collect_parameters(&parameters, hevc, sizeof(hevc), 1));
    CHECK(parameters.count == 3 && parameters.used == 19);
    CHECK(parameters.type[0] == 32 && parameters.type[1] == 33 && parameters.type[2] == 34);
    CHECK(!video_collect_parameters(&parameters, hevc, sizeof(hevc), 2));
    /* A UUID SEI must not hide separate mastering/light SEIs with the same
     * HEVC NAL type. Repeated static metadata must not consume the bound. */
    static const unsigned char sei[] = {
        0,    0,    1,    0x4e, 1,    5,    1,    0x55, 0x80, 0,    0,    1,    0x4e, 1,
        137,  24,   0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
        0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x80, 0,
        0,    1,    0x4e, 1,    144,  4,    0x22, 0x22, 0x22, 0x22, 0x80};
    memset(&parameters, 0, sizeof(parameters));
    CHECK(video_collect_parameters(&parameters, sei, sizeof(sei), 1));
    CHECK(parameters.count == 2 && parameters.used == 44);
    CHECK(video_collect_parameters(&parameters, sei, sizeof(sei), 1));
    CHECK(parameters.count == 2 && parameters.used == 44);
    static const unsigned char escaped_sei[] = {0, 0, 1, 6, 144, 4, 0, 0, 3, 0, 1, 0x80};
    memset(&parameters, 0, sizeof(parameters));
    CHECK(video_collect_parameters(&parameters, escaped_sei, sizeof(escaped_sei), 0));
    CHECK(parameters.count == 1 && parameters.type[0] == 6 && parameters.used == 12);
    static const unsigned char bad_sei[] = {0, 0, 1, 0x4e, 1, 137, 24, 0x11, 0x80};
    memset(&parameters, 0, sizeof(parameters));
    CHECK(!video_collect_parameters(&parameters, bad_sei, sizeof(bad_sei), 1));
    static unsigned char oversized[R3_VIDEO_PARAMETER_BYTES + 5];
    memset(oversized, 0x55, sizeof(oversized));
    oversized[0] = oversized[1] = 0;
    oversized[2] = 1;
    oversized[3] = 0x4e;
    memset(&parameters, 0, sizeof(parameters));
    CHECK(!video_collect_parameters(&parameters, oversized, sizeof(oversized), 1));
    CHECK(parameters.overflow && parameters.count == 0 && parameters.used == 0);
    static const unsigned char truncated[] = {0, 0, 1, 0x42};
    memset(&parameters, 0, sizeof(parameters));
    CHECK(!video_collect_parameters(&parameters, truncated, sizeof(truncated), 1));
}

static void header_packet_tests(void)
{
    /* Recovered wire layout, syntax-only fixtures; no hardware evidence. */
    unsigned char packet[0x1a8] = {0};
    uint64_t value = sizeof(packet);
    CHECK(media_copy(packet, sizeof(packet), &value, sizeof(value)));
    value = 3;
    CHECK(media_copy(packet + 0x10, sizeof(packet) - 0x10, &value, sizeof(value)));
    value = 2;
    CHECK(media_copy(packet + 0x98, sizeof(packet) - 0x98, &value, sizeof(value)));
    packet[0x18] = 0x67;
    packet[0x19] = 0x64;
    packet[0xa0] = 0x68;
    packet[0xa1] = 0xce;
    struct video_parameters parameters = {0};
    CHECK(video_collect_header_packet(&parameters, packet, sizeof(packet), 0));
    CHECK(parameters.count == 2 && parameters.used == 13);
    CHECK(parameters.type[0] == 7 && parameters.type[1] == 8);
    CHECK(!video_collect_header_packet(&parameters, packet, sizeof(packet) - 1, 0));
    CHECK(!video_collect_header_packet(&parameters, packet, sizeof(packet), 1));
    value = 129;
    CHECK(media_copy(packet + 0x98, sizeof(packet) - 0x98, &value, sizeof(value)));
    memset(&parameters, 0, sizeof(parameters));
    CHECK(!video_collect_header_packet(&parameters, packet, sizeof(packet), 0));
    CHECK(parameters.count == 0 && parameters.used == 0);
    value = UINT64_MAX;
    CHECK(media_copy(packet + 0x98, sizeof(packet) - 0x98, &value, sizeof(value)));
    CHECK(!video_collect_header_packet(&parameters, packet, sizeof(packet), 0));
    memset(packet, 0, sizeof(packet));
    value = sizeof(packet);
    CHECK(media_copy(packet, sizeof(packet), &value, sizeof(value)));
    packet[8] = 1;
    value = 3;
    CHECK(media_copy(packet + 0x10, sizeof(packet) - 0x10, &value, sizeof(value)));
    CHECK(media_copy(packet + 0x98, sizeof(packet) - 0x98, &value, sizeof(value)));
    CHECK(media_copy(packet + 0x120, sizeof(packet) - 0x120, &value, sizeof(value)));
    packet[0x18] = 0x42;
    packet[0x19] = packet[0xa1] = packet[0x129] = 1;
    packet[0xa0] = 0x44;
    packet[0x128] = 0x40;
    CHECK(video_collect_header_packet(&parameters, packet, sizeof(packet), 1));
    CHECK(parameters.count == 3 && parameters.used == 21);
    CHECK(parameters.type[0] == 32 && parameters.type[1] == 33 && parameters.type[2] == 34);
    memset(&parameters, 0, sizeof(parameters));
    packet[0xa0] = 0x26;
    CHECK(!video_collect_header_packet(&parameters, packet, sizeof(packet), 1));
    CHECK(parameters.count == 0 && parameters.used == 0);
    CHECK(!video_collect_header_packet(&parameters, NULL, sizeof(packet), 1));
}

static void recording_tests(void)
{
    static struct video_recording saved;
    static const unsigned char first[] = {0, 0, 0, 1, 0x26, 1, 0xaa};
    static const unsigned char second[] = {0, 0, 1, 0x02, 1, 0xbb, 0xcc};
    CHECK(video_record(NULL, NULL, SIZE_MAX, 99));
    CHECK(video_export(NULL, NULL, false));
    CHECK(video_record(&saved, first, sizeof(first), 1));
    CHECK(video_record(&saved, second, sizeof(second), 1));
    CHECK(saved.count == 2 && saved.used == 14 && !saved.invalid);
    CHECK(saved.length[0] == sizeof(first) && saved.length[1] == sizeof(second));
    CHECK(memcmp(saved.bytes, first, sizeof(first)) == 0);
    CHECK(memcmp(saved.bytes + sizeof(first), second, sizeof(second)) == 0);
    FILE *output = tmpfile();
    CHECK(output != NULL && video_export(output, &saved, true));
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    char text[1024] = {0};
    CHECK(fread(text, 1, sizeof(text) - 1, output) > 0);
    CHECK(strstr(text, "record=0 record_bytes=7 offset=0 bytes=7 hex=000000012601aa\n"));
    CHECK(strstr(text, "record=1 record_bytes=7 offset=0 bytes=7 hex=0000010201bbcc\n"));
    CHECK(strstr(text, "R3_VIDEO_EXPORT_END records=2 bytes=14 complete=1\n"));
    CHECK(fclose(output) == 0);
    output = fopen("/dev/null", "r");
    CHECK(output != NULL && !video_export(output, &saved, true));
    (void)fclose(output);
    unsigned char chunked[260];
    memset(chunked, 0xaa, sizeof(chunked));
    memcpy(chunked, first, sizeof(first));
    memset(&saved, 0, sizeof(saved));
    CHECK(video_record(&saved, chunked, sizeof(chunked), 1));
    output = tmpfile();
    CHECK(output != NULL && video_export(output, &saved, true));
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    memset(text, 0, sizeof(text));
    CHECK(fread(text, 1, sizeof(text) - 1, output) > 0);
    CHECK(strstr(text, "record=0 record_bytes=260 offset=0 bytes=128 hex="));
    CHECK(strstr(text, "record=0 record_bytes=260 offset=128 bytes=128 hex="));
    CHECK(strstr(text, "record=0 record_bytes=260 offset=256 bytes=4 hex=aaaaaaaa\n"));
    CHECK(strstr(text, "R3_VIDEO_EXPORT_END records=1 bytes=260 complete=1\n"));
    CHECK(fclose(output) == 0);
    for (unsigned mode = 0; mode < 4; ++mode) {
        memset(&saved, 0, sizeof(saved));
        CHECK(video_record(&saved, first, sizeof(first), 1));
        if (mode == 1)
            saved.invalid = true;
        if (mode == 2)
            saved.length[0]++;
        if (mode == 3)
            saved.used++;
        output = tmpfile();
        CHECK(output != NULL && !video_export(output, &saved, mode != 0));
        CHECK(fseek(output, 0, SEEK_SET) == 0);
        memset(text, 0, sizeof(text));
        CHECK(fread(text, 1, sizeof(text) - 1, output) > 0);
        CHECK(strstr(text, "valid=0") && !strstr(text, "R3_VIDEO_DATA") &&
              !strstr(text, "R3_VIDEO_EXPORT_END"));
        CHECK(fclose(output) == 0);
    }
    const unsigned char bad[][7] = {
        {0, 0, 1, 0x26, 1, 0, 0}, /* tested below with a truncated length */
        {0, 0, 1, 0xa6, 1, 0xaa, 0},
        {0, 0, 1, 0x26, 0, 0xaa, 0},
    };
    for (size_t i = 0; i < 3; ++i) {
        memset(&saved, 0, sizeof(saved));
        CHECK(!video_record(&saved, bad[i], i == 0 ? 5 : 7, 1));
        CHECK(saved.invalid && saved.used == 0 && saved.count == 0);
        CHECK(!video_record(&saved, first, sizeof(first), 1));
    }
    static const unsigned char dangling[] = {0, 0, 1, 0x26, 1, 0xaa, 0, 0, 1};
    memset(&saved, 0, sizeof(saved));
    CHECK(!video_record(&saved, dangling, sizeof(dangling), 1));
    CHECK(saved.invalid && !saved.used && !saved.count);
    memset(&saved, 0, sizeof(saved));
    saved.used = R3_VIDEO_SAVED_BYTES - sizeof(first) + 1;
    CHECK(!video_record(&saved, first, sizeof(first), 1));
    CHECK(saved.overflow && saved.invalid && saved.count == 0);
    memset(&saved, 0, sizeof(saved));
    for (size_t i = 0; i < R3_VIDEO_SAVED_RECORDS; ++i)
        CHECK(video_record(&saved, first, sizeof(first), 1));
    CHECK(!video_record(&saved, first, sizeof(first), 1));
    CHECK(saved.overflow && saved.invalid && saved.count == R3_VIDEO_SAVED_RECORDS);
    puts("T041 recording retention: exact order, disabled, malformed, truncated, capacity and "
         "export gates passed");
}

int main(void)
{
    recording_tests();
    module_tests();
    resource_removal_tests();
    unsigned char destination[4] = {0};
    const unsigned char source[] = {1, 2, 3, 4, 5};
    CHECK(!media_copy(destination, sizeof(destination), source, sizeof(source)));
    CHECK(destination[0] == 0 && destination[3] == 0);
    CHECK(media_copy(destination, sizeof(destination), source, sizeof(destination)));
    CHECK(destination[0] == 1 && destination[3] == 4);
    CHECK(!media_copy(destination, sizeof(destination), NULL, 1));
    uintptr_t address = 0;
    int (*bound)(void) = fake_initialize;
    _Static_assert(sizeof(bound) == sizeof(address), "test target has a flat function-pointer ABI");
    CHECK(media_copy(&address, sizeof(address), (const void *)&bound, sizeof(bound)));
    bound = NULL;
    CHECK(!media_bind_pointer((void *)&bound, sizeof(bound) - 1, address) && bound == NULL);
    CHECK(!media_bind_pointer((void *)&bound, sizeof(bound), 0) && bound == NULL);
    CHECK(media_bind_pointer((void *)&bound, sizeof(bound), address) && bound == fake_initialize);
    parameter_tests();
    header_packet_tests();
    resource_tests();
    struct audio_state s;
    reset();
    memset(&s, 0, sizeof(s));
    CHECK(audio_start(&api, &s));
    CHECK(audio_read(&api, &s));
    CHECK(s.frames == 1 && s.bytes == 8 && s.nonzero[0] == 1 && s.nonzero[1] == 1);
    read_result = 0;
    CHECK(audio_read(&api, &s));
    read_result = (int)UINT32_C(0x81950002);
    CHECK(audio_read(&api, &s));
    read_result = (int)UINT32_C(0x81950004);
    CHECK(audio_read(&api, &s));
    CHECK(s.empty == 2 && s.overruns == 1 && s.frames == 1);
    CHECK(audio_cleanup(&api, &s));
    CHECK(strcmp(calls, "IOSPCT") == 0);
    CHECK(audio_cleanup(&api, &s));
    CHECK(strcmp(calls, "IOSPCT") == 0);

    const int failures[] = {'I', 'O', 'S', 'P', 'C', 'T'};
    const char *expected[] = {"I", "IOT", "IOSCT", "IOSPCT", "IOSPCT", "IOSPCT"};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        reset();
        fail_at = failures[i];
        memset(&s, 0, sizeof(s));
        CHECK(audio_start(&api, &s) == (i >= 3));
        CHECK(audio_cleanup(&api, &s) == (i < 3));
        CHECK(strcmp(calls, expected[i]) == 0);
        CHECK(audio_cleanup(&api, &s) == (i < 3));
        CHECK(strcmp(calls, expected[i]) == 0);
    }
    const int bad_reads[] = {7, 8193, -1, (int)UINT32_C(0x81950008)};
    for (size_t i = 0; i < sizeof(bad_reads) / sizeof(bad_reads[0]); ++i) {
        reset();
        memset(&s, 0, sizeof(s));
        CHECK(audio_start(&api, &s));
        read_result = bad_reads[i];
        CHECK(!audio_read(&api, &s));
        CHECK(s.frames == 0 && s.bytes == 0);
        CHECK(audio_cleanup(&api, &s));
    }
    reset();
    memset(&s, 0, sizeof(s));
    CHECK(audio_start(&api, &s));
    malformed_sample = true;
    CHECK(!audio_read(&api, &s));
    CHECK(s.frames == 0 && s.bytes == 0);
    CHECK(audio_cleanup(&api, &s));

    reset();
    memset(&s, 0, sizeof(s));
    fail_at = 'O';
    unexpected_handle = true;
    CHECK(!audio_start(&api, &s));
    CHECK(!audio_cleanup(&api, &s));
    CHECK(strcmp(calls, "IOT") == 0);

    char *host_audio[] = {"capture_encode", "--audio-five-seconds"};
    CHECK(r3_media_main(2, host_audio) == R3_USAGE);
    CHECK(r3_media_main(0, NULL) == R3_INCOMPLETE);

    struct video_request request;
    CHECK(video_request_init(&request, 3, 30, 10000));
    CHECK(qword_at(request.bytes, 0x00) == 0x2e8 && qword_at(request.bytes, 0x08) == 0x28);
    CHECK(word_at(request.bytes, 0x10) == 0 && word_at(request.bytes, 0x14) == 2);
    CHECK(word_at(request.bytes, 0x18) == 3 && word_at(request.bytes, 0x1c) == 0);
    CHECK(word_at(request.bytes, 0x28) == R3_VIDEO_RESOURCE_ID);
    CHECK(word_at(request.bytes, 0x98) == 1);
    CHECK(word_at(request.bytes, 0xc8) == 8928571);
    CHECK(word_at(request.bytes, 0xcc) == 10000000);
    CHECK(word_at(request.bytes, 0xd0) == 333666);
    CHECK(word_at(request.bytes, 0x190) == 1 && word_at(request.bytes, 0x194) == 1);
    CHECK(word_at(request.bytes, 0x1a8) == 0 && word_at(request.bytes, 0x2ac) == 0);
    CHECK(qword_at(request.bytes, 0x1e0) == 0x88);
    CHECK(qword_at(request.bytes, 0x1e8) == UINT64_C(0x3ff0000000000000));
    CHECK(word_at(request.bytes, 0x20c) == 384 && word_at(request.bytes, 0x210) == 384);
    CHECK(word_at(request.bytes, 0x214) == 512 && qword_at(request.bytes, 0x218) == 0x48000);
    CHECK(video_request_init(&request, 4, 60, 10000));
    CHECK(word_at(request.bytes, 0x18) == 4 && word_at(request.bytes, 0x1c) == 1);
    CHECK(word_at(request.bytes, 0xd0) == 166833);
    CHECK(video_request_set_format(&request, 1, 10));
    CHECK(word_at(request.bytes, 0x20) == 1 && word_at(request.bytes, 0x24) == 1);
    CHECK(word_at(request.bytes, 0xc8) == 9090909);
    CHECK(!video_request_set_format(&request, 0, 10));
    CHECK(!video_request_set_format(&request, 1, 9));
    CHECK(!video_request_set_format(&request, 2, 8));
    CHECK(!video_request_init(&request, 2, 30, 10000));
    CHECK(!video_request_init(&request, 4, 59, 10000));
    CHECK(!video_request_init(&request, 4, 60, 0));
    CHECK(!video_request_init(&request, 7, 60, 10000));

    static struct video_state video;
    const struct video_api video_api = {.initialize = fake_initialize,
                                        .terminate = fake_terminate,
                                        .open = fake_video_open,
                                        .start = fake_start,
                                        .stop = fake_stop,
                                        .close = fake_close,
                                        .read = fake_video_read,
                                        .query_mode_flags = fake_video_query_mode_flags,
                                        .query_capture_state = fake_video_query_capture_state};
    struct video_api header_api = video_api;
    header_api.query_headers = fake_video_query_headers;
    reset();
    resource_available = true;
    memset(&video, 0, sizeof(video));
    CHECK(video_start(&header_api, &video));
    CHECK(video.parameters.count == 2 && video.parameters.used == 12);
    CHECK(video_cleanup(&header_api, &video));
    CHECK(strcmp(calls, "IHFVOSPCT") == 0);
    reset();
    fail_at = 'H';
    memset(&video, 0, sizeof(video));
    CHECK(!video_start(&header_api, &video) && video.initialized && !video.handle);
    CHECK(video_cleanup(&header_api, &video));
    CHECK(strcmp(calls, "IHT") == 0);
    reset();
    resource_available = true;
    memset(&video, 0, sizeof(video));
    video.checkpoint = R3_VIDEO_OPEN_ONLY;
    CHECK(video_start(&video_api, &video));
    CHECK(video.handle && !video.started && video.initialized);
    CHECK(video_cleanup(&video_api, &video));
    CHECK(strcmp(calls, "IFVOCT") == 0);
    reset();
    memset(&video, 0, sizeof(video));
    video.checkpoint = R3_VIDEO_INVALID_MODE;
    CHECK(!video_start(&video_api, &video));
    CHECK(!video.handle && !video.started && video.initialized && video.open_attempts == 1);
    CHECK((uint32_t)video.open_result == UINT32_C(0x81950001));
    CHECK(video_cleanup(&video_api, &video));
    CHECK(strcmp(calls, "IFVOT") == 0);
    reset();
    resource_available = false;
    automatic_resource_availability = true;
    struct resource_state integrated_resource = {.pid = 123};
    CHECK(resource_start(&resource_api, &integrated_resource, fake_resource_callback));
    CHECK(resource_query(&resource_api, &integrated_resource) && integrated_resource.available);
    static struct video_state integrated_video;
    memset(&integrated_video, 0, sizeof(integrated_video));
    CHECK(video_start(&video_api, &integrated_video));
    CHECK(video_cleanup(&video_api, &integrated_video));
    CHECK(resource_cleanup(&resource_api, &integrated_resource));
    CHECK(resource_query(&resource_api, &integrated_resource) && !integrated_resource.available);
    CHECK(strcmp(calls, "GRQIFVOSPCTEUQ") == 0);
    automatic_resource_availability = false;

    reset();
    resource_available = true;
    memset(&video, 0, sizeof(video));
    CHECK(video_start(&video_api, &video));
    read_result = 5;
    CHECK(video_read(&video_api, &video));
    CHECK(video.frames == 1 && video.bytes == 5 && video.annexb_frames == 1);
    CHECK(video.nonzero_bytes == 2);
    video.timing_us[video.timing_count++] = 16000;
    read_result = (int)UINT32_C(0x81950002);
    CHECK(video_read(&video_api, &video));
    read_result = (int)UINT32_C(0x8195000d);
    CHECK(video_read(&video_api, &video));
    read_result = (int)UINT32_C(0x81950004);
    CHECK(video_read(&video_api, &video));
    CHECK(video.empty == 2 && video.overruns == 1 && video.frames == 1);
    video.timing_us[video.timing_count++] = 17000;
    bool timing_valid = false;
    CHECK(video_timing_percentile99(&video, &timing_valid) == 17000 && timing_valid);
    CHECK(video_cleanup(&video_api, &video));
    CHECK(strcmp(calls, "IFVOSPCT") == 0);
    CHECK(video_cleanup(&video_api, &video));
    CHECK(strcmp(calls, "IFVOSPCT") == 0);

    /* Retention participates in the real read seam without changing teardown. */
    static struct video_recording read_recording;
    reset();
    resource_available = true;
    memset(&video, 0, sizeof(video));
    video.recording = &read_recording;
    CHECK(video_start(&video_api, &video));
    read_result = 6;
    CHECK(video_read(&video_api, &video));
    CHECK(read_recording.count == 1 && read_recording.used == 6);
    CHECK(memcmp(read_recording.bytes, video.buffer, 6) == 0);
    read_result = (int)UINT32_C(0x81950004);
    CHECK(video_read(&video_api, &video) && read_recording.invalid);
    CHECK(video_cleanup(&video_api, &video));
    CHECK(strcmp(calls, "IFVOSPCT") == 0);
    reset();
    resource_available = true;
    memset(&video, 0, sizeof(video));
    memset(&read_recording, 0, sizeof(read_recording));
    video.recording = &read_recording;
    CHECK(video_start(&video_api, &video));
    read_result = 5; /* Annex-B header without a slice payload. */
    CHECK(!video_read(&video_api, &video));
    CHECK(read_recording.invalid && !read_recording.count && !video.frames);
    CHECK(video_cleanup(&video_api, &video));
    CHECK(strcmp(calls, "IFVOSPCT") == 0);

    reset();
    memset(&video, 0, sizeof(video));
    busy_opens = 2;
    CHECK(video_start(&video_api, &video));
    CHECK(video.open_attempts == 3);
    CHECK(video_cleanup(&video_api, &video));
    CHECK(strcmp(calls, "IFVOFVOFVOSPCT") == 0);

    reset();
    resource_available = true;
    fake_mode_flags = 2;
    fake_capture_state_mode = 3;
    fake_capture_state_busy_remaining = 1;
    memset(&video, 0, sizeof(video));
    CHECK(video_start(&video_api, &video));
    CHECK(video.mode_flag_queries == 2 && video.capture_state_queries == 2);
    CHECK(video.open_attempts == 1);
    CHECK(strcmp(calls, "IFVFVOS") == 0);
    CHECK(video_cleanup(&video_api, &video));
    CHECK(strcmp(calls, "IFVFVOSPCT") == 0);

    reset();
    resource_available = true;
    fake_mode_flags = 1;
    fake_capture_state_mode = 1;
    memset(&video, 0, sizeof(video));
    CHECK(video_start(&video_api, &video));
    CHECK(video.mode_flag_queries == 1 && video.capture_state_queries == 1);
    CHECK(video_cleanup(&video_api, &video));

    reset();
    resource_available = true;
    fake_capture_state_busy_remaining = R3_VIDEO_QUERY_ATTEMPTS;
    memset(&video, 0, sizeof(video));
    CHECK(!video_start(&video_api, &video));
    CHECK(video.mode_flag_queries == R3_VIDEO_QUERY_ATTEMPTS);
    CHECK(video.capture_state_queries == R3_VIDEO_QUERY_ATTEMPTS);
    CHECK(video.open_attempts == 0);
    CHECK(video_cleanup(&video_api, &video));

    for (unsigned query_failure = 0; query_failure < 2; ++query_failure) {
        reset();
        fail_at = query_failure == 0 ? 'F' : 'V';
        memset(&video, 0, sizeof(video));
        CHECK(!video_start(&video_api, &video));
        CHECK(video_cleanup(&video_api, &video));
        CHECK(strcmp(calls, query_failure == 0 ? "IFT" : "IFVT") == 0);
    }

    const char *video_expected[] = {"I", "IFVOT", "IFVOSCT", "IFVOSPCT", "IFVOSPCT", "IFVOSPCT"};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        reset();
        fail_at = failures[i];
        memset(&video, 0, sizeof(video));
        CHECK(video_start(&video_api, &video) == (i >= 3));
        CHECK(video_cleanup(&video_api, &video) == (i < 3));
        CHECK(strcmp(calls, video_expected[i]) == 0);
        CHECK(video_cleanup(&video_api, &video) == (i < 3));
        CHECK(strcmp(calls, video_expected[i]) == 0);
    }
    const int bad_video_reads[] = {4, R3_VIDEO_BUFFER_BYTES + 1, -1, (int)UINT32_C(0x81950008)};
    for (size_t i = 0; i < sizeof(bad_video_reads) / sizeof(bad_video_reads[0]); ++i) {
        reset();
        memset(&video, 0, sizeof(video));
        CHECK(video_start(&video_api, &video));
        read_result = bad_video_reads[i];
        CHECK(!video_read(&video_api, &video));
        CHECK(video.frames == 0 && video.bytes == 0);
        CHECK(video_cleanup(&video_api, &video));
    }
    reset();
    memset(&video, 0, sizeof(video));
    CHECK(video_start(&video_api, &video));
    malformed_video = true;
    CHECK(!video_read(&video_api, &video));
    CHECK(video.frames == 0 && video.bytes == 0);
    CHECK(video_cleanup(&video_api, &video));

    reset();
    memset(&video, 0, sizeof(video));
    fail_at = 'O';
    unexpected_handle = true;
    CHECK(!video_start(&video_api, &video));
    CHECK(!video_cleanup(&video_api, &video));
    CHECK(strcmp(calls, "IFVOT") == 0);
    memset(&video, 0, sizeof(video));
    video.timing_overflow = 1;
    timing_valid = true;
    CHECK(video_timing_percentile99(&video, &timing_valid) == 0 && !timing_valid);

    char *host_video[] = {"capture_encode", "--video-ten-seconds"};
    CHECK(r3_media_main(2, host_video) == R3_USAGE);
    char *host_resource[] = {"capture_encode", "--video-admission-five-seconds"};
    CHECK(r3_media_main(2, host_resource) == R3_USAGE);
    puts("T041 media probe simulated lifecycle/validation tests passed; hardware evidence absent");
    return 0;
}
