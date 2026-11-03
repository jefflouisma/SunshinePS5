/* NONSHIPPING R4 calibration source. No capture, hooks, patches, or firmware offsets.
 * VideoOut ABI and address-bit layout: ps5-payload-dev/SDL commit
 * ee4c47dc0d617b3bc8f35108f9956baf228a1322,
 * src/video/ps5/SDL_ps5video.{c,h}, SDL_ps5tilemap.c.
 * Independent native renderer plus an optional maintained SDL surface branch.
 * Native HDR word/packing: LedyBacer/ProsperoLight commit
 * a5301b75f62d31a4d590aec9cad473bb26e76a9f/include/ps5_videoout_formats.h.
 * Registration success alone does not prove native PQ reaches HDMI unchanged.
 * POSIX declarations: SDK v0.45 a7685ce8e97ce1f8a4f38253a41d54241c9124f1,
 * target/include/{time.h,sys/mman.h,sys/socket.h,netinet/in.h,unistd.h}.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define WIDTH 1920U
#define HEIGHT 1080U
#define BUFFER_BYTES ((size_t)16 * 1024U * 1024U)
#define MEMORY_BYTES (2U * BUFFER_BYTES)

#ifdef TEST_PATTERN_HDR
#define CODE_MAX 1023U
#define SOURCE_FORMAT "R10G10B10A2-PQ-BT2020"
#define VIDEO_FORMAT UINT64_C(0x8100070422000000)
#else
#define CODE_MAX 255U
#define SOURCE_FORMAT "ABGR8888"
#define VIDEO_FORMAT UINT64_C(0x8000000022000000)
#endif
#define CODE8(value) ((value) * CODE_MAX / 255U)

/* SDL's ABGR8888 surface has little-endian memory bytes R,G,B,A. Exact numerical
 * words and source-image identity are recorded; HDMI conversion is a separate chain.
 */
static uint32_t rgba(uint32_t red, uint32_t green, uint32_t blue)
{
#ifdef TEST_PATTERN_HDR
    return UINT32_C(0xc0000000) | (blue << 20) | (green << 10) | red;
#else
    return UINT32_C(0xff000000) | (blue << 16) | (green << 8) | red;
#endif
}

typedef struct {
    uint32_t x;
    uint32_t y;
} Pixel;

static uint32_t pattern(Pixel pixel, uint32_t frame)
{
    uint32_t x = pixel.x;
    uint32_t y = pixel.y;
    if (y < 128U) {
        /* Sixteen 96-pixel cells encode frame ID, followed by its low four-bit
         * complement. A counter is identifiable without repeating within 65536 frames.
         */
        uint32_t cell = x / 96U;
        bool bit = cell < 16U ? ((frame >> cell) & 1U) != 0U : ((frame >> (cell - 16U)) & 1U) == 0U;
        return bit ? rgba(CODE_MAX, CODE_MAX, CODE_MAX) : rgba(0, 0, 0);
    }
    if (y < 384U) {
        /* All native full-range codes, in spatially ordered vertical ramps. */
        uint32_t value = x * (CODE_MAX + 1U) / WIDTH;
        return rgba(value, value, value);
    }
    if (y < 640U) {
        /* Primary/secondary/neutral patches with exactly known byte values. */
        static const uint16_t patches[8][3] = {
            {0, 0, 0},
            {CODE_MAX, CODE_MAX, CODE_MAX},
            {CODE_MAX, 0, 0},
            {0, CODE_MAX, 0},
            {0, 0, CODE_MAX},
            {CODE_MAX, CODE_MAX, 0},
            {0, CODE_MAX, CODE_MAX},
            {CODE_MAX, 0, CODE_MAX},
        };
        uint32_t patch = x / 240U;
        return rgba(patches[patch][0], patches[patch][1], patches[patch][2]);
    }
    if (y < 896U) {
        uint32_t value = x * (CODE_MAX + 1U) / WIDTH;
        uint32_t band = (y - 640U) / 64U;
        if (band == 0U) {
            return rgba(value, 0, 0);
        }
        if (band == 1U) {
            return rgba(0, value, 0);
        }
        if (band == 2U) {
            return rgba(0, 0, value);
        }
        return ((x / 8U + y / 8U) & 1U) ? rgba(CODE8(224U), CODE8(224U), CODE8(224U))
                                        : rgba(CODE8(32U), CODE8(32U), CODE8(32U));
    }
    /* A moving 32-pixel marker complements the nonrepeating binary frame ID. */
    return x >= frame % (WIDTH - 32U) && x < frame % (WIDTH - 32U) + 32U
               ? rgba(CODE_MAX, CODE_MAX, CODE_MAX)
               : rgba(CODE8(16U), CODE8(16U), CODE8(16U));
}

