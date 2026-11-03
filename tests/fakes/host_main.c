/* Runnable PORTABLE/SYNTHETIC host. No PS5 interfaces or hardware claims.
 * Protocol constants: tools/moonlight-mcp/docs/protocol-notes.md §§1-13,
 * Sunshine d0b4a88 / mcc 41cedb8. Internal commands travel ONLY over stdin;
 * they are fixture controls, not a management HTTP API. Ports follow the
 * real core HTTP advertisement. Bind to an actual permitted private address.
 * Input/control, media and metrics have separate owners and fixed buffers.
 * During streaming this bounded harness defers management queries until the
 * input owner ends; authenticated cancel requests stop input before routing.
 * Pairing, TLS and durable storage always stay on the management owner.
 * The real core web app/API (sp5_web) is served on the same private address,
 * dispatched with the accepted socket peer; its URL is the "web" event. Web
 * routing is deferred (503) while a session's independent owners run. */
#include "platform.h"
#include "protocol/audio.h"
#include "protocol/pairing_flow.h"
#include "protocol/rtsp.h"
#include "protocol/video.h"
#include "session/lifecycle.h"
#include "web/server.h"
#include "web_service.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/pem.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum { FIXTURE_LIMIT = 1024 * 1024, CONNECTIONS = 8, COMMAND_LIMIT = 128 };
#define REQUIRE(c)                                                                                 \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            if (fputs(__FILE__ ": " #c "\n", stderr) < 0)                                          \
                return 2;                                                                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
typedef struct {
    SSL *tls;
    uint64_t request_id, deadline_us;
    size_t used;
    int fd;
    bool secure, rtsp, deferred, ready;
    char peer[INET_ADDRSTRLEN];
    uint8_t input[SP5_RTSP_LIMIT + 24];
} connection;
typedef struct {
    sp5_fake_platform media_fake, input_fake;
    sp5_store *store;
    sp5_host host;
    sp5_authority *authority;
    sp5_session *session;
    sp5_http_service *http;
    sp5_pairing *pairing;
    sp5_pair_flow *pair;
    sp5_rtsp *rtsp;
    sp5_lifecycle *lifecycle;
    sp5_control_runtime *runtime;
    sp5_control *control;
    sp5_control_transport *transport;
    sp5_video *video;
    sp5_audio *audio;
    sp5_negotiated negotiated;
    sp5_http_launch launch;
    sp5_bytes client_certificate, staged_video;
    uint8_t client_certificate_storage[SP5_CERTIFICATE_CAPACITY];
    uint8_t video_fixture[FIXTURE_LIMIT], opus_fixture[SP5_AUDIO_PAYLOAD_LIMIT];
    size_t video_size, opus_size;
    int video_socket, audio_socket;
    const char *bind_address;
    char launch_peer[INET_ADDRSTRLEN];
    pthread_t input_thread, media_thread, metrics_thread;
    bool running;
    atomic_bool stopped, media_done, metrics_done;
    atomic_int end_request;
    atomic_uint_fast64_t input_count, video_count, audio_count;
    atomic_uint_fast64_t last_input_us;
    atomic_uint buttons_seen, pads_seen;
    atomic_bool axes_seen, triggers_seen, pause_media;
    uint64_t notifications, paired_requests;
    sp5_reason worker_error;
    unsigned fail_operation;
    bool inject_failure;
} fixture_host;
static int emit(const char *format, ...)
{
    char output[1024];
    va_list arguments;
    va_start(arguments, format);
    int size = vsnprintf(output, sizeof(output), format, arguments);
    va_end(arguments);
    if (size < 0 || (size_t)size >= sizeof(output))
        return -1;
    return fputs(output, stdout) == EOF ? -1 : size;
}
static volatile sig_atomic_t interrupted;
static sp5_result (*base_deliver)(void *, const sp5_controller_state *, bool, sp5_input_evidence *);
static void on_signal(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}
static uint64_t now_us(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0 || t.tv_sec < 0)
        return 0;
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}
static sp5_optional_u64 wall_us(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_REALTIME, &t) != 0 || t.tv_sec < 0)
        return (sp5_optional_u64){0};
    return (sp5_optional_u64){true, (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000};
}
static sp5_timestamp stamp(void)
{
    uint64_t now = now_us();
    return (sp5_timestamp){now, 1, 1, now ? SP5_VALIDITY_VALID : SP5_VALIDITY_INVALID,
                           now ? SP5_REASON_NONE : SP5_REASON_CLOCK};
}
static sp5_result clock_describe(void *context, sp5_clock_description *description)
{
    (void)context;
    struct timespec resolution;
    if (clock_getres(CLOCK_MONOTONIC, &resolution) != 0)
        return sp5_failure(SP5_REASON_CLOCK);
    /* Stored ticks are us, actual clock resolution is rounded UP. Uncertainty
     * remains absent: clock_getres is not a calibration of absolute accuracy. */
    uint64_t ns = (uint64_t)resolution.tv_sec * 1000000000 + (uint64_t)resolution.tv_nsec;
    *description = (sp5_clock_description){.domain = 1,
                                           .epoch = 1,
                                           .ticks_per_second = 1000000,
                                           .native_resolution_ticks = (ns + 999) / 1000,
                                           .monotonic = true,
                                           .validity = SP5_VALIDITY_VALID};
    if (description->native_resolution_ticks == 0)
        description->native_resolution_ticks = 1;
    return sp5_success();
}
static sp5_result clock_now(void *context, sp5_timestamp *output)
{
    (void)context;
    *output = stamp();
    return output->validity == SP5_VALIDITY_VALID ? sp5_success() : sp5_failure(SP5_REASON_CLOCK);
}
static void pause_step(void)
{
    struct timespec delay = {.tv_nsec = 1000000};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}
