/* Source-layout, sequence and simulated cleanup tests; no console delivery evidence. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../tools/ps5-probes/input_probe.h"
#include "../../tools/ps5-probes/input_system.h"

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            (void)fputs(__FILE__ ": check failed: " #expression "\n", stderr);                     \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

typedef struct {
    ProbeMode mode;
    unsigned slot;
    uint64_t elapsed;
    InputState expected;
} SequenceCase;

static void verify_neutral(const InputState *state)
{
    CHECK(state->buttons == 0);
    CHECK(state->lx == 0x80 && state->ly == 0x80);
    CHECK(state->rx == 0x80 && state->ry == 0x80);
    CHECK(state->l2 == 0 && state->r2 == 0);
}

static void verify_case(const SequenceCase *test)
{
    InputState state = experiment_state(test->mode, test->slot, test->elapsed);
    CHECK(state.buttons == test->expected.buttons);
    CHECK(state.lx == test->expected.lx && state.ly == test->expected.ly);
    CHECK(state.rx == test->expected.rx && state.ry == test->expected.ry);
    CHECK(state.l2 == test->expected.l2 && state.r2 == test->expected.r2);
}

#define AT(mode_value, slot_value, elapsed_value, buttons_value, lx_value, l2_value, r2_value)     \
    verify_case(&(SequenceCase){.mode = (mode_value),                                              \
                                .slot = (slot_value),                                              \
                                .elapsed = (elapsed_value),                                        \
                                .expected = {.buttons = (buttons_value),                           \
                                             .lx = (lx_value),                                     \
                                             .ly = 0x80,                                           \
                                             .rx = 0x80,                                           \
                                             .ry = 0x80,                                           \
                                             .l2 = (l2_value),                                     \
                                             .r2 = (r2_value)}})

typedef struct {
    unsigned mask, calls;
    int fail_call;
    char events[9];
} CleanupFixture;

static int cleanup_event(CleanupFixture *fixture, unsigned slot, int32_t handle, bool removal)
{
    CHECK(slot < R3_INPUT_CONTROLLER_CAPACITY && handle == (int32_t)(100 + slot));
    CHECK((fixture->mask & (1U << slot)) != 0 && fixture->calls < 8);
    unsigned call = fixture->calls++;
    fixture->events[call] = (char)((removal ? 'a' : 'A') + slot);
    fixture->events[fixture->calls] = '\0';
    return (int)call == fixture->fail_call ? -17 : 0;
}

static int fake_neutral(void *context, unsigned slot, int32_t handle, NeutralReport *report)
{
    /* Independent literal fixture checks every byte, including held-button,
     * trigger and motion release. Deliberate mutation tests fresh next report. */
    for (size_t i = 0; i < sizeof(report->bytes); ++i) {
        uint8_t expected = (i >= 0x10 && i <= 0x13) || i == 0x86 ? 0x80
                           : i == 0x17                           ? 1
                           : i == 0x87                           ? 0x3f
                                                                 : 0;
        CHECK(report->bytes[i] == expected);
    }
    memset(report, 0xff, sizeof(*report));
    return cleanup_event(context, slot, handle, false);
}

static int fake_remove(void *context, unsigned slot, int32_t handle)
{
    return cleanup_event(context, slot, handle, true);
}

