/* Non-shipping probe: maps the 160-byte /dev/hid virtual-pad report onto the
 * ScePadData a game reads. Writes test reports to an existing virtual pad and
 * reads the foreground user's standard pad back through libScePad after each,
 * printing which ScePadData bytes changed from the neutral reading.
 *
 *   -DPAD_HANDLE=<h>   virtual pad to write (from pad_census)
 *
 * Test reports never set face, D-pad, shoulder or Options bits, so a game or
 * menu in the foreground is not driven by them. Build with -lSceUserService
 * -lScePad.
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

#define HID_REPORT 0x8018482cUL

typedef struct {
    uint32_t handle;
    uint32_t reserved;
    void *input;
    void *output;
} pad_request;

int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int32_t *user_id);
int scePadInit(void);
int scePadSetProcessPrivilege(int privilege);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void *param);
int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void *data);

enum { REPORT_BYTES = 160, PAD_DATA_BYTES = 120 };

static void sleep_ms(long ms)
{
    struct timespec delay = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static void neutral(uint8_t *report)
{
    const float w = 1.0f;
    memset(report, 0, REPORT_BYTES);
    report[0x10] = report[0x11] = report[0x12] = report[0x13] = 0x80;
    report[0x17] = 1;
    memcpy(report + 0x84, &w, sizeof(w));
}

static int write_report(int fd, const uint8_t *report)
{
    pad_request request = {(uint32_t)(PAD_HANDLE), 0, (void *)report, NULL};
    return ioctl(fd, HID_REPORT, &request);
}

/* Holds the report for ~150 ms (as a live controller would) and reads once. */
static int sample(int fd, int pad, const uint8_t *report, uint8_t *data)
{
    for (int i = 0; i < 6; ++i) {
        (void)write_report(fd, report);
        sleep_ms(25);
    }
    memset(data, 0xee, PAD_DATA_BYTES);
    return scePadReadState(pad, data);
}

static void dump(const char *label, const uint8_t *data)
{
    printf("READBACK %-14s", label);
    for (int i = 0; i < PAD_DATA_BYTES; ++i)
        printf("%s%02x", i % 4 == 0 ? " " : "", data[i]);
    printf("\n");
}

static void diff(const char *label, const uint8_t *base, const uint8_t *data)
{
    printf("READBACK diff %-10s", label);
    int changed = 0;
    for (int i = 0; i < PAD_DATA_BYTES; ++i) {
        /* Skip the timestamp (changes every read). */
        if (i >= 80 && i < 88)
            continue;
        if (base[i] != data[i]) {
            printf(" [%d]=%02x>%02x", i, base[i], data[i]);
            ++changed;
        }
    }
    printf(changed ? "\n" : " none\n");
}

int main(void)
{
    int32_t user = -1;
    int rc_user = sceUserServiceInitialize(NULL);
    int rc_fg = sceUserServiceGetForegroundUser(&user);
    int rc_init = scePadInit();
    int rc_priv = scePadSetProcessPrivilege(1);
    int open0 = scePadOpen(user, 0, 0, NULL);
    int get0 = scePadGetHandle(user, 0, 0);
    printf("READBACK user=%d rc_user=0x%x rc_fg=0x%x init=0x%x priv=0x%x open=0x%x get=0x%x\n",
           (int)user, (unsigned)rc_user, (unsigned)rc_fg, (unsigned)rc_init, (unsigned)rc_priv,
           (unsigned)open0, (unsigned)get0);
    int pad = open0 >= 0 ? open0 : get0;
    int fd = open("/dev/hid", O_RDWR);
    if (pad < 0 || fd < 0) {
        printf("READBACK unavailable pad=%d fd=%d errno=%d\n", pad, fd, errno);
        return 1;
    }
    uint8_t report[REPORT_BYTES], base[PAD_DATA_BYTES], data[PAD_DATA_BYTES];
    neutral(report);
    int rc = sample(fd, pad, report, base);
    printf("READBACK read rc=0x%x\n", (unsigned)rc);
    dump("neutral", base);

    /* Candidate button bits outside the known face/D-pad/shoulder set. */
    static const uint32_t bits[] = {0x00020000U, 0x00040000U, 0x00080000U, 0x00100000U,
                                    0x00200000U, 0x00400000U, 0x00800000U, 0x01000000U};
    for (size_t b = 0; b < sizeof(bits) / sizeof(bits[0]); ++b) {
        neutral(report);
        memcpy(report + 0x0c, &bits[b], sizeof(bits[b]));
        (void)sample(fd, pad, report, data);
        char label[16];
        (void)snprintf(label, sizeof(label), "btn%08x", (unsigned)bits[b]);
        diff(label, base, data);
    }
    /* Marker bytes across the unexplored region 0x18..0x77 (touch candidates). */
    for (int offset = 0x18; offset < 0x78; offset += 4) {
        neutral(report);
        report[offset] = 0x11;
        report[offset + 1] = 0x22;
        report[offset + 2] = 0x33;
        report[offset + 3] = 0x44;
        (void)sample(fd, pad, report, data);
        char label[16];
        (void)snprintf(label, sizeof(label), "off%02x", offset);
        diff(label, base, data);
    }
    /* Motion block: eight distinct floats. */
    neutral(report);
    for (int i = 0; i < 8; ++i) {
        float value = (float)(i + 1) * 0.125f;
        memcpy(report + 0x78 + 4 * i, &value, sizeof(value));
    }
    (void)sample(fd, pad, report, data);
    diff("motion", base, data);
    dump("motion", data);
    /* Tail region 0x98..0x9f. */
    for (int offset = 0x98; offset < REPORT_BYTES; offset += 4) {
        neutral(report);
        report[offset] = 0x55;
        report[offset + 1] = 0x66;
        (void)sample(fd, pad, report, data);
        char label[16];
        (void)snprintf(label, sizeof(label), "off%02x", offset);
        diff(label, base, data);
    }
    neutral(report);
    (void)sample(fd, pad, report, data);
    diff("final", base, data);
    close(fd);
    return 0;
}
