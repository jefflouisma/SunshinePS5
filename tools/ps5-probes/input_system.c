/* NONSHIPPING R3/T042: /dev/hid lifecycle and opt-in input delivery probes.
 * ABI: leolobato/ps5-mcp 84c0f7b316772b13cd62e0121c154afc3ed2482c,
 * docs/vpad-abi.md and payload/system_ps5.c (including UserService signatures).
 * POSIX/firmware: SDK a7685ce8e97ce1f8a4f38253a41d54241c9124f1,
 * target/include/{sys/ioctl.h,signal.h,time.h,fcntl.h,unistd.h},
 * include/ps5/kernel.h, crt/kernel.c.
 * Default/neutral builds press no buttons. Navigation uses D-pad Right/Left.
 * STOP_HELD sends assignment Cross, then holds Right until six-second cleanup.
 * NEUTRAL_OBSERVE sends Cross at 2s, Down at 20-21s, then observes neutral for 3s.
 * GAME sends assignment Cross, button/trigger/stick sequences over 26 seconds,
 * and lowers/restores only its own scheduling priority; affinity is unchanged.
 * Other opt-in modes include assignment, control and typed PS/touch/motion tests;
 * typed mode requests pad-process privilege 1. No rest or title launch is issued.
 * Stock SDK crt/crt.c -> crt/patch.c DOES change current-process credentials and
 * syscall bounds before main; reversal is unverified. No network listener.
 * Driver acceptance is not proof of effective input delivery or removal.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input_probe.h"
#include "input_system.h"

#ifndef R3_HOST
#include <fcntl.h>
#include <ps5/kernel.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#ifdef R3_INPUT_GAME_AUTORUN
#include <sched.h>
#endif

int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int32_t *user_id);
int sceUserServiceTerminate(void);

#if defined(R3_INPUT_READBACK_AUTORUN) || defined(R3_INPUT_TYPED_AUTORUN)
int scePadInit(void);
int scePadReadState(int handle, PadData *data);
#endif
#ifdef R3_INPUT_READBACK_AUTORUN
int scePadGetHandle(int user_id, int type, int index);
#endif
#ifdef R3_INPUT_TYPED_AUTORUN
int scePadSetProcessPrivilege(int privilege);
int scePadVirtualDeviceAddDevice(void *param, int type);
int scePadVirtualDeviceInsertData(int handle, const PadData *data);
int scePadVirtualDeviceDeleteDevice(int handle);
uint64_t sceKernelGetProcessTime(void);
#endif
#endif

_Static_assert(sizeof(PadData) == 120, "SDL/YACE natural pad-data size");
_Static_assert(offsetof(PadData, motion) == 0x0c, "pad-data motion offset");
_Static_assert(offsetof(PadData, touch_count) == 0x34, "pad-data touch offset");
_Static_assert(offsetof(PadData, connected) == 0x4c, "pad-data connected offset");
_Static_assert(offsetof(PadData, timestamp_us) == 0x50, "pad-data timestamp offset");

enum { R3_INCOMPLETE = 2, R3_USAGE = 64, R3_CONTROLLERS = R3_INPUT_CONTROLLER_CAPACITY };
#define R3_ADD UINT32_C(0xc018482a)
#define R3_REPORT UINT32_C(0x8018482c)
#define R3_REMOVE UINT32_C(0x80104850)

_Static_assert(sizeof(PadRequest) == 24, "sourced ioctl header size");
_Static_assert(offsetof(PadRequest, input) == 8, "sourced input pointer offset");
_Static_assert(offsetof(PadRequest, output) == 16, "sourced output pointer offset");
_Static_assert(sizeof(RemoveRequest) == 16, "sourced removal size");
_Static_assert(offsetof(RemoveRequest, handle) == 8, "sourced removal handle offset");
_Static_assert(sizeof(float) == 4, "sourced float32 report");
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "sourced PS5 little endian ABI");
_Static_assert(sizeof(InputState) == 52, "state comparison has no tail padding");

static bool pulse(uint64_t elapsed, uint64_t start)
{
    return elapsed >= start && elapsed - start < 120000;
}

InputState experiment_state(ProbeMode mode, unsigned slot, uint64_t elapsed)
{
    InputState state = {.lx = 0x80, .ly = 0x80, .rx = 0x80, .ry = 0x80, .motion[3] = 1.0f};
    if (mode == R3_MODE_NAVIGATION && slot == 0) {
        state.buttons = pulse(elapsed, 2000000) ? 0x20U : pulse(elapsed, 4000000) ? 0x80U : 0;
    } else if (mode == R3_MODE_STOP_HELD && slot == 0) {
        state.buttons = pulse(elapsed, 2000000) ? 0x4000U : elapsed >= 4000000 ? 0x20U : 0;
    } else if (mode == R3_MODE_NEUTRAL_OBSERVE && slot == 0) {
        state.buttons = pulse(elapsed, 2000000)                     ? 0x4000U
                        : elapsed >= 20000000 && elapsed < 21000000 ? 0x40U
                                                                    : 0;
    } else if (mode == R3_MODE_ASSIGN_FOUR && slot < R3_CONTROLLERS) {
        state.buttons = pulse(elapsed, 2000000 + slot * 2000000)    ? 0x4000U
                        : pulse(elapsed, 10000000 + slot * 2000000) ? 0x20U
                                                                    : 0;
    } else if ((mode == R3_MODE_CONTROLS || mode == R3_MODE_READBACK) && slot == 0) {
        if (pulse(elapsed, 2000000)) {
            state.buttons = 0x4000U;
        } else if (pulse(elapsed, 4000000)) {
            state.buttons = 0x20U;
        } else if (elapsed >= 6000000 && elapsed < 6500000) {
            state.lx = 0xc0;
        } else if (elapsed >= 8000000 && elapsed < 8500000) {
            state.lx = 0xff;
            if (mode == R3_MODE_READBACK) {
                const float motion[8] = {0.25f, -0.5f, 0.75f, 0.5f, 1.0f, -2.0f, 3.0f, -4.0f};
                for (unsigned i = 0; i < 8; ++i) {
                    state.motion[i] = motion[i];
                }
            }
        } else if (elapsed >= 10000000 && elapsed < 10500000) {
            state.l2 = 0x40;
            state.r2 = 0xc0;
        } else if (pulse(elapsed, 12000000)) {
            state.buttons = 0x1U;
        } else if (pulse(elapsed, 14000000)) {
            state.buttons = 0x8U;
        } else if (pulse(elapsed, 16000000)) {
            state.buttons = 0x100000U;
        }
    } else if (mode == R3_MODE_GAME && slot == 0) {
        static const uint32_t buttons[17] = {0x10,   0x40,   0x80,  0x20,  0x1000,  0x2000,
                                             0x4000, 0x8000, 0x400, 0x800, 0x100,   0x200,
                                             0x2,    0x4,    0x8,   0x1,   0x100000};
        if (pulse(elapsed, 2000000)) {
            state.buttons = 0x4000U;
        } else if (elapsed >= 4000000 && elapsed < 21000000) {
            unsigned index = (unsigned)((elapsed - 4000000) / 1000000);
            if (pulse(elapsed, 4000000 + index * 1000000)) {
                state.buttons = buttons[index];
                if (index == 10) {
                    state.l2 = 0x40;
                } else if (index == 11) {
                    state.r2 = 0xc0;
                }
            }
        } else if (elapsed >= 21000000 && elapsed < 21500000) {
            state.lx = 0xc0;
        } else if (elapsed >= 22000000 && elapsed < 22500000) {
            state.lx = 0xff;
        } else if (elapsed >= 23000000 && elapsed < 23500000) {
            state.rx = 0xc0;
        } else if (elapsed >= 24000000 && elapsed < 24500000) {
            state.rx = 0xff;
        }
    } else if (mode == R3_MODE_TYPED && slot == 0) {
        if (pulse(elapsed, 2000000)) {
            state.buttons = 0x10000U;
        } else if (pulse(elapsed, 8000000)) {
            state.buttons = 0x1U;
        } else if (pulse(elapsed, 10000000)) {
            state.buttons = 0x100000U;
            state.touch_count = 1;
            state.touch_x = 960;
            state.touch_y = 540;
            state.touch_id = 1;
        } else if (elapsed >= 12000000 && elapsed < 12500000) {
            const float motion[8] = {0.25f, -0.5f, 0.75f, 0.5f, 1.0f, -2.0f, 3.0f, -4.0f};
            state.lx = 0xc0;
            state.l2 = 0x40;
            state.r2 = 0xc0;
            for (unsigned i = 0; i < 8; ++i) {
                state.motion[i] = motion[i];
            }
        }
    }
    return state;
}

unsigned input_controller_count(ProbeMode mode)
{
    return mode == R3_MODE_NAVIGATION || mode == R3_MODE_CONTROLS || mode == R3_MODE_READBACK ||
                   mode == R3_MODE_STOP_HELD || mode == R3_MODE_GAME ||
                   mode == R3_MODE_NEUTRAL_OBSERVE
               ? 1U
               : R3_CONTROLLERS;
}

void report_set_state(NeutralReport *report, const InputState *state)
{
    for (unsigned i = 0; i < 4; ++i) {
        report->bytes[0x0c + i] = (uint8_t)(state->buttons >> (8U * i));
    }
    report->bytes[0x10] = state->lx;
    report->bytes[0x11] = state->ly;
    report->bytes[0x12] = state->rx;
    report->bytes[0x13] = state->ry;
    report->bytes[0x14] = state->l2;
    report->bytes[0x15] = state->r2;
    const unsigned char *motion = (const unsigned char *)state->motion;
    for (unsigned i = 0; i < sizeof(state->motion); ++i) {
        report->bytes[0x78 + i] = motion[i];
    }
}

void typed_data_set_state(PadData *data, const InputState *state, uint64_t timestamp_us)
{
    *data = (PadData){0};
    data->buttons = state->buttons;
    data->sticks[0] = state->lx;
    data->sticks[1] = state->ly;
    data->sticks[2] = state->rx;
    data->sticks[3] = state->ry;
    data->triggers[0] = state->l2;
    data->triggers[1] = state->r2;
    for (unsigned i = 0; i < 8; ++i) {
        data->motion[i] = state->motion[i];
    }
    data->touch_count = state->touch_count;
    if (state->touch_count == 1) {
        data->touch[0].x = state->touch_x;
        data->touch[0].y = state->touch_y;
        data->touch[0].id = state->touch_id;
    }
    data->connected = 1;
    data->connected_count = 1;
    data->timestamp_us = timestamp_us;
}

static bool state_equal(const InputState *left, const InputState *right)
{
    if (left->buttons != right->buttons || left->lx != right->lx || left->ly != right->ly ||
        left->rx != right->rx || left->ry != right->ry || left->l2 != right->l2 ||
        left->r2 != right->r2 || left->touch_x != right->touch_x ||
        left->touch_y != right->touch_y || left->touch_id != right->touch_id ||
        left->touch_count != right->touch_count) {
        return false;
    }
    for (unsigned i = 0; i < 8; ++i) {
        if (left->motion[i] != right->motion[i]) {
            return false;
        }
    }
    return true;
}

bool update_slot_report(NeutralReport *report, InputState *previous, const InputState *state)
{
    report_set_state(report, state);
    if (state_equal(previous, state)) {
        return false;
    }
    *previous = *state;
    return true;
}

void descriptor_init(PadDescriptor *descriptor, int32_t user)
{
    *descriptor = (PadDescriptor){0};
    descriptor->bytes[0] = 3;
    for (unsigned i = 0; i < 4; ++i) {
        descriptor->bytes[0x1c + i] = (uint8_t)((uint32_t)user >> (8U * i));
    }
    descriptor->bytes[0x20] = 1;
}

void neutral_init(NeutralReport *report)
{
    const float orientation_w = 1.0f;
    const unsigned char *orientation_bytes = (const unsigned char *)&orientation_w;
    *report = (NeutralReport){0};
    for (unsigned i = 0; i < 4; ++i) {
        report->bytes[0x10 + i] = 0x80;
        report->bytes[0x84 + i] = orientation_bytes[i];
    }
    report->bytes[0x17] = 1;
}

bool input_cleanup_handles(const InputCleanupApi *api, int32_t handles[R3_CONTROLLERS])
{
    if (!api || !api->neutral || !api->remove || !handles)
        return false;
    bool ok = true;
    /* Neutralize every owned handle before any removal, even after an error.
     * Each returned removal consumes the local attempt; no uncertain retry. */
    for (unsigned slot = 0; slot < R3_CONTROLLERS; ++slot) {
        if (handles[slot] < 0)
            continue;
        NeutralReport neutral;
        neutral_init(&neutral);
        if (api->neutral(api->context, slot, handles[slot], &neutral) != 0)
            ok = false;
    }
    for (unsigned count = R3_CONTROLLERS; count > 0; --count) {
        unsigned slot = count - 1;
        if (handles[slot] < 0)
            continue;
        if (api->remove(api->context, slot, handles[slot]) != 0)
            ok = false;
        handles[slot] = -1;
    }
    return ok;
}