static void cleanup_tests(void)
{
    /* Every partial acquisition subset, including holes, and every returned
     * neutral/remove failure must still execute the complete two-phase trace. */
    for (unsigned mask = 0; mask < 16; ++mask) {
        char expected[9] = {0};
        unsigned events = 0;
        for (unsigned slot = 0; slot < 4; ++slot)
            if ((mask & (1U << slot)) != 0)
                expected[events++] = (char)('A' + slot);
        for (unsigned slot = 4; slot > 0; --slot)
            if ((mask & (1U << (slot - 1))) != 0)
                expected[events++] = (char)('a' + slot - 1);
        for (int fail = -1; fail < (int)events; ++fail) {
            CleanupFixture fixture = {.mask = mask, .fail_call = fail};
            InputCleanupApi api = {fake_neutral, fake_remove, &fixture};
            int32_t handles[4];
            for (unsigned slot = 0; slot < 4; ++slot)
                handles[slot] = (mask & (1U << slot)) != 0 ? (int32_t)(100 + slot) : -1;
            CHECK(input_cleanup_handles(&api, handles) == (fail < 0));
            CHECK(fixture.calls == events && strcmp(fixture.events, expected) == 0);
            for (unsigned slot = 0; slot < 4; ++slot)
                CHECK(handles[slot] == -1);
            CHECK(input_cleanup_handles(&api, handles));
            CHECK(fixture.calls == events); /* No retry of consumed handles. */
        }
    }
    CleanupFixture fixture = {0};
    InputCleanupApi api = {fake_neutral, fake_remove, &fixture};
    int32_t handles[4] = {100, -1, -1, -1};
    CHECK(!input_cleanup_handles(NULL, handles));
    CHECK(!input_cleanup_handles(&api, NULL));
    api.neutral = NULL;
    CHECK(!input_cleanup_handles(&api, handles));
    api.neutral = fake_neutral;
    api.remove = NULL;
    CHECK(!input_cleanup_handles(&api, handles));
    CHECK(fixture.calls == 0 && handles[0] == 100);
    puts("R3_INPUT_CLEANUP subsets=16 fault_positions=64 order=neutral_all_then_reverse_remove "
         "PASS");
}

typedef struct {
    CleanupFixture cleanup;
    ProbeMode mode;
    bool normal_completion, stopped;
    unsigned ticks, stop_tick, clock_calls, clock_failure;
    bool wait_failure, reverse_clock, overflow_clock, frozen_clock;
    uint64_t now;
    int outcome;
    int32_t *handles;
} ObservationFixture;

static bool observation_clock(void *context, uint64_t *now)
{
    ObservationFixture *fixture = context;
    ++fixture->clock_calls;
    if (fixture->clock_calls == fixture->clock_failure)
        return false;
    *now = fixture->overflow_clock                         ? UINT64_MAX - 1
           : fixture->reverse_clock && fixture->ticks != 0 ? fixture->now - 1
                                                           : fixture->now;
    return true;
}

static int observation_tick(void *context)
{
    ObservationFixture *fixture = context;
    /* The complete neutral report was accepted, and removal has not happened. */
    CHECK(fixture->handles[0] == 100);
    CHECK(strcmp(fixture->cleanup.events, "A") == 0);
    ++fixture->ticks;
    if (!fixture->frozen_clock && !fixture->reverse_clock)
        fixture->now += 10000;
    if (fixture->stop_tick == fixture->ticks)
        fixture->stopped = true;
    return fixture->wait_failure ? -1 : 0;
}

static bool observation_stop(void *context)
{
    return ((ObservationFixture *)context)->stopped;
}

static int observe_cleanup_neutral(void *context, unsigned slot, int32_t handle,
                                   NeutralReport *report)
{
    ObservationFixture *fixture = context;
    int rc = fake_neutral(&fixture->cleanup, slot, handle, report);
    InputObservationApi api = {observation_clock, observation_tick, observation_stop, fixture};
    fixture->outcome = input_observe_neutral(fixture->mode, fixture->normal_completion, rc, &api);
    return rc != 0 || fixture->outcome == R3_OBSERVATION_ERROR ? -1 : 0;
}

static int observe_cleanup_remove(void *context, unsigned slot, int32_t handle)
{
    ObservationFixture *fixture = context;
    return fake_remove(&fixture->cleanup, slot, handle);
}

static void observation_case(ObservationFixture fixture, int expected, unsigned ticks)
{
    int32_t handles[4] = {100, -1, -1, -1};
    fixture.handles = handles;
    InputCleanupApi api = {observe_cleanup_neutral, observe_cleanup_remove, &fixture};
    bool ok = input_cleanup_handles(&api, handles);
    CHECK(ok == (expected != R3_OBSERVATION_ERROR && fixture.cleanup.fail_call < 0));
    CHECK(fixture.outcome == expected && fixture.ticks == ticks);
    CHECK(strcmp(fixture.cleanup.events, "Aa") == 0 && handles[0] == -1);
}

