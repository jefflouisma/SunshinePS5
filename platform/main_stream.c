/* DIAGNOSTIC full-streaming console composition for T079 evidence.
 *
 * This host is NOT an acceptance artifact. It composes the real provisional
 * T071-T075 adapters (capture, encode, audio, input, system) with the portable
 * core HTTP/pairing/session/RTSP/control/video/audio/metrics/web services and
 * the private-link console web adapter. It streams one bounded SDR (1080p or
 * 720p, 60 or 30 fps) H.264 session and emits ready/streaming/stopped JSON on stdout.
 *
 * CAPTURE PROTECTION IS ASSUMED, NOT OBSERVED. Avcap2 exposes no readable
 * protection state (platform/capture/ps5.h), so after lifecycle start this host
 * calls sp5_ps5_capture_observe_protection(capture, SP5_PROTECTION_CLEAR,
 * "diagnostic:--assume-protection-clear"). The clear state is UNSOURCED and is
 * copied to the audio adapter for the same diagnostic premise. No protection,
 * acceptance or hardware-capability claim follows from this override.
 *
 * Audio has no Opus encoder in this tree: sp5_audio is an RTP/FEC packetizer.
 * The acquired stereo float32 block is therefore packetized as an opaque
 * diagnostic payload (leading bounded bytes, zero padded), not as decodable
 * Opus. Video is the real Avcap2 Annex-B access unit forwarded by the encode
 * adapter. No fixture media files are read.
 *
 * Port topology follows tests/fakes/host_main.c: 47989 plain HTTP,
 * 47984 secure RTSP, 48010 secure HTTP, 47998 video UDP, 48000 audio UDP,
 * 47999 control ENet. The core web app/status is served on 8081.
 *
 * Teardown ordering: the input owner invalidates admission and neutralizes
 * controllers (sp5_lifecycle_stop_input), the management owner joins input,
 * media and metrics owners, then ends the lifecycle, closes control/video/audio
 * and network listeners. */
#include "audio/opus_encoder.h"
#include "audio/ps5.h"
#include "capture/ps5.h"
#include "encode/ps5.h"
#include "input/ps5.h"
#include "platform.h"
#include "protocol/audio.h"
#include "protocol/control.h"
#include "protocol/pairing_flow.h"
#include "protocol/rtsp.h"
#include "protocol/video.h"
#include "ps5_clock.h"
#include "ps5_media_global.h"
#include "runtime.h"
#include "session/lifecycle.h"
#include "session/session.h"
#include "settings/host.h"
#include "system/ps5.h"
#include "web/server.h"
#include "web_service.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <openssl/pem.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef SP5_DIAGNOSTIC_PROTECTION_CLEAR
#define SP5_DIAGNOSTIC_PROTECTION_CLEAR 0
#endif

#ifndef SP5_VERSION
#define SP5_VERSION "dev"
#endif
#define SP5_HOST_VERSION "sunshineps5-" SP5_VERSION
/* /system_tmp is the console path that accepts AF_UNIX sockets (/data and
 * /user/temp refuse them); it is cleared on reboot. */
#define TAKEOVER_SOCKET "/system_tmp/sunshineps5-takeover.sock"

enum {
    CONNECTIONS = 8,
    COMMAND_LIMIT = 128,
    HTTP_PORT_DEFAULT = 47989,
    SECURE_HTTP_PORT = 47984,
    RTSP_PORT = 48010,
    VIDEO_PORT = 47998,
    AUDIO_PORT = 48000,
    CONTROL_PORT = 47999,
    LEGACY_TAKEOVER_PORT = 47990,
    PLAY_DEADLINE_US = 30000000,
    KEYFRAME_RESTART_INTERVAL_US = 1000000,
    KEYFRAME_WAIT_RECORDS = 30,
    SOURCE_RETRY_US = 250000,
    WORKER_EXIT_US = 3000000,
    SOURCE_RECOVERY_US = 15000000,
    SEND_RETRY_US = 20000,
    SEND_DROP_LOG_INTERVAL_US = 1000000,
    ADDRESS_CHECK_INTERVAL_US = 2000000,
    ACCEPT_ERROR_INTERVAL_US = 5000000,
    WEB_PORT_DEFAULT = 8081,
    RUN_SECONDS_DEFAULT = 0,
    VIDEO_DEADLINE_US = 100000,
    AUDIO_DEADLINE_US = 100000,
    DIAGNOSTIC_AUDIO_PAYLOAD = 128,
    DIAGNOSTIC_OPUS_BITRATE = 96000,
    DIAGNOSTIC_PCM_FRAMES = 4096
};

#define REQUIRE(c)                                                                                 \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            if (fputs(__FILE__ ": " #c "\n", stderr) < 0)                                          \
                return 2;                                                                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct {
    const char *bind_address;
    const char *state_directory;
    uint16_t http_port, web_port;
    uint64_t run_seconds;
    bool assume_protection_clear;
} stream_options;

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
    sp5_system_ops base;
} console_system;

typedef enum {
    STAGE_IDLE,
    STAGE_VIDEO_READ,
    STAGE_VIDEO_RESTART,
    STAGE_VIDEO_SEND,
    STAGE_AUDIO_READ,
    STAGE_AUDIO_SEND
} media_stage;

static const char *stage_name(int stage)
{
    static const char *const names[] = {"idle",       "video_read", "video_restart",
                                        "video_send", "audio_read", "audio_send"};
    return stage >= 0 && stage < (int)(sizeof(names) / sizeof(names[0])) ? names[stage] : "unknown";
}

typedef struct {
    stream_options options;
    sp5_posix_storage *storage;
    sp5_store *store;
    sp5_host host;
    sp5_authority *authority;
    sp5_session *session;
    sp5_pairing *pairing;
    sp5_pair_flow *pair;
    sp5_http_service *http;
    sp5_rtsp *rtsp;
    sp5_lifecycle *lifecycle;
    sp5_control_runtime *runtime;
    sp5_control *control;
    sp5_control_transport *transport;
    sp5_video *video;
    sp5_audio *audio;
    sp5_ps5_capture capture;
    sp5_ps5_encoder encoder;
    sp5_ps5_audio audio_adapter;
    sp5_ps5_input input_adapter;
    sp5_ps5_system system;
    console_system console_system;
    sp5_negotiated negotiated;
    sp5_http_launch launch;
    sp5_bytes client_certificate;
    uint8_t client_certificate_storage[SP5_CERTIFICATE_CAPACITY];
    uint8_t audio_payload[DIAGNOSTIC_AUDIO_PAYLOAD];
    float audio_pcm[DIAGNOSTIC_PCM_FRAMES * 2];
    size_t audio_pcm_frames;
    sp5_opus_encoder *opus;
    uint32_t opus_size;
    sp5_buffer video_buffer;
    int listeners[3];
    int video_socket, audio_socket;
    /* Takeover listeners: the AF_UNIX socket hands the /dev/hid descriptor
     * to the next host; the loopback TCP port serves v0.1.0 hosts. */
    int takeover_fd;
    int legacy_takeover_fd;
    bool legacy_handoff;
    /* Set when lifecycle_end failed; the main loop retries until it succeeds. */
    bool cleanup_pending;
    uint64_t cleanup_retry_us;
    uint64_t lifecycle_started_us;
    char launch_peer[INET_ADDRSTRLEN];
    /* Served console address. Without --bind it follows the console: when it
     * disappears from every interface the idle host rebinds to the new one. */
    char address[INET_ADDRSTRLEN];
    bool address_fixed, address_lost_reported;
    uint64_t address_check_us, accept_error_us;
    char web_authority[64];
    char mode_text[48];           /* negotiated mode of the current session */
    uint32_t rtp_ticks_per_frame; /* 90 kHz RTP clock / negotiated fps */
    char pin_url[128];
    char client_session_id[128];
    char host_observations[65536];
    size_t host_observations_size;
    bool host_observations_overflow;
    sp5_console_web web_service;
    pthread_t input_thread, media_thread, metrics_thread, audio_thread;
    bool running;
    atomic_bool stopped, media_done, metrics_done, audio_done;
    atomic_int end_request;
    atomic_uint_fast64_t input_count, video_count, audio_count;
    atomic_uint_fast64_t thread_cpu_us[5];
    uint64_t video_acquire_us, video_encode_us, video_send_us;
    uint32_t video_timing_frames, video_key_frames;
    /* Set by the input owner when Moonlight asks for an IDR (0x0302) or
     * reports lost reference frames (0x0301); served by the video owner. */
    atomic_bool keyframe_requested;
    uint64_t keyframe_restart_us;
    /* Console call each media owner is inside (media_stage), for diagnosing a
     * worker that does not exit at teardown. */
    atomic_int video_stage, audio_stage;
    /* A media owner never returned from a console call: its session resources
     * stay allocated, new sessions are refused, and takeover still works. */
    bool media_wedged;
    /* Video source lost (Avcap2 reads fail while the console changes video
     * mode, e.g. when a game starts or closes); the video owner reopens it. */
    bool source_recovering;
    uint64_t source_lost_us, source_retry_us;
    uint32_t source_attempts;
    /* After a restart: records seen while waiting for its IDR (0 = not waiting). */
    uint32_t keyframe_wait;
    /* send_drops/send_drop_logged_us: video and audio owners both update them;
     * a lost increment only skews a diagnostic count. */
    _Atomic uint64_t send_drops;
    uint64_t send_drop_logged_us;
    uint32_t video_sample_acquire[64], video_sample_encode[64];
    uint32_t video_sample_send[64], video_sample_pipeline[64];
    atomic_bool protection_assumed;
    bool protection_clear_used;
    sp5_reason worker_error;
} stream_host;

/* stdout is the payload loader's socket to whoever sent the ELF. It goes away
 * when that terminal closes or the console's address changes, so the log is
 * best effort: a line that cannot be sent at once is dropped, and after a hard
 * error the host stops writing. Log delivery never blocks or fails host work. */
static atomic_bool log_closed;
static atomic_bool log_line_open;

static bool log_send(const char *data, size_t size, size_t *sent)
{
    ssize_t count = send(STDOUT_FILENO, data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (count < 0 && errno == ENOTSOCK)
        count = write(STDOUT_FILENO, data, size);
    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            atomic_store(&log_closed, true);
        *sent = 0;
        return false;
    }
    *sent = (size_t)count;
    return true;
}

static int emit(const char *format, ...)
{
    char output[1024];
    va_list arguments;
    va_start(arguments, format);
    int size = vsnprintf(output, sizeof(output), format, arguments);
    va_end(arguments);
    if (size < 0 || (size_t)size >= sizeof(output))
        return -1;
    if (atomic_load(&log_closed))
        return size;
    size_t sent = 0;
    /* End a line cut short by a full send buffer before starting the next. */
    if (atomic_load(&log_line_open)) {
        if (!log_send("\n", 1, &sent) || sent != 1)
            return size;
        atomic_store(&log_line_open, false);
    }
    if (log_send(output, (size_t)size, &sent) && sent > 0 && sent < (size_t)size)
        atomic_store(&log_line_open, true);
    return size;
}

static volatile sig_atomic_t interrupted;

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

/* RUSAGE_SELF is the only process CPU source on this firmware that does not
 * simply track wall time. CLOCK_PROCESS_CPUTIME_ID is emitted alongside it so
 * the known PS5 discrepancy remains visible instead of hidden. */
static bool rusage_cpu_us(uint64_t *cpu_us)
{
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return false;
    *cpu_us = (uint64_t)usage.ru_utime.tv_sec * 1000000 + (uint64_t)usage.ru_utime.tv_usec +
              (uint64_t)usage.ru_stime.tv_sec * 1000000 + (uint64_t)usage.ru_stime.tv_usec;
    return true;
}

static bool clock_cpu_us(uint64_t *cpu_us)
{
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) != 0 || t.tv_sec < 0)
        return false;
    *cpu_us = (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
    return true;
}

enum { CPU_MAIN = 0, CPU_INPUT = 1, CPU_MEDIA = 2, CPU_AUDIO = 3, CPU_METRICS = 4, CPU_SLOTS = 5 };

static uint64_t thread_cpu_us_now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0 || t.tv_sec < 0)
        return 0;
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}

static void publish_thread_cpu(stream_host *h, size_t slot)
{
    if (h != NULL && slot < CPU_SLOTS)
        atomic_store_explicit(&h->thread_cpu_us[slot], thread_cpu_us_now(), memory_order_relaxed);
}

static uint64_t thread_cpu_us_total(const stream_host *h)
{
    uint64_t total = 0;
    for (size_t slot = 0; slot < CPU_SLOTS; ++slot)
        total += atomic_load_explicit(&h->thread_cpu_us[slot], memory_order_relaxed);
    return total;
}

static void emit_budget(stream_host *h, uint64_t window_us, uint64_t thread_cpu_us)
{
    uint64_t rusage_us = 0, process_clock_us = 0;
    struct rusage usage;
    long peak_kib = 0;
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        peak_kib = usage.ru_maxrss;
    (void)rusage_cpu_us(&rusage_us);
    (void)clock_cpu_us(&process_clock_us);
    (void)emit("{\"event\":\"budget\",\"synthetic\":false,\"diagnostic\":true,"
               "\"window_us\":%llu,\"thread_cpu_us\":%llu,\"thread_cpu_total_us\":%llu,"
               "\"rusage_cpu_us\":%llu,\"process_clock_us\":%llu,\"peak_rss_kib\":%ld,"
               "\"clock\":\"THREAD_CPUTIME\"}\n",
               (unsigned long long)window_us, (unsigned long long)thread_cpu_us,
               (unsigned long long)thread_cpu_us_total(h), (unsigned long long)rusage_us,
               (unsigned long long)process_clock_us, peak_kib);
}