int input_observe_neutral(ProbeMode mode, bool normal_completion, int neutral_rc,
                          const InputObservationApi *api)
{
    if (mode != R3_MODE_NEUTRAL_OBSERVE || !normal_completion || neutral_rc != 0)
        return R3_OBSERVATION_SKIPPED;
    if (!api || !api->now_us || !api->wait_tick || !api->stopping)
        return R3_OBSERVATION_ERROR;
    if (api->stopping(api->context))
        return R3_OBSERVATION_STOPPED;
    uint64_t start;
    if (!api->now_us(api->context, &start) || start > UINT64_MAX - UINT64_C(3000000))
        return R3_OBSERVATION_ERROR;
    uint64_t previous = start;
    /* A failed/frozen clock must not leave the controller registered forever.
     * Platform calls themselves still have no verified hard deadline. */
    for (unsigned tick = 0; tick < 600; ++tick) {
        if (api->stopping(api->context))
            return R3_OBSERVATION_STOPPED;
        uint64_t now;
        if (!api->now_us(api->context, &now) || now < previous)
            return R3_OBSERVATION_ERROR;
        if (now - start >= UINT64_C(3000000))
            return R3_OBSERVATION_COMPLETE;
        previous = now;
        if (api->wait_tick(api->context) != 0)
            return R3_OBSERVATION_ERROR;
    }
    return R3_OBSERVATION_ERROR;
}

