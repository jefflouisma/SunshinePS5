/* NONSHIPPING foreground pad tester. Reads the foreground user's standard pad
 * with scePadReadState (a payload cannot; a foreground application can) and
 * streams each 120-byte ScePadData as hex over TCP, so the virtual pad's
 * buttons, touch points and motion can be checked against what a game reads.
 * The screen shows a minimal live view to confirm the app is in front:
 * one box per button bit, the touchpad with up to two points, and bars for
 * the sticks, triggers, gyro and acceleration.
 *
 * Control: TCP 9041. Connect to receive "PAD <seq> <rc> <hex>" lines at
 * ~30 Hz; send "stop\n" to exit. Exits by itself after 15 minutes.
 * Build: tools/ps5-probes/build-pad-tester.sh (foreground SDL_main glue).
 */
#include <sys/types.h> /* before sys/event.h, which needs u_int */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <SDL_main.h>

#define WIDTH 1920U
#define HEIGHT 1080U
#define BUFFER_BYTES ((size_t)16 * 1024U * 1024U)
#define MEMORY_BYTES (2U * BUFFER_BYTES)
#define VIDEO_FORMAT UINT64_C(0x8000000022000000)
#define PAD_BYTES 120U
#define CONTROL_PORT 9041
#define MAXIMUM_US UINT64_C(900000000)

typedef struct {
    void *data;
    uint64_t reserved[3];
} VideoBuffer;
typedef struct {
    uint8_t reserved[80];
} VideoAttribute;

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
int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(int32_t *);
int scePadInit(void);
int scePadOpen(int32_t, int32_t, int32_t, const void *);
int scePadReadState(int32_t, void *);
int scePadSetMotionSensorState(int32_t, bool);

static uint64_t monotonic_us(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000) + (uint64_t)now.tv_nsec / UINT64_C(1000);
}

/* Same sourced 512x128 tile address permutation as test_pattern.c. */
static size_t tile_offset(uint32_t x, uint32_t y)
{
    static const uint32_t x_masks[9] = {1, 2, 32, 64, 2176, 512, 8448, 16384, 32768};
    static const uint32_t y_masks[7] = {4, 8, 16, 1088, 128, 256, 4608};
    uint32_t offset = 0;
    for (unsigned bit = 0; bit < 9U; ++bit)
        if (((x % 512U) >> bit) & 1U)
            offset ^= x_masks[bit];
    for (unsigned bit = 0; bit < 7U; ++bit)
        if (((y % 128U) >> bit) & 1U)
            offset ^= y_masks[bit];
    return (size_t)(x / 512U) * 65536U + (size_t)(y / 128U) * 128U * WIDTH + offset;
}

static uint32_t rgb(uint32_t red, uint32_t green, uint32_t blue)
{
    return UINT32_C(0xff000000) | (blue << 16) | (green << 8) | red;
}

static void fill(uint32_t *frame, int x0, int y0, int x1, int y1, uint32_t color)
{
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > (int)WIDTH ? (int)WIDTH : x1;
    y1 = y1 > (int)HEIGHT ? (int)HEIGHT : y1;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            frame[tile_offset((uint32_t)x, (uint32_t)y)] = color;
}

static float float_at(const uint8_t *pad, size_t offset)
{
    float value;
    memcpy(&value, pad + offset, sizeof(value));
    return value;
}

/* Horizontal bar centred at mid-screen-of-row: value in [-1, 1]. */
static void bar(uint32_t *frame, int y, float value, uint32_t color)
{
    if (!(value > -1.0f))
        value = -1.0f;
    if (!(value < 1.0f))
        value = 1.0f;
    int centre = 1160, half = 640;
    int end = centre + (int)(value * (float)half);
    fill(frame, centre - half, y, centre + half, y + 24, rgb(40, 40, 40));
    fill(frame, value < 0 ? end : centre, y, value < 0 ? centre : end, y + 24, color);
    fill(frame, centre - 1, y - 4, centre + 1, y + 28, rgb(200, 200, 200));
}

