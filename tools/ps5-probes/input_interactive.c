/* NONSHIPPING, firmware 13.60 only. Explicit navigation commands over loader stdin.
 * HID ABI: leolobato/ps5-mcp 84c0f7b316772b13cd62e0121c154afc3ed2482c,
 * payload/system_ps5.c, matching input_system.c. No automatic assignment pulse.
 * Platform calls have no proven completion deadline. SDK privilege restoration
 * and visual delivery/release remain independently unverified.
 */
#define _POSIX_C_SOURCE 200809L
#include "input_system.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum interactive_op { I_INIT, I_USER, I_OPEN, I_ADD, I_REPORT, I_HOLD, I_REMOVE, I_CLOSE, I_TERM };
struct interactive_api {
    int (*call)(enum interactive_op op, int32_t *value, void *bytes);
};
struct interactive_limits {
    uint64_t idle_ms, total_ms, output_ms;
};
static volatile sig_atomic_t interactive_stopped;

static bool interactive_now(uint64_t *ms)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0)
        return false;
    *ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    return true;
}

static bool interactive_deadline(uint64_t duration, uint64_t *deadline)
{
    uint64_t now;
    if (!interactive_now(&now) || duration > UINT64_MAX - now)
        return false;
    *deadline = now + duration;
    return true;
}

/* poll event mask and monotonic deadline intentionally retain their native types. */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static bool interactive_wait(int fd, short events, uint64_t deadline, bool cleanup)
{
    for (;;) {
        uint64_t now;
        if ((!cleanup && interactive_stopped) || !interactive_now(&now) || now >= deadline)
            return false;
        /* Short slices also observe signals if the platform restarts poll. */
        int remaining = deadline - now > 50 ? 50 : (int)(deadline - now);
        struct pollfd p = {fd, events, 0};
        int rc = poll(&p, 1, remaining);
        if (rc < 0 && errno != EINTR)
            return false;
        if (rc > 0) {
            if (p.revents & (POLLERR | POLLNVAL))
                return false;
            return (p.revents & (events | POLLHUP)) != 0;
        }
    }
}

static bool interactive_output(int fd, const char *text, uint64_t deadline, bool cleanup)
{
    size_t left = strlen(text);
    while (left != 0) {
        if (!interactive_wait(fd, POLLOUT, deadline, cleanup))
            return false;
        ssize_t n = write(fd, text, left);
        if (n > 0) {
            text += n;
            left -= (size_t)n;
        } else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            return false;
        }
    }
    return true;
}

/* No read-ahead: a command cannot cause an extra report before its predecessor ACK. */
static bool interactive_line(int fd, char line[48], uint64_t deadline)
{
    size_t used = 0;
    while (used < 47) {
        if (!interactive_wait(fd, POLLIN, deadline, false))
            return false;
        unsigned char c;
        ssize_t n = read(fd, &c, 1);
        if (n == 0)
            return false;
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return false;
        }
        if (c == '\n') {
            line[used] = '\0';
            return true;
        }
        if (c < 0x20 || c > 0x7e)
            return false;
        line[used++] = (char)c;
    }
    return false;
}

/* Exact grammar rejects signs, leading zeroes, extra spaces/tokens and raw masks. */
static int interactive_parse(const char *line, unsigned seq, uint32_t *mask)
{
    static const char *const actions[] = {"neutral",     "stop",        "pulse up",
                                          "pulse right", "pulse down",  "pulse left",
                                          "pulse cross", "pulse circle"};
    static const uint32_t masks[] = {0, 0, 0x10, 0x20, 0x40, 0x80, 0x4000, 0x2000};
    for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
        char expected[48];
        int n = snprintf(expected, sizeof(expected), "%u %s", seq, actions[i]);
        if (n > 0 && (size_t)n < sizeof(expected) && strcmp(line, expected) == 0) {
            *mask = masks[i];
            return i == 1 ? 2 : (i == 0 ? 0 : 1);
        }
    }
    return -1;
}

static void interactive_report(NeutralReport *report, uint32_t mask)
{
    memset(report, 0, sizeof(*report));
    for (unsigned i = 0; i < 4; ++i) {
        report->bytes[0x0c + i] = (uint8_t)(mask >> (8 * i));
        report->bytes[0x10 + i] = 0x80;
    }
    report->bytes[0x17] = 1;
    report->bytes[0x86] = 0x80;
    report->bytes[0x87] = 0x3f;
}