/* Console clock: platform/ps5_clock.h, CLOCK_MONOTONIC truncated to integer
 * microseconds. Resolution is rounded up; no accuracy claim is made. */
static sp5_result clock_describe(void *context, sp5_clock_description *description)
{
    (void)context;
    struct timespec resolution;
    if (clock_getres(CLOCK_MONOTONIC, &resolution) != 0)
        return sp5_failure(SP5_REASON_CLOCK);
    uint64_t ns = (uint64_t)resolution.tv_sec * 1000000000 + (uint64_t)resolution.tv_nsec;
    *description = (sp5_clock_description){.domain = SP5_PS5_CLOCK_DOMAIN,
                                           .epoch = SP5_PS5_CLOCK_EPOCH,
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
    uint64_t microseconds = 0;
    sp5_result result = sp5_ps5_clock_now(output, &microseconds);
    (void)microseconds;
    return result;
}

static void pause_step(void)
{
    struct timespec delay = {.tv_nsec = 1000000};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static sp5_result console_notify(void *context, const char *message, size_t size)
{
    console_system *system = context;
    sp5_result result = system->base.notify(system->base.context, message, size);
    /* No sourced console notification API exists. Pairing owns the web PIN page
     * and continues when only the visual notice is unavailable. */
    if (result.status != SP5_OK && result.reason == SP5_REASON_STAGE_UNAVAILABLE)
        return sp5_success();
    return result;
}

static sp5_system_ops console_system_ops(console_system *wrapper, sp5_ps5_system *system)
{
    wrapper->base = sp5_ps5_system_ops(system);
    sp5_system_ops ops = wrapper->base;
    ops.context = wrapper;
    ops.notify = console_notify;
    return ops;
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

static size_t json_escape(char *output, size_t capacity, const char *input)
{
    static const char hex[] = "0123456789abcdef";
    size_t size = 0;
    for (const unsigned char *at = (const unsigned char *)input; *at != 0; ++at) {
        if (*at == '"' || *at == '\\') {
            if (size + 2 >= capacity)
                return 0;
            output[size++] = '\\';
            output[size++] = (char)*at;
        } else if (*at < 0x20) {
            if (size + 6 >= capacity)
                return 0;
            output[size++] = '\\';
            output[size++] = 'u';
            output[size++] = '0';
            output[size++] = '0';
            output[size++] = hex[*at >> 4];
            output[size++] = hex[*at & 15];
        } else {
            if (size + 1 >= capacity)
                return 0;
            output[size++] = (char)*at;
        }
    }
    output[size] = 0;
    return size;
}

static void print_usage(FILE *stream)
{
    (void)fputs("usage: sunshineps5-stream [--bind IPV4] [--state DIR] [--http-port N]\n"
                "                          [--web-port N] [--run-seconds N] [--help]\n"
                "\n"
                "  --bind IPV4        private IPv4 to serve (default: the first private\n"
                "                     IPv4 address of an active console interface)\n"
                "  --state DIR        0700 state directory, created if missing\n"
                "                     (default /data/ps5-dev/sunshine-state)\n"
                "  --http-port N      Moonlight plain HTTP port (default 47989)\n"
                "  --web-port N       core web app/status port (default 8081)\n"
                "  --run-seconds N    exit after N seconds (default 0, forever; 0 = forever)\n"
                "  --assume-protection-clear\n"
                "                     opt into the unsourced diagnostic CLEAR premise\n"
                "  --help             print this text\n"
                "\n"
                "Fixed diagnostic topology: 47984 secure RTSP, 48010 secure HTTP,\n"
                "47998 video UDP, 48000 audio UDP, 47999 control ENet.\n"
                "Protection CLEAR is UNSOURCED. The reviewed diagnostic artifact is built\n"
                "with SP5_DIAGNOSTIC_PROTECTION_CLEAR=1; a source build must pass\n"
                "--assume-protection-clear or the session is refused.\n",
                stream);
}

static bool parse_port(const char *text, uint16_t *port)
{
    if (text == NULL || text[0] == 0)
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != 0 || value > 65535)
        return false;
    *port = (uint16_t)value;
    return true;
}

static bool parse_seconds(const char *text, uint64_t *seconds)
{
    if (text == NULL || text[0] == 0 || text[0] == '-')
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != 0 || value > UINT64_MAX / 1000000)
        return false;
    *seconds = (uint64_t)value;
    return true;
}

/* The payload loader passes no arguments, so the default bind address is the
 * first private IPv4 address of an up, non-loopback console interface. */
static const char *detect_bind_address(void)
{
    static char detected[INET_ADDRSTRLEN];
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0)
        return NULL;
    const char *found = NULL;
    for (struct ifaddrs *at = interfaces; at != NULL && found == NULL; at = at->ifa_next) {
        if (at->ifa_addr == NULL || at->ifa_addr->sa_family != AF_INET ||
            (at->ifa_flags & IFF_UP) == 0 || (at->ifa_flags & IFF_LOOPBACK) != 0)
            continue;
        const struct sockaddr_in *address = (const struct sockaddr_in *)(void *)at->ifa_addr;
        if (inet_ntop(AF_INET, &address->sin_addr, detected, sizeof(detected)) != NULL &&
            sp5_peer_permitted(detected, false))
            found = detected;
    }
    freeifaddrs(interfaces);
    return found;
}

/* True while an up interface still carries the served IPv4 address. */
static bool address_assigned(const char *address)
{
    struct in_addr wanted;
    struct ifaddrs *interfaces = NULL;
    if (inet_pton(AF_INET, address, &wanted) != 1 || getifaddrs(&interfaces) != 0)
        return true; /* unknown: keep serving rather than rebind on a failed query */
    bool assigned = false;
    for (struct ifaddrs *at = interfaces; at != NULL && !assigned; at = at->ifa_next)
        assigned =
            at->ifa_addr != NULL && at->ifa_addr->sa_family == AF_INET &&
            (at->ifa_flags & IFF_UP) != 0 &&
            ((const struct sockaddr_in *)(void *)at->ifa_addr)->sin_addr.s_addr == wanted.s_addr;
    freeifaddrs(interfaces);
    return assigned;
}

/* Creates missing components of an absolute state path as 0700 directories so
 * a first run on a fresh console works. Existing components are left as-is;
 * the storage adapter still enforces ownership and 0700 on the leaf. */
static bool ensure_state_directory(const char *path)
{
    char partial[PATH_MAX];
    size_t length = strlen(path);
    if (path[0] != '/' || length >= sizeof(partial))
        return false;
    memcpy(partial, path, length + 1);
    for (size_t at = 1; at <= length; ++at) {
        if (partial[at] != '/' && partial[at] != 0)
            continue;
        char saved = partial[at];
        partial[at] = 0;
        if (mkdir(partial, 0700) != 0 && errno != EEXIST)
            return false;
        partial[at] = saved;
    }
    return true;
}

static int parse_options(int argc, char **argv, stream_options *options, bool *help)
{
    *options = (stream_options){.bind_address = NULL,
                                .state_directory = "/data/ps5-dev/sunshine-state",
                                .http_port = HTTP_PORT_DEFAULT,
                                .web_port = WEB_PORT_DEFAULT,
                                .run_seconds = RUN_SECONDS_DEFAULT,
                                .assume_protection_clear = SP5_DIAGNOSTIC_PROTECTION_CLEAR != 0};
    *help = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            *help = true;
        } else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            options->bind_address = argv[++i];
        } else if (strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            options->state_directory = argv[++i];
        } else if (strcmp(argv[i], "--http-port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], &options->http_port))
                return 64;
        } else if (strcmp(argv[i], "--web-port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], &options->web_port))
                return 64;
        } else if (strcmp(argv[i], "--run-seconds") == 0 && i + 1 < argc) {
            if (!parse_seconds(argv[++i], &options->run_seconds))
                return 64;
        } else if (strcmp(argv[i], "--assume-protection-clear") == 0) {
            options->assume_protection_clear = true;
        } else {
            return 64;
        }
    }
    return options->http_port == 0 || options->web_port == 0 || options->http_port == RTSP_PORT ||
                   options->http_port == SECURE_HTTP_PORT || options->web_port == RTSP_PORT ||
                   options->web_port == SECURE_HTTP_PORT || options->web_port == options->http_port
               ? 64
               : 0;
}

static bool learn_ping(stream_host *h, int fd, struct sockaddr_in *destination)
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

/* A full interface queue (ENOBUFS) or socket buffer (EAGAIN) during a burst
 * of large frames is transient: back off briefly and retry. Returns 0 or the
 * errno of a send that still failed after SEND_RETRY_US. */
static int send_packet(int fd, const struct sockaddr_in *destination, sp5_bytes packet)
{
    uint64_t start = now_us();
    for (;;) {
        ssize_t sent = sendto(fd, packet.data, packet.size, 0, (const struct sockaddr *)destination,
                              sizeof(*destination));
        if (sent >= 0)
            return (size_t)sent == packet.size ? 0 : EMSGSIZE;
        int error = errno;
        if ((error != ENOBUFS && error != EAGAIN && error != EWOULDBLOCK && error != EINTR) ||
            now_us() - start >= SEND_RETRY_US)
            return error;
        struct timespec delay = {.tv_nsec = 200000};
        (void)nanosleep(&delay, NULL);
    }
}

/* Every media-owner failure that ends the session says where and why. */
static int media_fail(const char *stage, int reason)
{
    (void)emit("{\"event\":\"media_error\",\"synthetic\":false,\"diagnostic\":true,"
               "\"stage\":\"%s\",\"reason\":%d}\n",
               stage, reason);
    return 0;
}

/* A datagram that could not be sent loses its frame or audio packet; the
 * client recovers (an IDR request for video), so the session continues. */
static void note_send_drop(stream_host *h, bool video, int error)
{
    uint64_t now = now_us();
    ++h->send_drops;
    if (now - h->send_drop_logged_us < SEND_DROP_LOG_INTERVAL_US)
        return;
    h->send_drop_logged_us = now;
    (void)emit("{\"event\":\"send_drop\",\"synthetic\":false,\"diagnostic\":true,"
               "\"video\":%s,\"errno\":%d,\"drops\":%llu}\n",
               video ? "true" : "false", error, (unsigned long long)h->send_drops);
}

static uint32_t p99_u32(const uint32_t *values, size_t count)
{
    uint32_t sorted[64];
    if (count == 0 || count > sizeof(sorted) / sizeof(sorted[0]))
        return 0;
    memcpy(sorted, values, count * sizeof(sorted[0]));
    for (size_t i = 1; i < count; ++i) {
        uint32_t value = sorted[i];
        size_t j = i;
        while (j > 0 && sorted[j - 1] > value) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = value;
    }
    size_t rank = (count * 99 + 99) / 100;
    if (rank == 0)
        rank = 1;
    return sorted[rank - 1];
}

/* The console's video source changed under the open Avcap2 stream. Reopen
 * it every SOURCE_RETRY_US until a read succeeds; the client keeps its last
 * picture and the reopened stream starts with an IDR. Ends the session only
 * if the source stays unavailable for SOURCE_RECOVERY_US. */
static int recover_source(stream_host *h, uint64_t now, sp5_reason reason)
{
    if (!h->source_recovering) {
        h->source_recovering = true;
        h->source_lost_us = now;
        h->source_retry_us = now;
        h->source_attempts = 0;
        (void)emit("{\"event\":\"source_lost\",\"synthetic\":false,\"diagnostic\":true,"
                   "\"reason\":%d,\"read_status\":\"0x%08x\",\"read_size\":%u}\n",
                   (int)reason, (unsigned)h->capture.last_read_status,
                   (unsigned)h->capture.last_read_size);
    }
    if (now - h->source_lost_us >= SOURCE_RECOVERY_US)
        return media_fail("video_source", (int)reason);
    if (now < h->source_retry_us)
        return 2;
    h->source_retry_us = now + SOURCE_RETRY_US;
    ++h->source_attempts;
    atomic_store(&h->video_stage, STAGE_VIDEO_RESTART);
    sp5_result restarted = sp5_ps5_capture_restart(&h->capture);
    atomic_store(&h->video_stage, STAGE_IDLE);
    if (restarted.status == SP5_OK) {
        h->keyframe_restart_us = now;
        h->keyframe_wait = 1;
    }
    return 2;
}

/* Returns 1 after sending a record, 2 when no record was available (acquire
 * WOULD_BLOCK/TIMEOUT/COVERAGE_GAP; retried after a short pause), and 0 to end
 * the media loop with an error. send_audio follows the same convention. */
