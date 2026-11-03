#define _POSIX_C_SOURCE 200809L
#include <time.h>
static unsigned clock_calls, clock_fail_at;
static int tested_clock_gettime(clockid_t id, struct timespec *ts);
#define clock_gettime tested_clock_gettime
#define R3_INTERACTIVE_TEST
/* Exercise the private transport and lifecycle with the actual implementation. */
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../../tools/ps5-probes/input_interactive.c"
#undef clock_gettime
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            (void)fputs(__FILE__ ": check failed: " #x "\n", stderr);                              \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static unsigned calls, fail_at, stop_at, report_count, event_count;
static enum interactive_op events[256];
static uint8_t reports[80][160];
static bool invalid_user, invalid_handle;
static int output_reader = -1;
static char transcript[16384];
static size_t transcript_used;
static unsigned acknowledgements;
static bool strict_ack_order;

static int tested_clock_gettime(clockid_t id, struct timespec *ts)
{
    if (++clock_calls == clock_fail_at) {
        errno = EIO;
        return -1;
    }
    return clock_gettime(id, ts);
}

static void collect(void)
{
    if (output_reader < 0)
        return;
    for (;;) {
        ssize_t n = read(output_reader, transcript + transcript_used,
                         sizeof(transcript) - transcript_used - 1);
        if (n <= 0)
            break;
        transcript_used += (size_t)n;
    }
    transcript[transcript_used] = 0;
}

static int fake_call(enum interactive_op op, int32_t *value, void *bytes)
{
    collect();
    CHECK(event_count < 256);
    events[event_count++] = op;
    ++calls;
    if (op == I_USER)
        *value = invalid_user ? -1 : 0x12345678;
    if (op == I_ADD) {
        uint8_t expected[168] = {
            [0] = 3, [0x1c] = 0x78, [0x1d] = 0x56, [0x1e] = 0x34, [0x1f] = 0x12, [0x20] = 1};
        CHECK(memcmp(bytes, expected, sizeof(expected)) == 0);
        *value = invalid_handle ? -1 : 42;
    }
    if (op == I_REPORT) {
        CHECK(*value == 42);
        CHECK(report_count < 80);
        memcpy(reports[report_count++], bytes, 160);
        /* ACK count must trail the current report until its neutral call returns. */
        acknowledgements = 0;
        for (const char *p = transcript; (p = strstr(p, "ACK seq=")) != NULL; p += 8)
            ++acknowledgements;
        if (strict_ack_order) {
            unsigned index = report_count - 1;
            unsigned expected = index == 0 ? 0 : (index <= 12 ? (index - 1) / 2 : index - 7);
            CHECK(acknowledgements == expected);
        }
    }
    if (op == I_REMOVE)
        CHECK(*value == 42);
    if (calls == stop_at)
        interactive_stopped = 1;
    return calls == fail_at ? -17 : 0;
}

static const struct interactive_api api = {fake_call};
static const struct interactive_limits limits = {25, 200, 25};

static void reset(void)
{
    calls = fail_at = stop_at = report_count = event_count = acknowledgements = 0;
    clock_calls = clock_fail_at = 0;
    invalid_user = invalid_handle = false;
    strict_ack_order = false;
    output_reader = -1;
    transcript_used = 0;
    transcript[0] = 0;
    memset(reports, 0, sizeof(reports));
}

static int scenario(const void *commands, size_t length, bool eof, uint32_t firmware)
{
    int input[2], output[2];
    CHECK(pipe(input) == 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, output) == 0);
    CHECK(write(input[1], commands, length) == (ssize_t)length);
    if (eof)
        CHECK(close(input[1]) == 0);
    output_reader = output[1];
    CHECK(fcntl(output_reader, F_SETFL, O_NONBLOCK) == 0);
    int original_in = fcntl(input[0], F_GETFL), original_out = fcntl(output[0], F_GETFL);
    int result = interactive_run(firmware, &api, input[0], output[0], &limits);
    CHECK((fcntl(input[0], F_GETFL) & O_NONBLOCK) == (original_in & O_NONBLOCK));
    CHECK((fcntl(output[0], F_GETFL) & O_NONBLOCK) == (original_out & O_NONBLOCK));
    collect();
    CHECK(close(input[0]) == 0 && close(output[0]) == 0 && close(output[1]) == 0);
    if (!eof)
        CHECK(close(input[1]) == 0);
    output_reader = -1;
    return result;
}

