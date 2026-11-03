/* Internal nonshipping probe ABI and host-test seam. */
#ifndef R3_MEDIA_PROBE_H
#define R3_MEDIA_PROBE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
enum { R3_INCOMPLETE = 2, R3_USAGE = 64 };

/* Probe-owned modules only; a returned call is distinct from observed cleanup.
 * Attempts are sticky because a blocking platform call has no verified deadline. */
enum { R3_MEDIA_MODULE_CAPACITY = 3, R3_RESOURCE_CLEANUP_QUERIES = 64 };
struct media_module {
    const char *name;
    int handle;
    bool owned, attempted, completed;
    int rc, result;
};
struct media_modules {
    struct media_module entries[R3_MEDIA_MODULE_CAPACITY];
    size_t count;
    bool failed;
};
struct media_module_api {
    int (*unload)(void *, int, int *);
    bool (*observe)(void *, const struct media_module *);
    void *context;
};

/* Experimental audio ABI only: SoloBridge 71fc2af65e04b188ed364274f58351f8f413fc60
 * solobridge/capture.c; HearBridge 5da0744d28fe360d6cb236999800e8a4de042167
 * src/avcap2.c. Only kind=0, source=2, mode=0, NOWAIT=1 are admitted here.
 * Metadata is initialized as in HearBridge, retained as opaque bytes, and never
 * interpreted as a timestamp/protection record. These public callers disagree
 * on its semantics: running this candidate still requires admission/approval.
 */
struct audio_request {
    uint64_t size, unknown08;
    uint32_t unknown10, source, mode, unknown1c;
    uint64_t unknown20;
    uint32_t bitrate, reserved2c;
    uint64_t unknown30;
};
_Static_assert(sizeof(struct audio_request) == 0x38, "Avcap2 request size");
_Static_assert(offsetof(struct audio_request, source) == 0x14, "Avcap2 source offset");
_Static_assert(offsetof(struct audio_request, unknown30) == 0x30, "Avcap2 request tail");
_Static_assert(sizeof(float) == 4, "Sourced PCM format requires float32");

struct audio_api {
    int (*initialize)(void);
    int (*terminate)(void);
    int (*open)(void **, const void *, uint8_t);
    int (*start)(void *);
    int (*stop)(void *);
    int (*close)(void *);
    int (*read)(void *, void *, size_t, void *, uint32_t);
};

struct audio_state {
    void *handle;
    bool initialized, started, cleanup_failed;
    uint64_t reads, bytes, frames, empty, overruns, nonzero[2];
    float peak[2];
    _Alignas(16) float samples[2048];
    _Alignas(16) unsigned char metadata[16];
};

/* Firmware 13.60 ResourceArbitrator image, recovered in run PID328:
 * register 0xdb0, request 0x10b0, query 0x11f0, unregister 0xee0.
 * Sony's Remote Play caller at 0x41c231a0/0x41c23500/0x41c23440 supplies
 * resource 0xb4, RUNNING/STOP, an opaque token and getpid().
 * The callback's third argument is opaque and is never dereferenced here. */
enum { R3_VIDEO_RESOURCE_ID = 0xb4, R3_RESOURCE_SELECTORS = 32 };
typedef void (*resource_callback)(int32_t, int32_t, const void *);
struct resource_api {
    int (*register_client)(int32_t, resource_callback, void *);
    int (*unregister_client)(int32_t);
    int (*request_state)(int32_t, const char *, void *, int32_t);
    int (*query_selectors)(int32_t *);
};
struct resource_state {
    bool registered, requested, cleanup_failed, available;
    int32_t pid;
    int32_t selectors[R3_RESOURCE_SELECTORS];
    uint32_t queries;
};