#ifndef R3_INPUT_TEST
static int layout_test(void)
{
    PadDescriptor descriptor;
    NeutralReport report;
    descriptor_init(&descriptor, INT32_C(0x12345678));
    neutral_init(&report);
    /* Independent literal byte fixture from the documented ioctl layout.
     * This checks source-layout preparation only, never console behavior. */
    for (size_t i = 0; i < sizeof(descriptor); ++i) {
        uint8_t expected = 0;
        if (i == 0) {
            expected = 3;
        } else if (i == 0x20) {
            expected = 1;
        } else if (i >= 0x1c && i <= 0x1f) {
            static const uint8_t user_bytes[] = {0x78, 0x56, 0x34, 0x12};
            expected = user_bytes[i - 0x1c];
        }
        if (descriptor.bytes[i] != expected) {
            return 1;
        }
    }
    for (size_t i = 0; i < sizeof(report); ++i) {
        uint8_t expected = 0;
        if ((i >= 0x10 && i <= 0x13) || i == 0x86) {
            expected = 0x80;
        } else if (i == 0x17) {
            expected = 1;
        } else if (i == 0x87) {
            expected = 0x3f;
        }
        if (report.bytes[i] != expected) {
            return 1;
        }
    }
    InputState state = experiment_state(R3_MODE_CONTROLS, 0, 10000000);
    report_set_state(&report, &state);
    if (state.l2 != 0x40 || state.r2 != 0xc0 || report.bytes[0x14] != 0x40 ||
        report.bytes[0x15] != 0xc0) {
        return 1;
    }
    puts("R3_INPUT_LAYOUT status=pass evidence=source_layout_only hardware_evidence=absent");
    return fflush(stdout) == EOF || ferror(stdout) ? 1 : 0;
}