/* The arguments are a literal fixture's report index, byte offset and byte value. */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void expect_report(unsigned index, unsigned byte, unsigned value)
{
    uint8_t expected[160] = {[0x10] = 0x80, [0x11] = 0x80, [0x12] = 0x80, [0x13] = 0x80,
                             [0x17] = 1,    [0x86] = 0x80, [0x87] = 0x3f};
    if (value != 0)
        expected[byte] = (uint8_t)value;
    CHECK(index < report_count && memcmp(expected, reports[index], sizeof(expected)) == 0);
}

static void expect_cleanup(void)
{
    CHECK(event_count >= 4);
    CHECK(events[event_count - 4] == I_REPORT && events[event_count - 3] == I_REMOVE &&
          events[event_count - 2] == I_CLOSE && events[event_count - 1] == I_TERM);
    expect_report(report_count - 1, 0, 0);
}

static void test_commands(void)
{
    reset();
    strict_ack_order = true;
    const char commands[] = "1 pulse up\n2 pulse right\n3 pulse down\n4 pulse left\n"
                            "5 pulse cross\n6 pulse circle\n7 neutral\n8 neutral\n9 stop\n";
    CHECK(scenario(commands, sizeof(commands) - 1, true, 0x13600000) == 0);
    CHECK(report_count == 16 && acknowledgements == 8);
    expect_report(0, 0, 0);
    const unsigned bytes[] = {0x0c, 0x0c, 0x0c, 0x0c, 0x0d, 0x0d};
    const unsigned values[] = {0x10, 0x20, 0x40, 0x80, 0x40, 0x20};
    for (unsigned i = 0; i < 6; ++i) {
        expect_report(1 + i * 2, bytes[i], values[i]);
        expect_report(2 + i * 2, 0, 0);
    }
    expect_report(13, 0, 0);
    expect_report(14, 0, 0);
    expect_cleanup();
    CHECK(strstr(transcript, "READY version=1") && strstr(transcript, "ACK seq=8") &&
          strstr(transcript,
                 "DONE code=0 owned=1 neutral_rc=0 remove_rc=0 close_rc=0 terminate_rc=0"));
    puts("PASS explicit-six-actions repeated-neutral neutral-before-ACK "
         "literal-descriptor-and-reports");
}

static void test_rejections(void)
{
    const char *const invalid[] = {"",
                                   "1",
                                   "1 pulse down",
                                   "0 stop\n",
                                   "2 stop\n",
                                   "01 stop\n",
                                   "+1 stop\n",
                                   "1 stop \n",
                                   "1  stop\n",
                                   "1 pulse ps\n",
                                   "1 pulse home\n",
                                   "1 pulse 0x40\n",
                                   "1 pulse up+cross\n",
                                   "1 neutral extra\n",
                                   "1 stop\r\n",
                                   "111111111111111111111111111111111111111111111111111111\n"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        reset();
        CHECK(scenario(invalid[i], strlen(invalid[i]), true, 0x13600000) == 1);
        CHECK(report_count == 2);
        expect_cleanup();
    }
    reset();
    const char nul[] = "1 pulse\0down\n";
    CHECK(scenario(nul, sizeof(nul) - 1, true, 0x13600000) == 1);
    expect_cleanup();
    reset();
    const char stale[] = "1 neutral\n1 pulse down\n";
    CHECK(scenario(stale, sizeof(stale) - 1, true, 0x13600000) == 1);
    CHECK(report_count == 3);
    reset();
    CHECK(scenario("1 stop\n", 7, true, 0x12000000) == 3 && calls == 0);
    reset();
    CHECK(scenario("", 0, false, 0x13600000) == 1);
    expect_cleanup();
    reset();
    char maximum[512];
    size_t length = 0;
    for (unsigned i = 1; i <= 32; ++i)
        length += (size_t)snprintf(maximum + length, sizeof(maximum) - length, "%u neutral\n", i);
    CHECK(scenario(maximum, length, true, 0x13600000) == 1);
    CHECK(report_count == 34);
    expect_cleanup();
    puts("PASS grammar sequence firmware EOF-partial-line idle-timeout no-unsolicited-action");
}