static int send_video(stream_host *h, const struct sockaddr_in *destination)
{
    sp5_timestamp begin;
    uint64_t begin_us = 0;
    if (sp5_ps5_clock_now(&begin, &begin_us).status != SP5_OK)
        return 0;
    sp5_timestamp deadline = begin;
    if (deadline.ticks > UINT64_MAX - VIDEO_DEADLINE_US)
        return 0;
    deadline.ticks += VIDEO_DEADLINE_US;
    /* A client that lost a frame shows nothing until the next IDR, so a
     * keyframe request restarts the encoder stream (at most once a second;
     * further requests while it recovers are absorbed). */
    if (atomic_load_explicit(&h->keyframe_requested, memory_order_acquire) &&
        begin_us - h->keyframe_restart_us >= KEYFRAME_RESTART_INTERVAL_US) {
        atomic_store_explicit(&h->keyframe_requested, false, memory_order_release);
        h->keyframe_restart_us = begin_us;
        atomic_store(&h->video_stage, STAGE_VIDEO_RESTART);
        sp5_result restarted = sp5_ps5_capture_restart(&h->capture);
        atomic_store(&h->video_stage, STAGE_IDLE);
        (void)emit("{\"event\":\"keyframe_restart\",\"synthetic\":false,\"diagnostic\":true,"
                   "\"ok\":%s,\"reason\":%d,\"restart_us\":%llu}\n",
                   restarted.status == SP5_OK ? "true" : "false", (int)restarted.reason,
                   (unsigned long long)(now_us() - begin_us));
        if (restarted.status != SP5_OK)
            return recover_source(h, begin_us, restarted.reason);
        h->keyframe_wait = 1;
    }
    sp5_video_frame frame;
    atomic_store(&h->video_stage, STAGE_VIDEO_READ);
    sp5_result acquired =
        sp5_ps5_capture_operations(&h->capture).acquire(&h->capture, deadline, false, &frame);
    atomic_store(&h->video_stage, STAGE_IDLE);
    if (acquired.status != SP5_OK) {
        if (acquired.reason == SP5_REASON_WOULD_BLOCK || acquired.reason == SP5_REASON_TIMEOUT ||
            acquired.reason == SP5_REASON_COVERAGE_GAP)
            return 2;
        if (acquired.reason == SP5_REASON_PLATFORM ||
            (h->source_recovering && acquired.reason == SP5_REASON_OWNERSHIP))
            return recover_source(h, begin_us, acquired.reason);
        return media_fail("video_acquire", (int)acquired.reason);
    }
    uint64_t acquired_us = now_us();
    if (h->source_recovering) {
        h->source_recovering = false;
        (void)emit("{\"event\":\"source_recovered\",\"synthetic\":false,\"diagnostic\":true,"
                   "\"after_us\":%llu,\"attempts\":%u}\n",
                   (unsigned long long)(acquired_us - h->source_lost_us), h->source_attempts);
    }
    h->video_buffer.size = 0;
    sp5_encoded_frame encoded;
    sp5_result encoded_result =
        sp5_ps5_encoder_operations(&h->encoder)
            .encode(&h->encoder, &frame, false, deadline, false, &h->video_buffer, &encoded);
    sp5_result released =
        sp5_ps5_capture_operations(&h->capture).release(&h->capture, frame.lease_id);
    if (encoded_result.status != SP5_OK)
        return media_fail("video_encode", (int)encoded_result.reason);
    if (released.status != SP5_OK)
        return media_fail("video_release", (int)released.reason);
    uint64_t encoded_us = now_us();
    /* A record queued before the restart can still arrive first; if no IDR
     * follows within KEYFRAME_WAIT_RECORDS, ask for another restart. */
    if (h->keyframe_wait != 0) {
        if (encoded.key_frame) {
            (void)emit("{\"event\":\"keyframe_first\",\"synthetic\":false,\"diagnostic\":true,"
                       "\"records\":%u,\"after_restart_us\":%llu}\n",
                       h->keyframe_wait, (unsigned long long)(encoded_us - h->keyframe_restart_us));
            h->keyframe_wait = 0;
        } else if (++h->keyframe_wait > KEYFRAME_WAIT_RECORDS) {
            (void)emit("{\"event\":\"keyframe_missing\",\"synthetic\":false,"
                       "\"diagnostic\":true,\"records\":%u}\n",
                       KEYFRAME_WAIT_RECORDS);
            h->keyframe_wait = 0;
            atomic_store_explicit(&h->keyframe_requested, true, memory_order_release);
        }
    }
    /* Acquire blocks until the encoder emits the next record, so processing
     * time starts when the record arrives, not when the wait began. */
    uint64_t elapsed = encoded_us >= acquired_us ? encoded_us - acquired_us : 0;
    /* Official Moonlight waits for an IDR with in-band AVC SPS/PPS; the Avcap2
     * read record carries slices only, so prepend the parsed configuration
     * header to every key frame. Non-key frames are never modified. */
    const uint8_t *parameters = NULL;
    size_t parameter_size = 0;
    if (encoded.key_frame &&
        sp5_ps5_capture_parameters(&h->capture, &parameters, &parameter_size)) {
        sp5_buffer *buffer = &h->video_buffer;
        if (buffer->data == NULL || parameter_size > buffer->capacity ||
            buffer->size > buffer->capacity - parameter_size)
            return media_fail("video_parameters", (int)SP5_REASON_CAPACITY);
        memmove(buffer->data + parameter_size, buffer->data, buffer->size);
        memcpy(buffer->data, parameters, parameter_size);
        buffer->size += parameter_size;
    }
    sp5_video_frame_input input = {.bitstream = {h->video_buffer.data, h->video_buffer.size},
                                   .frame_index = (uint32_t)frame.frame_id,
                                   .rtp_timestamp =
                                       (uint32_t)(frame.frame_id * h->rtp_ticks_per_frame),
                                   .key_frame = encoded.key_frame,
                                   .processing_us = {true, elapsed}};
    sp5_result begun = sp5_video_begin(h->video, &input);
    if (begun.status != SP5_OK)
        return media_fail("video_begin", (int)begun.reason);
    uint8_t wire[SP5_VIDEO_WIRE_LIMIT];
    sp5_buffer output = {wire, sizeof(wire), 0};
    sp5_result result;
    while ((result = sp5_video_next(h->video, &output)).status == SP5_OK) {
        if (atomic_load_explicit(&h->stopped, memory_order_acquire)) {
            sp5_video_cancel(h->video);
            return 1;
        }
        int error =
            send_packet(h->video_socket, destination, (sp5_bytes){output.data, output.size});
        if (error != 0) {
            sp5_video_cancel(h->video);
            note_send_drop(h, true, error);
            return 1;
        }
    }
    if (result.reason != SP5_REASON_WOULD_BLOCK)
        return media_fail("video_packetize", (int)result.reason);
    uint64_t sent_us = now_us();
    uint32_t acquire_us = acquired_us >= begin_us ? (uint32_t)(acquired_us - begin_us) : 0;
    uint32_t encode_us = encoded_us >= acquired_us ? (uint32_t)(encoded_us - acquired_us) : 0;
    uint32_t send_us = sent_us >= encoded_us ? (uint32_t)(sent_us - encoded_us) : 0;
    h->video_acquire_us += acquire_us;
    h->video_encode_us += encode_us;
    h->video_send_us += send_us;
    size_t index = h->video_timing_frames;
    h->video_sample_acquire[index] = acquire_us;
    h->video_sample_encode[index] = encode_us;
    h->video_sample_send[index] = send_us;
    h->video_sample_pipeline[index] = encode_us + send_us; /* excludes the wait for the record */
    if (encoded.key_frame)
        ++h->video_key_frames;
    if (++h->video_timing_frames >= 60) {
        uint64_t count = h->video_timing_frames;
        (void)emit("{\"event\":\"video_timing\",\"synthetic\":false,\"diagnostic\":true,"
                   "\"frames\":%llu,\"acquire_us\":%llu,\"encode_us\":%llu,\"send_us\":%llu,"
                   "\"acquire_p99_us\":%u,\"encode_p99_us\":%u,\"send_p99_us\":%u,"
                   "\"pipeline_p99_us\":%u,\"key_frames\":%u}\n",
                   (unsigned long long)count, (unsigned long long)(h->video_acquire_us / count),
                   (unsigned long long)(h->video_encode_us / count),
                   (unsigned long long)(h->video_send_us / count),
                   p99_u32(h->video_sample_acquire, (size_t)count),
                   p99_u32(h->video_sample_encode, (size_t)count),
                   p99_u32(h->video_sample_send, (size_t)count),
                   p99_u32(h->video_sample_pipeline, (size_t)count), h->video_key_frames);
        h->video_acquire_us = h->video_encode_us = h->video_send_us = 0;
        h->video_timing_frames = h->video_key_frames = 0;
    }
    atomic_fetch_add_explicit(&h->video_count, 1, memory_order_relaxed);
    return 1;
}

static int send_audio(stream_host *h, const struct sockaddr_in *destination)
{
    sp5_timestamp deadline = {0};
    if (sp5_ps5_audio_now(&deadline).status != SP5_OK ||
        deadline.ticks > UINT64_MAX - AUDIO_DEADLINE_US)
        return 0;
    deadline.ticks += AUDIO_DEADLINE_US;
    sp5_audio_block block;
    atomic_store(&h->audio_stage, STAGE_AUDIO_READ);
    sp5_result acquired =
        sp5_ps5_audio_ops(&h->audio_adapter).acquire(&h->audio_adapter, deadline, &block);
    atomic_store(&h->audio_stage, STAGE_IDLE);
    if (acquired.status != SP5_OK) {
        return acquired.reason == SP5_REASON_WOULD_BLOCK || acquired.reason == SP5_REASON_TIMEOUT
                   ? 2
                   : media_fail("audio_acquire", (int)acquired.reason);
    }
    size_t frames = block.samples.size / (2 * sizeof(float));
    if (frames > DIAGNOSTIC_PCM_FRAMES - h->audio_pcm_frames)
        frames = DIAGNOSTIC_PCM_FRAMES - h->audio_pcm_frames;
    if (frames != 0)
        memcpy(h->audio_pcm + h->audio_pcm_frames * 2, block.samples.data,
               frames * 2 * sizeof(float));
    h->audio_pcm_frames += frames;
    sp5_result released =
        sp5_ps5_audio_ops(&h->audio_adapter).release(&h->audio_adapter, block.lease_id);
    if (released.status != SP5_OK)
        return media_fail("audio_release", (int)released.reason);
    uint32_t frame_samples = sp5_opus_encoder_frame_samples(h->opus);
    if (frame_samples == 0)
        return 0;
    while (h->audio_pcm_frames >= frame_samples) {
        sp5_buffer encoded = {h->audio_payload, h->opus_size, 0};
        if (sp5_opus_encoder_encode_float(h->opus, h->audio_pcm, h->opus_size, &encoded).status !=
                SP5_OK ||
            encoded.size > h->opus_size)
            return media_fail("audio_encode", (int)SP5_REASON_PLATFORM);
        if (encoded.size < h->opus_size)
            memset(h->audio_payload + encoded.size, 0, h->opus_size - encoded.size);
        sp5_result submitted =
            sp5_audio_submit(h->audio, (sp5_bytes){h->audio_payload, h->opus_size});
        if (submitted.status != SP5_OK)
            return media_fail("audio_submit", (int)submitted.reason);
        uint8_t wire[SP5_AUDIO_WIRE_LIMIT];
        sp5_buffer output = {wire, sizeof(wire), 0};
        sp5_result result;
        bool dropped = false; /* the packet must still be drained before the next submit */
        while ((result = sp5_audio_next(h->audio, &output)).status == SP5_OK) {
            if (atomic_load_explicit(&h->stopped, memory_order_acquire))
                return 1;
            int error = dropped ? 0
                                : send_packet(h->audio_socket, destination,
                                              (sp5_bytes){output.data, output.size});
            if (error != 0) {
                dropped = true;
                note_send_drop(h, false, error);
            }
        }
        if (result.reason != SP5_REASON_WOULD_BLOCK)
            return media_fail("audio_packetize", (int)result.reason);
        atomic_fetch_add_explicit(&h->audio_count, 1, memory_order_relaxed);
        memmove(h->audio_pcm, h->audio_pcm + frame_samples * 2,
                (h->audio_pcm_frames - frame_samples) * 2 * sizeof(float));
        h->audio_pcm_frames -= frame_samples;
    }
    return 1;
}

/* Video is paced by the console encoder: each encoded record is forwarded as
 * soon as Avcap2 returns it, and the thread backs off ~1 ms only while the
 * record queue is empty. A fixed host-side period ran slightly slower than
 * the source (sleep overshoot), so the queue grew until records were lost. */
static void *video_owner(void *context)
{
    stream_host *h = context;
    struct sockaddr_in destination = {0};
    bool known = false;
    while (!atomic_load_explicit(&h->stopped, memory_order_acquire)) {
        publish_thread_cpu(h, CPU_MEDIA);
        if (learn_ping(h, h->video_socket, &destination))
            known = true;
        if (!known) {
            pause_step();
            continue;
        }
        int sent = send_video(h, &destination);
        if (sent == 0)
            break;
        if (sent == 2)
            pause_step();
    }
    if (!atomic_load_explicit(&h->stopped, memory_order_acquire))
        atomic_store_explicit(&h->end_request, SP5_END_ERROR, memory_order_release);
    publish_thread_cpu(h, CPU_MEDIA);
    atomic_store_explicit(&h->media_done, true, memory_order_release);
    return NULL;
}

/* Audio is paced by the source: the adapter's acquire polls (1 ms steps, up
 * to the deadline) until the next ~21.3 ms block is available, so blocks are
 * drained as they arrive instead of on a host period that drifts behind. */