static int describe(void)
{
    puts("R3_INPUT_REQUIRED capability=delivery_boundary status=unavailable "
         "reason=ioctl_acceptance_is_not_effective_delivery");
    puts("R3_INPUT_REQUIRED capability=four_controller_release_removal status=untested "
         "reason=needs_approved_console_and_owner_observation");
    puts("R3_INPUT_REQUIRED capability=rumble_motion_touch status=unavailable "
         "reason=no_admitted_reverse_rumble_or_touch_coordinates_or_motion_units");
    puts("R3_INPUT_REQUIRED capability=remote_play_share_play status=unavailable "
         "reason=no_admitted_typed_query_transition_interface");
    puts("R3_INPUT_REQUIRED capability=rest_cleanup_game_scheduling status=untested "
         "reason=no_owner_assisted_game_or_rest_observation");
    puts("R3_INPUT_RESULT status=incomplete readiness_gate=open");
    return fflush(stdout) == EOF || ferror(stdout) ? 1 : R3_INCOMPLETE;
}
#endif

#ifndef R3_HOST
static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static bool monotonic_us(uint64_t *value)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0 ||
        now.tv_nsec >= 1000000000L || (uint64_t)now.tv_sec > (UINT64_MAX - 999999) / 1000000) {
        return false;
    }
    *value = (uint64_t)now.tv_sec * UINT64_C(1000000) + (uint64_t)now.tv_nsec / 1000;
    return true;
}

static void log_result(const char *operation, unsigned slot, int32_t handle, int rc, int error)
{
    uint64_t stamp;
    printf("R3_INPUT_OP operation=%s slot=%u handle=%" PRId32 " rc=%d errno=%d ", operation, slot,
           handle, rc, error);
    if (monotonic_us(&stamp)) {
        printf("observed_us=%" PRIu64 "\n", stamp);
    } else {
        puts("observed_us=absent reason=clock_failure");
    }
}

static int submit_report(int fd, NeutralReport *report, int32_t handle)
{
    PadRequest request = {(uint32_t)handle, 0, report->bytes, NULL};
    return ioctl(fd, R3_REPORT, &request);
}

typedef struct {
    int fd;
    const uint32_t *accepted;
    ProbeMode mode;
    bool normal_completion;
} ControllerCleanup;

static bool observation_now(void *context, uint64_t *now)
{
    (void)context;
    return monotonic_us(now);
}

static bool observation_stopping(void *context)
{
    (void)context;
    return stop_requested != 0;
}

static int observation_wait(void *context)
{
    (void)context;
    struct timespec delay = {0, 10000000};
    return nanosleep(&delay, NULL) == 0 || errno == EINTR ? 0 : -1;
}

static int cleanup_neutral(void *context, unsigned slot, int32_t handle, NeutralReport *report)
{
    ControllerCleanup *cleanup = context;
    log_result("cleanup_neutral_begin", slot, handle, 0, 0);
    bool log_ok = fflush(stdout) != EOF && !ferror(stdout);
    errno = 0;
    int rc = submit_report(cleanup->fd, report, handle);
    int error = rc == 0 ? 0 : errno;
    log_result("cleanup_neutral", slot, handle, rc, error);
    log_ok = fflush(stdout) != EOF && !ferror(stdout) && log_ok;
    if (cleanup->mode == R3_MODE_NEUTRAL_OBSERVE && cleanup->normal_completion && rc == 0 &&
        log_ok && !stop_requested) {
        log_result("neutral_observation_begin", slot, handle, 0, 0);
        if (fflush(stdout) == EOF || ferror(stdout))
            return -1;
        const InputObservationApi api = {observation_now, observation_wait, observation_stopping,
                                         NULL};
        int observed = input_observe_neutral(cleanup->mode, cleanup->normal_completion, rc, &api);
        log_result("neutral_observation_end", slot, handle, observed, 0);
        log_ok = fflush(stdout) != EOF && !ferror(stdout);
        if (observed == R3_OBSERVATION_ERROR)
            return -1;
    }
    return log_ok ? rc : -1;
}

static int cleanup_remove(void *context, unsigned slot, int32_t handle)
{
    ControllerCleanup *cleanup = context;
    RemoveRequest request = {.operation = 5, .handle = (uint32_t)handle};
    log_result("remove_begin", slot, handle, 0, 0);
    bool log_ok = fflush(stdout) != EOF && !ferror(stdout);
    errno = 0;
    int rc = ioctl(cleanup->fd, R3_REMOVE, &request);
    int error = rc == 0 ? 0 : errno;
    log_result("remove", slot, handle, rc, error);
    printf("R3_INPUT_SLOT slot=%u reports_accepted=%" PRIu32 "\n", slot, cleanup->accepted[slot]);
    log_ok = fflush(stdout) != EOF && !ferror(stdout) && log_ok;
    return log_ok ? rc : -1;
}

typedef struct {
    unsigned seconds;
    ProbeMode mode;
} ProbeConfig;