static void test_failures(void)
{
    const char commands[] = "1 pulse down\n2 stop\n";
    reset();
    CHECK(scenario(commands, sizeof(commands) - 1, true, 0x13600000) == 0);
    unsigned baseline = calls;
    for (unsigned i = 1; i <= baseline; ++i) {
        reset();
        fail_at = i;
        CHECK(scenario(commands, sizeof(commands) - 1, true, 0x13600000) == 1);
        if (i > 4)
            expect_cleanup();
        if (i == 1)
            CHECK(event_count == 1);
        if (i == 2 || i == 3)
            CHECK(events[event_count - 1] == I_TERM);
        if (i == 4)
            CHECK(events[event_count - 2] == I_CLOSE && events[event_count - 1] == I_TERM);
    }
    for (unsigned i = 1; i <= 8; ++i) {
        reset();
        stop_at = i;
        CHECK(scenario(commands, sizeof(commands) - 1, true, 0x13600000) == 1);
        if (i >= 4)
            expect_cleanup();
    }
    reset();
    invalid_user = true;
    CHECK(scenario(commands, sizeof(commands) - 1, true, 0x13600000) == 1);
    CHECK(event_count == 3 && events[2] == I_TERM);
    reset();
    invalid_handle = true;
    CHECK(scenario(commands, sizeof(commands) - 1, true, 0x13600000) == 1);
    CHECK(event_count == 6 && events[4] == I_CLOSE && events[5] == I_TERM);
    reset();
    CHECK(scenario(commands, sizeof(commands) - 1, true, 0x13600000) == 0);
    unsigned clock_baseline = clock_calls;
    for (unsigned i = 1; i <= clock_baseline; ++i) {
        reset();
        clock_fail_at = i;
        CHECK(scenario(commands, sizeof(commands) - 1, true, 0x13600000) == 1);
        if (event_count > 4)
            expect_cleanup();
    }
    puts("PASS every-observed-clock-call-failure cleanup-before-final-output");
    puts("PASS every-platform-call-failure acquisition-ownership "
         "signal-during-acquisition-or-pulse");
}

static void test_transport(void)
{
    reset();
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(fflush(stdout) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(pair[0]);
        char ready[256];
        CHECK(read(pair[1], ready, sizeof(ready)) > 0);
        const char *command = "1 neutral\n2 stop\n";
        for (size_t i = 0; i < strlen(command); ++i) {
            CHECK(write(pair[1], command + i, 1) == 1);
            const struct timespec delay = {0, 1000000};
            (void)nanosleep(&delay, NULL);
        }
        while (read(pair[1], ready, sizeof(ready)) > 0) {
        }
        close(pair[1]);
        _exit(0);
    }
    close(pair[1]);
    const struct interactive_limits integration = {1000, 2000, 100};
    CHECK(interactive_run(0x13600000, &api, pair[0], pair[0], &integration) == 0);
    expect_cleanup();
    close(pair[0]);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    reset();
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(fflush(stdout) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(pair[0]);
        char response[256];
        CHECK(read(pair[1], response, sizeof(response)) > 0);
        CHECK(kill(getppid(), SIGTERM) == 0);
        while (read(pair[1], response, sizeof(response)) > 0) {
        }
        close(pair[1]);
        _exit(0);
    }
    close(pair[1]);
    CHECK(interactive_run(0x13600000, &api, pair[0], pair[0], &integration) == 1);
    CHECK(interactive_stopped == 1);
    expect_cleanup();
    close(pair[0]);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* A full output socket must time out, release resources and return. */
    reset();
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
    char fill[4096] = {0};
    while (write(pair[0], fill, sizeof(fill)) > 0) {
    }
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
    uint64_t before, after;
    CHECK(interactive_now(&before));
    CHECK(interactive_run(0x13600000, &api, pair[0], pair[0], &limits) == 1);
    CHECK(interactive_now(&after) && after - before < 1000);
    expect_cleanup();
    close(pair[0]);
    close(pair[1]);

    reset();
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    close(pair[1]);
    CHECK(interactive_run(0x13600000, &api, pair[0], pair[0], &limits) == 1);
    expect_cleanup();
    close(pair[0]);
    reset();
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const struct interactive_limits total_timeout = {1000, 10, 25};
    CHECK(interactive_run(0x13600000, &api, pair[0], pair[0], &total_timeout) == 1);
    expect_cleanup();
    close(pair[0]);
    close(pair[1]);
    puts("PASS real-socket-fragmentation real-SIGTERM output-backpressure peer-disconnect "
         "bounded-cleanup");
}

int main(void)
{
    test_commands();
    test_rejections();
    test_failures();
    test_transport();
    puts("INTERACTIVE_TEST pass=1 synthetic_platform=1 device_access=none");
    return 0;
}