static void *audio_owner(void *context)
{
    stream_host *h = context;
    struct sockaddr_in destination = {0};
    bool known = false;
    while (!atomic_load_explicit(&h->stopped, memory_order_acquire)) {
        publish_thread_cpu(h, CPU_AUDIO);
        if (learn_ping(h, h->audio_socket, &destination))
            known = true;
        if (!known) {
            pause_step();
            continue;
        }
        int sent = send_audio(h, &destination);
        if (sent == 0)
            break;
        if (sent == 2)
            pause_step();
    }
    if (!atomic_load_explicit(&h->stopped, memory_order_acquire))
        atomic_store_explicit(&h->end_request, SP5_END_ERROR, memory_order_release);
    publish_thread_cpu(h, CPU_AUDIO);
    atomic_store_explicit(&h->audio_done, true, memory_order_release);
    return NULL;
}

static sp5_result input_end(void *context, sp5_end_reason reason)
{
    stream_host *h = context;
    sp5_result result = sp5_lifecycle_stop_input(h->lifecycle, reason);
    atomic_store_explicit(&h->stopped, true, memory_order_release);
    return result;
}

static void *input_owner(void *context)
{
    stream_host *h = context;
    uint64_t event_id = 0;
    while (!atomic_load_explicit(&h->stopped, memory_order_acquire)) {
        publish_thread_cpu(h, CPU_INPUT);
        int requested = atomic_load_explicit(&h->end_request, memory_order_acquire);
        if (requested >= 0) {
            h->worker_error = sp5_control_end(h->control, (sp5_end_reason)requested).reason;
            break;
        }
        sp5_result step = sp5_control_transport_step(h->transport, now_us());
        if (step.status != SP5_OK && !atomic_load_explicit(&h->stopped, memory_order_acquire)) {
            h->worker_error = step.reason;
            (void)media_fail("control_step", (int)step.reason);
            (void)sp5_control_end(h->control, SP5_END_ERROR);
            break;
        }
        sp5_control_event event;
        unsigned drained = 0;
        while (drained++ < SP5_CONTROL_QUEUE_CAPACITY &&
               sp5_control_take(h->control, &event).status == SP5_OK) {
            if (event.type == 0x0302 || event.type == 0x0301) {
                atomic_store_explicit(&h->keyframe_requested, true, memory_order_release);
                (void)emit("{\"event\":\"keyframe_request\",\"synthetic\":false,"
                           "\"diagnostic\":true,\"type\":%u}\n",
                           event.type);
                continue;
            }
            if (event.type == 0x0206) {
                sp5_timestamp received = {event.received_us, 1, 1, SP5_VALIDITY_VALID,
                                          SP5_REASON_NONE};
                sp5_input_evidence evidence;
                sp5_result result =
                    sp5_lifecycle_input(h->lifecycle, &event, received, ++event_id, &evidence);
                if (result.status != SP5_OK) {
                    /* One undeliverable report must not end the stream; only a
                     * lost session/controller ownership does. */
                    (void)emit("{\"event\":\"input_error\",\"synthetic\":false,"
                               "\"diagnostic\":true,\"event_id\":%llu,\"reason\":%d}\n",
                               (unsigned long long)event_id, (int)result.reason);
                    if (result.reason != SP5_REASON_OWNERSHIP)
                        continue;
                    h->worker_error = result.reason;
                    (void)sp5_control_end(h->control, SP5_END_ERROR);
                    break;
                }
                (void)emit("{\"event\":\"input\",\"synthetic\":false,\"diagnostic\":true,"
                           "\"event_id\":%llu,\"type\":%u,\"controller\":%u,"
                           "\"attempted\":%s,\"delivery\":%u,\"reason\":%u}\n",
                           (unsigned long long)event_id, event.type, evidence.controller_id,
                           evidence.delivery_attempted ? "true" : "false",
                           (unsigned)evidence.delivery, (unsigned)evidence.reason);
                if (h->client_session_id[0] != 0 && !h->host_observations_overflow) {
                    uint64_t observed = evidence.host_received.ticks;
                    int written = snprintf(
                        h->host_observations + h->host_observations_size,
                        sizeof(h->host_observations) - h->host_observations_size,
                        "{\"schema_version\":\"1.1.0\",\"id\":\"input-%llu\","
                        "\"session_id\":\"%s\",\"provenance\":\"console_hardware\","
                        "\"source\":\"host_pipeline\",\"kind\":\"input\","
                        "\"event_id\":\"%llu\",\"controller_id\":%u,"
                        "\"boundary\":\"host_received\",\"authenticated\":true,"
                        "\"clock_domain\":\"host-monotonic\",\"source_clock\":{"
                        "\"id\":\"host-monotonic\",\"type\":\"monotonic\","
                        "\"epoch\":\"session\",\"native_resolution_ns\":1000.0,"
                        "\"uncertainty_ns\":null,\"uncertainty_reason\":\"clock resolution is not "
                        "calibrated accuracy\","
                        "\"conversion\":\"CLOCK_MONOTONIC microseconds\","
                        "\"quantization_bound_ns\":1000.0,\"validity\":\"valid\",\"reason\":null},"
                        "\"observed_at\":%llu,\"population\":{\"expected\":null,"
                        "\"observed\":1,\"represented\":1,\"excluded\":0,\"missing\":0,"
                        "\"overflow\":0,\"failed_delivery\":0},\"coverage\":{"
                        "\"state\":\"complete\",\"enabled_intervals\":[{\"start\":0,"
                        "\"end\":%llu}],\"gaps\":[],\"reason\":null},"
                        "\"validity\":\"valid\",\"reason\":null,\"artifact_refs\":[],"
                        "\"measurements\":[]}\n",
                        (unsigned long long)event_id, h->client_session_id,
                        (unsigned long long)event_id, evidence.controller_id,
                        (unsigned long long)observed, (unsigned long long)observed);
                    if (written < 0 ||
                        (size_t)written >= sizeof(h->host_observations) - h->host_observations_size)
                        h->host_observations_overflow = true;
                    else
                        h->host_observations_size += (size_t)written;
                }
                atomic_fetch_add_explicit(&h->input_count, 1, memory_order_relaxed);
            }
        }
        pause_step();
    }
    publish_thread_cpu(h, CPU_INPUT);
    atomic_store_explicit(&h->stopped, true, memory_order_release);
    return NULL;
}

static void *metrics_owner(void *context)
{
    stream_host *h = context;
    uint64_t next = 0;
    while (!atomic_load_explicit(&h->stopped, memory_order_acquire)) {
        publish_thread_cpu(h, CPU_METRICS);
        uint64_t now = now_us();
        if (now >= next) {
            (void)sp5_metrics_worker_step(sp5_lifecycle_metrics(h->lifecycle), now, NULL);
            next = now + 500000;
        }
        pause_step();
    }
    publish_thread_cpu(h, CPU_METRICS);
    atomic_store_explicit(&h->metrics_done, true, memory_order_release);
    return NULL;
}

static int clean_session(stream_host *h);

static int session_fail(stream_host *h, const char *stage, sp5_result result)
{
    (void)emit("{\"event\":\"session_error\",\"synthetic\":false,\"diagnostic\":true,"
               "\"stage\":\"%s\",\"status\":%d,\"reason\":%d}\n",
               stage, result.status, (int)result.reason);
    (void)clean_session(h);
    return 1;
}

static int start_session(stream_host *h)
{
    atomic_store(&h->input_count, 0);
    atomic_store(&h->video_count, 0);
    atomic_store(&h->audio_count, 0);
    sp5_session_view view = sp5_session_read(h->session);
    sp5_result negotiated = sp5_rtsp_negotiated(h->rtsp, &h->negotiated);
    if (negotiated.status != SP5_OK)
        return session_fail(h, "rtsp_negotiated", negotiated);
    if (h->negotiated.mode_index >= h->host.supported_mode_count)
        return session_fail(h, "mode", sp5_failure(SP5_REASON_UNSUPPORTED));
    if (h->negotiated.audio_duration_ms != 5 && h->negotiated.audio_duration_ms != 10)
        return session_fail(h, "audio_duration", sp5_failure(SP5_REASON_UNSUPPORTED));
    sp5_lifecycle_ops ops = {sp5_ps5_capture_operations(&h->capture),
                             sp5_ps5_encoder_operations(&h->encoder),
                             sp5_ps5_audio_ops(&h->audio_adapter),
                             sp5_ps5_input_ops(&h->input_adapter), sp5_ps5_system_ops(&h->system)};
    h->lifecycle = sp5_lifecycle_create(view.id, &ops);
    h->lifecycle_started_us = now_us();
    if (h->lifecycle == NULL)
        return session_fail(h, "lifecycle_create", sp5_failure(SP5_REASON_CAPACITY));
    sp5_lifecycle_policy policy = {.mode_count = h->host.supported_mode_count,
                                   .media_bytes = (size_t)64 * 1024 * 1024,
                                   .core_bytes = (size_t)32 * 1024 * 1024,
                                   .streaming_cpu_us_per_10s = 10000000,
                                   .idle_cpu_us_per_10s = 500000,
                                   .validity = SP5_VALIDITY_VALID,
                                   .synthetic = false,
                                   .evidence_ref = "diagnostic:--assume-protection-clear",
                                   .clock = {NULL, clock_describe, clock_now},
                                   .metrics_capacity = {1024, 4096, 256},
                                   .metrics_enabled = false};
    memcpy(policy.modes, h->host.supported_modes,
           h->host.supported_mode_count * sizeof(policy.modes[0]));
    policy.metrics_context.synthetic = false;
    (void)snprintf(policy.metrics_context.host_version, sizeof(policy.metrics_context.host_version),
                   SP5_HOST_VERSION);
    (void)snprintf(policy.metrics_context.firmware_version,
                   sizeof(policy.metrics_context.firmware_version), "13.60");
    (void)snprintf(policy.metrics_context.client_name, sizeof(policy.metrics_context.client_name),
                   "diagnostic-client");
    policy.metrics_context.requested_bitrate_kbps = view.requested_bitrate_kbps;
    sp5_result configured = sp5_lifecycle_configure(h->lifecycle, &policy);
    if (configured.status != SP5_OK)
        return session_fail(h, "lifecycle_configure", configured);
    sp5_result bound = sp5_lifecycle_bind(h->lifecycle, h->session, h->client_certificate);
    if (bound.status != SP5_OK)
        return session_fail(h, "lifecycle_bind", bound);
    uint32_t bitrate = view.chosen_bitrate_kbps;
    const sp5_host_mode *mode = &h->host.supported_modes[h->negotiated.mode_index];
    if (bitrate < mode->minimum_bitrate_kbps)
        bitrate = mode->minimum_bitrate_kbps;
    if (bitrate > mode->maximum_bitrate_kbps)
        bitrate = mode->maximum_bitrate_kbps;
    h->rtp_ticks_per_frame = 90000 / mode->fps;
    (void)snprintf(h->mode_text, sizeof(h->mode_text), "%ux%u@%u %s SDR", (unsigned)mode->width,
                   (unsigned)mode->height, (unsigned)mode->fps, mode->codec);
    sp5_media_config config = {mode->width, mode->height,        mode->fps, 1,
                               mode->codec, mode->dynamic_range, bitrate};
    sp5_result opened = sp5_lifecycle_start(h->lifecycle, &config, now_us());
    if (opened.status != SP5_OK)
        return session_fail(h, "lifecycle_start", opened);
    /* UNSOURCED diagnostic override: no Avcap2 protection getter exists. The
     * diagnostic artifact is built with SP5_DIAGNOSTIC_PROTECTION_CLEAR=1; a
     * source build without the flag refuses to stream. */
    if (!h->options.assume_protection_clear)
        return session_fail(h, "protection_required", sp5_failure(SP5_REASON_UNVERIFIED));
    sp5_result protection = sp5_ps5_capture_observe_protection(
        &h->capture, SP5_PROTECTION_CLEAR, "diagnostic:--assume-protection-clear");
    if (protection.status == SP5_OK) {
        atomic_store_explicit(&h->protection_assumed, true, memory_order_release);
        h->protection_clear_used = true;
    }
    sp5_source_state source;
    sp5_result source_result =
        sp5_ps5_capture_operations(&h->capture).source_state(&h->capture, &source);
    sp5_result audio_source = source_result.status == SP5_OK
                                  ? sp5_ps5_audio_observe_source(&h->audio_adapter, &source)
                                  : source_result;
    if (protection.status != SP5_OK)
        return session_fail(h, "capture_protection", protection);
    if (audio_source.status != SP5_OK)
        return session_fail(h, "audio_source", audio_source);
    sp5_result media =
        sp5_lifecycle_media_buffer(h->lifecycle, 0, &h->video_buffer, SP5_PS5_CAPTURE_READ_BYTES);
    if (media.status != SP5_OK)
        return session_fail(h, "media_buffer", media);
    sp5_video_config vc = {.packet_size = (uint16_t)h->negotiated.packet_size,
                           .fec_percent = 20,
                           .minimum_parity = 2,
                           .encrypted = (h->negotiated.encryption_flags & 2) != 0};
    memcpy(vc.key, h->launch.key, sizeof(vc.key));
    h->video = sp5_video_create(&vc);
    uint32_t duration_ms = h->negotiated.audio_duration_ms;
    h->opus_size = (DIAGNOSTIC_OPUS_BITRATE / 8) * duration_ms / 1000;
    if (h->opus_size == 0 || h->opus_size > sizeof(h->audio_payload))
        return session_fail(h, "opus_size", sp5_failure(SP5_REASON_UNSUPPORTED));
    sp5_result opus_result;
    h->opus = sp5_opus_encoder_create(48000, 2, DIAGNOSTIC_OPUS_BITRATE, duration_ms, &opus_result);
    if (h->opus == NULL)
        return session_fail(h, "opus_create", opus_result);
    sp5_audio_config ac = {.sample_rate = 48000,
                           .channels = 2,
                           .duration_ms = (uint8_t)duration_ms,
                           .opus_size = (uint16_t)h->opus_size,
                           .key_id = h->launch.key_id,
                           .encrypted = (h->negotiated.encryption_flags & 4) != 0};
    memcpy(ac.key, h->launch.key, sizeof(ac.key));
    h->audio = sp5_audio_create(&ac);
    if (h->video == NULL || h->audio == NULL)
        return session_fail(h, "packetizer_create", sp5_failure(SP5_REASON_CAPACITY));
    sp5_result activated = sp5_session_activate(h->session);
    if (activated.status != SP5_OK)
        return session_fail(h, "session_activate", activated);
    sp5_control_config cc = {.session = h->session,
                             .verified_certificate = h->client_certificate,
                             .launch = h->launch,
                             .negotiated = h->negotiated,
                             .peer = h->launch_peer,
                             .started_us = now_us(),
                             .end_context = h,
                             .release_input = input_end};
    h->control = sp5_control_create(&cc);
    if (h->control == NULL)
        return session_fail(h, "control_create", sp5_failure(SP5_REASON_CAPACITY));
    h->transport =
        sp5_control_transport_open(h->runtime, h->control, h->options.bind_address, CONTROL_PORT);
    if (h->transport == NULL)
        return session_fail(h, "control_transport", sp5_failure(SP5_REASON_PLATFORM));
    if (emit("{\"event\":\"reserved\",\"synthetic\":false,\"diagnostic\":true,\"session_id\":%llu,"
             "\"requested_kbps\":%u,\"chosen_kbps\":%u,\"mode\":\"%s\","
             "\"protection\":\"diagnostic:--assume-protection-clear\"}\n",
             (unsigned long long)view.id, view.requested_bitrate_kbps, view.chosen_bitrate_kbps,
             h->mode_text) < 0)
        return session_fail(h, "reserved_emit", sp5_failure(SP5_REASON_PLATFORM));
    return 0;
}