static bool interactive_emit(int output, const char *text, uint64_t total,
                             const struct interactive_limits *limits)
{
    uint64_t deadline;
    if (!interactive_deadline(limits->output_ms, &deadline))
        return false;
    return interactive_output(output, text, deadline < total ? deadline : total, false);
}

/* Input/output descriptor order follows interactive_run and standard streams. */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int interactive_session(uint32_t fw, const struct interactive_api *api, int input,
                               int output, const struct interactive_limits *limits)
{
    if ((fw & UINT32_C(0xffff0000)) != UINT32_C(0x13600000))
        return 3;
    bool initialized = false, opened = false, owned = false, failed = true;
    int32_t value = -1;
    uint64_t total;
    NeutralReport report;
    if (!interactive_deadline(limits->total_ms, &total) || interactive_stopped)
        return 1;
    if (api->call(I_INIT, &value, NULL) != 0)
        goto cleanup;
    initialized = true;
    if (interactive_stopped || api->call(I_USER, &value, NULL) != 0 || value < 0)
        goto cleanup;
    PadDescriptor descriptor = {{0}};
    descriptor.bytes[0] = 3;
    for (unsigned i = 0; i < 4; ++i)
        descriptor.bytes[0x1c + i] = (uint8_t)((uint32_t)value >> (8 * i));
    descriptor.bytes[0x20] = 1;
    if (interactive_stopped || api->call(I_OPEN, &value, NULL) != 0)
        goto cleanup;
    opened = true;
    value = -1;
    if (interactive_stopped || api->call(I_ADD, &value, &descriptor) != 0)
        goto cleanup;
    if (value < 0)
        goto cleanup;
    owned = true;
    interactive_report(&report, 0);
    if (interactive_stopped || api->call(I_REPORT, &value, &report) != 0)
        goto cleanup;
    char text[256];
    (void)snprintf(text, sizeof(text),
                   "READY version=1 pid=%ld firmware=0x%08" PRIx32
                   " neutral=accepted next=1 delivery=unverified\n",
                   (long)getpid(), fw);
    if (!interactive_emit(output, text, total, limits))
        goto cleanup;
    for (unsigned seq = 1; seq <= 32; ++seq) {
        char line[48];
        uint64_t idle, begin, release, end;
        uint32_t mask;
        if (!interactive_deadline(limits->idle_ms, &idle) ||
            !interactive_line(input, line, idle < total ? idle : total))
            goto cleanup;
        int action = interactive_parse(line, seq, &mask);
        if (action < 0 || interactive_stopped || !interactive_now(&begin) || begin >= total)
            goto cleanup;
        if (action == 2) {
            failed = false;
            break;
        }
        if (action == 1) {
            interactive_report(&report, mask);
            if (api->call(I_REPORT, &value, &report) != 0 || api->call(I_HOLD, &value, NULL) != 0)
                goto cleanup;
        }
        /* Always explicitly neutralize, including a repeated neutral command. */
        interactive_report(&report, 0);
        if (!interactive_now(&release) || api->call(I_REPORT, &value, &report) != 0 ||
            !interactive_now(&end) || interactive_stopped || end >= total)
            goto cleanup;
        (void)snprintf(text, sizeof(text),
                       "ACK seq=%u mask=0x%04" PRIx32 " neutral=accepted begin_ms=%" PRIu64
                       " release_ms=%" PRIu64 " end_ms=%" PRIu64 "\n",
                       seq, mask, begin, release, end);
        if (!interactive_emit(output, text, total, limits))
            goto cleanup;
    }
cleanup:;
    /* No output may delay these four attempts. Retain ownership across all errors. */
    int neutral_rc = 0, remove_rc = 0, close_rc = 0, term_rc = 0;
    if (owned) {
        interactive_report(&report, 0);
        neutral_rc = api->call(I_REPORT, &value, &report);
        remove_rc = api->call(I_REMOVE, &value, NULL);
    }
    if (opened)
        close_rc = api->call(I_CLOSE, &value, NULL);
    if (initialized)
        term_rc = api->call(I_TERM, &value, NULL);
    if (neutral_rc || remove_rc || close_rc || term_rc)
        failed = true;
    char done[256];
    (void)snprintf(done, sizeof(done),
                   "DONE code=%d owned=%d neutral_rc=%d remove_rc=%d close_rc=%d terminate_rc=%d "
                   "hardware_acceptance=incomplete\n",
                   failed ? 1 : 0, owned, neutral_rc, remove_rc, close_rc, term_rc);
    uint64_t deadline;
    if (!interactive_deadline(limits->output_ms, &deadline) ||
        !interactive_output(output, done, deadline, true))
        failed = true;
    return failed ? 1 : 0;
}