#ifndef TEST_PATTERN_SDL
static size_t tile_offset(uint32_t x, uint32_t y)
{
    /* XOR address masks encode the sourced 512x128 tile address permutation.
     * The host self-test checks injectivity and buffer bounds for every pixel.
     */
    static const uint32_t x_masks[9] = {1, 2, 32, 64, 2176, 512, 8448, 16384, 32768};
    static const uint32_t y_masks[7] = {4, 8, 16, 1088, 128, 256, 4608};
    uint32_t offset = 0;
    for (unsigned bit = 0; bit < 9U; ++bit) {
        if (((x % 512U) >> bit) & 1U) {
            offset ^= x_masks[bit];
        }
    }
    for (unsigned bit = 0; bit < 7U; ++bit) {
        if (((y % 128U) >> bit) & 1U) {
            offset ^= y_masks[bit];
        }
    }
    return (size_t)(x / 512U) * 65536U + (size_t)(y / 128U) * 128U * WIDTH + offset;
}

#endif /* !TEST_PATTERN_SDL */

#ifdef TEST_PATTERN_HOST
int main(int argc, char **argv)
{
    uint8_t *seen = calloc(BUFFER_BYTES / sizeof(uint32_t), 1);
    if (seen == NULL) {
        return 1;
    }
    for (uint32_t y = 0; y < HEIGHT; ++y) {
        for (uint32_t x = 0; x < WIDTH; ++x) {
            size_t offset = tile_offset(x, y);
            if (offset >= BUFFER_BYTES / sizeof(uint32_t) || seen[offset] != 0U) {
                free(seen);
                return fputs("R4_PATTERN_SELF_TEST invalid tile offset\n", stderr) == EOF ? 2 : 1;
            }
            seen[offset] = 1;
        }
    }
    free(seen);
    bool ramp_codes[CODE_MAX + 1U] = {false};
    for (uint32_t x = 0; x < WIDTH; ++x) {
        uint32_t expected = x * (CODE_MAX + 1U) / WIDTH;
        if (pattern((Pixel){x, 128}, 0) != rgba(expected, expected, expected)) {
            return fputs("R4_PATTERN_SELF_TEST incorrect ramp\n", stderr) == EOF ? 2 : 1;
        }
        ramp_codes[expected] = true;
    }
    for (uint32_t code = 0; code <= CODE_MAX; ++code) {
        if (!ramp_codes[code]) {
            return fputs("R4_PATTERN_SELF_TEST missing native ramp code\n", stderr) == EOF ? 2 : 1;
        }
    }
#ifdef TEST_PATTERN_HDR
    if (rgba(1, 2, 3) != UINT32_C(0xc0300801)) {
        return fputs("R4_PATTERN_SELF_TEST incorrect ten-bit packing\n", stderr) == EOF ? 2 : 1;
    }
#endif
    if (pattern((Pixel){0, 0}, 0) != rgba(0, 0, 0) ||
        pattern((Pixel){0, 0}, 1) != rgba(CODE_MAX, CODE_MAX, CODE_MAX) ||
        pattern((Pixel){0, 128}, 0) != rgba(0, 0, 0) ||
        pattern((Pixel){WIDTH - 1U, 128}, 0) != rgba(CODE_MAX, CODE_MAX, CODE_MAX) ||
        pattern((Pixel){480, 400}, 0) != rgba(CODE_MAX, 0, 0)) {
        return fputs("R4_PATTERN_SELF_TEST incorrect known code values\n", stderr) == EOF ? 2 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "--emit") == 0) {
        char *end = NULL;
        unsigned long frame = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || frame > 65535UL) {
            return 1;
        }
        for (uint32_t y = 0; y < HEIGHT; ++y) {
            for (uint32_t x = 0; x < WIDTH; ++x) {
                uint32_t pixel = pattern((Pixel){x, y}, (uint32_t)frame);
                uint8_t bytes[4] = {(uint8_t)pixel, (uint8_t)(pixel >> 8), (uint8_t)(pixel >> 16),
                                    (uint8_t)(pixel >> 24)};
                if (fwrite(bytes, sizeof(bytes), 1, stdout) != 1U) {
                    return 1;
                }
            }
        }
        return fflush(stdout) == 0 ? 0 : 1;
    }
    return puts("R4_PATTERN_SELF_TEST format=" SOURCE_FORMAT
                " pixels=2073600 unique=yes bounds=yes native_ramp=complete; synthetic only") == EOF
               ? 1
               : 0;
}
#else
#include <emmintrin.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef TEST_PATTERN_SDL
#include <SDL.h>
#elif defined(TEST_PATTERN_FOREGROUND_NATIVE)
#include <SDL_main.h>
#endif

