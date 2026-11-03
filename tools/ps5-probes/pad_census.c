/* Nonshipping diagnostic payload: list registered virtual pad handles.
 *
 * /dev/hid report handles are global: a host can report on a pad another
 * process registered (takeover reuse depends on this). A neutral report is
 * therefore a harmless existence check. Handles observed on 13.60 look like
 * (sequence << 16) | (kind << 8) | slot, e.g. 983819 = 0x000F030B.
 *
 * Build-time actions (default: census only):
 *   -DPAD_CENSUS_PRESS=<handle>   press and release a button on that handle
 *   -DPAD_CENSUS_BUTTON=<mask>    HID button mask for PRESS (default Cross)
 *   -DPAD_CENSUS_REMOVE=<handle>  remove that handle (operation 5)
 *   -DPAD_CENSUS_INTERACTIVE      accept one-line commands on TCP 47992
 *                                 (see interactive()). Link -lSceUserService
 *                                 -lScePad.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define HID_REPORT 0x8018482cUL
#define HID_REMOVE 0x80104850UL
#ifndef PAD_CENSUS_BUTTON
#define PAD_CENSUS_BUTTON 0x00004000U
#endif

typedef struct {
    uint32_t operation_or_handle;
    uint32_t reserved;
    void *input;
    void *output;
} pad_request;

typedef struct {
    uint8_t operation;
    uint8_t padding[7];
    uint32_t handle;
    uint32_t reserved;
} remove_request;

static void neutral(uint8_t report[160], uint32_t buttons)
{
    const float orientation = 1.0f;
    memset(report, 0, 160);
    report[0x10] = report[0x11] = report[0x12] = report[0x13] = 0x80;
    report[0x17] = 1;
    memcpy(report + 0x84, &orientation, sizeof(orientation));
    for (unsigned i = 0; i < 4; ++i)
        report[0x0c + i] = (uint8_t)(buttons >> (8U * i));
}

static int send_report(int fd, uint32_t handle, uint32_t buttons)
{
    uint8_t report[160];
    neutral(report, buttons);
    pad_request request = {handle, 0, report, NULL};
    return ioctl(fd, HID_REPORT, &request);
}

static __attribute__((unused)) void sleep_ms(long ms)
{
    struct timespec delay = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

#ifdef PAD_CENSUS_INTERACTIVE
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>

int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int32_t *user_id);
int scePadInit(void);
int scePadSetLoginUserNumber(int32_t handle, int32_t user_number);

#define HID_ADD 0xc018482aUL

static int32_t add_pad(int fd, int32_t user)
{
    uint8_t descriptor[168] = {0};
    descriptor[0] = 3;
    for (unsigned i = 0; i < 4; ++i)
        descriptor[0x1c + i] = (uint8_t)((uint32_t)user >> (8U * i));
    descriptor[0x20] = 1;
    int32_t handle = -1;
    pad_request add = {3, 0, descriptor, &handle};
    if (ioctl(fd, HID_ADD, &add) != 0 || handle < 0)
        return -1;
    (void)scePadSetLoginUserNumber(handle, 1);
    return handle;
}

static int remove_pad(int fd, int32_t handle)
{
    remove_request remove = {.operation = 5, .handle = (uint32_t)handle};
    return ioctl(fd, HID_REMOVE, &remove);
}

/* Commands: "add", "press <mask> [handle]", "remove [handle]", "quit".
 * Without a handle, press/remove use the most recently added pad. quit
 * removes every pad this probe added. */