static void interactive_stop(int signo)
{
    (void)signo;
    interactive_stopped = 1;
}

/* Configure before acquiring any console resources; restore caller fd/signal state. */
static int interactive_run(uint32_t fw, const struct interactive_api *api, int input, int output,
                           const struct interactive_limits *limits)
{
    int in_flags = fcntl(input, F_GETFL), out_flags = fcntl(output, F_GETFL);
    if (in_flags < 0 || out_flags < 0)
        return 1;
    struct sigaction stop = {0}, ignore = {0}, old[3];
    const int signals[] = {SIGINT, SIGTERM, SIGPIPE};
    stop.sa_handler = interactive_stop;
    ignore.sa_handler = SIG_IGN;
    unsigned installed = 0;
    int result = 1;
    interactive_stopped = 0;
    if (sigemptyset(&stop.sa_mask) || sigemptyset(&ignore.sa_mask))
        return 1;
    for (; installed < 3; ++installed)
        if (sigaction(signals[installed], installed == 2 ? &ignore : &stop, &old[installed]) != 0)
            goto restore;
    if (fcntl(input, F_SETFL, in_flags | O_NONBLOCK) < 0 ||
        fcntl(output, F_SETFL, out_flags | O_NONBLOCK) < 0)
        goto restore;
    result = interactive_session(fw, api, input, output, limits);
restore:
    if (fcntl(input, F_SETFL, in_flags) < 0)
        result = 1;
    if (fcntl(output, F_SETFL, out_flags) < 0)
        result = 1;
    while (installed > 0) {
        --installed;
        if (sigaction(signals[installed], &old[installed], NULL) != 0)
            result = 1;
    }
    return result;
}

#ifndef R3_INTERACTIVE_TEST
#include <ps5/kernel.h>
#include <sys/ioctl.h>
int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(int32_t *);
int sceUserServiceTerminate(void);
_Static_assert(sizeof(PadRequest) == 24 && offsetof(PadRequest, input) == 8, "HID request ABI");
_Static_assert(sizeof(RemoveRequest) == 16 && offsetof(RemoveRequest, handle) == 8, "remove ABI");
static int interactive_hid = -1;
static int interactive_target(enum interactive_op op, int32_t *value, void *bytes)
{
    switch (op) {
    case I_INIT:
        return sceUserServiceInitialize(NULL);
    case I_USER:
        return sceUserServiceGetForegroundUser(value);
    case I_OPEN:
        interactive_hid = open("/dev/hid", O_RDWR);
        return interactive_hid < 0 ? -1 : 0;
    case I_ADD: {
        PadRequest req = {3, 0, bytes, value};
        return ioctl(interactive_hid, UINT32_C(0xc018482a), &req);
    }
    case I_REPORT: {
        PadRequest req = {(uint32_t)*value, 0, bytes, NULL};
        return ioctl(interactive_hid, UINT32_C(0x8018482c), &req);
    }
    case I_HOLD: {
        if (interactive_stopped)
            return -1;
        const struct timespec delay = {0, 120000000};
        return nanosleep(&delay, NULL);
    }
    case I_REMOVE: {
        RemoveRequest req = {.operation = 5, .handle = (uint32_t)*value};
        return ioctl(interactive_hid, UINT32_C(0x80104850), &req);
    }
    case I_CLOSE:
        return close(interactive_hid);
    case I_TERM:
        return sceUserServiceTerminate();
    }
    return -1;
}
int main(void)
{
    const struct interactive_api api = {interactive_target};
    const struct interactive_limits limits = {30000, 300000, 250};
    return interactive_run(kernel_get_fw_version(), &api, STDIN_FILENO, STDOUT_FILENO, &limits);
}
#endif