typedef struct {
    void *data;
    uint64_t reserved[3];
} VideoBuffer;
typedef struct {
    uint8_t reserved[80];
} VideoAttribute;

/* Exact signatures and opaque sizes come from the pinned maintained SDL header. */
int sceSystemServiceHideSplashScreen(void);
int sceKernelAllocateMainDirectMemory(size_t, size_t, int, intptr_t *);
int sceKernelMapDirectMemory(void **, size_t, int, int, intptr_t, size_t);
int sceKernelReleaseDirectMemory(intptr_t, size_t);
int sceKernelCreateEqueue(struct kevent **, const char *);
int sceKernelWaitEqueue(struct kevent *, struct kevent *, int, int *, unsigned int *);
int sceKernelDeleteEqueue(struct kevent *);
int sceVideoOutOpen(int, int, int, const void *);
void sceVideoOutClose(int);
int sceVideoOutAddFlipEvent(struct kevent *, int, void *);
int sceVideoOutSetFlipRate(int, int);
int sceVideoOutSubmitFlip(int, int, uint32_t, int64_t);
int sceVideoOutDeleteFlipEvent(struct kevent *, int);
void sceVideoOutSetBufferAttribute2(VideoAttribute *, uint64_t, uint32_t, uint32_t, uint32_t,
                                    uint64_t, uint32_t, uint64_t);
int sceVideoOutRegisterBuffers2(int, int, int, VideoBuffer *, int, VideoAttribute *, int, void *);
#ifdef TEST_PATTERN_HDR_SWITCH
/* Kodi PS5 83a7be649914535d0cb7837eea65f6f7e6eaaa21,
 * patches/ps5-opengl/kodi-additions.py and platform/ps5/VideoOutInfo.cpp.
 * Change the owned set in place; never unregister/re-register a live set.
 */
int sceVideoOutSubmitChangeBufferAttribute2(int, int, const void *, int, void *);
typedef struct {
    uint32_t resolution;
    uint32_t dynamic_range;
    uint64_t refresh_rate;
    uint64_t flags;
    uint64_t reserved[3];
} VideoOutputStatus;
_Static_assert(sizeof(VideoOutputStatus) == 48, "sourced output status size");
int sceVideoOutGetOutputStatus(int, VideoOutputStatus *);
#endif

static bool output_failed;

static void report(const char *format, ...)
{
    char *text = NULL;
    va_list arguments;
    va_start(arguments, format);
    int size = vasprintf(&text, format, arguments);
    va_end(arguments);
    if (size < 0 || size >= 2048 || text == NULL ||
        fwrite(text, 1, (size_t)size, stdout) != (size_t)size) {
        output_failed = true;
    }
    free(text);
}

#ifdef TEST_PATTERN_NETWORK
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

/* Read-only application query: websrv v0.34 hbldr.c at
 * 4d35610914273f8f4fc37af2b1d5bfb069dfba5b declares this exact ABI.
 */
int sceSystemServiceGetAppIdOfRunningBigApp(void);