static int controller_probe(ProbeConfig config)
{
    unsigned seconds = config.seconds;
    ProbeMode mode = config.mode;
    uint32_t firmware = kernel_get_fw_version();
    printf("R3_INPUT_CONTEXT pid=%ld firmware_raw=0x%08" PRIx32 "\n", (long)getpid(), firmware);
    puts("R3_INPUT_RUNTIME stock_sdk_crt=per_process_credential_writes_before_main "
         "sdk_reversal=unverified "
         "probe_defined_privileged_changes=none");
    if ((firmware & UINT32_C(0xffff0000)) != UINT32_C(0x13600000)) {
        puts("R3_INPUT_REFUSED reason=unsupported_or_unknown_firmware");
        return 1;
    }
    int result = 1;
    int fd = -1;
#ifdef R3_INPUT_GAME_AUTORUN
    int old_policy = -1;
    struct sched_param old_param = {0};
    bool scheduling_changed = false;
#endif
#ifdef R3_INPUT_READBACK_AUTORUN
    int pad_handle = -1;
#endif
    int32_t handles[R3_CONTROLLERS] = {-1, -1, -1, -1};
    uint32_t accepted[R3_CONTROLLERS] = {0};
    PadDescriptor descriptor;
    NeutralReport report;
    bool user_initialized = false;
    bool int_handler_owned = false;
    bool term_handler_owned = false;
    bool cleanup_failed = false;
    bool normal_completion = false;
#ifdef R3_INPUT_GAME_AUTORUN
    old_policy = sched_getscheduler(getpid());
    int schedule_rc = old_policy < 0 || sched_getparam(getpid(), &old_param) != 0 ? -1 : 0;
    log_result("schedule_get", 0, -1, schedule_rc, 0);
    if (schedule_rc != 0) {
        goto cleanup;
    }
    int minimum = sched_get_priority_min(old_policy);
    log_result("schedule_minimum", 0, -1, minimum < 0 ? -1 : 0, 0);
    if (minimum < 0) {
        goto cleanup;
    }
    printf("R3_INPUT_SCHEDULE policy=%d old_priority=%d selected_priority=%d affinity=unchanged\n",
           old_policy, old_param.sched_priority, minimum);
    if (old_param.sched_priority != minimum) {
        struct sched_param selected = {.sched_priority = minimum};
        schedule_rc = sched_setparam(getpid(), &selected);
        log_result("schedule_lower", 0, -1, schedule_rc, 0);
        if (schedule_rc != 0) {
            goto cleanup;
        }
        scheduling_changed = true;
    }
#endif
    struct sigaction action = {0}, old_int = {0}, old_term = {0};
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, &old_int) != 0) {
        goto cleanup;
    }
    int_handler_owned = true;
    if (sigaction(SIGTERM, &action, &old_term) != 0) {
        goto cleanup;
    }
    term_handler_owned = true;
    int rc = sceUserServiceInitialize(NULL);
    log_result("user_initialize", 0, -1, rc, 0);
    if (rc != 0) {
        goto cleanup;
    }
    user_initialized = true;
    int32_t user_id = -1;
    rc = sceUserServiceGetForegroundUser(&user_id);
    log_result("foreground_user", 0, -1, rc, 0);
    if (rc != 0 || user_id < 0) {
        goto cleanup;
    }
#ifdef R3_INPUT_READBACK_AUTORUN
    if (mode == R3_MODE_READBACK) {
        rc = scePadInit();
        log_result("pad_initialize", 0, -1, rc, 0);
        if (rc != 0) {
            goto cleanup;
        }
    }
