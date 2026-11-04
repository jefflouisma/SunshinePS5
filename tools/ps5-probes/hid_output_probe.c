/* Non-shipping probe: looks for where the /dev/hid driver exposes output
 * (vibration) destined for a virtual pad to the pad's owner. Read-shaped
 * requests only; never the generic command ioctl 0x80104850 (its ops include
 * device deletion).
 *
 *   -DPAD_HANDLE=<h>   virtual pad (pad_census) that a foreground app vibrates
 *
 * Takes three snapshots six seconds apart (drive "vib L S" on the pad tester
 * between them) and prints every candidate whose return value or buffer
 * changes, plus a table of return values.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifndef PAD_HANDLE
#error "define PAD_HANDLE"
#endif

enum { BUFFER = 256, MODES = 32, CANDIDATES = MODES + 11 };

typedef struct {
    uint8_t mode;
    uint8_t reserved[3];
    uint32_t handle;
    void *buffer;
} read_request; /* 0xc0184852 as libScePad issues it */

typedef struct {
    uint32_t handle;
    uint32_t reserved;
    void *in;
    void *out;
} pad_request; /* 24-byte form used by add/insert/vibration */

typedef struct {
    uint32_t handle;
    uint32_t reserved;
    void *buffer;
} short_request; /* 16-byte form */

typedef struct {
    unsigned long code;
    int mode;
    int rc, error;
    uint8_t data[BUFFER];
} result;

static const unsigned long long_codes[] = {0x8018482bUL, 0x8018482eUL, 0xc018482bUL, 0xc018482eUL,
                                           0xc0184853UL};
static const unsigned long short_codes[] = {0x8010482bUL, 0x8010482eUL, 0xc0104870UL,
                                            0xc0104871UL, 0xc010487bUL, 0xc010487cUL};

static void snapshot(int fd, result *results)
{
    size_t n = 0;
    for (int mode = 0; mode < MODES; ++mode, ++n) {
        result *r = &results[n];
        memset(r->data, 0, sizeof(r->data));
        read_request request = {
            .mode = (uint8_t)mode, .handle = (uint32_t)(PAD_HANDLE), .buffer = r->data};
        r->code = 0xc0184852UL;
        r->mode = mode;
        r->rc = ioctl(fd, r->code, &request);
        r->error = r->rc != 0 ? errno : 0;
    }
    for (size_t i = 0; i < sizeof(long_codes) / sizeof(long_codes[0]); ++i, ++n) {
        result *r = &results[n];
        static uint8_t in[BUFFER];
        memset(r->data, 0, sizeof(r->data));
        pad_request request = {(uint32_t)(PAD_HANDLE), 0, in, r->data};
        r->code = long_codes[i];
        r->mode = -1;
        r->rc = ioctl(fd, r->code, &request);
        r->error = r->rc != 0 ? errno : 0;
    }
    for (size_t i = 0; i < sizeof(short_codes) / sizeof(short_codes[0]); ++i, ++n) {
        result *r = &results[n];
        memset(r->data, 0, sizeof(r->data));
        short_request request = {(uint32_t)(PAD_HANDLE), 0, r->data};
        r->code = short_codes[i];
        r->mode = -1;
        r->rc = ioctl(fd, r->code, &request);
        r->error = r->rc != 0 ? errno : 0;
    }
}

static void compare(const char *label, const result *a, const result *b)
{
    for (size_t n = 0; n < CANDIDATES; ++n) {
        if (a[n].rc == b[n].rc && a[n].error == b[n].error &&
            memcmp(a[n].data, b[n].data, BUFFER) == 0)
            continue;
        printf("HID_OUT diff %s code=0x%lx mode=%d rc=%d/%d errno=%d/%d bytes:", label, a[n].code,
               a[n].mode, a[n].rc, b[n].rc, a[n].error, b[n].error);
        for (int i = 0; i < BUFFER; ++i)
            if (a[n].data[i] != b[n].data[i])
                printf(" [%d]%02x>%02x", i, a[n].data[i], b[n].data[i]);
        printf("\n");
    }
}

static void sleep_s(int seconds)
{
    struct timespec delay = {seconds, 0};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

int main(void)
{
    int fd = open("/dev/hid", O_RDWR);
    if (fd < 0) {
        printf("HID_OUT open errno=%d\n", errno);
        return 1;
    }
    static result first[CANDIDATES], second[CANDIDATES], third[CANDIDATES];
    snapshot(fd, first);
    for (size_t n = 0; n < CANDIDATES; ++n) {
        int nonzero = 0;
        for (int i = 0; i < BUFFER; ++i)
            nonzero += first[n].data[i] != 0;
        printf("HID_OUT base code=0x%lx mode=%d rc=%d errno=%d nonzero=%d\n", first[n].code,
               first[n].mode, first[n].rc, first[n].error, nonzero);
    }
    printf("HID_OUT phase=vibrate-now\n");
    (void)fflush(stdout);
    sleep_s(6);
    snapshot(fd, second);
    printf("HID_OUT phase=stop-now\n");
    (void)fflush(stdout);
    sleep_s(6);
    snapshot(fd, third);
    compare("off->on", first, second);
    compare("on->off", second, third);
    printf("HID_OUT done\n");
    close(fd);
    return 0;
}