static void observation_tests(void)
{
    ObservationFixture normal = {.cleanup = {.mask = 1, .fail_call = -1},
                                 .mode = R3_MODE_NEUTRAL_OBSERVE,
                                 .normal_completion = true,
                                 .now = 1000000};
    observation_case(normal, R3_OBSERVATION_COMPLETE, 300);
    ObservationFixture test = normal;
    test.normal_completion = false;
    observation_case(test, R3_OBSERVATION_SKIPPED, 0);
    test = normal;
    test.cleanup.fail_call = 0;
    observation_case(test, R3_OBSERVATION_SKIPPED, 0);
    test = normal;
    test.stopped = true;
    observation_case(test, R3_OBSERVATION_STOPPED, 0);
    test = normal;
    test.stop_tick = 7;
    observation_case(test, R3_OBSERVATION_STOPPED, 7);
    test = normal;
    test.wait_failure = true;
    observation_case(test, R3_OBSERVATION_ERROR, 1);
    for (unsigned failure = 1; failure <= 3; ++failure) {
        test = normal;
        test.clock_failure = failure;
        observation_case(test, R3_OBSERVATION_ERROR, failure > 2 ? 1 : 0);
    }
    test = normal;
    test.reverse_clock = true;
    observation_case(test, R3_OBSERVATION_ERROR, 1);
    test = normal;
    test.overflow_clock = true;
    observation_case(test, R3_OBSERVATION_ERROR, 0);
    test = normal;
    test.frozen_clock = true;
    observation_case(test, R3_OBSERVATION_ERROR, 600);
    for (ProbeMode mode = R3_MODE_NEUTRAL; mode < R3_MODE_NEUTRAL_OBSERVE; ++mode) {
        test = normal;
        test.mode = mode;
        observation_case(test, R3_OBSERVATION_SKIPPED, 0);
    }
    CHECK(input_observe_neutral(R3_MODE_NEUTRAL_OBSERVE, true, 0, NULL) == R3_OBSERVATION_ERROR);
    InputObservationApi api = {0};
    CHECK(input_observe_neutral(R3_MODE_NEUTRAL_OBSERVE, true, 0, &api) == R3_OBSERVATION_ERROR);
    CHECK(input_controller_count(R3_MODE_NEUTRAL_OBSERVE) == 1);
    AT(R3_MODE_NEUTRAL_OBSERVE, 0, 1999999, 0, 0x80, 0, 0);
    AT(R3_MODE_NEUTRAL_OBSERVE, 0, 2000000, 0x4000, 0x80, 0, 0);
    AT(R3_MODE_NEUTRAL_OBSERVE, 0, 2119999, 0x4000, 0x80, 0, 0);
    AT(R3_MODE_NEUTRAL_OBSERVE, 0, 2120000, 0, 0x80, 0, 0);
    AT(R3_MODE_NEUTRAL_OBSERVE, 0, 19999999, 0, 0x80, 0, 0);
    AT(R3_MODE_NEUTRAL_OBSERVE, 0, 20000000, 0x40, 0x80, 0, 0);
    AT(R3_MODE_NEUTRAL_OBSERVE, 0, 20999999, 0x40, 0x80, 0, 0);
    AT(R3_MODE_NEUTRAL_OBSERVE, 0, 21000000, 0, 0x80, 0, 0);
    AT(R3_MODE_NEUTRAL_OBSERVE, 0, UINT64_MAX, 0, 0x80, 0, 0);
    for (unsigned slot = 1; slot < 4; ++slot) {
        AT(R3_MODE_NEUTRAL_OBSERVE, slot, 2000000, 0, 0x80, 0, 0);
        AT(R3_MODE_NEUTRAL_OBSERVE, slot, 20000000, 0, 0x80, 0, 0);
    }
    puts("R3_INPUT_OBSERVATION PASS timeline=bounded single_controller=yes neutral_bytes=160 "
         "order=neutral_dwell_remove normal_ticks=300 stop_error_bypass=yes frozen_clock_cap=600");
}