static sp5_result notify(void *context, const char *message, size_t size)
{
    fixture_host *h = context;
    if (message == NULL || size == 0)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    ++h->notifications;
    /* Do not retain untrusted names/PINs; this event asserts fake notification only. */
    if (emit("{\"event\":\"notification\",\"synthetic\":true,\"count\":%llu}\n",
             (unsigned long long)h->notifications) < 0)
        return sp5_failure(SP5_REASON_PLATFORM);
    return sp5_success();
}
static sp5_result delivered(void *context, const sp5_controller_state *state, bool metrics,
                            sp5_input_evidence *evidence)
{
    sp5_fake_platform *fake = context;
    /* Actual fake-sink invocation time, never a console consumption claim. */
    fake->now = stamp();
    return base_deliver(context, state, metrics, evidence);
}
static int nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
static int open_socket(int type, const char *bind_address, uint16_t port)
{
    int fd = socket(AF_INET, type, 0);
    if (fd < 0)
        return -1;
    int yes = 1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0 ||
        inet_pton(AF_INET, bind_address, &address.sin_addr) != 1 ||
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        (type == SOCK_STREAM && listen(fd, CONNECTIONS) != 0) || nonblocking(fd) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}
static bool load_fixture(const char *path, uint8_t *buffer, size_t capacity, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return false;
    *size = fread(buffer, 1, capacity, file);
    bool valid = *size > 0 && !ferror(file) && fgetc(file) == EOF;
    if (fclose(file) != 0)
        valid = false;
    return valid;
}
static bool peer_certificate(SSL *tls, uint8_t *storage, sp5_bytes *out)
{
    X509 *certificate = tls == NULL ? NULL : SSL_get0_peer_certificate(tls);
    BIO *bio = BIO_new(BIO_s_mem());
    char *data = NULL;
    long size = 0;
    bool valid = certificate != NULL && bio != NULL && PEM_write_bio_X509(bio, certificate) == 1;
    if (valid)
        size = BIO_get_mem_data(bio, &data);
    valid = valid && size > 0 && size <= SP5_CERTIFICATE_CAPACITY && data != NULL;
    if (valid) {
        memcpy(storage, data, (size_t)size);
        *out = (sp5_bytes){storage, (size_t)size};
    }
    BIO_free(bio);
    return valid;
}
static sp5_result input_end(void *context, sp5_end_reason reason)
{
    fixture_host *h = context;
    sp5_result result = sp5_lifecycle_stop_input(h->lifecycle, reason);
    atomic_store_explicit(&h->stopped, true, memory_order_release);
    return result;
}
static void *input_owner(void *context)
{
    fixture_host *h = context;
    uint64_t event_id = 0;
    while (!atomic_load_explicit(&h->stopped, memory_order_acquire)) {
        int requested = atomic_load_explicit(&h->end_request, memory_order_acquire);
        if (requested >= 0) {
            h->worker_error = sp5_control_end(h->control, (sp5_end_reason)requested).reason;
            break;
        }
        sp5_result step = sp5_control_transport_step(h->transport, now_us());
        if (step.status != SP5_OK && !atomic_load_explicit(&h->stopped, memory_order_acquire)) {
            h->worker_error = step.reason;
            (void)sp5_control_end(h->control, SP5_END_ERROR);
            break;
        }
        sp5_control_event event;
        unsigned drained = 0;
        while (drained++ < SP5_CONTROL_QUEUE_CAPACITY &&
               sp5_control_take(h->control, &event).status == SP5_OK) {
            if (event.type == 0x0206) {
                /* transport stamps at recv before its parse/auth; no invented
                 * client clocks. Metrics-off receipt stays unavailable. */
                sp5_timestamp received = {event.received_us, 1, 1, SP5_VALIDITY_VALID,
                                          SP5_REASON_NONE};
                sp5_input_evidence evidence;
                sp5_result result =
                    sp5_lifecycle_input(h->lifecycle, &event, received, ++event_id, &evidence);
                if (result.status != SP5_OK) {
                    h->worker_error = result.reason;
                    (void)sp5_control_end(h->control, SP5_END_ERROR);
                    break;
                }
                atomic_fetch_add_explicit(&h->input_count, 1, memory_order_relaxed);
                atomic_fetch_or(&h->pads_seen, 1U << evidence.controller_id);
                const sp5_controller_state *state =
                    &h->input_fake.controllers[evidence.controller_id].state;
                atomic_fetch_or(&h->buttons_seen, state->buttons);
                if (state->axes[0] != 0 || state->axes[1] != 0)
                    atomic_store(&h->axes_seen, true);
                if (state->triggers[0] != 0 || state->triggers[1] != 0)
                    atomic_store(&h->triggers_seen, true);
                atomic_store_explicit(&h->last_input_us, now_us(), memory_order_relaxed);
            }
        }
        pause_step();
    }
    atomic_store_explicit(&h->stopped, true, memory_order_release);
    return NULL;
}
static bool learn_ping(fixture_host *h, int fd, struct sockaddr_in *destination)
{
    uint8_t wire[64];
    struct sockaddr_in peer;
    socklen_t length = sizeof(peer);
    ssize_t size = recvfrom(fd, wire, sizeof(wire), 0, (struct sockaddr *)&peer, &length);
    if (size < 0)
        return false;
    char address[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address)) == NULL ||
        strcmp(address, h->launch_peer) != 0)
        return false;
    static const char hex[] = "0123456789abcdef";
    uint8_t token[16];
    for (size_t i = 0; i < 8; ++i) {
        token[i * 2] = (uint8_t)hex[h->launch.ping[i] >> 4];
        token[i * 2 + 1] = (uint8_t)hex[h->launch.ping[i] & 15];
    }
    if (size != 20 || memcmp(wire, token, sizeof(token)) != 0)
        return false;
    *destination = peer;
    return true;
}
static bool send_packet(int fd, const struct sockaddr_in *destination, sp5_bytes packet)
{
    ssize_t sent = sendto(fd, packet.data, packet.size, 0, (const struct sockaddr *)destination,
                          sizeof(*destination));
    return sent >= 0 && (size_t)sent == packet.size;
}
static sp5_stage_span stage(sp5_timestamp start, sp5_timestamp end)
{
    return (sp5_stage_span){start, end, SP5_VALIDITY_VALID, SP5_REASON_NONE};
}
static bool send_video(fixture_host *h, const struct sockaddr_in *destination)
{
    sp5_fake_platform *f = &h->media_fake;
    f->now = stamp();
    f->clock_description.domain = 1;
    f->clock_description.epoch = 1;
    sp5_timestamp begin = f->now;
    sp5_timestamp deadline = begin;
    deadline.ticks += 100000;
    sp5_video_frame frame;
    if (f->capture.acquire(f, deadline, true, &frame).status != SP5_OK)
        return false;
    sp5_timestamp acquired = stamp();
    uint8_t validation[64];
    sp5_buffer fake_bytes = {validation, sizeof(validation), 0};
    sp5_encoded_frame encoded;
    sp5_result encode_result =
        f->encoder.encode(f, &frame, true, deadline, true, &fake_bytes, &encoded);
    sp5_timestamp completed = stamp();
    sp5_result released = f->capture.release(f, frame.lease_id);
    if (encode_result.status != SP5_OK || released.status != SP5_OK)
        return false;
    /* Replace deliberately non-playable fake output with the explicitly supplied
     * deterministic encoded fixture. This is never hardware encoder evidence. */
    sp5_protocol_latency_evidence latency;
    if (sp5_metrics_protocol_header(sp5_lifecycle_metrics(h->lifecycle), frame.frame_id, begin,
                                    false, &latency)
            .status != SP5_OK)
        return false;
    uint64_t elapsed = latency.header_measurement.ticks >= begin.ticks
                           ? latency.header_measurement.ticks - begin.ticks
                           : 0;
    sp5_video_frame_input input = {.bitstream = h->staged_video,
                                   .frame_index = (uint32_t)frame.frame_id,
                                   .rtp_timestamp = (uint32_t)(frame.frame_id * 1500),
                                   .key_frame = true,
                                   .processing_us = {true, elapsed}};
    if (sp5_video_begin(h->video, &input).status != SP5_OK)
        return false;
    sp5_timestamp send_begin = {0};
    bool first_packet = true;
    uint8_t wire[SP5_VIDEO_WIRE_LIMIT];
    sp5_buffer output = {wire, sizeof(wire), 0};
    uint64_t network = 0;
    sp5_result result;
    while ((result = sp5_video_next(h->video, &output)).status == SP5_OK) {
        if (atomic_load_explicit(&h->stopped, memory_order_acquire)) {
            sp5_video_cancel(h->video);
            return true;
        }
        if (first_packet) {
            send_begin = stamp();
            first_packet = false;
        }
        if (!send_packet(h->video_socket, destination, (sp5_bytes){output.data, output.size})) {
            sp5_video_cancel(h->video);
            return false;
        }
        network += output.size;
    }
    if (result.reason != SP5_REASON_WOULD_BLOCK)
        return false;
    sp5_timestamp sent = stamp();
    sp5_frame_evidence evidence = {.session_id = h->launch.session_id,
                                   .frame_id = frame.frame_id,
                                   .source_epoch = frame.source_epoch,
                                   .outcome = SP5_FRAME_NETWORK_SUBMITTED};
    evidence.stages[0] = stage(begin, acquired);
    evidence.stages[1] = stage(acquired, completed);
    /* Packetization and sends are interleaved by this bounded pull interface;
     * individual stage boundaries unavailable, full pipeline remains observed. */
    evidence.stages[2] = (sp5_stage_span){.reason = SP5_REASON_STAGE_UNAVAILABLE};
    evidence.stages[3] = stage(send_begin, sent);
    evidence.stages[4] = stage(begin, sent);
    (void)sp5_metrics_record_frame(sp5_lifecycle_metrics(h->lifecycle), &evidence);
    (void)sp5_metrics_count_stream(
        sp5_lifecycle_metrics(h->lifecycle),
        (sp5_metrics_stream_event){SP5_FRAME_NETWORK_SUBMITTED, h->video_size, network});
    atomic_fetch_add_explicit(&h->video_count, 1, memory_order_relaxed);
    return true;
}
static bool send_audio(fixture_host *h, const struct sockaddr_in *destination)
{
    sp5_audio_block block;
    sp5_fake_platform *f = &h->media_fake;
    f->now = stamp();
    sp5_timestamp deadline = f->now;
    deadline.ticks += 100000;
    if (f->audio.acquire(f, deadline, &block).status != SP5_OK)
        return false;
    sp5_result released = f->audio.release(f, block.lease_id);
    if (released.status != SP5_OK ||
        sp5_audio_submit(h->audio, (sp5_bytes){h->opus_fixture, h->opus_size}).status != SP5_OK)
        return false;
    uint8_t wire[SP5_AUDIO_WIRE_LIMIT];
    sp5_buffer output = {wire, sizeof(wire), 0};
    sp5_result result;
    while ((result = sp5_audio_next(h->audio, &output)).status == SP5_OK) {
        if (atomic_load_explicit(&h->stopped, memory_order_acquire))
            return true;
        if (!send_packet(h->audio_socket, destination, (sp5_bytes){output.data, output.size}))
            return false;
    }
    atomic_fetch_add_explicit(&h->audio_count, 1, memory_order_relaxed);
    return result.reason == SP5_REASON_WOULD_BLOCK;
}
static void *media_owner(void *context)
{
    fixture_host *h = context;
    struct sockaddr_in video_destination = {0}, audio_destination = {0};
    bool video_known = false, audio_known = false;
    uint64_t next_video = 0, next_audio = 0;
    while (!atomic_load_explicit(&h->stopped, memory_order_acquire)) {
        if (atomic_load(&h->pause_media)) {
            pause_step();
            continue;
        }
        if (learn_ping(h, h->video_socket, &video_destination))
            video_known = true;
        if (learn_ping(h, h->audio_socket, &audio_destination))
            audio_known = true;
        uint64_t now = now_us();
        bool success = true;
        if (video_known && now >= next_video) {
            success = send_video(h, &video_destination);
            next_video = now + 16666;
        }
        if (success && audio_known && now >= next_audio) {
            success = send_audio(h, &audio_destination);
            next_audio = now + 5000;
        }
        if (!success) {
            atomic_store_explicit(&h->end_request, SP5_END_ERROR, memory_order_release);
            break;
        }
        pause_step();
    }
    atomic_store_explicit(&h->media_done, true, memory_order_release);
    return NULL;
}
static void *metrics_owner(void *context)
{
    fixture_host *h = context;
    uint64_t next = 0;
    while (!atomic_load_explicit(&h->stopped, memory_order_acquire)) {
        uint64_t now = now_us();
        if (now >= next) {
            (void)sp5_metrics_worker_step(sp5_lifecycle_metrics(h->lifecycle), now, NULL);
            next = now + 500000;
        }
        pause_step();
    }
    atomic_store_explicit(&h->metrics_done, true, memory_order_release);
    return NULL;
}
static sp5_result write_metrics(void *context, sp5_bytes chunk)
{
    FILE *file = context;
    return fwrite(chunk.data, 1, chunk.size, file) == chunk.size ? sp5_success()
                                                                 : sp5_failure(SP5_REASON_PLATFORM);
}
static int clean_session(fixture_host *h, const char *metrics_path)
{
    if (h->running) {
        REQUIRE(atomic_load_explicit(&h->stopped, memory_order_acquire));
        REQUIRE(pthread_join(h->input_thread, NULL) == 0);
        REQUIRE(pthread_join(h->media_thread, NULL) == 0);
        REQUIRE(pthread_join(h->metrics_thread, NULL) == 0);
        h->running = false;
    }
    if (h->lifecycle == NULL)
        return 0;
    /* Input was already invalidated and released by its independent owner.
     * Only now can management wait for producers and close media. */
    sp5_end_reason reason = sp5_lifecycle_read(h->lifecycle).has_end_reason
                                ? sp5_lifecycle_read(h->lifecycle).end_reason
                                : SP5_END_ERROR;
    REQUIRE(sp5_lifecycle_end(h->lifecycle, reason).status == SP5_OK);
    sp5_control_transport_close(h->transport);
    h->transport = NULL;
    sp5_control_destroy(h->control);
    h->control = NULL;
    sp5_video_destroy(h->video);
    h->video = NULL;
    sp5_audio_destroy(h->audio);
    h->audio = NULL;
    bool clean = !h->media_fake.capture_open && !h->media_fake.encoder_open &&
                 !h->media_fake.audio_open && !h->media_fake.video_leased &&
                 !h->media_fake.audio_leased && !h->media_fake.prepared;
    for (size_t i = 0; i < SP5_MAX_CONTROLLERS; ++i)
        clean = clean && !h->input_fake.controllers[i].live &&
                h->input_fake.controllers[i].state.buttons == 0;
    if (metrics_path != NULL && sp5_lifecycle_metrics(h->lifecycle) != NULL) {
        REQUIRE(
            sp5_metrics_worker_step(sp5_lifecycle_metrics(h->lifecycle), now_us(), NULL).status ==
            SP5_OK);
        FILE *file = fopen(metrics_path, "wb");
        REQUIRE(file != NULL);
        sp5_metrics_sink sink = {file, write_metrics};
        sp5_result exported = sp5_metrics_export(sp5_lifecycle_metrics(h->lifecycle), &sink);
        REQUIRE(fclose(file) == 0 && exported.status == SP5_OK);
    }
    sp5_control_pool_view pool = sp5_control_pool_read(h->runtime);
    REQUIRE(emit("{\"event\":\"ended\",\"synthetic\":true,\"reason\":%u,\"clean\":%s,\"inputs\":%"
                 "llu,\"video_frames\":%llu,\"audio_frames\":%llu,\"input_release_sequence\":%"
                 "llu,\"media_closes\":%llu,\"enet_blocks\":%zu,\"enet_exhausted\":%llu}\n",
                 (unsigned)reason, clean ? "true" : "false",
                 (unsigned long long)atomic_load(&h->input_count),
                 (unsigned long long)atomic_load(&h->video_count),
                 (unsigned long long)atomic_load(&h->audio_count),
                 (unsigned long long)h->input_fake.last_input_release_sequence,
                 (unsigned long long)(h->media_fake.capture_closes + h->media_fake.encoder_closes +
                                      h->media_fake.audio_closes),
                 pool.used_blocks, (unsigned long long)pool.exhausted) > 0);
    REQUIRE(emit("{\"event\":\"input_observations\",\"synthetic\":true,\"buttons_seen\":%u,\"pads_"
                 "seen\":%u,\"axes_seen\":%s,\"triggers_seen\":%s}\n",
                 atomic_load(&h->buttons_seen), atomic_load(&h->pads_seen),
                 atomic_load(&h->axes_seen) ? "true" : "false",
                 atomic_load(&h->triggers_seen) ? "true" : "false") > 0);
    REQUIRE(clean && pool.used_blocks == 0 && pool.exhausted == 0);
    if (h->client_certificate.size != 0 &&
        sp5_devices_authorized(sp5_store_devices(h->store), h->client_certificate))
        REQUIRE(sp5_devices_session_ended(sp5_store_devices(h->store), h->client_certificate,
                                          wall_us().value)
                    .status == SP5_OK);
    sp5_lifecycle_destroy(h->lifecycle);
    h->lifecycle = NULL;
    return 0;
}
static int start_session(fixture_host *h)
{
    /* The fake adapters are single-owner contexts. Reset only after all prior
     * producers joined and lifecycle cleanup returned their leases/resources. */
    sp5_fake_init(&h->media_fake);
    sp5_fake_init(&h->input_fake);
    h->input_fake.confirm_delivery = true;
    h->input_fake.input.deliver = delivered;
    atomic_store(&h->input_count, 0);
    atomic_store(&h->video_count, 0);
    atomic_store(&h->audio_count, 0);
    atomic_store(&h->buttons_seen, 0);
    atomic_store(&h->pads_seen, 0);
    atomic_store(&h->axes_seen, false);
    atomic_store(&h->triggers_seen, false);
    sp5_session_view view = sp5_session_read(h->session);
    REQUIRE(sp5_rtsp_negotiated(h->rtsp, &h->negotiated).status == SP5_OK);
    REQUIRE(h->negotiated.audio_duration_ms == 5);
    sp5_lifecycle_ops ops = {h->media_fake.capture, h->media_fake.encoder, h->media_fake.audio,
                             h->input_fake.input, h->media_fake.system};
    h->lifecycle = sp5_lifecycle_create(view.id, &ops);
    REQUIRE(h->lifecycle != NULL);
    sp5_lifecycle_policy policy = {.mode_count = 1,
                                   .media_bytes = FIXTURE_LIMIT,
                                   .core_bytes = (size_t)8 * FIXTURE_LIMIT,
                                   .streaming_cpu_us_per_10s = 100000,
                                   .idle_cpu_us_per_10s = 1000,
                                   .validity = SP5_VALIDITY_VALID,
                                   .synthetic = true,
                                   .evidence_ref = "fixture-only:declared-test-costs",
                                   .clock = {NULL, clock_describe, clock_now},
                                   .metrics_capacity = {1024, 4096, 256},
                                   .metrics_enabled = true};
    policy.modes[0] = h->host.supported_modes[0];
    policy.metrics_context.synthetic = true;
    (void)snprintf(policy.metrics_context.host_version, sizeof(policy.metrics_context.host_version),
                   "portable-fake-T068");
    (void)snprintf(policy.metrics_context.firmware_version,
                   sizeof(policy.metrics_context.firmware_version), "synthetic-no-console");
    (void)snprintf(policy.metrics_context.client_name, sizeof(policy.metrics_context.client_name),
                   "independent-client");
    policy.metrics_context.requested_bitrate_kbps = view.requested_bitrate_kbps;
    REQUIRE(sp5_lifecycle_configure(h->lifecycle, &policy).status == SP5_OK);
    REQUIRE(sp5_lifecycle_bind(h->lifecycle, h->session, h->client_certificate).status == SP5_OK);
    if (h->inject_failure) {
        h->media_fake.fail_next[h->fail_operation] = SP5_REASON_PLATFORM;
        h->inject_failure = false;
    }
    sp5_media_config config = {
        view.width, view.height, view.fps, 1, "h264", SP5_SDR, view.chosen_bitrate_kbps};
    sp5_result opened = sp5_lifecycle_start(h->lifecycle, &config, now_us());
    if (opened.status != SP5_OK) {
        REQUIRE(clean_session(h, NULL) == 0);
        return 1;
    }
    sp5_buffer staged;
    REQUIRE(sp5_lifecycle_media_buffer(h->lifecycle, 0, &staged, h->video_size).status == SP5_OK);
    REQUIRE(sp5_buffer_append(&staged, (sp5_bytes){h->video_fixture, h->video_size}).status ==
            SP5_OK);
    h->staged_video = (sp5_bytes){staged.data, staged.size};
    sp5_video_config vc = {.packet_size = (uint16_t)h->negotiated.packet_size,
                           .fec_percent = 20,
                           .minimum_parity = 2,
                           .encrypted = (h->negotiated.encryption_flags & 2) != 0};
    memcpy(vc.key, h->launch.key, sizeof(vc.key));
    h->video = sp5_video_create(&vc);
    sp5_audio_config ac = {.sample_rate = 48000,
                           .channels = 2,
                           .duration_ms = 5,
                           .opus_size = (uint16_t)h->opus_size,
                           .key_id = h->launch.key_id,
                           .encrypted = (h->negotiated.encryption_flags & 4) != 0};
    memcpy(ac.key, h->launch.key, sizeof(ac.key));
    h->audio = sp5_audio_create(&ac);
    REQUIRE(h->video != NULL && h->audio != NULL);
    REQUIRE(sp5_session_activate(h->session).status == SP5_OK);
    sp5_control_config cc = {.session = h->session,
                             .verified_certificate = h->client_certificate,
                             .launch = h->launch,
                             .negotiated = h->negotiated,
                             .peer = h->launch_peer,
                             .started_us = now_us(),
                             .end_context = h,
                             .release_input = input_end};
    h->control = sp5_control_create(&cc);
    REQUIRE(h->control != NULL);
    h->transport = sp5_control_transport_open(h->runtime, h->control, h->bind_address, 47999);
    REQUIRE(h->transport != NULL);
    REQUIRE(emit("{\"event\":\"reserved\",\"synthetic\":true,\"session_id\":%llu,\"requested_"
                 "kbps\":%u,\"chosen_kbps\":%u}\n",
                 (unsigned long long)view.id, view.requested_bitrate_kbps,
                 view.chosen_bitrate_kbps) > 0);
    return 0;
}
static int play(fixture_host *h)
{
    atomic_store(&h->stopped, false);
    atomic_store(&h->media_done, false);
    atomic_store(&h->metrics_done, false);
    atomic_store(&h->end_request, -1);
    atomic_store(&h->input_count, 0);
    atomic_store(&h->video_count, 0);
    atomic_store(&h->audio_count, 0);
    atomic_store(&h->last_input_us, 0);
    atomic_store(&h->buttons_seen, 0);
    atomic_store(&h->pads_seen, 0);
    atomic_store(&h->axes_seen, false);
    atomic_store(&h->triggers_seen, false);
    atomic_store(&h->pause_media, false);
    h->worker_error = SP5_REASON_NONE;
    REQUIRE(pthread_create(&h->input_thread, NULL, input_owner, h) == 0);
    if (pthread_create(&h->media_thread, NULL, media_owner, h) != 0) {
        atomic_store(&h->end_request, SP5_END_ERROR);
        REQUIRE(pthread_join(h->input_thread, NULL) == 0);
        return 1;
    }
    if (pthread_create(&h->metrics_thread, NULL, metrics_owner, h) != 0) {
        atomic_store(&h->end_request, SP5_END_ERROR);
        REQUIRE(pthread_join(h->input_thread, NULL) == 0);
        REQUIRE(pthread_join(h->media_thread, NULL) == 0);
        return 1;
    }
    h->running = true;
    REQUIRE(fputs("{\"event\":\"streaming\",\"synthetic\":true}\n", stdout) != EOF);
    return 0;
}
static void close_connection(connection *c)
{
    SSL_free(c->tls);
    c->tls = NULL;
    if (c->fd >= 0)
        (void)close(c->fd);
    *c = (connection){.fd = -1};
}
static bool write_connection(connection *c, sp5_bytes wire)
{
    /* Management-only bounded writes; no media/input owner can wait here. */
    size_t sent = 0;
    uint64_t deadline = now_us() + 1000000;
    while (sent < wire.size && now_us() < deadline) {
        int n = c->tls != NULL ? SSL_write(c->tls, wire.data + sent, (int)(wire.size - sent))
                               : (int)send(c->fd, wire.data + sent, wire.size - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        int error = c->tls != NULL ? SSL_get_error(c->tls, n) : SSL_ERROR_SYSCALL;
        if ((c->tls != NULL && error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) ||
            (c->tls == NULL && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            return false;
        pause_step();
    }
    return sent == wire.size;
}
static int http_reply(connection *c, const sp5_http_response *response)
{
    uint8_t wire[SP5_HTTP_LIMIT + 512];
    sp5_buffer output = {wire, sizeof(wire), 0};
    REQUIRE(sp5_http_render(response, &output).status == SP5_OK);
    REQUIRE(write_connection(c, (sp5_bytes){output.data, output.size}));
    close_connection(c);
    return 0;
}
static int service_request(fixture_host *h, connection *c, const char *metrics_path)
{
    sp5_request request;
    if (!c->ready) {
        if (c->tls != NULL && !SSL_is_init_finished(c->tls)) {
            int accepted = SSL_accept(c->tls);
            if (accepted != 1) {
                int error = SSL_get_error(c->tls, accepted);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
                    return 0;
                close_connection(c);
                return 0; /* Expected for unpaired/revoked TLS. */
            }
        }
        ssize_t count =
            c->tls != NULL ? SSL_read(c->tls, c->input + c->used, (int)(sizeof(c->input) - c->used))
                           : recv(c->fd, c->input + c->used, sizeof(c->input) - c->used, 0);
        if (count <= 0) {
            int error = c->tls != NULL ? SSL_get_error(c->tls, (int)count) : SSL_ERROR_SYSCALL;
            if (c->tls != NULL && (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE))
                return 0;
            if (c->tls == NULL && count < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                return 0;
            close_connection(c);
            return 0;
        }
        c->used += (size_t)count;
        if (c->rtsp && h->launch.encrypted_rtsp) {
            if (c->used < 24)
                return 0;
            size_t size = (size_t)(c->input[0] & 0x7f) << 24 | (size_t)c->input[1] << 16 |
                          (size_t)c->input[2] << 8 | c->input[3];
            if (size > SP5_RTSP_LIMIT) {
                close_connection(c);
                return 0;
            }
            if (c->used < size + 24)
                return 0;
            c->ready = c->used == size + 24;
        } else {
            sp5_result parsed =
                c->rtsp
                    ? sp5_rtsp_parse((sp5_bytes){c->input, c->used}, sizeof(c->input), &request)
                    : sp5_http_parse((sp5_bytes){c->input, c->used}, sizeof(c->input), &request);
            c->ready = parsed.status == SP5_OK;
        }
        if (!c->ready) {
            if (c->used == sizeof(c->input))
                close_connection(c);
            return 0;
        }
    }
    if (c->rtsp) {
        if (h->running || h->rtsp == NULL) {
            close_connection(c);
            return 0;
        }
        uint8_t wire[SP5_RTSP_LIMIT + 24];
        sp5_buffer output = {wire, sizeof(wire), 0};
        REQUIRE(sp5_rtsp_exchange(h->rtsp, c->peer, false, (sp5_bytes){c->input, c->used}, now_us(),
                                  wall_us(), &output)
                    .status == SP5_OK);
        sp5_session_view view = sp5_session_read(h->session);
        if (view.mode_configured && view.state == SP5_SESSION_NEGOTIATING && h->lifecycle == NULL) {
            if (start_session(h) != 0) {
                /* ANNOUNCE failed to reserve: decline rather than a fake success. */
                sp5_rtsp_destroy(h->rtsp);
                h->rtsp = NULL;
                close_connection(c);
                return 0;
            }
        }
        REQUIRE(write_connection(c, (sp5_bytes){output.data, output.size}));
        close_connection(c);
        /* PLAY is the only exchange after resources activated: encrypted/plain
         * parser already validated its sequence. There are no later SETUPs. */
        if (h->lifecycle != NULL && view.state == SP5_SESSION_ACTIVE && sp5_rtsp_playing(h->rtsp))
            REQUIRE(play(h) == 0);
        return 0;
    }
    uint8_t fields[SP5_HTTP_LIMIT];
    sp5_http_response response = {.code = 200, .fields = {fields, sizeof(fields), 0}};
    if (c->deferred) {
        sp5_result result =
            sp5_pair_flow_take_certificate(h->pair, c->request_id, now_us(), &response);
        if (result.reason == SP5_REASON_WOULD_BLOCK)
            return 0;
        REQUIRE(result.status == SP5_OK);
        return http_reply(c, &response);
    }
    REQUIRE(sp5_http_parse((sp5_bytes){c->input, c->used}, sizeof(c->input), &request).status ==
            SP5_OK);
    bool cancel = request.target.size >= 7 && memcmp(request.target.data, "/cancel", 7) == 0;
    if (h->running) {
        uint8_t cert[SP5_CERTIFICATE_CAPACITY];
        sp5_bytes authenticated = {0};
        bool owns = c->secure && peer_certificate(c->tls, cert, &authenticated) &&
                    authenticated.size == h->client_certificate.size &&
                    memcmp(authenticated.data, h->client_certificate.data, authenticated.size) == 0;
        if (cancel && owns) {
            atomic_store_explicit(&h->end_request, SP5_END_CLIENT_QUIT, memory_order_release);
            return 0; /* Retry after input owner releases and joins. */
        }
        /* No mutable session snapshots race its independent owner. Refusals are
         * actual refusal responses, never placeholder protocol successes. */
        response.code = 409;
        return http_reply(c, &response);
    }
    REQUIRE(
        sp5_http_route(h->http, c->peer, false, c->tls, &request, now_us(), wall_us(), &response)
            .status == SP5_OK);
    if (response.deferred) {
        c->deferred = true;
        c->request_id = sp5_pairing_read(h->pairing, now_us()).request_id;
        c->deadline_us = now_us() + 300000000;
        REQUIRE(emit("{\"event\":\"pair_pending\",\"synthetic\":true,\"request_id\":%llu}\n",
                     (unsigned long long)c->request_id) > 0);
        return 0;
    }
    if (response.code == 200 && request.target.size >= 7 &&
        memcmp(request.target.data, "/launch", 7) == 0) {
        if (h->lifecycle != NULL)
            REQUIRE(clean_session(h, metrics_path) == 0);
        sp5_rtsp_destroy(h->rtsp);
        REQUIRE(sp5_http_launch_read(h->http, &h->launch).status == SP5_OK);
        memcpy(h->launch_peer, c->peer, strlen(c->peer) + 1);
        REQUIRE(peer_certificate(c->tls, h->client_certificate_storage, &h->client_certificate));
        sp5_rtsp_config config = {.host = &h->host,
                                  .devices = sp5_store_devices(h->store),
                                  .session = h->session,
                                  .launch = h->launch,
                                  .verified_certificate = h->client_certificate,
                                  .peer = h->launch_peer,
                                  .audio_port = 48000,
                                  .video_port = 47998,
                                  .control_port = 47999};
        h->rtsp = sp5_rtsp_create(&config);
        REQUIRE(h->rtsp != NULL);
    }
    return http_reply(c, &response);
}
static int accept_connection(fixture_host *h, int listener, bool secure, bool rtsp,
                             connection *connections)
{
    struct sockaddr_in remote;
    socklen_t size = sizeof(remote);
    int fd = accept(listener, (struct sockaddr *)&remote, &size);
    if (fd < 0) {
        REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
        return 0;
    }
    char peer[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &remote.sin_addr, peer, sizeof(peer)) == NULL ||
        !sp5_peer_permitted(peer, false)) {
        (void)close(fd);
        return 0;
    }
    size_t slot = 0;
    while (slot < CONNECTIONS && connections[slot].fd >= 0)
        ++slot;
    if (slot == CONNECTIONS) {
        (void)close(fd);
        return 0;
    }
    REQUIRE(nonblocking(fd) == 0);
    connection *c = &connections[slot];
    *c = (connection){.fd = fd, .secure = secure, .rtsp = rtsp, .deadline_us = now_us() + 10000000};
    memcpy(c->peer, peer, strlen(peer) + 1);
    if (secure) {
        c->tls = SSL_new(sp5_http_tls_context(h->http));
        REQUIRE(c->tls != NULL && SSL_set_fd(c->tls, fd) == 1);
        SSL_set_accept_state(c->tls);
    }
    return 0;
}
static int command(fixture_host *h, const char *line, bool *stop)
{
    if (strncmp(line, "pin ", 4) == 0) {
        sp5_pair_view pending = sp5_pairing_read(h->pairing, now_us());
        sp5_bytes pin = {(const uint8_t *)line + 4, strlen(line + 4)};
        sp5_result result =
            sp5_pair_flow_submit_pin(h->pair, pending.request_id, now_us(), pin,
                                     (sp5_bytes){(const uint8_t *)"Portable fixture client", 23});
        REQUIRE(emit("{\"event\":\"pin_submitted\",\"accepted\":%s}\n",
                     result.status == SP5_OK ? "true" : "false") > 0);
    } else if (strcmp(line, "status") == 0) {
        sp5_pair_view pairing = sp5_pairing_read(h->pairing, now_us());
        REQUIRE(emit("{\"event\":\"status\",\"synthetic\":true,\"running\":%s,\"paired\":%zu,"
                     "\"pairing_pending\":%s,\"pairing_failures\":%u,"
                     "\"inputs\":%llu,\"video_frames\":%llu,\"audio_frames\":%llu}\n",
                     h->running ? "true" : "false", sp5_devices_count(sp5_store_devices(h->store)),
                     pairing.state == SP5_PAIR_PENDING ? "true" : "false", pairing.failures,
                     (unsigned long long)atomic_load(&h->input_count),
                     (unsigned long long)atomic_load(&h->video_count),
                     (unsigned long long)atomic_load(&h->audio_count)) > 0);
    } else if (strcmp(line, "revoke") == 0) {
        if (h->running) {
            atomic_store_explicit(&h->end_request, SP5_END_REVOKED, memory_order_release);
            return 2;
        }
        if (h->client_certificate.size != 0)
            REQUIRE(sp5_authority_revoke(h->authority, h->client_certificate).status == SP5_OK);
        REQUIRE(fputs("{\"event\":\"revoked\",\"synthetic\":true}\n", stdout) != EOF);
    } else if (strcmp(line, "fail audio-open") == 0) {
        REQUIRE(!h->running);
        h->inject_failure = true;
        h->fail_operation = SP5_FAKE_AUDIO_OPEN;
        REQUIRE(fputs("{\"event\":\"failure_armed\"}\n", stdout) != EOF);
    } else if (strcmp(line, "pause-media") == 0 || strcmp(line, "resume-media") == 0) {
        atomic_store(&h->pause_media, strcmp(line, "pause-media") == 0);
        REQUIRE(emit("{\"event\":\"media_pause\",\"paused\":%s}\n",
                     atomic_load(&h->pause_media) ? "true" : "false") > 0);
    } else if (strcmp(line, "disconnect") == 0) {
        if (h->running)
            atomic_store_explicit(&h->end_request, SP5_END_CLIENT_LOST, memory_order_release);
    } else if (strcmp(line, "stop") == 0) {
        *stop = true;
        if (h->running)
            atomic_store_explicit(&h->end_request, SP5_END_HOST_STOPPING, memory_order_release);
    } else
        return 1;
    return 0;
}
int main(int argc, char **argv)
{
    if (argc != 6 && argc != 7) {
        REQUIRE(fputs("usage: sp5_fake_host PRIVATE_IPV4 STATE_DIR VIDEO_720P60.h264 "
                      "AUDIO_5MS.opus METRICS.ndjson [WEB_PORT]\n",
                      stderr) != EOF);
        return 64;
    }
    char *web_port_end = NULL;
    unsigned long web_port = argc == 7 ? strtoul(argv[6], &web_port_end, 10) : 0;
    REQUIRE(argc == 6 || (web_port_end != argv[6] && *web_port_end == 0 && web_port <= 65535));
    REQUIRE(sp5_peer_permitted(argv[1], false));
    REQUIRE(signal(SIGPIPE, SIG_IGN) != SIG_ERR && signal(SIGINT, on_signal) != SIG_ERR &&
            signal(SIGTERM, on_signal) != SIG_ERR);
    REQUIRE(setvbuf(stdout, NULL, _IOLBF, 0) == 0);
    fixture_host *h = calloc(1, sizeof(*h));
    REQUIRE(h != NULL);
    h->bind_address = argv[1];
    atomic_init(&h->stopped, true);
    atomic_init(&h->media_done, true);
    atomic_init(&h->metrics_done, true);
    atomic_init(&h->end_request, -1);
    atomic_init(&h->input_count, 0);
    atomic_init(&h->video_count, 0);
    atomic_init(&h->audio_count, 0);
    atomic_init(&h->last_input_us, 0);
    atomic_init(&h->buttons_seen, 0);
    atomic_init(&h->pads_seen, 0);
    atomic_init(&h->axes_seen, false);
    atomic_init(&h->triggers_seen, false);
    atomic_init(&h->pause_media, false);
    REQUIRE(load_fixture(argv[3], h->video_fixture, sizeof(h->video_fixture), &h->video_size));
    REQUIRE(load_fixture(argv[4], h->opus_fixture, sizeof(h->opus_fixture), &h->opus_size));
    REQUIRE(h->video_size >= 4 && memcmp(h->video_fixture, "\0\0\0\1", 4) == 0 &&
            h->opus_size <= UINT16_MAX);
    sp5_fake_init(&h->media_fake);
    sp5_fake_init(&h->input_fake);
    h->input_fake.confirm_delivery = true;
    base_deliver = h->input_fake.input.deliver;
    h->input_fake.input.deliver = delivered;
    sp5_posix_storage *storage = sp5_posix_storage_open(argv[2]);
    REQUIRE(storage != NULL);
    sp5_storage_ops storage_ops = sp5_posix_storage_operations(storage);
    h->store = sp5_store_create(&storage_ops);
    REQUIRE(h->store != NULL && sp5_store_load(h->store).status == SP5_OK);
    REQUIRE(sp5_host_init(&h->host, "portable-fake-T068", "13.60", argv[1]).status == SP5_OK);
    REQUIRE(sp5_host_refresh(&h->host, h->store).status == SP5_OK);
    h->host.supported_mode_count = 1;
    h->host.supported_modes[0] = (sp5_host_mode){1280, 720, 60, "h264", SP5_SDR, 100, 20000};
    h->host.controller_touch_events = true;
    /* This fixture offers encrypted control/audio and plain video; the independent
     * runner does not yet acquire encrypted video. Supported bits per notes §3. */
    h->host.encryption_supported = 5;
    REQUIRE(sp5_host_transition(&h->host, SP5_HOST_RUNNING).status == SP5_OK);
    h->authority = sp5_authority_create(sp5_store_devices(h->store));
    h->session = sp5_session_create(h->authority);
    h->pairing = sp5_pairing_create();
    REQUIRE(h->authority != NULL && h->session != NULL && h->pairing != NULL);
    sp5_bytes identity = sp5_store_identity(h->store);
    const char cert_start[] = "-----BEGIN CERTIFICATE-----";
    size_t cert_offset = 0;
    while (cert_offset + sizeof(cert_start) - 1 <= identity.size &&
           memcmp(identity.data + cert_offset, cert_start, sizeof(cert_start) - 1) != 0)
        ++cert_offset;
    REQUIRE(cert_offset + sizeof(cert_start) - 1 <= identity.size);
    sp5_bytes certificate = {identity.data + cert_offset, identity.size - cert_offset};
    sp5_bytes key = {identity.data, cert_offset};
    sp5_system_ops notifications = h->media_fake.system;
    notifications.context = h;
    notifications.notify = notify;
    sp5_pair_flow_config pc = {h->pairing,    sp5_store_devices(h->store), certificate, key,
                               notifications, "fixture:stdin-pin"};
    h->pair = sp5_pair_flow_create(&pc);
    REQUIRE(h->pair != NULL);
    sp5_http_config hc = {.host = &h->host,
                          .devices = sp5_store_devices(h->store),
                          .session = h->session,
                          .certificate_pem = certificate,
                          .private_key_pem = key,
                          .pair_context = h->pair,
                          .pair = sp5_pair_flow_handle};
    h->http = sp5_http_create(&hc);
    REQUIRE(h->http != NULL);
    h->runtime = sp5_control_runtime_create();
    REQUIRE(h->runtime != NULL);
    int listeners[3] = {open_socket(SOCK_STREAM, argv[1], 47989),
                        open_socket(SOCK_STREAM, argv[1], 47984),
                        open_socket(SOCK_STREAM, argv[1], 48010)};
    h->video_socket = open_socket(SOCK_DGRAM, argv[1], 47998);
    h->audio_socket = open_socket(SOCK_DGRAM, argv[1], 48000);
    REQUIRE(listeners[0] >= 0 && listeners[1] >= 0 && listeners[2] >= 0 && h->video_socket >= 0 &&
            h->audio_socket >= 0);
    sp5_fake_web *web = calloc(1, sizeof(*web));
    REQUIRE(web != NULL);
    char web_authority[64];
    sp5_fake_web_init(
        web, NULL,
        sp5_fake_web_listen(argv[1], (uint16_t)web_port, web_authority, sizeof(web_authority)),
        NULL);
    REQUIRE(web->listener >= 0);
    /* Session cleanup belongs to this harness's owners, so the web config has
     * no lifecycle: web mutations run only while no session resources exist. */
    sp5_web_config wc = {.store = h->store,
                         .host = &h->host,
                         .session = h->session,
                         .pairing = h->pairing,
                         .pair_flow = h->pair,
                         .authority = web_authority};
    web->web = sp5_web_create(&wc);
    REQUIRE(web->web != NULL);
    REQUIRE(emit("{\"event\":\"web\",\"synthetic\":true,\"url\":\"http://%s\"}\n", web_authority) >
            0);
    REQUIRE(nonblocking(STDIN_FILENO) == 0);
    connection connections[CONNECTIONS];
    for (size_t i = 0; i < CONNECTIONS; ++i)
        connections[i] = (connection){.fd = -1};
    REQUIRE(fputs("{\"event\":\"ready\",\"synthetic\":true,\"hardware_evidence\":false}\n",
                  stdout) != EOF);
    bool stop = false, revoke_pending = false;
    char line[COMMAND_LIMIT];
    size_t line_size = 0;
    while (!stop || h->running) {
        if (interrupted) {
            stop = true;
            atomic_store_explicit(&h->end_request, SP5_END_HOST_STOPPING, memory_order_release);
        }
        if (h->running && atomic_load_explicit(&h->stopped, memory_order_acquire))
            REQUIRE(clean_session(h, argv[5]) == 0);
        if (revoke_pending && !h->running) {
            REQUIRE(command(h, "revoke", &stop) == 0);
            revoke_pending = false;
        }
        if (stop && !h->running)
            break;
        struct pollfd polls[4 + SP5_FAKE_WEB_POLLS] = {{STDIN_FILENO, POLLIN, 0},
                                                       {listeners[0], POLLIN, 0},
                                                       {listeners[1], POLLIN, 0},
                                                       {listeners[2], POLLIN, 0}};
        sp5_fake_web_poll(web, polls + 4);
        int ready = poll(polls, 4 + SP5_FAKE_WEB_POLLS, 1);
        REQUIRE(ready >= 0 || errno == EINTR);
        if (ready > 0 && (polls[0].revents & POLLIN) != 0) {
            char chunk[COMMAND_LIMIT];
            ssize_t count = read(STDIN_FILENO, chunk, sizeof(chunk));
            if (count == 0)
                stop = true;
            for (ssize_t i = 0; i < count; ++i) {
                if (chunk[i] == '\n') {
                    line[line_size] = 0;
                    int result = command(h, line, &stop);
                    REQUIRE(result == 0 || result == 2);
                    if (result == 2)
                        revoke_pending = true;
                    line_size = 0;
                } else {
                    REQUIRE(line_size + 1 < sizeof(line));
                    line[line_size++] = chunk[i];
                }
            }
        }
        for (size_t i = 0; i < 3; ++i)
            if ((polls[i + 1].revents & POLLIN) != 0)
                REQUIRE(accept_connection(h, listeners[i], i == 1, i == 2, connections) == 0);
        for (size_t i = 0; i < CONNECTIONS; ++i)
            if (connections[i].fd >= 0) {
                if (now_us() > connections[i].deadline_us)
                    close_connection(&connections[i]);
                else
                    REQUIRE(service_request(h, &connections[i], argv[5]) == 0);
            }
        sp5_fake_web_step(web, polls + 4, now_us(), h->running || h->lifecycle != NULL);
        REQUIRE(!web->failed);
        if (web->stopped)
            stop = true;
        REQUIRE(sp5_pair_flow_tick(h->pair, now_us()).status == SP5_OK);
    }
    if (h->lifecycle != NULL)
        REQUIRE(clean_session(h, argv[5]) == 0);
    for (size_t i = 0; i < CONNECTIONS; ++i)
        close_connection(&connections[i]);
    sp5_fake_web_close(web);
    REQUIRE(!web->failed);
    sp5_web_destroy(web->web);
    free(web);
    for (size_t i = 0; i < 3; ++i)
        REQUIRE(close(listeners[i]) == 0);
    REQUIRE(close(h->video_socket) == 0 && close(h->audio_socket) == 0);
    sp5_rtsp_destroy(h->rtsp);
    sp5_http_destroy(h->http);
    sp5_pair_flow_destroy(h->pair);
    sp5_pairing_destroy(h->pairing);
    sp5_session_destroy(h->session);
    sp5_authority_destroy(h->authority);
    REQUIRE(sp5_control_runtime_destroy(h->runtime).status == SP5_OK);
    sp5_store_destroy(h->store);
    REQUIRE(sp5_posix_storage_close(storage).status == SP5_OK);
    free(h);
    return 0;
}