#endif
    descriptor_init(&descriptor, user_id);
    neutral_init(&report);
    fd = open("/dev/hid", O_RDWR);
    if (fd < 0) {
        log_result("hid_open", 0, -1, -1, errno);
        goto cleanup;
    }
    uint64_t start;
    if (!monotonic_us(&start) || start > UINT64_MAX - (uint64_t)seconds * 1000000) {
        goto cleanup;
    }
    uint64_t deadline = start + (uint64_t)seconds * 1000000;
    unsigned controller_count = input_controller_count(mode);
    printf("R3_INPUT_BEGIN mode=%u controllers=%u assignment=cross_only delivery=unverified\n",
           mode, controller_count);
    for (unsigned slot = 0; slot < controller_count && !stop_requested; ++slot) {
        int32_t candidate = -1;
        PadRequest request = {3, 0, descriptor.bytes, &candidate};
        errno = 0;
        rc = ioctl(fd, R3_ADD, &request);
        int error = rc == 0 ? 0 : errno;
        log_result("add", slot, candidate, rc, error);
        if (rc != 0 || candidate < 0) {
            if (candidate >= 0) {
                puts("R3_INPUT_SAFETY status=uncertain reason=handle_written_on_failed_add");
            }
            goto cleanup;
        }
        for (unsigned previous = 0; previous < slot; ++previous) {
            if (handles[previous] == candidate) {
                puts("R3_INPUT_SAFETY status=uncertain reason=duplicate_handle");
                goto cleanup;
            }
        }
        handles[slot] = candidate;
        errno = 0;
        rc = submit_report(fd, &report, candidate);
        error = rc == 0 ? 0 : errno;
        log_result("initial_neutral", slot, candidate, rc, error);
        if (rc != 0) {
            goto cleanup;
        }
        ++accepted[slot];
    }
    InputState previous[R3_CONTROLLERS];
    for (unsigned slot = 0; slot < controller_count; ++slot) {
        previous[slot] = experiment_state(mode, slot, 0);
    }
    while (!stop_requested) {
        uint64_t now;
        if (!monotonic_us(&now) || now < start) {
            goto cleanup;
        }
        if (now >= deadline) {
            normal_completion = true;
            break;
        }
        uint64_t elapsed = now - start;
#ifdef R3_INPUT_READBACK_AUTORUN
        if (mode == R3_MODE_READBACK && pad_handle < 0 && elapsed >= 5000000) {
            for (unsigned index = 0; index < R3_CONTROLLERS; ++index) {
                int candidate = scePadGetHandle(user_id, 3, (int)index);
                log_result("pad_get_virtual", index, candidate, candidate, 0);
                if (candidate > 0 && candidate < 64) {
                    pad_handle = candidate;
                    break;
                }
            }
            if (pad_handle < 0) {
                goto cleanup;
            }
        }
#endif
        for (unsigned slot = 0; slot < controller_count && !stop_requested; ++slot) {
            if (handles[slot] < 0) {
                goto cleanup;
            }
            InputState state = experiment_state(mode, slot, elapsed);
            bool changed = update_slot_report(&report, &previous[slot], &state);
            if (changed) {
                printf("R3_INPUT_EVENT slot=%u buttons=0x%08" PRIx32
                       " lx=0x%02x l2=0x%02x r2=0x%02x elapsed_us=%" PRIu64
                       " delivery=unverified\n",
                       slot, state.buttons, state.lx, state.l2, state.r2, elapsed);
                if (fflush(stdout) == EOF || ferror(stdout))
                    goto cleanup;
            }
            errno = 0;
            rc = submit_report(fd, &report, handles[slot]);
            if (rc != 0) {
                log_result("report_failed", slot, handles[slot], rc, errno);
                goto cleanup;
            }
            ++accepted[slot];
#ifdef R3_INPUT_READBACK_AUTORUN
            if (mode == R3_MODE_READBACK && changed && pad_handle >= 0) {
                PadData data = {0};
                rc = scePadReadState(pad_handle, &data);
                bool match = rc == 0 && data.buttons == state.buttons &&
                             data.sticks[0] == state.lx && data.sticks[1] == state.ly &&
                             data.sticks[2] == state.rx && data.sticks[3] == state.ry &&
                             data.triggers[0] == state.l2 && data.triggers[1] == state.r2;
                for (unsigned i = 0; i < 8; ++i) {
                    match &= data.motion[i] == state.motion[i];
                }
                printf("R3_INPUT_READBACK rc=%d match=%s buttons=0x%08" PRIx32
                       " sticks=%02x,%02x,%02x,%02x triggers=%02x,%02x touch_count=%u "
                       "motion=%g,%g,%g,%g,%g,%g,%g,%g delivery=independent_readback\n",
                       rc, match ? "yes" : "no", data.buttons, data.sticks[0], data.sticks[1],
                       data.sticks[2], data.sticks[3], data.triggers[0], data.triggers[1],
                       data.touch_count, (double)data.motion[0], (double)data.motion[1],
                       (double)data.motion[2], (double)data.motion[3], (double)data.motion[4],
                       (double)data.motion[5], (double)data.motion[6], (double)data.motion[7]);
                if (rc != 0) {
                    goto cleanup;
                }
            }
#endif
        }
        struct timespec delay = {0, 10000000};
        if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
            goto cleanup;
        }
    }
    result = R3_INCOMPLETE;

cleanup:
    /* First neutralize ALL owned handles, then remove them; neither phase waits
     * for any media, network, UI or statistics work. Each operation is attempted
     * once. NEUTRAL_OBSERVE alone retains its one handle after successful neutral
     * for an interruptible diagnostic observation. A driver call itself is not
     * known to have a bounded completion time. */
    {
        ControllerCleanup cleanup_context = {
            .fd = fd, .accepted = accepted, .mode = mode, .normal_completion = normal_completion};
        const InputCleanupApi cleanup_api = {cleanup_neutral, cleanup_remove, &cleanup_context};
        cleanup_failed = !input_cleanup_handles(&cleanup_api, handles);
    }
#ifdef R3_INPUT_GAME_AUTORUN
    if (scheduling_changed) {
        int cleanup_rc = sched_setparam(getpid(), &old_param);
        log_result("schedule_restore", 0, -1, cleanup_rc, 0);
        cleanup_failed |= cleanup_rc != 0;
    }
#endif
    if (fd >= 0) {
        int cleanup_rc = close(fd);
        int error = cleanup_rc == 0 ? 0 : errno;
        log_result("hid_close", 0, -1, cleanup_rc, error);
        cleanup_failed |= cleanup_rc != 0;
    }
    if (user_initialized) {
        int cleanup_rc = sceUserServiceTerminate();
        log_result("user_terminate", 0, -1, cleanup_rc, 0);
        cleanup_failed |= cleanup_rc != 0;
    }
    if (term_handler_owned && sigaction(SIGTERM, &old_term, NULL) != 0) {
        cleanup_failed = true;
    }
    if (int_handler_owned && sigaction(SIGINT, &old_int, NULL) != 0) {
        cleanup_failed = true;
    }
    printf("R3_INPUT_END cleanup_calls=%s delivery=unverified removal=unverified "
           "hardware_acceptance=incomplete stop_requested=%d\n",
           cleanup_failed ? "failed" : "returned_without_error", (int)stop_requested);
    return fflush(stdout) == EOF || ferror(stdout) || cleanup_failed ? 1 : result;
}