int main(void)
{
    /* This build performs read-only network inventory and never opens VideoOut. */
    int result = 0;
    struct ifaddrs *interfaces = NULL;
    report("R4_NETWORK_BEGIN pid=%d source_code_check=%08x tile_check=%zu\n", getpid(),
           pattern((Pixel){0, 0}, 0), tile_offset(0, 0));
    if (getifaddrs(&interfaces) != 0) {
        perror("getifaddrs");
        result = 1;
    } else {
        for (struct ifaddrs *item = interfaces; item != NULL; item = item->ifa_next) {
            if (item->ifa_addr == NULL) {
                continue;
            }
            int family = item->ifa_addr->sa_family;
            const void *address = NULL;
            if (family == AF_INET) {
                address = &((const struct sockaddr_in *)item->ifa_addr)->sin_addr;
            } else if (family == AF_INET6) {
                address = &((const struct sockaddr_in6 *)item->ifa_addr)->sin6_addr;
            } else {
                continue;
            }
            char text[INET6_ADDRSTRLEN];
            if (inet_ntop(family, address, text, sizeof(text)) == NULL) {
                perror("inet_ntop");
                result = 1;
                continue;
            }
            report("R4_NETWORK_INTERFACE name=%s family=%d flags=%u address=%s\n", item->ifa_name,
                   family, item->ifa_flags, text);
        }
        freeifaddrs(interfaces);
    }
    for (unsigned which = 0; which < 2U; ++which) {
        int family = which == 0U ? AF_INET : AF_INET6;
        int mib[6] = {CTL_NET, PF_ROUTE, 0, family, NET_RT_DUMP, 0};
        size_t size = 0;
        if (sysctl(mib, 6, NULL, &size, NULL, 0) != 0 || size > 65536U) {
            report("R4_NETWORK_ROUTE family=%d unavailable errno=%d bytes=%zu\n", family, errno,
                   size);
            result = 1;
            continue;
        }
        uint8_t *bytes = malloc(size == 0U ? 1U : size);
        if (bytes == NULL) {
            return 1;
        }
        if (sysctl(mib, 6, bytes, &size, NULL, 0) != 0) {
            report("R4_NETWORK_ROUTE family=%d unavailable errno=%d\n", family, errno);
            result = 1;
        } else {
            report("R4_NETWORK_ROUTE family=%d bytes=%zu hex=", family, size);
            for (size_t i = 0; i < size; ++i) {
                report("%02x", bytes[i]);
            }
            report("\n");
        }
        free(bytes);
    }
    int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
    report("R4_CONTEXT active_bigapp=%d\n", app_id);
    static const char *const placeholder_paths[] = {
        "/system_ex/app/FAKE00000",
        "/system_ex/app/FAKE00000/sce_sys/param.json",
        "/system_ex/app/FAKE00000/eboot.bin",
    };
    for (size_t i = 0; i < sizeof(placeholder_paths) / sizeof(placeholder_paths[0]); ++i) {
        struct stat info;
        int status = stat(placeholder_paths[i], &info);
        report("R4_CONTEXT path=%s stat_status=%d errno=%d\n", placeholder_paths[i], status,
               status == 0 ? 0 : errno);
    }
    struct statfs volume;
    int mount_status = statfs("/system_ex", &volume);
    if (mount_status == 0) {
        report("R4_CONTEXT system_ex_mount_flags=%" PRIu64 " readonly=%u\n",
               (uint64_t)volume.f_flags, (volume.f_flags & MNT_RDONLY) != 0 ? 1U : 0U);
    } else {
        report("R4_CONTEXT system_ex_statfs_error=%d\n", errno);
    }
    const int limits[] = {RLIMIT_DATA, RLIMIT_AS, RLIMIT_MEMLOCK};
    for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i) {
        struct rlimit limit;
        if (getrlimit(limits[i], &limit) == 0) {
            report("R4_CONTEXT rlimit=%d current=%" PRIu64 " maximum=%" PRIu64 "\n", limits[i],
                   (uint64_t)limit.rlim_cur, (uint64_t)limit.rlim_max);
        } else {
            report("R4_CONTEXT rlimit=%d unavailable_errno=%d\n", limits[i], errno);
        }
    }
