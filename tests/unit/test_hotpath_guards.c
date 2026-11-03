/* T114 portable regression guard. Synthetic checks do not prove PS5 timing,
 * allocator behavior inside dependencies, or platform callback behavior. */
#include "../fakes/platform.h"
#include "metrics/internal.h"
#include "protocol/audio.h"
#include "protocol/video.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            if (fputs("hotpath guard: " #condition "\n", stderr) == EOF)                           \
                return 2;                                                                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

/* Deliberately restricted to direct source identifiers, including function
 * pointers. Comments and literals are ignored; this is not a C call graph. */
static const char *token(const char *source, char out[128])
{
    for (;;) {
        while (isspace((unsigned char)*source))
            ++source;
        if (source[0] == '/' && source[1] == '/') {
            while (*source != '\0' && *source != '\n')
                ++source;
        } else if (source[0] == '/' && source[1] == '*') {
            source += 2;
            while (*source != '\0' && !(source[0] == '*' && source[1] == '/'))
                ++source;
            if (*source != '\0')
                source += 2;
        } else if (*source == '\"' || *source == '\'') {
            char quote = *source++;
            while (*source != '\0' && *source != quote) {
                if (*source == '\\' && source[1] != '\0')
                    ++source;
                ++source;
            }
            if (*source != '\0')
                ++source;
        } else {
            break;
        }
    }
    size_t length = 0;
    if (isalpha((unsigned char)*source) || *source == '_') {
        do {
            if (length < 127)
                out[length++] = *source;
            ++source;
        } while (isalnum((unsigned char)*source) || *source == '_');
    } else if (*source != '\0') {
        out[length++] = *source++;
    }
    out[length] = '\0';
    return source;
}

static bool listed(const char *name, const char *const *list, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(name, list[i]) == 0)
            return true;
    }
    return false;
}

static unsigned scan(const char *source, const char *const *setup, size_t setup_count,
                     bool diagnostics)
{
    static const char *const forbidden[] = {"malloc",
                                            "calloc",
                                            "realloc",
                                            "reallocarray",
                                            "free",
                                            "aligned_alloc",
                                            "posix_memalign",
                                            "strdup",
                                            "strndup",
                                            "mmap",
                                            "munmap",
                                            "alloca",
                                            "EVP_CIPHER_CTX_new",
                                            "EVP_CIPHER_CTX_free",
                                            "OPENSSL_malloc",
                                            "OPENSSL_zalloc",
                                            "OPENSSL_realloc",
                                            "OPENSSL_free",
                                            "pthread_mutex_lock",
                                            "pthread_rwlock_rdlock",
                                            "pthread_rwlock_wrlock",
                                            "pthread_cond_wait",
                                            "pthread_cond_timedwait",
                                            "pthread_join",
                                            "sem_wait",
                                            "sem_timedwait",
                                            "sleep",
                                            "usleep",
                                            "nanosleep",
                                            "poll",
                                            "select",
                                            "read",
                                            "write",
                                            "open",
                                            "close",
                                            "fsync",
                                            "fopen",
                                            "fread",
                                            "fwrite",
                                            "fflush",
                                            "printf",
                                            "fprintf",
                                            "vprintf",
                                            "vfprintf",
                                            "puts",
                                            "fputs",
                                            "syslog",
                                            "recv",
                                            "recvfrom",
                                            "recvmsg",
                                            "send",
                                            "sendto",
                                            "sendmsg"};
    char current[128], previous[128] = "", function[128] = "";
    size_t braces = 0, parentheses = 0;
    bool body = false, exempt = false;
    unsigned violations = 0, functions = 0;
    for (;;) {
        source = token(source, current);
        if (current[0] == '\0')
            break;
        if (braces == 0 && strcmp(current, "(") == 0) {
            if (parentheses == 0)
                memcpy(function, previous, sizeof(function));
            ++parentheses;
        } else if (braces == 0 && strcmp(current, ")") == 0) {
            if (parentheses == 0)
                return violations + 1;
            --parentheses;
        } else if (strcmp(current, "{") == 0) {
            if (braces == 0) {
                body = strcmp(previous, ")") == 0 && function[0] != '\0';
                exempt = listed(function, setup, setup_count);
                functions += body ? 1U : 0U;
            }
            ++braces;
        } else if (strcmp(current, "}") == 0) {
            if (braces == 0)
                return violations + 1;
            if (--braces == 0) {
                body = false;
                function[0] = '\0';
            }
        } else if (body && !exempt && strcmp(previous, ".") != 0 && strcmp(previous, ">") != 0 &&
                   listed(current, forbidden, sizeof(forbidden) / sizeof(forbidden[0]))) {
            ++violations;
            if (diagnostics && (fputs("forbidden hot-path identifier ", stderr) == EOF ||
                                fputs(current, stderr) == EOF || fputs(" in ", stderr) == EOF ||
                                fputs(function, stderr) == EOF || fputs("\n", stderr) == EOF))
                return violations + 1;
        }
        memcpy(previous, current, sizeof(previous));
    }
    return violations + (braces != 0 || parentheses != 0 || functions == 0 ? 1U : 0U);
}