static void draw(uint32_t *frame, const uint8_t *pad, bool valid)
{
    fill(frame, 0, 0, (int)WIDTH, (int)HEIGHT, valid ? rgb(16, 16, 24) : rgb(80, 0, 0));
    uint32_t buttons;
    memcpy(&buttons, pad, sizeof(buttons));
    for (int bit = 0; bit < 32; ++bit) {
        int x = 40 + bit * 58;
        bool on = (buttons >> bit) & 1U;
        fill(frame, x, 40, x + 50, 110, on ? rgb(80, 220, 80) : rgb(60, 60, 60));
    }
    /* Touchpad 1920x1080 scaled to 640x360 at (40, 160). */
    fill(frame, 40, 160, 680, 520, rgb(50, 50, 70));
    uint8_t count = pad[0x34];
    for (int i = 0; i < 2 && i < count; ++i) {
        uint16_t tx, ty;
        memcpy(&tx, pad + 0x3c + 8 * i, sizeof(tx));
        memcpy(&ty, pad + 0x3e + 8 * i, sizeof(ty));
        int px = 40 + tx / 3, py = 160 + ty / 3;
        fill(frame, px - 12, py - 12, px + 12, py + 12,
             i == 0 ? rgb(255, 200, 0) : rgb(0, 200, 255));
    }
    /* Sticks and triggers. */
    for (int i = 0; i < 4; ++i)
        bar(frame, 160 + i * 40, ((float)pad[4 + i] - 128.0f) / 128.0f, rgb(120, 160, 255));
    bar(frame, 330, (float)pad[8] / 255.0f, rgb(255, 120, 120));
    bar(frame, 370, (float)pad[9] / 255.0f, rgb(255, 120, 120));
    /* Acceleration (g) and angular velocity (rad/s, scaled by 1/4). */
    for (int i = 0; i < 3; ++i)
        bar(frame, 440 + i * 40, float_at(pad, 0x1c + 4 * (size_t)i), rgb(255, 220, 80));
    for (int i = 0; i < 3; ++i)
        bar(frame, 580 + i * 40, float_at(pad, 0x28 + 4 * (size_t)i) / 4.0f, rgb(200, 120, 255));
    /* Orientation quaternion x, y, z, w. */
    for (int i = 0; i < 4; ++i)
        bar(frame, 720 + i * 40, float_at(pad, 0x0c + 4 * (size_t)i), rgb(120, 255, 200));
}

static int control_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int on = 1;
    struct sockaddr_in address = {0};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(CONTROL_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0 ||
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 1) != 0 ||
        fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

/* Streams to at most one client; returns true when it asked to stop. */
static bool serve(int listener, int *client, uint32_t sequence, int rc, const uint8_t *pad)
{
    if (*client < 0) {
        *client = accept(listener, NULL, NULL);
        if (*client >= 0)
            (void)fcntl(*client, F_SETFL, O_NONBLOCK);
    }
    if (*client < 0)
        return false;
    char command[16] = {0};
    ssize_t count = read(*client, command, sizeof(command) - 1U);
    if (count == 0 || (count > 0 && strncmp(command, "stop", 4) == 0)) {
        bool stop = count > 0;
        (void)close(*client);
        *client = -1;
        return stop;
    }
    char line[32 + 2 * PAD_BYTES + 2];
    int used = snprintf(line, sizeof(line), "PAD %" PRIu32 " %d ", sequence, rc);
    for (unsigned i = 0; i < PAD_BYTES; ++i)
        used += snprintf(line + used, sizeof(line) - (size_t)used, "%02x", pad[i]);
    line[used++] = '\n';
    if (send(*client, line, (size_t)used, MSG_NOSIGNAL) < 0 && errno != EAGAIN &&
        errno != EWOULDBLOCK) {
        (void)close(*client);
        *client = -1;
    }
    return false;
}