#ifdef TEST_PATTERN_ALLOCATION_PROBE
    /* Directed size experiment: one live type-3 allocation at a time, same sourced
     * ABI/alignment as SDL, with immediate release and no map or VideoOut handle.
     * result=0 means the bounded experiment completed, not that allocation worked.
     */
    const size_t requests[] = {0x20000, 0x100000, 0x800000, 0x1000000, 0x2000000, 0x4000000};
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i) {
        intptr_t physical = 0;
        int status = sceKernelAllocateMainDirectMemory(requests[i], 0x20000, 3, &physical);
        report("R4_ALLOCATION_PROBE bytes=%zu type=3 alignment=131072 status=%d\n", requests[i],
               status);
        if (status == 0) {
            int released = sceKernelReleaseDirectMemory(physical, requests[i]);
            report("R4_ALLOCATION_PROBE release_bytes=%zu status=%d\n", requests[i], released);
            if (released != 0) {
                result = 1;
                break;
            }
        }
    }
#endif
    report("R4_NETWORK_END result=%d\n", result);
    return output_failed ? 1 : result;
}
#else
static int monotonic_us(uint64_t *value)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) {
        return -1;
    }
    *value = (uint64_t)now.tv_sec * UINT64_C(1000000) + (uint64_t)now.tv_nsec / UINT64_C(1000);
    return 0;
}

static int control_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in address = {0};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(9040);
    address.sin_addr.s_addr = htonl(UINT32_C(0xc0a80202));
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 2) != 0 ||
        fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        int saved = errno;
        if (close(fd) != 0) {
            perror("close control socket");
        }
        errno = saved;
        return -1;
    }
    return fd;
}

static int control_poll(int listener, bool *motion, bool *stop)
{
    int fd = accept(listener, NULL, NULL);
    if (fd < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    }
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
    char command[16] = {0};
    int result = -1;
    char *clock_reply = NULL;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0) {
        size_t used = 0;
        while (used < sizeof(command) - 1U && strchr(command, '\n') == NULL) {
            ssize_t count = read(fd, command + used, sizeof(command) - 1U - used);
            if (count <= 0) {
                break;
            }
            used += (size_t)count;
        }
        if (strcmp(command, "clock\n") == 0) {
            uint64_t stamp = 0;
            if (monotonic_us(&stamp) != 0) {
                goto done;
            }
            int length = asprintf(&clock_reply, "clock_us=%" PRIu64 "\n", stamp);
            if (length < 0 || length >= 64 || clock_reply == NULL) {
                goto done;
            }
        } else if (strcmp(command, "motion\n") == 0) {
            *motion = true;
        } else if (strcmp(command, "still\n") == 0) {
            *motion = false;
        } else if (strcmp(command, "stop\n") == 0) {
            *stop = true;
        } else if (strcmp(command, "status\n") != 0) {
            report("R4_PATTERN invalid bounded control command\n");
            goto done;
        }
        const char *reply = clock_reply != NULL
                                ? clock_reply
                                : (*stop ? "stopping\n" : (*motion ? "motion\n" : "still\n"));
        size_t size = strlen(reply);
        result = write(fd, reply, size) == (ssize_t)size ? 0 : -1;
    }
done:
    free(clock_reply);
    if (close(fd) != 0) {
        perror("close control client");
        result = -1;
    }
    return output_failed ? 1 : result;
}