static int scanner_contract(void)
{
    const char *const setup[] = {"setup"};
    CHECK(scan("void producer(void) { malloc(1); }", NULL, 0, false) == 1);
    CHECK(scan("void producer(void) { pthread_mutex_lock(&m); }", NULL, 0, false) == 1);
    CHECK(scan("void producer(void) { puts(\"frame\"); }", NULL, 0, false) == 1);
    CHECK(scan("void producer(void) { void *p = &calloc; }", NULL, 0, false) == 1);
    CHECK(scan("void setup(void) { calloc(1, 1); }", setup, 1, false) == 0);
    CHECK(scan("void setup(void) {} void helper(void) { realloc(p, 1); }", setup, 1, false) == 1);
    CHECK(scan("/* malloc() { */ void producer(void) { const char *s = \"free() }\"; "
               "char c = '}'; // pthread_mutex_lock()\n memcpy(a, b, 1); }",
               NULL, 0, false) == 0);
    CHECK(scan("void producer(void) { if (ready) { nanosleep(&t, 0); } }", NULL, 0, false) == 1);
    CHECK(scan("void producer(void) { if (ops.open != 0 && ops->close != 0) { return; } }", NULL, 0,
               false) == 0);
    return 0;
}

typedef struct {
    const char *path;
    const char *setup[8];
    size_t setup_count;
} source_policy;

static int source_guard(void)
{
    const source_policy policies[] = {
        {"core/protocol/video.c", {"sp5_video_create", "sp5_video_destroy"}, 2},
        {"core/protocol/audio.c", {"sp5_audio_create", "sp5_audio_destroy"}, 2},
        {"core/protocol/fec.c", {NULL}, 0},
        {"core/input/controller.c", {"sp5_controller_create", "sp5_controller_destroy"}, 2},
        {"core/metrics/records.c", {"sp5_metrics_create", "sp5_metrics_destroy"}, 2},
        {"core/metrics/coverage.c", {NULL}, 0},
        {"core/runtime.c", {NULL}, 0},
        {"core/session/session.c", {"sp5_authority_create", "sp5_authority_destroy"}, 2},
        {"core/session/lifecycle.c",
         {"sp5_lifecycle_create", "sp5_lifecycle_start", "sp5_lifecycle_end",
          "sp5_lifecycle_destroy"},
         4},
        {"core/protocol/control.c",
         {"sp5_control_runtime_create", "sp5_control_runtime_destroy", "sp5_control_create",
          "sp5_control_destroy", "sp5_control_transport_open", "sp5_control_transport_close"},
         6},
    };
    static char source[131072];
    for (size_t i = 0; i < sizeof(policies) / sizeof(policies[0]); ++i) {
        char path[4096];
        int length = snprintf(path, sizeof(path), "%s/%s", SP5_SOURCE_DIR, policies[i].path);
        CHECK(length > 0 && (size_t)length < sizeof(path));
        FILE *file = fopen(path, "rb");
        CHECK(file != NULL);
        size_t size = fread(source, 1, sizeof(source) - 1, file);
        bool complete = feof(file) != 0 && ferror(file) == 0;
        CHECK(fclose(file) == 0 && complete && size != 0);
        source[size] = '\0';
        CHECK(fputs("source guard: ", stdout) != EOF && fputs(policies[i].path, stdout) != EOF &&
              fputs("\n", stdout) != EOF);
        CHECK(scan(source, policies[i].setup, policies[i].setup_count, true) == 0);
    }
    return 0;
}