static int tester_main(void)
{
    int result = 1, handle = -1, listener = -1, client = -1, pad = -1;
    intptr_t physical = 0;
    void *base = NULL;
    struct kevent *queue = NULL;
    bool allocated = false, mapped = false, event_added = false;
    uint64_t start = monotonic_us();
    (void)signal(SIGPIPE, SIG_IGN);
    listener = control_socket();
    int32_t user = -1;
    int user_rc = sceUserServiceInitialize(NULL);
    int foreground_rc = sceUserServiceGetForegroundUser(&user);
    int init_rc = scePadInit();
    pad = scePadOpen(user, 0, 0, NULL);
    /* Motion fields stay zero unless the application enables the sensor, as a
     * gyro-aiming game would. */
    int motion_rc = pad >= 0 ? scePadSetMotionSensorState(pad, true) : -1;
    printf("PAD_TESTER_BEGIN pid=%d listener=%d user=%d user_rc=0x%x fg_rc=0x%x init=0x%x "
           "open=0x%x motion=0x%x\n",
           getpid(), listener, (int)user, (unsigned)user_rc, (unsigned)foreground_rc,
           (unsigned)init_rc, (unsigned)pad, (unsigned)motion_rc);
    (void)sceSystemServiceHideSplashScreen();
    handle = sceVideoOutOpen(0xff, 0, 0, NULL);
    if (handle < 0)
        goto cleanup;
    if (sceKernelAllocateMainDirectMemory(MEMORY_BYTES, 0x20000, 3, &physical) != 0)
        goto cleanup;
    allocated = true;
    if (sceKernelMapDirectMemory(&base, MEMORY_BYTES, 0x33, 0, physical, 0x20000) != 0 ||
        base == NULL)
        goto cleanup;
    mapped = true;
    if (sceKernelCreateEqueue(&queue, "pad tester flip queue") != 0 ||
        sceVideoOutAddFlipEvent(queue, handle, NULL) != 0)
        goto cleanup;
    event_added = true;
    if (sceVideoOutSetFlipRate(handle, 0) != 0)
        goto cleanup;
    VideoBuffer buffers[2] = {{.data = base}, {.data = (uint8_t *)base + BUFFER_BYTES}};
    VideoAttribute attribute = {0};
    sceVideoOutSetBufferAttribute2(&attribute, VIDEO_FORMAT, 0, WIDTH, HEIGHT, 0, 0, 0);
    if (sceVideoOutRegisterBuffers2(handle, 0, 0, buffers, 2, &attribute, 0, NULL) != 0)
        goto cleanup;
    printf("PAD_TESTER_READY control=:%d\n", CONTROL_PORT);
    (void)fflush(stdout);
    uint8_t state[PAD_BYTES];
    uint32_t sequence = 0;
    bool stop = false;
    while (!stop && monotonic_us() - start < MAXIMUM_US) {
        memset(state, 0, sizeof(state));
        int rc = pad >= 0 ? scePadReadState(pad, state) : pad;
        if (listener >= 0)
            stop = serve(listener, &client, sequence, rc, state);
        unsigned index = sequence % 2U;
        draw(buffers[index].data, state, rc == 0);
        __builtin_ia32_sfence();
        if (sceVideoOutSubmitFlip(handle, (int)index, 1, sequence) != 0)
            goto cleanup;
        struct kevent event;
        int count = 0;
        unsigned wait_us = 500000;
        if (sceKernelWaitEqueue(queue, &event, 1, &count, &wait_us) != 0)
            goto cleanup;
        ++sequence;
    }
    result = 0;
cleanup:
    if (client >= 0)
        (void)close(client);
    if (event_added)
        (void)sceVideoOutDeleteFlipEvent(queue, handle);
    if (handle >= 0)
        sceVideoOutClose(handle);
    if (queue != NULL)
        (void)sceKernelDeleteEqueue(queue);
    if (mapped)
        (void)munmap(base, MEMORY_BYTES);
    if (allocated)
        (void)sceKernelReleaseDirectMemory(physical, MEMORY_BYTES);
    if (listener >= 0)
        (void)close(listener);
    printf("PAD_TESTER_END result=%d\n", result);
    return result;
}

int SDL_main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    return tester_main();
}