#ifdef TEST_PATTERN_SDL
int SDL_main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    int result = 1;
    int listener = -1;
    SDL_Window *window = NULL;
    bool sdl_attempted = false;
    bool motion = false;
    bool stop = false;
    uint64_t start = 0;
    uint32_t frame = 0;
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR || monotonic_us(&start) != 0) {
        perror("R4_SDL signal/clock admission");
        goto cleanup;
    }
    listener = control_socket();
    if (listener < 0) {
        perror("R4_SDL control admission");
        goto cleanup;
    }
    report("R4_PATTERN_BEGIN pid=%d backend=SDL-2.30.12 format=ABGR8888 width=%u height=%u "
           "max_seconds=900 timestamp_boundary=SDL_UpdateWindowSurface-entry-return\n",
           getpid(), WIDTH, HEIGHT);
    sdl_attempted = true;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        report("R4_SDL_ERROR Init=%s\n", SDL_GetError());
        goto cleanup;
    }
    const char *driver = SDL_GetCurrentVideoDriver();
    if (driver == NULL || SDL_strcasecmp(driver, "ps5") != 0) {
        report("R4_SDL_ERROR unexpected_driver=%s\n", driver == NULL ? "absent" : driver);
        goto cleanup;
    }
    window = SDL_CreateWindow("SunshinePS5 R4 source pattern", SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, (int)WIDTH, (int)HEIGHT, SDL_WINDOW_SHOWN);
    if (window == NULL) {
        report("R4_SDL_ERROR CreateWindow=%s\n", SDL_GetError());
        goto cleanup;
    }
    SDL_Surface *surface = SDL_GetWindowSurface(window);
    if (surface == NULL || surface->w != (int)WIDTH || surface->h != (int)HEIGHT ||
        surface->pitch < (int)(WIDTH * 4U) || surface->pixels == NULL || surface->format == NULL ||
        surface->format->format != SDL_PIXELFORMAT_ABGR8888) {
        report("R4_SDL_ERROR native_surface_geometry_or_format\n");
        goto cleanup;
    }
    report("R4_PATTERN_READY backend=SDL driver=%s pixel_format=%s pitch=%d "
           "control=192.168.2.2:9040 commands=still,motion,status,clock,stop\n",
           driver, SDL_GetPixelFormatName(surface->format->format), surface->pitch);
    while (!stop) {
        uint64_t before = 0;
        uint64_t after = 0;
        if (control_poll(listener, &motion, &stop) != 0 || monotonic_us(&before) != 0) {
            perror("R4_SDL control/clock");
            goto cleanup;
        }
        if (stop || before - start >= UINT64_C(900000000)) {
            break;
        }
        uint32_t source_frame = motion ? frame : 0U;
        if (SDL_LockSurface(surface) != 0) {
            report("R4_SDL_ERROR LockSurface=%s\n", SDL_GetError());
            goto cleanup;
        }
        for (uint32_t y = 0; y < HEIGHT; ++y) {
            uint32_t *row =
                (uint32_t *)((uint8_t *)surface->pixels + (size_t)y * (size_t)surface->pitch);
            for (uint32_t x = 0; x < WIDTH; ++x) {
                row[x] = pattern((Pixel){x, y}, source_frame);
            }
        }
        SDL_UnlockSurface(surface);
        if (monotonic_us(&before) != 0) {
            goto cleanup;
        }
        if (SDL_UpdateWindowSurface(window) != 0 || monotonic_us(&after) != 0) {
            report("R4_SDL_ERROR UpdateWindowSurface=%s\n", SDL_GetError());
            goto cleanup;
        }
        report("R4_PATTERN_FRAME sequence=%u source_frame=%u source_update_begin_us=%" PRIu64
               " source_update_end_us=%" PRIu64 "\n",
               frame, source_frame, before, after);
        if (fflush(stdout) != 0 || output_failed) {
            goto cleanup;
        }
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            if (event.type == SDL_QUIT) {
                stop = true;
            }
        }
        ++frame;
    }
    result = 0;