static int play(stream_host *h)
{
    atomic_store(&h->stopped, false);
    atomic_store(&h->media_done, false);
    atomic_store(&h->metrics_done, false);
    atomic_store(&h->audio_done, false);
    atomic_store(&h->end_request, -1);
    atomic_store(&h->keyframe_requested, false); /* the new stream starts with an IDR */
    h->keyframe_wait = 0;
    h->source_recovering = false;
    h->video_timing_frames = h->video_key_frames = 0;
    h->worker_error = SP5_REASON_NONE;
    if (pthread_create(&h->input_thread, NULL, input_owner, h) != 0) {
        atomic_store(&h->stopped, true);
        return 1;
    }
    if (pthread_create(&h->media_thread, NULL, video_owner, h) != 0) {
        atomic_store(&h->end_request, SP5_END_ERROR);
        (void)pthread_join(h->input_thread, NULL);
        atomic_store(&h->stopped, true);
        return 1;
    }
    if (pthread_create(&h->audio_thread, NULL, audio_owner, h) != 0) {
        atomic_store(&h->end_request, SP5_END_ERROR);
        (void)pthread_join(h->input_thread, NULL);
        (void)pthread_join(h->media_thread, NULL);
        atomic_store(&h->stopped, true);
        return 1;
    }
    if (pthread_create(&h->metrics_thread, NULL, metrics_owner, h) != 0) {
        atomic_store(&h->end_request, SP5_END_ERROR);
        (void)pthread_join(h->input_thread, NULL);
        (void)pthread_join(h->media_thread, NULL);
        (void)pthread_join(h->audio_thread, NULL);
        atomic_store(&h->stopped, true);
        return 1;
    }
    h->running = true;
    if (emit("{\"event\":\"streaming\",\"synthetic\":false,\"diagnostic\":true,"
             "\"session_id\":%llu,\"mode\":\"%s\",\"bitrate_kbps\":%u,"
             "\"protection\":\"diagnostic:--assume-protection-clear\"}\n",
             (unsigned long long)sp5_session_read(h->session).id, h->mode_text,
             sp5_session_read(h->session).chosen_bitrate_kbps) < 0)
        return 1;
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

static bool query_value(sp5_bytes target, const char *key, char *output, size_t capacity)
{
    size_t key_length = strlen(key);
    for (size_t i = 0; i + key_length < target.size; ++i) {
        if ((target.data[i] != '?' && target.data[i] != '&') ||
            memcmp(target.data + i + 1, key, key_length) != 0 ||
            target.data[i + 1 + key_length] != '=')
            continue;
        size_t at = i + 2 + key_length, end = at;
        while (end < target.size && target.data[end] != '&' && target.data[end] != ' ')
            ++end;
        if (end == at || end - at >= capacity)
            return false;
        memcpy(output, target.data + at, end - at);
        output[end - at] = 0;
        return true;
    }
    return false;
}

static bool console_web_route(void *context, const sp5_request *request, uint64_t now_us,
                              sp5_web_response *response)
{
    stream_host *h = context;
    (void)now_us;
    static const char path[] = "/api/v1/host-observations";
    if (request == NULL || response == NULL || h == NULL || request->method.size != 3 ||
        memcmp(request->method.data, "GET", 3) != 0 || request->target.size < sizeof(path) - 1 ||
        memcmp(request->target.data, path, sizeof(path) - 1) != 0)
        return false;
    if (h->client_session_id[0] == 0 || h->host_observations_size == 0 ||
        h->host_observations_overflow)
        return sp5_web_error(response, 404, "observations_unavailable",
                             "No session-bound host observations are retained.",
                             "Rerun a diagnostic session with a bound client session ID.", 0);
    char requested[128];
    if (!query_value(request->target, "session", requested, sizeof(requested)) ||
        strcmp(requested, h->client_session_id) != 0)
        return sp5_web_error(response, 403, "session_mismatch",
                             "Host observations belong to another session.",
                             "Request the session ID carried by the launch request.", 0);
    response->status = 200;
    response->content_type = "application/x-ndjson";
    response->body.data = (uint8_t *)h->host_observations;
    response->body.capacity = sizeof(h->host_observations);
    response->body.size = h->host_observations_size;
    return true;
}

static int service_request(stream_host *h, connection *c)
{
    sp5_request request;
    if (!c->ready) {
        if (c->tls != NULL && !SSL_is_init_finished(c->tls)) {
            int accepted = SSL_accept(c->tls);
            if (accepted != 1) {
                int error = SSL_get_error(c->tls, accepted);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
                    return 0;
                (void)emit("{\"event\":\"ssl_accept_error\",\"synthetic\":false,"
                           "\"diagnostic\":true,\"error\":%d}\n",
                           error);
                close_connection(c);
                return 0;
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
            (void)emit("{\"event\":\"read_error\",\"synthetic\":false,\"diagnostic\":true,"
                       "\"rtsp\":%s,\"count\":%d,\"ssl_error\":%d}\n",
                       c->rtsp ? "true" : "false", (int)count, error);
            close_connection(c);
            return 0;
        }
        c->used += (size_t)count;
        if (c->rtsp && h->launch.encrypted_rtsp) {
            if (c->used < 24)
                return 0;
            size_t size = (size_t)(c->input[0] & 0x7f) << 24 | (size_t)c->input[1] << 16 |
                          (size_t)c->input[2] << 8 | (size_t)c->input[3];
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
            if (c->used == sizeof(c->input)) {
                (void)emit("{\"event\":\"parse_full\",\"synthetic\":false,"
                           "\"diagnostic\":true,\"rtsp\":%s}\n",
                           c->rtsp ? "true" : "false");
                close_connection(c);
            }
            return 0;
        }
        (void)emit("{\"event\":\"request_ready\",\"synthetic\":false,\"diagnostic\":true,"
                   "\"rtsp\":%s,\"bytes\":%u}\n",
                   c->rtsp ? "true" : "false", (unsigned)c->used);
    }
    if (c->rtsp) {
        if (h->running || h->rtsp == NULL) {
            close_connection(c);
            return 0;
        }
        uint8_t wire[SP5_RTSP_LIMIT + 24];
        sp5_buffer output = {wire, sizeof(wire), 0};
        sp5_result exchanged = sp5_rtsp_exchange(
            h->rtsp, c->peer, false, (sp5_bytes){c->input, c->used}, now_us(), wall_us(), &output);
        (void)emit("{\"event\":\"rtsp_exchange\",\"synthetic\":false,\"diagnostic\":true,"
                   "\"status\":%d,\"reason\":%d}\n",
                   exchanged.status, (int)exchanged.reason);
        if (exchanged.status != SP5_OK) {
            close_connection(c);
            return 0;
        }
        sp5_session_view view = sp5_session_read(h->session);
        if (view.mode_configured && view.state == SP5_SESSION_NEGOTIATING && h->lifecycle == NULL) {
            if (start_session(h) != 0) {
                sp5_rtsp_destroy(h->rtsp);
                h->rtsp = NULL;
                close_connection(c);
                return 0;
            }
        }
        REQUIRE(write_connection(c, (sp5_bytes){output.data, output.size}));
        close_connection(c);
        if (h->lifecycle != NULL && view.state == SP5_SESSION_ACTIVE && sp5_rtsp_playing(h->rtsp)) {
            if (play(h) != 0) {
                atomic_store_explicit(&h->end_request, SP5_END_ERROR, memory_order_release);
                atomic_store_explicit(&h->stopped, true, memory_order_release);
                (void)clean_session(h);
            }
        }
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
    bool launch = request.target.size >= 7 && (memcmp(request.target.data, "/launch", 7) == 0 ||
                                               memcmp(request.target.data, "/resume", 7) == 0);
    if (!h->running && h->lifecycle != NULL && (cancel || launch))
        (void)clean_session(h); /* reclaim a session abandoned before PLAY */
    if (h->running) {
        uint8_t cert[SP5_CERTIFICATE_CAPACITY];
        sp5_bytes authenticated = {0};
        bool owns = c->secure && peer_certificate(c->tls, cert, &authenticated) &&
                    authenticated.size == h->client_certificate.size &&
                    memcmp(authenticated.data, h->client_certificate.data, authenticated.size) == 0;
        if (cancel && owns) {
            atomic_store_explicit(&h->end_request, SP5_END_CLIENT_QUIT, memory_order_release);
            return 0;
        }
        response.code = 409;
        return http_reply(c, &response);
    }
    if (launch && h->media_wedged) {
        response.code = 503;
        response.status_message = "The PS5 host is stuck after a console error. Send the payload "
                                  "to the PS5 again, then reconnect.";
        return http_reply(c, &response);
    }
    if (launch) {
        char session[128];
        if (query_value(request.target, "x-sp5-session", session, sizeof(session))) {
            memcpy(h->client_session_id, session, strlen(session) + 1);
            h->host_observations_size = 0;
            h->host_observations_overflow = false;
        }
    }
    sp5_result routed =
        sp5_http_route(h->http, c->peer, false, c->tls, &request, now_us(), wall_us(), &response);
    if (routed.status != SP5_OK) {
        (void)emit("{\"event\":\"http_route_error\",\"synthetic\":false,\"diagnostic\":true,"
                   "\"status\":%d,\"reason\":%d}\n",
                   routed.status, (int)routed.reason);
        close_connection(c);
        return 0;
    }
    if (request.target.size >= 7 && memcmp(request.target.data, "/launch", 7) == 0) {
        (void)emit("{\"event\":\"http_route\",\"synthetic\":false,\"diagnostic\":true,"
                   "\"target\":\"/launch\",\"code\":%u}\n",
                   response.code);
    } else if (request.target.size >= 7 && memcmp(request.target.data, "/resume", 7) == 0) {
        (void)emit("{\"event\":\"http_route\",\"synthetic\":false,\"diagnostic\":true,"
                   "\"target\":\"/resume\",\"code\":%u}\n",
                   response.code);
    }
    if (response.deferred) {
        c->deferred = true;
        c->request_id = sp5_pairing_read(h->pairing, now_us()).request_id;
        c->deadline_us = now_us() + SP5_PAIR_EXPIRY_US;
        REQUIRE(emit("{\"event\":\"pair_pending\",\"synthetic\":false,\"request_id\":%llu}\n",
                     (unsigned long long)c->request_id) > 0);
        return 0;
    }
    if (response.code == 200 && launch) {
        if (h->rtsp != NULL) {
            sp5_rtsp_destroy(h->rtsp);
            h->rtsp = NULL;
        }
        REQUIRE(sp5_http_launch_read(h->http, &h->launch).status == SP5_OK);
        memcpy(h->launch_peer, c->peer, strlen(c->peer) + 1);
        REQUIRE(peer_certificate(c->tls, h->client_certificate_storage, &h->client_certificate));
        sp5_rtsp_config config = {.host = &h->host,
                                  .devices = sp5_store_devices(h->store),
                                  .session = h->session,
                                  .launch = h->launch,
                                  .verified_certificate = h->client_certificate,
                                  .peer = h->launch_peer,
                                  .audio_port = AUDIO_PORT,
                                  .video_port = VIDEO_PORT,
                                  .control_port = CONTROL_PORT};
        h->rtsp = sp5_rtsp_create(&config);
        REQUIRE(h->rtsp != NULL);
    }
    return http_reply(c, &response);
}

static int accept_connection(stream_host *h, int listener, bool secure, bool rtsp,
                             connection *connections)
{
    struct sockaddr_in remote;
    socklen_t size = sizeof(remote);
    int fd = accept(listener, (struct sockaddr *)&remote, &size);
    if (fd < 0) {
        /* An aborted handshake, a network change or descriptor exhaustion
         * fails only this accept; the listener keeps serving. */
        int error = errno;
        uint64_t now = now_us();
        if (error != EAGAIN && error != EWOULDBLOCK && error != EINTR &&
            now - h->accept_error_us >= ACCEPT_ERROR_INTERVAL_US) {
            h->accept_error_us = now;
            (void)emit("{\"event\":\"accept_error\",\"synthetic\":false,\"diagnostic\":true,"
                       "\"errno\":%d}\n",
                       error);
        }
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
    if (nonblocking(fd) != 0) {
        (void)close(fd);
        return 0;
    }
    connection *c = &connections[slot];
    *c = (connection){.fd = fd, .secure = secure, .rtsp = rtsp, .deadline_us = now_us() + 10000000};
    memcpy(c->peer, peer, strlen(peer) + 1);
    if (secure) {
        c->tls = SSL_new(sp5_http_tls_context(h->http));
        if (c->tls == NULL || SSL_set_fd(c->tls, fd) != 1) {
            close_connection(c); /* never serve a secure port without TLS */
            return 0;
        }
        SSL_set_accept_state(c->tls);
    }
    (void)emit("{\"event\":\"accept\",\"synthetic\":false,\"diagnostic\":true,"
               "\"port_kind\":\"%s\",\"secure\":%s,\"peer\":\"%s\"}\n",
               rtsp ? "rtsp" : "http", secure ? "true" : "false", peer);
    return 0;
}

/* Waits up to WORKER_EXIT_US for a worker's done flag. For the video owner it
 * then interrupts the Avcap2 stream and waits once more. */
static bool worker_exited(stream_host *h, atomic_bool *done, atomic_int *stage, const char *name,
                          bool interruptible)
{
    for (int round = 0; round < (interruptible ? 2 : 1); ++round) {
        uint64_t deadline = now_us() + WORKER_EXIT_US;
        while (!atomic_load_explicit(done, memory_order_acquire) && now_us() < deadline)
            pause_step();
        if (atomic_load_explicit(done, memory_order_acquire))
            return true;
        bool interrupted = round == 0 && interruptible && sp5_ps5_capture_interrupt(&h->capture);
        (void)emit("{\"event\":\"worker_stuck\",\"synthetic\":false,\"diagnostic\":true,"
                   "\"worker\":\"%s\",\"stage\":\"%s\",\"round\":%d,\"interrupted\":%s}\n",
                   name, stage_name(stage == NULL ? STAGE_IDLE : atomic_load(stage)), round,
                   interrupted ? "true" : "false");
    }
    return false;
}

static int clean_session(stream_host *h)
{
    if (h->media_wedged)
        return 1;
    if (h->running) {
        REQUIRE(atomic_load_explicit(&h->stopped, memory_order_acquire));
        REQUIRE(pthread_join(h->input_thread, NULL) == 0);
        /* A media owner blocked in a console call must not hang the host:
         * wait with a deadline, then interrupt the capture stream. */
        bool video = worker_exited(h, &h->media_done, &h->video_stage, "video", true);
        bool audio = worker_exited(h, &h->audio_done, &h->audio_stage, "audio", false);
        bool metrics = worker_exited(h, &h->metrics_done, NULL, "metrics", false);
        h->running = false;
        if (!video || !audio || !metrics) {
            /* Joinable threads that may still return touch session state:
             * detach them and keep that state allocated for good. */
            if (!video)
                (void)pthread_detach(h->media_thread);
            else
                REQUIRE(pthread_join(h->media_thread, NULL) == 0);
            if (!audio)
                (void)pthread_detach(h->audio_thread);
            else
                REQUIRE(pthread_join(h->audio_thread, NULL) == 0);
            if (!metrics)
                (void)pthread_detach(h->metrics_thread);
            else
                REQUIRE(pthread_join(h->metrics_thread, NULL) == 0);
            /* Session state stays allocated and is never closed or freed: a
             * stuck owner may still return and use it. clean_session is a
             * no-op from now on; a takeover replaces this process. */
            h->media_wedged = true;
            (void)emit("{\"event\":\"media_wedged\",\"synthetic\":false,\"diagnostic\":true,"
                       "\"video\":%s,\"audio\":%s,\"metrics\":%s}\n",
                       video ? "true" : "false", audio ? "true" : "false",
                       metrics ? "true" : "false");
            return 1;
        }
        REQUIRE(pthread_join(h->media_thread, NULL) == 0);
        REQUIRE(pthread_join(h->audio_thread, NULL) == 0);
        REQUIRE(pthread_join(h->metrics_thread, NULL) == 0);
    }
    if (h->lifecycle == NULL)
        return 0;
    sp5_session_view view = sp5_lifecycle_read(h->lifecycle);
    sp5_end_reason reason = view.has_end_reason ? view.end_reason : SP5_END_ERROR;
    /* All producers are joined. Invalidate admission/neutralize controllers
     * before ending media, even when the input owner did not take this path. */
    (void)sp5_lifecycle_stop_input(h->lifecycle, reason);
    sp5_result ended = sp5_lifecycle_end(h->lifecycle, reason);
    sp5_control_transport_close(h->transport);
    h->transport = NULL;
    sp5_control_destroy(h->control);
    h->control = NULL;
    sp5_video_destroy(h->video);
    h->video = NULL;
    sp5_audio_destroy(h->audio);
    h->audio = NULL;
    sp5_opus_encoder_destroy(h->opus);
    h->opus = NULL;
    h->audio_pcm_frames = 0;
    bool clean = !h->capture.opened && !h->capture.leased && !h->capture.global_acquired &&
                 !h->encoder.opened && !h->audio_adapter.open && !h->audio_adapter.leased &&
                 !h->audio_adapter.global_acquired && sp5_ps5_media_global_refcount() == 0;
    for (size_t i = 0; i < SP5_MAX_CONTROLLERS; ++i)
        clean = clean && !h->input_adapter.controllers[i].live;
    (void)emit("{\"event\":\"ended\",\"synthetic\":false,\"diagnostic\":true,\"reason\":%u,"
               "\"clean\":%s,\"inputs\":%llu,\"video_frames\":%llu,\"audio_frames\":%llu}\n",
               (unsigned)reason, clean ? "true" : "false",
               (unsigned long long)atomic_load(&h->input_count),
               (unsigned long long)atomic_load(&h->video_count),
               (unsigned long long)atomic_load(&h->audio_count));
    if (ended.status != SP5_OK) {
        /* lifecycle_end is retry-safe; keep the lifecycle so the main loop
         * retries instead of blocking every later session. */
        h->cleanup_pending = true;
        h->cleanup_retry_us = now_us() + 1000000;
        (void)emit("{\"event\":\"cleanup_retry\",\"synthetic\":false,\"diagnostic\":true,"
                   "\"reason\":%d}\n",
                   (int)ended.reason);
        return 1;
    }
    h->cleanup_pending = false;
    /* A failed device-history write is reported but never blocks teardown. */
    if (h->client_certificate.size != 0 &&
        sp5_devices_authorized(sp5_store_devices(h->store), h->client_certificate) &&
        sp5_devices_session_ended(sp5_store_devices(h->store), h->client_certificate,
                                  wall_us().value)
                .status != SP5_OK)
        clean = false;
    sp5_lifecycle_destroy(h->lifecycle);
    h->lifecycle = NULL;
    atomic_store(&h->protection_assumed, false);
    return clean ? 0 : 1;
}

static void destroy_stream(stream_host *h)
{
    if (h->lifecycle != NULL)
        (void)clean_session(h);
    sp5_console_web_close(&h->web_service);
    sp5_web_destroy(h->web_service.web);
    h->web_service.web = NULL;
    for (size_t i = 0; i < 3; ++i)
        if (h->listeners[i] >= 0)
            (void)close(h->listeners[i]);
    if (h->video_socket >= 0)
        (void)close(h->video_socket);
    if (h->audio_socket >= 0)
        (void)close(h->audio_socket);
    sp5_rtsp_destroy(h->rtsp);
    h->rtsp = NULL;
    sp5_http_destroy(h->http);
    h->http = NULL;
    sp5_pair_flow_destroy(h->pair);
    h->pair = NULL;
    sp5_pairing_destroy(h->pairing);
    h->pairing = NULL;
    sp5_session_destroy(h->session);
    h->session = NULL;
    sp5_authority_destroy(h->authority);
    h->authority = NULL;
    if (h->runtime != NULL)
        (void)sp5_control_runtime_destroy(h->runtime);
    h->runtime = NULL;
    sp5_store_destroy(h->store);
    h->store = NULL;
    /* Release the state-directory lock before the takeover listeners close:
     * the next host treats a closed listener as "previous host gone" and
     * opens storage immediately. */
    if (h->storage != NULL)
        (void)sp5_posix_storage_close(h->storage);
    h->storage = NULL;
    if (h->takeover_fd >= 0) {
        (void)close(h->takeover_fd);
        h->takeover_fd = -1;
    }
    if (h->legacy_takeover_fd >= 0) {
        (void)close(h->legacy_takeover_fd);
        h->legacy_takeover_fd = -1;
    }
}

static void takeover_sleep_ms(unsigned milliseconds)
{
    struct timespec delay = {.tv_sec = 0, .tv_nsec = (long)milliseconds * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

/* When the previous host had no pad to hand over, register and park one
 * neutral pad so a host restart cannot leave the console with zero
 * controllers. */
static bool takeover_reserve_pad(stream_host *h)
{
    const uint64_t session = UINT64_C(0x74616b656f766572);
    sp5_input_ops ops = sp5_ps5_input_ops(&h->input_adapter);
    if (ops.add == NULL || ops.remove == NULL || ops.add(ops.context, session, 0).status != SP5_OK)
        return false;
    return ops.remove(ops.context, session, 0).status == SP5_OK;
}

static int takeover_connect_legacy(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(LEGACY_TAKEOVER_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static struct sockaddr_un takeover_address(void)
{
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    (void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", TAKEOVER_SOCKET);
    return address;
}

/* A connection is the liveness check: a socket file left by an exited host
 * refuses connections. */
static int takeover_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un address = takeover_address();
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

/* Sends "h=N" and, when this host has /dev/hid open, the descriptor itself.
 * The pad's registration belongs to the open file, not the process, so it
 * stays registered after this host exits as long as the receiver holds it. */
static bool takeover_send(int fd, int32_t handle, int hid)
{
    char text[32];
    int length = snprintf(text, sizeof(text), "h=%d\n", (int)handle);
    if (length <= 0)
        return false;
    struct iovec payload = {text, (size_t)length};
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));
    if (hid >= 0) {
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &hid, sizeof(int));
    }
    return sendmsg(fd, &message, 0) == (ssize_t)length;
}

/* Receives the reply of takeover_send. Returns the handle (or -1) and stores
 * a received descriptor in *hid. */
static int32_t takeover_receive(int fd, int *hid)
{
    *hid = -1;
    char text[32] = {0};
    struct iovec payload = {text, sizeof(text) - 1};
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    ssize_t count = recvmsg(fd, &message, 0);
    if (count <= 0)
        return -1;
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header != NULL;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS &&
            header->cmsg_len >= CMSG_LEN(sizeof(int)))
            memcpy(hid, CMSG_DATA(header), sizeof(int));
    }
    int handle = -1;
    if (sscanf(text, "h=%d", &handle) != 1 || handle < 0)
        return -1;
    return (int32_t)handle;
}

static bool takeover_wait_gone(int (*connect_old)(void))
{
    for (unsigned attempt = 0; attempt < 400; ++attempt) {
        takeover_sleep_ms(50);
        int probe = connect_old();
        if (probe < 0)
            return true;
        (void)close(probe);
    }
    return false;
}

/* Returns 1 when an old host was retired, 0 when none was present, -1 on a
 * takeover error. The previous host passes its pad handle and /dev/hid
 * descriptor, then exits completely; this host now owns the pad, so the
 * console never sees a disconnect or a new controller. A v0.1.0 host only
 * returns the handle and keeps its descriptor in a resident holder. */
static int takeover_retire_old(stream_host *h)
{
    int32_t reused = -1;
    int hid = -1;
    bool legacy = false;
    int fd = takeover_connect();
    if (fd >= 0) {
        if (write(fd, "takeover", 8) != 8) {
            (void)close(fd);
            return -1;
        }
        reused = takeover_receive(fd, &hid);
        (void)close(fd);
    } else {
        fd = takeover_connect_legacy();
        if (fd < 0)
            return 0;
        legacy = true;
        if (write(fd, "takeover", 8) != 8) {
            (void)close(fd);
            return -1;
        }
        char response[32] = {0};
        ssize_t response_size = read(fd, response, sizeof(response) - 1);
        (void)close(fd);
        int value = -1;
        if (response_size > 0 && sscanf(response, "h=%d", &value) == 1 && value >= 0)
            reused = (int32_t)value;
    }
    if (hid >= 0 && reused < 0) {
        (void)close(hid);
        hid = -1;
    }
    if (reused >= 0 && hid >= 0) {
        if (!sp5_ps5_input_adopt(&h->input_adapter, hid, reused)) {
            (void)close(hid);
            hid = -1;
        }
    }
    /* Without the descriptor (a v0.1.0 host) the pad stays owned by its
     * resident holder; this host only reports on the borrowed handle. */
    if (reused >= 0 && hid < 0)
        h->input_adapter.external_handle = reused;
    (void)emit("{\"event\":\"takeover\",\"synthetic\":false,\"diagnostic\":true,"
               "\"reused_handle\":%d,\"descriptor_received\":%s,\"legacy\":%s}\n",
               (int)reused, hid >= 0 ? "true" : "false", legacy ? "true" : "false");
    if (!takeover_wait_gone(legacy ? takeover_connect_legacy : takeover_connect))
        return -1;
    if (reused >= 0)
        return 1;
    return takeover_reserve_pad(h) ? 1 : -1;
}

static int takeover_listen(stream_host *h)
{
    /* Only reached after any previous host has exited, so a socket file
     * still present is stale. */
    (void)unlink(TAKEOVER_SOCKET);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un local = takeover_address();
    if (bind(fd, (const struct sockaddr *)&local, sizeof(local)) != 0 || listen(fd, 4) != 0) {
        (void)close(fd);
        return -1;
    }
    h->takeover_fd = fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(LEGACY_TAKEOVER_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 4) != 0) {
        (void)close(fd);
        return -1;
    }
    h->legacy_takeover_fd = fd;
    return 0;
}

static int32_t takeover_handle(const stream_host *h)
{
    for (size_t i = 0; i < SP5_PS5_INPUT_HID_MAX; ++i) {
        if (h->input_adapter.controllers[i].live)
            return h->input_adapter.controllers[i].handle;
    }
    for (size_t i = 0; i < SP5_PS5_INPUT_HID_MAX; ++i) {
        if (h->input_adapter.parked[i] >= 0)
            return h->input_adapter.parked[i];
    }
    /* A host that borrowed its handle but never added a controller (e.g. an
     * official client without a gamepad) must still pass it on; otherwise
     * the next host creates a second, unassigned pad. */
    return h->input_adapter.external_handle;
}

static void takeover_accept(stream_host *h, bool legacy, bool *stop, const char **reason)
{
    int client = accept(legacy ? h->legacy_takeover_fd : h->takeover_fd, NULL, NULL);
    if (client < 0)
        return;
    /* Bound the read so an idle local client cannot stall the host loop. */
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 200000};
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    char command[16];
    ssize_t count = read(client, command, sizeof(command));
    if (count == 8 && memcmp(command, "takeover", 8) == 0) {
        int32_t handle = takeover_handle(h);
        bool sent;
        if (legacy) {
            char response[32];
            int length = snprintf(response, sizeof(response), "h=%d\n", (int)handle);
            sent = length > 0 && write(client, response, (size_t)length) == length;
            h->legacy_handoff = sent;
        } else {
            sent = takeover_send(client, handle, handle >= 0 ? h->input_adapter.hid : -1);
        }
        (void)emit("{\"event\":\"takeover_served\",\"synthetic\":false,\"diagnostic\":true,"
                   "\"handle\":%d,\"legacy\":%s,\"sent\":%s}\n",
                   (int)handle, legacy ? "true" : "false", sent ? "true" : "false");
        *stop = true;
        *reason = "takeover";
        if (h->running)
            atomic_store_explicit(&h->end_request, SP5_END_HOST_STOPPING, memory_order_release);
    }
    (void)close(client);
}

static sp5_web *create_web(stream_host *h, const char *authority)
{
    sp5_web_config config = {.store = h->store,
                             .host = &h->host,
                             .session = h->session,
                             .pairing = h->pairing,
                             .pair_flow = h->pair,
                             .authority = authority,
                             .extensions = {.context = h, .route = console_web_route}};
    return sp5_web_create(&config);
}

static int start_stream(stream_host *h)
{
    for (size_t i = 0; i < 3; ++i)
        h->listeners[i] = -1;
    h->video_socket = -1;
    h->audio_socket = -1;
    h->takeover_fd = -1;
    h->legacy_takeover_fd = -1;
    sp5_console_web_init(&h->web_service, NULL, -1);

    /* The previous host holds the exclusive state-directory flock until it
     * exits, so retire it before opening storage. The replacement pad is
     * already registered by then. */
    int retired = takeover_retire_old(h);
    if (retired < 0) {
        (void)fputs("could not retire the previous host cleanly\n", stderr);
        return 1;
    }
    if (takeover_listen(h) != 0) {
        (void)fputs("could not create the takeover listener\n", stderr);
        return 1;
    }
    if (!ensure_state_directory(h->options.state_directory)) {
        (void)fputs("could not create the state directory\n", stderr);
        return 1;
    }
    h->storage = sp5_posix_storage_open(h->options.state_directory);
    if (h->storage == NULL) {
        (void)fputs("state directory is not an owned 0700 directory\n", stderr);
        return 1;
    }
    sp5_storage_ops storage_ops = sp5_posix_storage_operations(h->storage);
    h->store = sp5_store_create(&storage_ops);
    if (h->store == NULL || sp5_store_load(h->store).status != SP5_OK) {
        (void)fputs("could not load the console store\n", stderr);
        return 1;
    }
    if (sp5_host_init(&h->host, SP5_HOST_VERSION, "13.60", h->options.bind_address).status !=
            SP5_OK ||
        sp5_host_refresh(&h->host, h->store).status != SP5_OK) {
        (void)fputs("could not initialize the host\n", stderr);
        return 1;
    }
    /* SDR H.264 only. Bitrate ceilings are the AVC level bounds recovered for
     * each Avcap2 mode (sp5_ps5_mode_validate). */
    static const sp5_host_mode modes[] = {
        {1920, 1080, 60, "h264", SP5_SDR, 1000, 50000},
        {1920, 1080, 30, "h264", SP5_SDR, 1000, 20000},
        {1280, 720, 60, "h264", SP5_SDR, 1000, 20000},
        {1280, 720, 30, "h264", SP5_SDR, 1000, 14000},
    };
    h->host.supported_mode_count = sizeof(modes) / sizeof(modes[0]);
    memcpy(h->host.supported_modes, modes, sizeof(modes));
    h->host.controller_touch_events = false;
    h->host.encryption_supported = 5;
    if (sp5_host_transition(&h->host, SP5_HOST_RUNNING).status != SP5_OK) {
        (void)fputs("could not start the host\n", stderr);
        return 1;
    }
    h->authority = sp5_authority_create(sp5_store_devices(h->store));
    h->session = sp5_session_create(h->authority);
    h->pairing = sp5_pairing_create();
    if (h->authority == NULL || h->session == NULL || h->pairing == NULL) {
        (void)fputs("could not create session state\n", stderr);
        return 1;
    }
    sp5_bytes identity = sp5_store_identity(h->store);
    const char certificate_begin[] = "-----BEGIN CERTIFICATE-----";
    size_t certificate_offset = 0;
    while (certificate_offset + sizeof(certificate_begin) - 1 <= identity.size &&
           memcmp(identity.data + certificate_offset, certificate_begin,
                  sizeof(certificate_begin) - 1) != 0)
        ++certificate_offset;
    if (certificate_offset + sizeof(certificate_begin) - 1 > identity.size) {
        (void)fputs("stored identity has no PEM certificate\n", stderr);
        return 1;
    }
    sp5_bytes certificate = {identity.data + certificate_offset,
                             identity.size - certificate_offset};
    sp5_bytes key = {identity.data, certificate_offset};

    sp5_ps5_system_init(&h->system);
    sp5_platform_provenance provenance;
    if (sp5_ps5_system_probe(&h->system, &provenance).status != SP5_OK ||
        provenance.validity != SP5_VALIDITY_VALID) {
        (void)fputs("firmware 13.60 system probe refused or unavailable\n", stderr);
        return 1;
    }
    sp5_system_ops notifications = console_system_ops(&h->console_system, &h->system);

    int web_listener = sp5_console_web_listen(h->options.bind_address, h->options.web_port,
                                              h->web_authority, sizeof(h->web_authority));
    if (web_listener < 0) {
        (void)fputs("could not listen on the web port\n", stderr);
        return 1;
    }
    int url_size = snprintf(h->pin_url, sizeof(h->pin_url), "http://%s/", h->web_authority);
    if (url_size < 0 || (size_t)url_size >= sizeof(h->pin_url)) {
        (void)close(web_listener);
        (void)fputs("web authority is too long\n", stderr);
        return 1;
    }
    sp5_pair_flow_config pc = {
        h->pairing, sp5_store_devices(h->store), certificate, key, notifications, h->pin_url};
    h->pair = sp5_pair_flow_create(&pc);
    if (h->pair == NULL) {
        (void)close(web_listener);
        (void)fputs("could not create the pairing flow\n", stderr);
        return 1;
    }
    sp5_http_config hc = {.host = &h->host,
                          .devices = sp5_store_devices(h->store),
                          .session = h->session,
                          .certificate_pem = certificate,
                          .private_key_pem = key,
                          .pair_context = h->pair,
                          .pair = sp5_pair_flow_handle};
    h->http = sp5_http_create(&hc);
    if (h->http == NULL) {
        (void)close(web_listener);
        (void)fputs("could not create the HTTP service\n", stderr);
        return 1;
    }
    h->runtime = sp5_control_runtime_create();
    if (h->runtime == NULL) {
        (void)close(web_listener);
        (void)fputs("could not create the control runtime\n", stderr);
        return 1;
    }
    h->listeners[0] = open_socket(SOCK_STREAM, h->options.bind_address, h->options.http_port);
    h->listeners[1] = open_socket(SOCK_STREAM, h->options.bind_address, SECURE_HTTP_PORT);
    h->listeners[2] = open_socket(SOCK_STREAM, h->options.bind_address, RTSP_PORT);
    h->video_socket = open_socket(SOCK_DGRAM, h->options.bind_address, VIDEO_PORT);
    h->audio_socket = open_socket(SOCK_DGRAM, h->options.bind_address, AUDIO_PORT);
    if (h->listeners[0] < 0 || h->listeners[1] < 0 || h->listeners[2] < 0 || h->video_socket < 0 ||
        h->audio_socket < 0) {
        (void)close(web_listener);
        (void)fputs("could not bind a streaming listener\n", stderr);
        return 1;
    }
    sp5_web *web = create_web(h, h->web_authority);
    if (web == NULL) {
        (void)close(web_listener);
        (void)fputs("could not create the web service\n", stderr);
        return 1;
    }
    sp5_console_web_init(&h->web_service, web, web_listener);
    if (emit("{\"event\":\"web\",\"synthetic\":false,\"diagnostic\":true,\"url\":\"http://%s\"}\n",
             h->web_authority) < 0)
        return 1;
    return 0;
}

static void close_all(const int *fds, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (fds[i] >= 0)
            (void)close(fds[i]);
}

/* Moves every listener to the console's new address. Only runs between
 * sessions; the controller's /dev/hid descriptor stays with this process, so
 * the console keeps its pad and user assignment. All new sockets are bound
 * before anything is closed: on failure the host keeps its old state and the
 * next check retries. */
static bool rebind_address(stream_host *h, connection *connections, const char *next)
{
    char authority[sizeof(h->web_authority)];
    char pin_url[sizeof(h->pin_url)];
    int fds[6] = {sp5_console_web_listen(next, h->options.web_port, authority, sizeof(authority)),
                  open_socket(SOCK_STREAM, next, h->options.http_port),
                  open_socket(SOCK_STREAM, next, SECURE_HTTP_PORT),
                  open_socket(SOCK_STREAM, next, RTSP_PORT),
                  open_socket(SOCK_DGRAM, next, VIDEO_PORT),
                  open_socket(SOCK_DGRAM, next, AUDIO_PORT)};
    bool bound = true;
    for (size_t i = 0; i < 6; ++i)
        bound = bound && fds[i] >= 0;
    int url_size = bound ? snprintf(pin_url, sizeof(pin_url), "http://%s/", authority) : -1;
    sp5_web *web = url_size > 0 && (size_t)url_size < sizeof(pin_url) &&
                           strlen(next) < sizeof(h->address) &&
                           strlen(next) < sizeof(h->host.address)
                       ? create_web(h, authority)
                       : NULL;
    if (web == NULL || !sp5_pair_flow_set_pin_url(h->pair, pin_url)) {
        sp5_web_destroy(web);
        close_all(fds, 6);
        return false;
    }
    for (size_t i = 0; i < CONNECTIONS; ++i)
        close_connection(&connections[i]);
    sp5_console_web_close(&h->web_service);
    sp5_web_destroy(h->web_service.web);
    sp5_console_web_init(&h->web_service, web, fds[0]);
    int old[5] = {h->listeners[0], h->listeners[1], h->listeners[2], h->video_socket,
                  h->audio_socket};
    close_all(old, 5);
    for (size_t i = 0; i < 3; ++i)
        h->listeners[i] = fds[i + 1];
    h->video_socket = fds[4];
    h->audio_socket = fds[5];
    sp5_rtsp_destroy(h->rtsp); /* a launch prepared for the old address */
    h->rtsp = NULL;
    char previous[sizeof(h->address)];
    memcpy(previous, h->address, sizeof(previous));
    memcpy(h->address, next, strlen(next) + 1);
    memcpy(h->host.address, next, strlen(next) + 1);
    memcpy(h->web_authority, authority, strlen(authority) + 1);
    memcpy(h->pin_url, pin_url, (size_t)url_size + 1);
    (void)emit("{\"event\":\"address_changed\",\"synthetic\":false,\"diagnostic\":true,"
               "\"from\":\"%s\",\"to\":\"%s\",\"web\":\"http://%s\"}\n",
               previous, h->address, h->web_authority);
    return true;
}

/* Rechecks the served address between sessions. A console whose address
 * changed (new network, DHCP renewal) is served on its new address without
 * resending the payload. */
static void follow_address(stream_host *h, connection *connections, uint64_t now)
{
    if (h->address_fixed || now < h->address_check_us)
        return;
    h->address_check_us = now + ADDRESS_CHECK_INTERVAL_US;
    if (address_assigned(h->address)) {
        h->address_lost_reported = false;
        return;
    }
    const char *next = detect_bind_address();
    if (next != NULL && rebind_address(h, connections, next)) {
        h->address_lost_reported = false;
        return;
    }
    if (!h->address_lost_reported)
        (void)emit("{\"event\":\"address_lost\",\"synthetic\":false,\"diagnostic\":true,"
                   "\"address\":\"%s\",\"replacement\":%s}\n",
                   h->address, next != NULL ? "\"rebind_failed\"" : "null");
    h->address_lost_reported = true;
}

int main(int argc, char **argv)
{
    stream_host *h = calloc(1, sizeof(*h));
    if (h == NULL)
        return 1;
    bool help = false;
    int parsed = parse_options(argc, argv, &h->options, &help);
    if (parsed != 0) {
        print_usage(stderr);
        free(h);
        return parsed;
    }
    if (help) {
        print_usage(stdout);
        free(h);
        return 0;
    }
    h->address_fixed = h->options.bind_address != NULL;
    if (h->options.bind_address == NULL)
        h->options.bind_address = detect_bind_address();
    if (h->options.bind_address == NULL || strlen(h->options.bind_address) >= sizeof(h->address)) {
        (void)fputs("no private IPv4 console address found; pass --bind IPV4\n", stderr);
        free(h);
        return 64;
    }
    memcpy(h->address, h->options.bind_address, strlen(h->options.bind_address) + 1);
    h->options.bind_address = h->address;
    struct in_addr bind_address;
    if (inet_pton(AF_INET, h->options.bind_address, &bind_address) != 1 ||
        !sp5_peer_permitted(h->options.bind_address, false)) {
        (void)fputs("bind address is not a permitted private IPv4 address\n", stderr);
        free(h);
        return 64;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR || signal(SIGINT, on_signal) == SIG_ERR ||
        signal(SIGTERM, on_signal) == SIG_ERR || setvbuf(stdout, NULL, _IOLBF, 0) != 0) {
        (void)fputs("could not install process handlers\n", stderr);
        free(h);
        return 1;
    }
    atomic_init(&h->stopped, true);
    atomic_init(&h->media_done, true);
    atomic_init(&h->metrics_done, true);
    atomic_init(&h->audio_done, true);
    atomic_init(&h->end_request, -1);
    atomic_init(&h->input_count, 0);
    atomic_init(&h->video_count, 0);
    atomic_init(&h->audio_count, 0);
    for (size_t slot = 0; slot < CPU_SLOTS; ++slot)
        atomic_init(&h->thread_cpu_us[slot], 0);
    atomic_init(&h->protection_assumed, false);
    atomic_init(&h->keyframe_requested, false);
    atomic_init(&h->video_stage, STAGE_IDLE);
    atomic_init(&h->audio_stage, STAGE_IDLE);
    sp5_ps5_capture_init(&h->capture);
    sp5_ps5_encoder_init(&h->encoder);
    sp5_ps5_audio_init(&h->audio_adapter);
    sp5_ps5_input_init(&h->input_adapter);
    sp5_ps5_system_init(&h->system);
    if (start_stream(h) != 0) {
        (void)emit("{\"event\":\"stopped\",\"synthetic\":false,\"diagnostic\":true,"
                   "\"reason\":\"error\"}\n");
        destroy_stream(h);
        free(h);
        return 1;
    }
    sp5_settings settings = sp5_store_settings(h->store);
    char escaped[SP5_NAME_CAPACITY * 6];
    if (json_escape(escaped, sizeof(escaped), settings.host_name) == 0 ||
        emit("{\"event\":\"ready\",\"synthetic\":false,\"diagnostic\":true,\"host\":\"%s\","
             "\"address\":\"%s\",\"web\":\"http://%s\",\"http_port\":%u,\"rtsp_port\":%u,"
             "\"https_port\":%u,\"video_port\":%u,\"audio_port\":%u,\"control_port\":%u,"
             "\"modes\":\"h264 SDR 1920x1080@60/30 1280x720@60/30\","
             "\"protection\":\"%s\",\"firmware\":\"13.60\",\"run_seconds\":%llu}\n",
             escaped, h->options.bind_address, h->web_authority, (unsigned)h->options.http_port,
             (unsigned)RTSP_PORT, (unsigned)SECURE_HTTP_PORT, (unsigned)VIDEO_PORT,
             (unsigned)AUDIO_PORT, (unsigned)CONTROL_PORT,
             h->options.assume_protection_clear ? "diagnostic:--assume-protection-clear"
                                                : "UNKNOWN-sourced",
             (unsigned long long)h->options.run_seconds) < 0) {
        destroy_stream(h);
        free(h);
        return 1;
    }
    (void)nonblocking(STDIN_FILENO);
    (void)nonblocking(STDERR_FILENO); /* diagnostics must not stall on a gone log peer */
    sp5_optional_u64 clock_wall = wall_us();
    (void)emit("{\"event\":\"clock\",\"synthetic\":false,\"diagnostic\":true,"
               "\"wall_us\":%llu,\"monotonic_us\":%llu}\n",
               (unsigned long long)(clock_wall.present ? clock_wall.value : 0),
               (unsigned long long)now_us());
    connection connections[CONNECTIONS];
    for (size_t i = 0; i < CONNECTIONS; ++i)
        connections[i] = (connection){.fd = -1};
    const char *reason = "error";
    int status = 0;
    bool stop = false;
    uint64_t started = now_us();
    uint64_t deadline =
        h->options.run_seconds == 0 ? UINT64_MAX : started + h->options.run_seconds * 1000000;
    uint64_t budget_next = started + 10000000;
    uint64_t budget_last_us = started;
    uint64_t budget_last_thread_cpu = 0;
    uint64_t heartbeat_next = started + 1000000;
    char line[COMMAND_LIMIT];
    size_t line_size = 0;
    bool watch_stdin = true;
    while (!stop || h->running) {
        if (interrupted) {
            stop = true;
            reason = "signal";
            if (h->running)
                atomic_store_explicit(&h->end_request, SP5_END_HOST_STOPPING, memory_order_release);
        }
        if (!stop && now_us() >= deadline) {
            stop = true;
            reason = "run-seconds";
            if (h->running)
                atomic_store_explicit(&h->end_request, SP5_END_HOST_STOPPING, memory_order_release);
        }
        uint64_t budget_now = now_us();
        publish_thread_cpu(h, CPU_MAIN);
        if (!h->running && budget_now >= heartbeat_next) {
            sp5_ps5_input_heartbeat(&h->input_adapter);
            heartbeat_next = budget_now + 1000000;
        }
        if (budget_now >= budget_next) {
            uint64_t total = thread_cpu_us_total(h);
            emit_budget(h, budget_now - budget_last_us, total - budget_last_thread_cpu);
            budget_last_thread_cpu = total;
            budget_last_us = budget_now;
            while (budget_next <= budget_now)
                budget_next += 10000000;
        }
        if (h->running && atomic_load_explicit(&h->stopped, memory_order_acquire)) {
            if (clean_session(h) != 0)
                status = 1;
        }
        if (!h->running && h->lifecycle != NULL) {
            /* Retry a failed teardown, or reclaim a session whose client never
             * reached PLAY (aborted after ANNOUNCE) so later launches work. */
            uint64_t now = now_us();
            if ((h->cleanup_pending && now >= h->cleanup_retry_us) ||
                (!h->cleanup_pending && now - h->lifecycle_started_us > PLAY_DEADLINE_US))
                (void)clean_session(h);
        }
        if (stop && !h->running)
            break;
        if (!stop && !h->running && h->lifecycle == NULL && !h->web_service.stopping)
            follow_address(h, connections, budget_now);
        struct pollfd polls[6 + SP5_CONSOLE_WEB_POLLS] = {
            {watch_stdin ? STDIN_FILENO : -1, POLLIN, 0},
            {h->listeners[0], POLLIN, 0},
            {h->listeners[1], POLLIN, 0},
            {h->listeners[2], POLLIN, 0},
            {h->takeover_fd, POLLIN, 0},
            {h->legacy_takeover_fd, POLLIN, 0}};
        sp5_console_web_poll(&h->web_service, polls + 6);
        int ready = poll(polls, 6 + SP5_CONSOLE_WEB_POLLS, 1);
        if (ready < 0 && errno != EINTR) {
            /* Fatal host errors stop through the normal path: the loop exits
             * only after the stream's worker threads are joined. */
            status = 1;
            reason = "poll";
            stop = true;
            if (h->running)
                atomic_store_explicit(&h->end_request, SP5_END_ERROR, memory_order_release);
            pause_step();
            continue;
        }
        if (ready > 0 && !stop && (polls[4].revents & POLLIN) != 0)
            takeover_accept(h, false, &stop, &reason);
        if (ready > 0 && !stop && (polls[5].revents & POLLIN) != 0)
            takeover_accept(h, true, &stop, &reason);
        if (ready > 0 && (polls[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
            watch_stdin = false;
        if (ready > 0 && (polls[0].revents & POLLIN) != 0) {
            char chunk[COMMAND_LIMIT];
            ssize_t count = read(STDIN_FILENO, chunk, sizeof(chunk));
            if (count <= 0)
                watch_stdin = false;
            /* The loader closes stdin after dispatch; EOF is not a stop request. */
            for (ssize_t i = 0; i < count; ++i) {
                if (chunk[i] == '\n') {
                    line[line_size] = 0;
                    if (strcmp(line, "status") == 0) {
                        (void)emit("{\"event\":\"status\",\"synthetic\":false,\"running\":%s,"
                                   "\"inputs\":%llu,\"video_frames\":%llu,"
                                   "\"audio_frames\":%llu,\"protection_assumed\":%s}\n",
                                   h->running ? "true" : "false",
                                   (unsigned long long)atomic_load(&h->input_count),
                                   (unsigned long long)atomic_load(&h->video_count),
                                   (unsigned long long)atomic_load(&h->audio_count),
                                   atomic_load(&h->protection_assumed) ? "true" : "false");
                    } else if (strcmp(line, "stop") == 0) {
                        stop = true;
                        reason = "stdin-stop";
                        if (h->running)
                            atomic_store_explicit(&h->end_request, SP5_END_HOST_STOPPING,
                                                  memory_order_release);
                    }
                    line_size = 0;
                } else if (line_size + 1 < sizeof(line)) {
                    line[line_size++] = chunk[i];
                }
            }
        }
        for (size_t i = 0; i < 3; ++i)
            if (ready > 0 && (polls[i + 1].revents & POLLIN) != 0)
                (void)accept_connection(h, h->listeners[i], i == 1, i == 2, connections);
        for (size_t i = 0; i < CONNECTIONS; ++i) {
            if (connections[i].fd >= 0) {
                if (now_us() > connections[i].deadline_us)
                    close_connection(&connections[i]);
                else if (service_request(h, &connections[i]) != 0)
                    close_connection(&connections[i]); /* never replay a failed request */
            }
        }
        sp5_console_web_step(&h->web_service, polls + 6, now_us());
        if (h->web_service.failed && !stop) {
            status = 1;
            reason = "web";
            stop = true;
            if (h->running)
                atomic_store_explicit(&h->end_request, SP5_END_ERROR, memory_order_release);
        }
        if (h->web_service.stopped) {
            stop = true;
            reason = "web-stop";
            if (h->running)
                atomic_store_explicit(&h->end_request, SP5_END_HOST_STOPPING, memory_order_release);
        }
        if (!stop && sp5_pair_flow_tick(h->pair, now_us()).status != SP5_OK) {
            status = 1;
            reason = "pairing";
            stop = true;
            if (h->running)
                atomic_store_explicit(&h->end_request, SP5_END_ERROR, memory_order_release);
        }
    }
    if (h->lifecycle != NULL && clean_session(h) != 0)
        status = 1;
    for (size_t i = 0; i < CONNECTIONS; ++i)
        close_connection(&connections[i]);
    (void)emit("{\"event\":\"stopped\",\"synthetic\":false,\"diagnostic\":true,\"reason\":\"%s\","
               "\"protection_assumed_clear\":%s,"
               "\"protection_evidence\":\"%s\","
               "\"inputs\":%llu,\"video_frames\":%llu,\"audio_frames\":%llu}\n",
               reason, h->protection_clear_used ? "true" : "false",
               h->protection_clear_used ? "diagnostic:--assume-protection-clear" : "none",
               (unsigned long long)atomic_load(&h->input_count),
               (unsigned long long)atomic_load(&h->video_count),
               (unsigned long long)atomic_load(&h->audio_count));
    destroy_stream(h);
    bool hold_pad = h->legacy_handoff && h->input_adapter.external_handle < 0 &&
                    h->input_adapter.parked_count > 0;
    if (h->media_wedged)
        _exit(status); /* a detached media owner may still reference h */
    free(h);
    if (hold_pad) {
        /* Only a v0.1.0 successor cannot receive the descriptor: keep it and
         * the assigned pad open so that host can reuse the handle. Current
         * successors receive the descriptor and this host exits. */
        for (;;)
            pause();
    }
    return status;
}