#ifdef R3_INPUT_TYPED_AUTORUN
static bool typed_match(const PadData *data, const InputState *state)
{
    bool match = data->buttons == state->buttons && data->sticks[0] == state->lx &&
                 data->sticks[1] == state->ly && data->sticks[2] == state->rx &&
                 data->sticks[3] == state->ry && data->triggers[0] == state->l2 &&
                 data->triggers[1] == state->r2 && data->connected == 1 &&
                 data->connected_count == 1 && data->touch_count == state->touch_count;
    for (unsigned i = 0; i < 8; ++i) {
        match &= data->motion[i] == state->motion[i];
    }
    if (state->touch_count == 1) {
        match &= data->touch[0].x == state->touch_x && data->touch[0].y == state->touch_y &&
                 data->touch[0].id == state->touch_id;
    }
    return match;
}

static int typed_virtual_probe(void)
{
    uint32_t firmware = kernel_get_fw_version();
    printf("R3_INPUT_CONTEXT pid=%ld firmware_raw=0x%08" PRIx32 "\n", (long)getpid(), firmware);
    puts("R3_INPUT_RUNTIME stock_sdk_crt=per_process_credential_writes_before_main "
         "sdk_reversal=unverified probe_defined_privileged_changes=pad_process_privilege_1");
    if ((firmware & UINT32_C(0xffff0000)) != UINT32_C(0x13600000)) {
        puts("R3_INPUT_REFUSED reason=unsupported_or_unknown_firmware");
        return 1;
    }
    int result = 1;
    int handle = -1;
    bool user_initialized = false;
    bool int_handler_owned = false;
    bool term_handler_owned = false;
    bool cleanup_failed = false;
    struct sigaction action = {0}, old_int = {0}, old_term = {0};
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, &old_int) != 0) {
        goto cleanup;
    }
    int_handler_owned = true;
    if (sigaction(SIGTERM, &action, &old_term) != 0) {
        goto cleanup;
    }
    term_handler_owned = true;
    int rc = sceUserServiceInitialize(NULL);
    log_result("user_initialize", 0, -1, rc, 0);
    if (rc != 0) {
        goto cleanup;
    }
    user_initialized = true;
    int32_t user_id = -1;
    rc = sceUserServiceGetForegroundUser(&user_id);
    log_result("foreground_user", 0, -1, rc, 0);
    if (rc != 0 || user_id < 0) {
        goto cleanup;
    }
    rc = scePadInit();
    log_result("pad_initialize", 0, -1, rc, 0);
    if (rc != 0) {
        goto cleanup;
    }
    rc = scePadSetProcessPrivilege(1);
    log_result("pad_process_privilege", 0, -1, rc, 0);
    if (rc != 0) {
        goto cleanup;
    }
    struct {
        int32_t size;
        int32_t user_id;
        int32_t reserved[6];
    } param = {.size = 32, .user_id = user_id};
    handle = scePadVirtualDeviceAddDevice(&param, 3);
    log_result("typed_add", 0, handle, handle > 0 && handle < 64 ? 0 : handle, 0);
    if (handle <= 0 || handle >= 64) {
        handle = -1;
        goto cleanup;
    }
    uint64_t start;
    if (!monotonic_us(&start) || start > UINT64_MAX - UINT64_C(16000000)) {
        goto cleanup;
    }
    uint64_t deadline = start + UINT64_C(16000000);
    InputState previous = experiment_state(R3_MODE_TYPED, 0, 0);
    puts("R3_INPUT_BEGIN mode=typed controllers=1 ps_create_touch_readback=requested");
    while (!stop_requested) {
        uint64_t now;
        if (!monotonic_us(&now) || now < start) {
            goto cleanup;
        }
        if (now >= deadline) {
            break;
        }
        InputState state = experiment_state(R3_MODE_TYPED, 0, now - start);
        bool changed = !state_equal(&previous, &state);
        PadData data;
        typed_data_set_state(&data, &state, sceKernelGetProcessTime());
        rc = scePadVirtualDeviceInsertData(handle, &data);
        if (rc != 0) {
            log_result("typed_report_failed", 0, handle, rc, 0);
            goto cleanup;
        }
        if (changed) {
            previous = state;
            printf("R3_INPUT_EVENT slot=0 buttons=0x%08" PRIx32
                   " lx=0x%02x l2=0x%02x r2=0x%02x touch=%u,%u,%u elapsed_us=%" PRIu64
                   " delivery=unverified\n",
                   state.buttons, state.lx, state.l2, state.r2, state.touch_count, state.touch_x,
                   state.touch_y, now - start);
            PadData actual = {0};
            rc = scePadReadState(handle, &actual);
            bool match = rc == 0 && typed_match(&actual, &state);
            printf("R3_INPUT_TYPED_READBACK rc=%d match=%s buttons=0x%08" PRIx32
                   " sticks=%02x,%02x,%02x,%02x triggers=%02x,%02x touch=%u,%u,%u "
                   "connected=%d count=%u delivery=independent_readback\n",
                   rc, match ? "yes" : "no", actual.buttons, actual.sticks[0], actual.sticks[1],
                   actual.sticks[2], actual.sticks[3], actual.triggers[0], actual.triggers[1],
                   actual.touch_count, actual.touch[0].x, actual.touch[0].y, actual.connected,
                   actual.connected_count);
            if (rc != 0) {
                goto cleanup;
            }
        }
        struct timespec delay = {0, 10000000};
        if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
            goto cleanup;
        }
    }
    result = R3_INCOMPLETE;