cleanup:
    if (window != NULL) {
        SDL_DestroyWindow(window);
        report("R4_PATTERN_CLEANUP SDL_DestroyWindow called\n");
    }
    if (sdl_attempted) {
        SDL_Quit();
        report("R4_PATTERN_CLEANUP SDL_Quit called; inspect R4_SDL_CLEANUP status records\n");
    }
    if (listener >= 0 && close(listener) != 0) {
        perror("R4_SDL close listener");
        result = 1;
    }
    report("R4_PATTERN_END result=%d frames=%u\n", result, frame);
    return output_failed ? 1 : result;
}
#else
static int native_main(void)
{
    const uint64_t maximum_us = UINT64_C(900000000);
    int result = 1;
    int handle = -1;
    int listener = -1;
    intptr_t physical = 0;
    void *base = NULL;
    struct kevent *queue = NULL;
    bool allocated = false;
    bool mapped = false;
    bool event_added = false;
    bool motion = false;
    bool stop = false;
    uint64_t start = 0;
    uint32_t frame = 0;
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR || monotonic_us(&start) != 0) {
        perror("R4_PATTERN initial signal/clock admission");
        goto cleanup;
    }
    listener = control_socket();
    if (listener < 0) {
        perror("R4_PATTERN initial clock/control admission");
        goto cleanup;
    }
    report("R4_PATTERN_BEGIN pid=%d format=%s code_peak=%u width=%u height=%u max_seconds=900\n",
           getpid(), SOURCE_FORMAT, CODE_MAX, WIDTH, HEIGHT);
    int rc = sceSystemServiceHideSplashScreen();
    report("R4_PATTERN_CALL HideSplashScreen=%d\n", rc);
    /* The maintained SDL VideoInit path treats HideSplashScreen as advisory:
     * a raw payload may have no application splash. Preserve its exact error
     * above, then let VideoOutOpen establish whether this context can present.
     */
    handle = sceVideoOutOpen(0xff, 0, 0, NULL);
    if (handle < 0) {
        goto cleanup;
    }
    report("R4_PATTERN_CALL VideoOutOpen=%d\n", handle);
    rc = sceKernelAllocateMainDirectMemory(MEMORY_BYTES, 0x20000, 3, &physical);
    report("R4_PATTERN_CALL AllocateMainDirectMemory=%d\n", rc);
    if (rc != 0) {
        goto cleanup;
    }
    allocated = true;
    rc = sceKernelMapDirectMemory(&base, MEMORY_BYTES, 0x33, 0, physical, 0x20000);
    report("R4_PATTERN_CALL MapDirectMemory=%d\n", rc);
    if (rc != 0 || base == NULL) {
        goto cleanup;
    }
    mapped = true;
    rc = sceKernelCreateEqueue(&queue, "R4 calibration flip queue");
    report("R4_PATTERN_CALL CreateEqueue=%d\n", rc);
    if (rc != 0) {
        goto cleanup;
    }
    rc = sceVideoOutAddFlipEvent(queue, handle, NULL);
    report("R4_PATTERN_CALL AddFlipEvent=%d\n", rc);
    if (rc != 0) {
        goto cleanup;
    }
    event_added = true;
    rc = sceVideoOutSetFlipRate(handle, 0);
    report("R4_PATTERN_CALL SetFlipRate=%d\n", rc);
    if (rc != 0) {
        goto cleanup;
    }
    VideoBuffer buffers[2] = {{.data = base}, {.data = (uint8_t *)base + BUFFER_BYTES}};
    VideoAttribute attribute = {0};
#ifdef TEST_PATTERN_HDR_SWITCH
    const uint64_t registration_format = UINT64_C(0x8000000022000000);
#else
    const uint64_t registration_format = VIDEO_FORMAT;
#endif
    sceVideoOutSetBufferAttribute2(&attribute, registration_format, 0, WIDTH, HEIGHT, 0, 0, 0);
    report("R4_PATTERN_ATTRIBUTE pixel_format=0x%016" PRIx64 "\n", registration_format);
    rc = sceVideoOutRegisterBuffers2(handle, 0, 0, buffers, 2, &attribute, 0, NULL);
    report("R4_PATTERN_CALL RegisterBuffers2=%d\n", rc);
    if (rc != 0) {
        goto cleanup;
    }
#ifdef TEST_PATTERN_HDR_SWITCH
    sceVideoOutSetBufferAttribute2(&attribute, VIDEO_FORMAT, 0, WIDTH, HEIGHT, 0, 0, 0);
    rc = sceVideoOutSubmitChangeBufferAttribute2(handle, 0, &attribute, 0, NULL);
    report("R4_PATTERN_CALL SubmitChangeBufferAttribute2=%d target_format=0x%016" PRIx64 "\n", rc,
           VIDEO_FORMAT);
    if (rc != 0) {
        goto cleanup;
    }