static int bounded_metrics(void)
{
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_metrics *metrics = sp5_metrics_create(1, (sp5_metrics_capacity){8, 8, 3}, &fake.clock);
    CHECK(metrics != NULL && sp5_metrics_enable(metrics, true, 0).status == SP5_OK);
    const void *frame_storage = metrics->frames.records;
    const void *input_storage = metrics->inputs.records;
    const size_t bytes = metrics->allocated_bytes;
    for (uint64_t cycle = 0; cycle < 100; ++cycle) {
        /* A producer that starts waiting on the worker lock now deadlocks and
         * fails CTest's timeout. No clock-duration threshold is inferred. */
        CHECK(pthread_mutex_lock(&metrics->state_lock) == 0);
        for (uint64_t i = 0; i < 20; ++i) {
            sp5_frame_record frame = {cycle * 20 + i, cycle, {1, 2, 3, 4, 5}, SP5_FRAME_ALL_VALID};
            sp5_input_record input = {cycle * 20 + i, cycle, 1, 0,
                                      SP5_INPUT_VALID | SP5_INPUT_CONFIRMED |
                                          SP5_INPUT_AUTHENTICATED};
            sp5_result f = sp5_metrics_push_frame(metrics, &frame);
            sp5_result in = sp5_metrics_push_input(metrics, &input);
            CHECK(i < 3 ? f.status == SP5_OK && in.status == SP5_OK
                        : f.reason == SP5_REASON_CAPACITY && in.reason == SP5_REASON_CAPACITY);
        }
        CHECK(pthread_mutex_unlock(&metrics->state_lock) == 0);
        CHECK(metrics->frames.records == frame_storage && metrics->inputs.records == input_storage);
        CHECK(metrics->allocated_bytes == bytes && bytes <= SP5_METRICS_BYTES);
        CHECK(sp5_metrics_worker_step(metrics, cycle, NULL).status == SP5_OK);
    }
    CHECK(atomic_load(&metrics->frames.lost) == 1700);
    CHECK(atomic_load(&metrics->inputs.lost) == 1700);
    CHECK(metrics->frame_ring.count == 8 && metrics->input_ring.count == 8);
    CHECK(sp5_metrics_enable(metrics, false, 100).status == SP5_OK);
    sp5_metrics_destroy(metrics);
    return 0;
}

static int bounded_media(void)
{
    sp5_video *video = sp5_video_create(&(sp5_video_config){.packet_size = 128, .fec_percent = 20});
    CHECK(video != NULL);
    uint8_t payload[512] = {0};
    uint8_t packet[SP5_VIDEO_WIRE_LIMIT];
    sp5_buffer wire = {packet, sizeof(packet), 0};
    for (unsigned frame = 0; frame < 100; ++frame) {
        sp5_video_frame_input input = {.bitstream = {payload, sizeof(payload)},
                                       .frame_index = frame};
        CHECK(sp5_video_begin(video, &input).status == SP5_OK);
        CHECK(sp5_video_begin(video, &input).reason == SP5_REASON_OWNERSHIP);
        unsigned packets = 0;
        sp5_result result;
        while ((result = sp5_video_next(video, &wire)).status == SP5_OK) {
            CHECK(++packets <= 1020 && wire.size <= sizeof(packet));
        }
        CHECK(result.reason == SP5_REASON_WOULD_BLOCK && packets == 6);
    }
    sp5_video_frame_input oversized = {.bitstream = {payload, SIZE_MAX}};
    CHECK(sp5_video_begin(video, &oversized).reason == SP5_REASON_CAPACITY);
    sp5_video_destroy(video);

    sp5_audio *audio = sp5_audio_create(&(sp5_audio_config){
        .sample_rate = 48000, .channels = 2, .duration_ms = 5, .opus_size = 16});
    CHECK(audio != NULL);
    for (unsigned group = 0; group < 100; ++group) {
        CHECK(sp5_audio_submit(audio, (sp5_bytes){payload, 16}).status == SP5_OK);
        CHECK(sp5_audio_submit(audio, (sp5_bytes){payload, 16}).reason == SP5_REASON_WOULD_BLOCK);
        unsigned packets = 0;
        sp5_result result;
        while ((result = sp5_audio_next(audio, &wire)).status == SP5_OK) {
            CHECK(++packets <= 3 && wire.size <= SP5_AUDIO_WIRE_LIMIT);
        }
        CHECK(result.reason == SP5_REASON_WOULD_BLOCK && packets == (group % 4 == 3 ? 3U : 1U));
    }
    sp5_audio_destroy(audio);
    return 0;
}

int main(void)
{
    CHECK(scanner_contract() == 0);
    CHECK(source_guard() == 0);
    CHECK(bounded_metrics() == 0);
    CHECK(bounded_media() == 0);
    CHECK(
        puts("T114 portable source and bounded-producer guards passed; hardware evidence absent") !=
        EOF);
    return 0;
}