cleanup:
    if (handle > 0 && handle < 64) {
        InputState neutral = experiment_state(R3_MODE_TYPED, 0, UINT64_C(16000000));
        PadData data;
        typed_data_set_state(&data, &neutral, sceKernelGetProcessTime());
        int cleanup_rc = scePadVirtualDeviceInsertData(handle, &data);
        log_result("cleanup_neutral", 0, handle, cleanup_rc, 0);
        cleanup_failed |= cleanup_rc != 0;
        cleanup_rc = scePadVirtualDeviceDeleteDevice(handle);
        log_result("typed_remove", 0, handle, cleanup_rc, 0);
        cleanup_failed |= cleanup_rc != 0;
    }
    if (user_initialized) {
        int cleanup_rc = sceUserServiceTerminate();
        log_result("user_terminate", 0, -1, cleanup_rc, 0);
        cleanup_failed |= cleanup_rc != 0;
    }
    if (term_handler_owned && sigaction(SIGTERM, &old_term, NULL) != 0) {
        cleanup_failed = true;
    }
    if (int_handler_owned && sigaction(SIGINT, &old_int, NULL) != 0) {
        cleanup_failed = true;
    }
    printf("R3_INPUT_END cleanup_calls=%s delivery=unverified removal=unverified "
           "hardware_acceptance=incomplete stop_requested=%d\n",
           cleanup_failed ? "failed" : "returned_without_error", (int)stop_requested);
    return fflush(stdout) == EOF || ferror(stdout) || cleanup_failed ? 1 : result;
}
#endif
#endif

#ifndef R3_INPUT_TEST
int main(int argc, char **argv)
{
#ifndef R3_HOST
    if (argc <= 1) {
#if defined(R3_INPUT_TYPED_AUTORUN)
        return typed_virtual_probe();
#elif defined(R3_INPUT_READBACK_AUTORUN)
        return controller_probe((ProbeConfig){12, R3_MODE_READBACK});
#elif defined(R3_INPUT_STOP_HELD_AUTORUN)
        return controller_probe((ProbeConfig){6, R3_MODE_STOP_HELD});
#elif defined(R3_INPUT_NEUTRAL_OBSERVE_AUTORUN)
        return controller_probe((ProbeConfig){21, R3_MODE_NEUTRAL_OBSERVE});
#elif defined(R3_INPUT_GAME_AUTORUN)
        return controller_probe((ProbeConfig){26, R3_MODE_GAME});
#elif defined(R3_INPUT_NAVIGATION_AUTORUN)
        return controller_probe((ProbeConfig){6, R3_MODE_NAVIGATION});
#elif defined(R3_INPUT_CONTROLS_AUTORUN)
        return controller_probe((ProbeConfig){18, R3_MODE_CONTROLS});
#elif defined(R3_INPUT_ASSIGN_FOUR_AUTORUN)
        return controller_probe((ProbeConfig){18, R3_MODE_ASSIGN_FOUR});
#elif defined(R3_INPUT_AUTORUN)
        return controller_probe((ProbeConfig){10, R3_MODE_NEUTRAL});
#endif
    }
#endif
    if (argc <= 1 || (argc == 2 && strcmp(argv[1], "--describe") == 0)) {
        return describe();
    }
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) {
        return layout_test();
    }
    if (argc == 2 &&
        (strcmp(argv[1], "--navigation-once") == 0 || strcmp(argv[1], "--controls-once") == 0 ||
         strcmp(argv[1], "--assign-four") == 0)) {
#ifdef R3_HOST
        return R3_USAGE;
#else
        if (strcmp(argv[1], "--navigation-once") == 0) {
            return controller_probe((ProbeConfig){6, R3_MODE_NAVIGATION});
        }
        if (strcmp(argv[1], "--controls-once") == 0) {
            return controller_probe((ProbeConfig){18, R3_MODE_CONTROLS});
        }
        return controller_probe((ProbeConfig){18, R3_MODE_ASSIGN_FOUR});
#endif
    }
    if ((argc == 2 || argc == 4) && strcmp(argv[1], "--four-neutral") == 0) {
        unsigned seconds = 10;
        if (argc == 4) {
            if (strcmp(argv[2], "--seconds") != 0 || argv[3][0] == '\0') {
                return R3_USAGE;
            }
            for (const char *digit = argv[3]; *digit; ++digit) {
                if (*digit < '0' || *digit > '9') {
                    return R3_USAGE;
                }
            }
            errno = 0;
            unsigned long parsed = strtoul(argv[3], NULL, 10);
            if (errno != 0 || parsed == 0 || parsed > 30) {
                return R3_USAGE;
            }
            seconds = (unsigned)parsed;
        }
#ifdef R3_HOST
        (void)seconds;
        return fputs("R3_INPUT_REFUSED reason=host_build_cannot_supply_console_evidence\n",
                     stderr) == EOF
                   ? 1
                   : R3_USAGE;
#else
        return controller_probe((ProbeConfig){seconds, R3_MODE_NEUTRAL});
#endif
    }
    return fputs("usage: input-system [--describe|--self-test|--navigation-once|"
                 "--controls-once|--assign-four|--four-neutral [--seconds 1..30]]\n",
                 stderr) == EOF
               ? 1
               : R3_USAGE;
}
#endif