#endif
    report("%s\n",
           "R4_PATTERN_READY control=192.168.2.2:9040 commands=still,motion,status,clock,stop");
    while (!stop) {
        uint64_t before = 0;
        uint64_t after = 0;
        if (control_poll(listener, &motion, &stop) != 0 || monotonic_us(&before) != 0) {
            perror("R4_PATTERN bounded control/clock");
            goto cleanup;
        }
        if (stop || before - start >= maximum_us) {
            break;
        }
        unsigned index = frame % 2U;
        uint32_t *destination = buffers[index].data;
        uint32_t source_frame = motion ? frame : 0U;
        for (uint32_t y = 0; y < HEIGHT; ++y) {
            for (uint32_t x = 0; x < WIDTH; ++x) {
                destination[tile_offset(x, y)] = pattern((Pixel){x, y}, source_frame);
            }
        }
        /* Match SDL_ps5tilemap.c's store fence before GPU scanout. The exact
         * source-submit boundary follows CPU drawing; it is not HDMI exposure.
         */
        _mm_sfence();
        if (monotonic_us(&before) != 0) {
            perror("R4_PATTERN submit clock");
            goto cleanup;
        }
        rc = sceVideoOutSubmitFlip(handle, (int)index, 1, source_frame);
        if (rc != 0) {
            report("R4_PATTERN_CALL SubmitFlip=%d\n", rc);
            goto cleanup;
        }
        struct kevent event;
        int count = 0;
        unsigned wait_us = 500000;
        rc = sceKernelWaitEqueue(queue, &event, 1, &count, &wait_us);
        if (rc != 0 || count != 1 || monotonic_us(&after) != 0) {
            report("R4_PATTERN_CALL WaitEqueue=%d count=%d\n", rc, count);
            goto cleanup;
        }
#ifdef TEST_PATTERN_HDR_SWITCH
        if (frame == 0U) {
            VideoOutputStatus status = {0};
            rc = sceVideoOutGetOutputStatus(handle, &status);
            report("R4_PATTERN_OUTPUT status=%d dynamic_range=%u flags=0x%016" PRIx64
                   " resolution=%u refresh_code=%" PRIu64 "\n",
                   rc, status.dynamic_range, status.flags, status.resolution, status.refresh_rate);
            if (rc != 0 || !(status.dynamic_range == 2U || (status.flags & 1U) != 0U)) {
                goto cleanup;
            }
        }
#endif
        report("R4_PATTERN_FRAME sequence=%u source_frame=%u source_submit_us=%" PRIu64
               " source_retired_us=%" PRIu64 "\n",
               frame, source_frame, before, after);
        if (fflush(stdout) != 0 || output_failed) {
            goto cleanup;
        }
        ++frame;
    }
    result = 0;
cleanup:
    if (event_added) {
        int status = sceVideoOutDeleteFlipEvent(queue, handle);
        report("R4_PATTERN_CLEANUP DeleteFlipEvent=%d\n", status);
        if (status != 0) {
            result = 1;
        }
    }
    if (handle >= 0) {
        sceVideoOutClose(handle);
        report("%s\n", "R4_PATTERN_CLEANUP VideoOutClose called; ABI has no return status");
    }
    if (queue != NULL) {
        int status = sceKernelDeleteEqueue(queue);
        report("R4_PATTERN_CLEANUP DeleteEqueue=%d\n", status);
        if (status != 0) {
            result = 1;
        }
    }
    if (mapped) {
        int status = munmap(base, MEMORY_BYTES);
        report("R4_PATTERN_CLEANUP munmap=%d\n", status);
        if (status != 0) {
            result = 1;
        }
    }
    if (allocated) {
        int status = sceKernelReleaseDirectMemory(physical, MEMORY_BYTES);
        report("R4_PATTERN_CLEANUP ReleaseDirectMemory=%d\n", status);
        if (status != 0) {
            result = 1;
        }
    }
    if (listener >= 0 && close(listener) != 0) {
        perror("R4_PATTERN close listener");
        result = 1;
    }
    report("R4_PATTERN_END result=%d frames=%u\n", result, frame);
    return output_failed ? 1 : result;
}
#ifdef TEST_PATTERN_FOREGROUND_NATIVE
int SDL_main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    return native_main();
}
#else
int main(void)
{
    return native_main();
}
#endif
#endif /* TEST_PATTERN_SDL */
#endif /* TEST_PATTERN_NETWORK */
#endif /* TEST_PATTERN_HOST */