int main(void)
{
    cleanup_tests();
    observation_tests();
    for (unsigned slot = 0; slot < 4; ++slot) {
        AT(R3_MODE_NEUTRAL, slot, 99999999, 0, 0x80, 0, 0);
        AT(R3_MODE_ASSIGN_FOUR, slot, 0, 0, 0x80, 0, 0);
        uint64_t cross = UINT64_C(2000000) + slot * UINT64_C(2000000);
        AT(R3_MODE_ASSIGN_FOUR, slot, cross, 0x4000, 0x80, 0, 0);
        AT(R3_MODE_ASSIGN_FOUR, slot, cross + 119999, 0x4000, 0x80, 0, 0);
        AT(R3_MODE_ASSIGN_FOUR, slot, cross + 120000, 0, 0x80, 0, 0);
        uint64_t right = UINT64_C(10000000) + slot * UINT64_C(2000000);
        AT(R3_MODE_ASSIGN_FOUR, slot, right, 0x20, 0x80, 0, 0);
        AT(R3_MODE_ASSIGN_FOUR, slot, right + 120000, 0, 0x80, 0, 0);
    }
    AT(R3_MODE_NAVIGATION, 0, 2000000, 0x20, 0x80, 0, 0);
    AT(R3_MODE_NAVIGATION, 0, 4000000, 0x80, 0x80, 0, 0);
    AT(R3_MODE_CONTROLS, 0, 2000000, 0x4000, 0x80, 0, 0);
    AT(R3_MODE_CONTROLS, 0, 4000000, 0x20, 0x80, 0, 0);
    AT(R3_MODE_CONTROLS, 0, 6000000, 0, 0xc0, 0, 0);
    AT(R3_MODE_CONTROLS, 0, 8000000, 0, 0xff, 0, 0);
    AT(R3_MODE_CONTROLS, 0, 10000000, 0, 0x80, 0x40, 0xc0);
    AT(R3_MODE_CONTROLS, 0, 12000000, 1, 0x80, 0, 0);
    AT(R3_MODE_CONTROLS, 0, 14000000, 8, 0x80, 0, 0);
    AT(R3_MODE_CONTROLS, 0, 16000000, 0x100000, 0x80, 0, 0);
    AT(R3_MODE_CONTROLS, 0, 16120000, 0, 0x80, 0, 0);
    AT(R3_MODE_CONTROLS, 0, 18000000, 0, 0x80, 0, 0);
    AT(R3_MODE_TYPED, 0, 2000000, 0x10000, 0x80, 0, 0);
    AT(R3_MODE_TYPED, 0, 8000000, 1, 0x80, 0, 0);
    AT(R3_MODE_TYPED, 0, 10000000, 0x100000, 0x80, 0, 0);
    AT(R3_MODE_TYPED, 0, 12000000, 0, 0xc0, 0x40, 0xc0);
    AT(R3_MODE_TYPED, 0, 16000000, 0, 0x80, 0, 0);
    AT(R3_MODE_STOP_HELD, 0, 2000000, 0x4000, 0x80, 0, 0);
    AT(R3_MODE_STOP_HELD, 0, 4000000, 0x20, 0x80, 0, 0);
    AT(R3_MODE_STOP_HELD, 0, 5999999, 0x20, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 2000000, 0x4000, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 4000000, 0x10, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 5000000, 0x40, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 6000000, 0x80, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 7000000, 0x20, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 8000000, 0x1000, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 9000000, 0x2000, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 10000000, 0x4000, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 11000000, 0x8000, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 12000000, 0x400, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 13000000, 0x800, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 14000000, 0x100, 0x80, 0x40, 0);
    AT(R3_MODE_GAME, 0, 15000000, 0x200, 0x80, 0, 0xc0);
    AT(R3_MODE_GAME, 0, 16000000, 0x2, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 17000000, 0x4, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 18000000, 0x8, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 19000000, 0x1, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 20000000, 0x100000, 0x80, 0, 0);
    AT(R3_MODE_GAME, 0, 21000000, 0, 0xc0, 0, 0);
    AT(R3_MODE_GAME, 0, 22000000, 0, 0xff, 0, 0);
    AT(R3_MODE_GAME, 0, 26000000, 0, 0x80, 0, 0);

    InputState state = experiment_state(R3_MODE_GAME, 0, 23000000);
    CHECK(state.rx == 0xc0 && state.ry == 0x80);
    state = experiment_state(R3_MODE_GAME, 0, 24000000);
    CHECK(state.rx == 0xff && state.ry == 0x80);
    state = experiment_state(R3_MODE_CONTROLS, 0, 10000000);
    NeutralReport report;
    neutral_init(&report);
    report_set_state(&report, &state);
    CHECK(report.bytes[0x0c] == 0 && report.bytes[0x10] == 0x80);
    CHECK(report.bytes[0x14] == 0x40 && report.bytes[0x15] == 0xc0);
    CHECK(report.bytes[0x17] == 1 && report.bytes[0x86] == 0x80);
    CHECK(report.bytes[0x87] == 0x3f);
    state = experiment_state(R3_MODE_READBACK, 0, 8000000);
    const float expected_motion[8] = {0.25f, -0.5f, 0.75f, 0.5f, 1.0f, -2.0f, 3.0f, -4.0f};
    const unsigned char *expected_bytes = (const unsigned char *)expected_motion;
    report_set_state(&report, &state);
    for (unsigned i = 0; i < 8; ++i) {
        CHECK(state.motion[i] == expected_motion[i]);
    }
    for (unsigned i = 0; i < sizeof(expected_motion); ++i) {
        CHECK(report.bytes[0x78 + i] == expected_bytes[i]);
    }
    state = experiment_state(R3_MODE_TYPED, 0, 10000000);
    CHECK(state.touch_count == 1 && state.touch_x == 960 && state.touch_y == 540);
    CHECK(state.touch_id == 1);
    PadData typed;
    typed_data_set_state(&typed, &state, UINT64_C(123456));
    _Static_assert(sizeof(PadData) == 120, "typed pad-data size");
    _Static_assert(offsetof(PadData, touch_count) == 0x34, "typed touch offset");
    _Static_assert(offsetof(PadData, connected) == 0x4c, "typed connected offset");
    _Static_assert(offsetof(PadData, timestamp_us) == 0x50, "typed timestamp offset");
    CHECK(typed.buttons == 0x100000 && typed.touch_count == 1);
    CHECK(typed.touch[0].x == 960 && typed.touch[0].y == 540 && typed.touch[0].id == 1);
    CHECK(typed.touch[1].x == 0 && typed.touch[1].y == 0);
    CHECK(typed.connected == 1 && typed.connected_count == 1);
    CHECK(typed.timestamp_us == 123456);
    state = experiment_state(R3_MODE_TYPED, 0, 12000000);
    typed_data_set_state(&typed, &state, UINT64_C(123457));
    for (unsigned i = 0; i < 8; ++i) {
        CHECK(typed.motion[i] == expected_motion[i]);
    }
    CHECK(typed.motion[8] == 0 && typed.motion[9] == 0);
    CHECK(typed.touch_count == 0);
    state = experiment_state(R3_MODE_CONTROLS, 0, 18000000);
    verify_neutral(&state);
    report_set_state(&report, &state);
    for (unsigned i = 0x10; i <= 0x13; ++i)
        CHECK(report.bytes[i] == 0x80);
    CHECK(report.bytes[0x14] == 0 && report.bytes[0x15] == 0);
    InputState previous = experiment_state(R3_MODE_ASSIGN_FOUR, 0, 0);
    InputState active = experiment_state(R3_MODE_ASSIGN_FOUR, 0, 2000000);
    InputState other_neutral = experiment_state(R3_MODE_ASSIGN_FOUR, 1, 2000000);
    CHECK(update_slot_report(&report, &previous, &active));
    CHECK(report.bytes[0x0c] == 0x00 && report.bytes[0x0d] == 0x40);
    previous = other_neutral;
    CHECK(!update_slot_report(&report, &previous, &other_neutral));
    CHECK(report.bytes[0x0c] == 0 && report.bytes[0x0d] == 0);
    puts("R3_INPUT_SEQUENCE source layouts and bounded transitions PASS; hardware_evidence=absent");
    return 0;
}