static int interactive(int fd)
{
    int32_t user = -1;
    if (sceUserServiceInitialize(NULL) != 0 || sceUserServiceGetForegroundUser(&user) != 0)
        return 1;
    (void)scePadInit();
    int32_t pads[16];
    unsigned count_pads = 0;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(47992);
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0)
        return 1;
    printf("PAD_CENSUS listening user=%d\n", (int)user);
    (void)fflush(stdout);
    for (;;) {
        for (unsigned i = 0; i < count_pads; ++i)
            (void)send_report(fd, (uint32_t)pads[i], 0);
        struct pollfd ready = {listener, POLLIN, 0};
        if (poll(&ready, 1, 50) <= 0)
            continue;
        int client = accept(listener, NULL, NULL);
        if (client < 0)
            continue;
        char line[64] = {0};
        ssize_t count = read(client, line, sizeof(line) - 1);
        char reply[96];
        int length = 0;
        int32_t target = count_pads > 0 ? pads[count_pads - 1] : -1;
        if (count >= 3 && strncmp(line, "add", 3) == 0) {
            int32_t handle = count_pads < 16 ? add_pad(fd, user) : -1;
            if (handle >= 0)
                pads[count_pads++] = handle;
            length = snprintf(reply, sizeof(reply), "added %d hex=0x%08x\n", (int)handle,
                              (unsigned)handle);
        } else if (count > 6 && strncmp(line, "press ", 6) == 0) {
            char *rest = NULL;
            uint32_t mask = (uint32_t)strtoul(line + 6, &rest, 0);
            if (rest != NULL && *rest == ' ')
                target = (int32_t)strtol(rest + 1, NULL, 0);
            int down = 0;
            for (int i = 0; i < 6; ++i) {
                down |= send_report(fd, (uint32_t)target, mask);
                sleep_ms(25);
            }
            int up = send_report(fd, (uint32_t)target, 0);
            length = snprintf(reply, sizeof(reply), "pressed 0x%x on %d down=%d up=%d\n", mask,
                              (int)target, down, up);
        } else if (count >= 6 && strncmp(line, "remove", 6) == 0) {
            if (count > 7)
                target = (int32_t)strtol(line + 7, NULL, 0);
            int rc = remove_pad(fd, target);
            for (unsigned i = 0; i < count_pads; ++i) {
                if (pads[i] == target) {
                    pads[i] = pads[--count_pads];
                    break;
                }
            }
            length = snprintf(reply, sizeof(reply), "removed %d rc=%d\n", (int)target, rc);
        } else if (count >= 4 && strncmp(line, "quit", 4) == 0) {
            while (count_pads > 0)
                (void)remove_pad(fd, pads[--count_pads]);
            (void)write(client, "bye\n", 4);
            (void)close(client);
            break;
        } else {
            length = snprintf(reply, sizeof(reply), "unknown\n");
        }
        if (length > 0)
            (void)write(client, reply, (size_t)length);
        (void)close(client);
    }
    (void)close(listener);
    return 0;
}
#endif

int main(void)
{
    int fd = open("/dev/hid", O_RDWR);
    printf("PAD_CENSUS pid=%ld fd=%d errno=%d\n", (long)getpid(), fd, fd < 0 ? errno : 0);
    if (fd < 0)
        return 1;
    unsigned found = 0;
    for (uint32_t sequence = 0; sequence <= 0x60; ++sequence) {
        for (uint32_t kind = 0; kind <= 4; ++kind) {
            for (uint32_t slot = 0; slot <= 0x0f; ++slot) {
                uint32_t handle = (sequence << 16) | (kind << 8) | slot;
                if (handle == 0)
                    continue;
                if (send_report(fd, handle, 0) == 0) {
                    printf("PAD_CENSUS live handle=%u hex=0x%08x\n", handle, handle);
                    ++found;
                }
            }
        }
    }
    printf("PAD_CENSUS found=%u\n", found);
#ifdef PAD_CENSUS_INTERACTIVE
    (void)fflush(stdout);
    printf("PAD_CENSUS interactive=%d\n", interactive(fd));
#endif
#ifdef PAD_CENSUS_PRESS
    {
        uint32_t handle = (uint32_t)(PAD_CENSUS_PRESS);
        int down = send_report(fd, handle, PAD_CENSUS_BUTTON);
        int down_errno = errno;
        for (int i = 0; i < 8; ++i) {
            sleep_ms(25);
            (void)send_report(fd, handle, PAD_CENSUS_BUTTON);
        }
        int up = send_report(fd, handle, 0);
        printf("PAD_CENSUS press handle=%u button=0x%x down=%d errno=%d up=%d\n", handle,
               (unsigned)(PAD_CENSUS_BUTTON), down, down_errno, up);
    }
#endif
#ifdef PAD_CENSUS_REMOVE
    {
        uint32_t handle = (uint32_t)(PAD_CENSUS_REMOVE);
        (void)send_report(fd, handle, 0);
        remove_request remove = {.operation = 5, .handle = handle};
        int rc = ioctl(fd, HID_REMOVE, &remove);
        printf("PAD_CENSUS remove handle=%u rc=%d errno=%d after=%d\n", handle, rc,
               rc != 0 ? errno : 0, send_report(fd, handle, 0));
    }
#endif
    (void)fflush(stdout);
    (void)close(fd);
    return 0;
}