#define R3_VIDEO_BUFFER_BYTES 0x200000
#define R3_VIDEO_TIMING_CAPACITY 4096
#define R3_VIDEO_PARAMETER_BYTES 8192
#define R3_VIDEO_SAVED_BYTES 0x200000
#define R3_VIDEO_SAVED_RECORDS 1024
struct video_recording {
    unsigned char bytes[R3_VIDEO_SAVED_BYTES];
    size_t length[R3_VIDEO_SAVED_RECORDS], used, count;
    bool invalid, overflow;
};
struct video_parameters {
    unsigned char bytes[R3_VIDEO_PARAMETER_BYTES];
    size_t offset[8], length[8], count, used;
    unsigned type[8];
    uint64_t changes;
    bool overflow;
};
enum { R3_VIDEO_CAPTURE_STATE_BYTES = 0x20, R3_VIDEO_QUERY_ATTEMPTS = 64 };
enum video_checkpoint { R3_VIDEO_STREAM, R3_VIDEO_OPEN_ONLY, R3_VIDEO_INVALID_MODE };
struct video_request {
    unsigned char bytes[0x2e8];
};
struct video_api {
    int (*initialize)(void);
    int (*terminate)(void);
    int (*open)(void **, const void *, uint8_t);
    int (*start)(void *);
    int (*stop)(void *);
    int (*close)(void *);
    int (*read)(void *, void *, size_t, void *, uint32_t);
    int (*query_mode_flags)(uint32_t *);
    int (*query_capture_state)(void *);
    int (*query_headers)(const void *, void *);
};
struct video_state {
    void *handle;
    bool initialized, started, cleanup_failed;
    unsigned tier, fps;
    unsigned codec, depth;
    enum video_checkpoint checkpoint;
    int open_result;
    uint32_t bitrate_kbps;
    unsigned open_attempts;
    unsigned mode_flag_queries, capture_state_queries;
    uint64_t reads, bytes, frames, empty, overruns, annexb_frames, nonzero_bytes;
    uint32_t timing_us[R3_VIDEO_TIMING_CAPACITY];
    size_t timing_count;
    uint64_t timing_overflow;
    struct video_parameters parameters;
    uint64_t parameter_errors;
    struct video_recording *recording; /* NULL keeps payload retention disabled. */
    _Alignas(16) unsigned char buffer[R3_VIDEO_BUFFER_BYTES];
    _Alignas(16) unsigned char info[112];
    unsigned char first_info[112];
};

#ifdef R3_MEDIA_TEST
#define R3_MEDIA_LOCAL
bool media_copy(void *, size_t, const void *, size_t);
bool media_bind_pointer(void *, size_t, uintptr_t);
bool media_modules_cleanup(const struct media_module_api *, struct media_modules *, bool);
bool resource_start(const struct resource_api *, struct resource_state *, resource_callback);
bool resource_query(const struct resource_api *, struct resource_state *);
bool resource_cleanup(const struct resource_api *, struct resource_state *);
bool resource_wait_removed(const struct resource_api *, struct resource_state *, bool (*)(void *),
                           void *);
bool video_request_init(struct video_request *, unsigned, unsigned, uint32_t);
bool video_request_set_format(struct video_request *, unsigned, unsigned);
bool video_collect_parameters(struct video_parameters *, const unsigned char *, size_t, unsigned);
bool video_collect_header_packet(struct video_parameters *, const unsigned char *, size_t,
                                 unsigned);
bool video_start(const struct video_api *, struct video_state *);
bool video_read(const struct video_api *, struct video_state *);
bool video_record(struct video_recording *, const unsigned char *, size_t, unsigned);
bool video_export(FILE *, const struct video_recording *, bool);
bool video_cleanup(const struct video_api *, struct video_state *);
uint32_t video_timing_percentile99(const struct video_state *, bool *);
bool audio_start(const struct audio_api *, struct audio_state *);
bool audio_read(const struct audio_api *, struct audio_state *);
bool audio_cleanup(const struct audio_api *, struct audio_state *);
int r3_media_main(int, char **);
#else
#define R3_MEDIA_LOCAL static
#endif
#endif
