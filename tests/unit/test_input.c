/* Synthetic portable contract tests; no console or interoperability evidence. */
#if defined(SP5_PENDING_CORE) && defined(__APPLE__)
#define SP5_CORE_API __attribute__((weak_import))
#elif defined(SP5_PENDING_CORE)
#define SP5_CORE_API __attribute__((weak))
#endif

#include "../fakes/platform.h"
#include "controller.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            if (fputs(__FILE__ ": " #condition "\n", stderr) == EOF) {                             \
                return 2;                                                                          \
            }                                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
#ifdef SP5_PENDING_CORE
#define NEED(api)                                                                                  \
    do {                                                                                           \
        if ((api) == NULL) {                                                                       \
            if (fputs("TDD missing implementation: " #api "\n", stderr) == EOF) {                  \
                return 2;                                                                          \
            }                                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
#else
#define NEED(api) ((void)0)
#endif

static void put16(uint8_t *where, uint16_t value)
{
    where[0] = (uint8_t)value;
    where[1] = (uint8_t)(value >> 8);
}

/* Wire layout/button facts: protocol-notes.md 12.3/12.5, Input.h 87-109
 * and Limelight.h 720-742 @ mcc 41cedb8. These bytes are assembled independently. */
typedef struct {
    uint16_t wire_id;
    uint16_t active_mask;
    uint32_t buttons;
    uint8_t triggers[2];
} gamepad_fields;

static void gamepad(uint8_t wire[34], gamepad_fields fields)
{
    const uint8_t empty[34] = {0, 0, 0, 30, 0x0c, 0, 0, 0, 0x1a, 0, 0, 0,    0, 0, 0x14, 0,    0,
                               0, 0, 0, 0,  0,    0, 0, 0, 0,    0, 0, 0x9c, 0, 0, 0,    0x55, 0};
    memcpy(wire, empty, sizeof(empty));
    put16(wire + 10, fields.wire_id);
    put16(wire + 12, fields.active_mask);
    put16(wire + 16, (uint16_t)fields.buttons);
    put16(wire + 30, (uint16_t)(fields.buttons >> 16));
    wire[18] = fields.triggers[0];
    wire[19] = fields.triggers[1];
    put16(wire + 20, 32767);
    put16(wire + 22, 0x8000);
    put16(wire + 24, 123);
    put16(wire + 26, (uint16_t)-456);
}

static int translation(void)
{
    NEED(sp5_controller_create);
    NEED(sp5_controller_destroy);
    NEED(sp5_controller_receive);
    NEED(sp5_controller_close);
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_controller_input *input = sp5_controller_create(91, &fake.input);
    CHECK(input != NULL);
    uint8_t wire[34];
    /* Limelight.h 720-742 @ 41cedb8: all required named controls, including
     * guide 0x400, Sony touchpad click 0x100000, Create/Back 0x20. */
    const uint32_t flags[] = {1,     2,     4,     8,      0x10,   0x20,   0x40,   0x80,
                              0x100, 0x200, 0x400, 0x1000, 0x2000, 0x4000, 0x8000, 0x100000};
    sp5_input_evidence event = {.session_id = 91, .event_id = 1, .host_received = fake.now};
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        gamepad(wire,
                (gamepad_fields){
                    .wire_id = 7, .active_mask = 0x80, .buttons = flags[i], .triggers = {0, 255}});
        CHECK(
            sp5_controller_receive(input, (sp5_bytes){wire, sizeof(wire)}, false, &event).status ==
            SP5_OK);
        CHECK(fake.controllers[0].state.buttons == flags[i]);
        CHECK(fake.controllers[0].state.axes[0] == 32767 &&
              fake.controllers[0].state.axes[1] == -32768);
        CHECK(fake.controllers[0].state.axes[2] == 123 &&
              fake.controllers[0].state.axes[3] == -456);
        CHECK(event.controller_id == 0 && event.delivery == SP5_INPUT_DELIVERY_CONFIRMED);
    }
    const uint8_t triggers[] = {0, 1, 127, 128, 254, 255};
    for (size_t i = 0; i < sizeof(triggers) / sizeof(triggers[0]); ++i) {
        gamepad(wire, (gamepad_fields){.wire_id = 7,
                                       .active_mask = 0x80,
                                       .buttons = 0,
                                       .triggers = {triggers[i], triggers[5 - i]}});
        CHECK(
            sp5_controller_receive(input, (sp5_bytes){wire, sizeof(wire)}, false, &event).status ==
            SP5_OK);
        CHECK(fake.controllers[0].state.triggers[0] == (uint16_t)(triggers[i] * 257U));
        CHECK(fake.controllers[0].state.triggers[1] == (uint16_t)(triggers[5 - i] * 257U));
    }
    CHECK(fake.calls[SP5_FAKE_CLOCK_NOW] == 0); /* metrics off */
    CHECK(sp5_controller_close(input).status == SP5_OK);
    CHECK(!fake.controllers[0].live && fake.controllers[0].state.buttons == 0);
    sp5_controller_destroy(input);
    return 0;
}

static int controllers(void)
{
    NEED(sp5_controller_create);
    NEED(sp5_controller_destroy);
    NEED(sp5_controller_receive);
    NEED(sp5_controller_close);
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_controller_input *input = sp5_controller_create(91, &fake.input);
    CHECK(input != NULL);
    const uint16_t ids[4] = {7, 2, 15, 1};
    uint16_t mask = 0;
    uint8_t wire[34];
    sp5_input_evidence event = {.session_id = 91, .host_received = fake.now};
    for (uint16_t i = 0; i < 4; ++i) {
        mask |= (uint16_t)(1U << ids[i]);
        gamepad(wire, (gamepad_fields){.wire_id = ids[i],
                                       .active_mask = mask,
                                       .buttons = (uint32_t)(1U << i),
                                       .triggers = {255, 128}});
        CHECK(
            sp5_controller_receive(input, (sp5_bytes){wire, sizeof(wire)}, false, &event).status ==
            SP5_OK);
        CHECK(event.controller_id == i && fake.controllers[i].live);
        CHECK(fake.controllers[i].state.buttons == (uint32_t)(1U << i));
    }
    gamepad(wire, (gamepad_fields){.wire_id = 3,
                                   .active_mask = (uint16_t)(mask | 8),
                                   .buttons = 0,
                                   .triggers = {0, 0}});
    CHECK(sp5_controller_receive(input, (sp5_bytes){wire, sizeof(wire)}, false, &event).reason ==
          SP5_REASON_CAPACITY);
    CHECK(fake.controllers[0].state.buttons == 1 && fake.controllers[3].state.buttons == 8);
    mask &= (uint16_t) ~(1U << ids[1]);
    gamepad(wire, (gamepad_fields){
                      .wire_id = ids[1], .active_mask = mask, .buttons = 0, .triggers = {0, 0}});
    CHECK(sp5_controller_receive(input, (sp5_bytes){wire, sizeof(wire)}, false, &event).status ==
          SP5_OK);
    CHECK(!fake.controllers[1].live && fake.controllers[1].state.buttons == 0);
    CHECK(fake.controllers[0].live && fake.controllers[2].live && fake.controllers[3].live);
    gamepad(wire, (gamepad_fields){.wire_id = ids[1],
                                   .active_mask = (uint16_t)(mask | (1U << ids[1])),
                                   .buttons = 0x1000,
                                   .triggers = {1, 1}});
    CHECK(sp5_controller_receive(input, (sp5_bytes){wire, sizeof(wire)}, false, &event).status ==
          SP5_OK);
    CHECK(fake.controllers[1].live && fake.controllers[1].state.buttons == 0x1000);
    CHECK(sp5_controller_close(input).status == SP5_OK);
    uint64_t deliveries = fake.calls[SP5_FAKE_INPUT_DELIVER];
    CHECK(sp5_controller_receive(input, (sp5_bytes){wire, sizeof(wire)}, false, &event).status ==
          SP5_ERROR);
    CHECK(fake.calls[SP5_FAKE_INPUT_DELIVER] == deliveries);
    for (size_t i = 0; i < 4; ++i) {
        CHECK(!fake.controllers[i].live && fake.controllers[i].state.buttons == 0);
        CHECK(fake.controllers[i].state.triggers[0] == 0 &&
              fake.controllers[i].state.triggers[1] == 0);
    }
    CHECK(sp5_controller_close(input).status == SP5_OK);
    sp5_controller_destroy(input);
    return 0;
}

static int independent_delivery(void)
{
    NEED(sp5_controller_create);
    NEED(sp5_controller_destroy);
    NEED(sp5_controller_receive);
    NEED(sp5_controller_close);
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_media_config mode = {2, 2, 60, 1, "synthetic", SP5_SDR, 100};
    CHECK(fake.capture.open(&fake, &mode).status == SP5_OK);
    sp5_video_frame held;
    CHECK(fake.capture.acquire(&fake, fake.now, false, &held).status == SP5_OK);
    sp5_controller_input *input = sp5_controller_create(91, &fake.input);
    CHECK(input != NULL);
    uint8_t wire[34];
    gamepad(wire, (gamepad_fields){
                      .wire_id = 0, .active_mask = 1, .buttons = 0x1000, .triggers = {128, 64}});
    sp5_input_evidence event = {.session_id = 91, .event_id = 2, .host_received = fake.now};
    fake.fail_next[SP5_FAKE_INPUT_ADD] = SP5_REASON_PLATFORM;
    CHECK(sp5_controller_receive(input, (sp5_bytes){wire, sizeof(wire)}, false, &event).status ==
          SP5_ERROR);
    CHECK(!fake.controllers[0].live);
    CHECK(sp5_controller_receive(input, (sp5_bytes){wire, sizeof(wire)}, true, &event).status ==
          SP5_OK);
    CHECK(fake.video_leased && event.delivery == SP5_INPUT_DELIVERY_CONFIRMED);
    CHECK(event.host_received.ticks == 0 && event.console_delivered.validity == SP5_VALIDITY_VALID);
    CHECK(fake.calls[SP5_FAKE_ENCODE] == 0);
    uint64_t deliveries = fake.calls[SP5_FAKE_INPUT_DELIVER];
    CHECK(sp5_controller_receive(input, (sp5_bytes){wire, sizeof(wire) - 1}, true, &event).status ==
          SP5_ERROR);
    CHECK(fake.calls[SP5_FAKE_INPUT_DELIVER] == deliveries);
    fake.fail_next[SP5_FAKE_INPUT_DELIVER] = SP5_REASON_PLATFORM;
    CHECK(sp5_controller_receive(input, (sp5_bytes){wire, sizeof(wire)}, true, &event).status ==
          SP5_ERROR);
    CHECK(event.delivery == SP5_INPUT_DELIVERY_FAILED &&
          event.console_delivered.validity != SP5_VALIDITY_VALID);
    CHECK(sp5_controller_close(input).status == SP5_OK);
    CHECK(!fake.controllers[0].live);
    CHECK(fake.capture.release(&fake, held.lease_id).status == SP5_OK);
    CHECK(fake.capture.close(&fake).status == SP5_OK);
    CHECK(fake.last_input_release_sequence < fake.last_media_close_sequence);
    sp5_controller_destroy(input);
    return 0;
}

/* 12.5 extension fixtures: gyro degrees/s -> portable radians/s; touch floats
 * are LE IEEE754. These do not assert a PS5 SDK axis or injection convention. */
static int motion_touch(void)
{
    NEED(sp5_controller_create);
    NEED(sp5_controller_destroy);
    NEED(sp5_controller_receive);
    NEED(sp5_controller_close);
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_controller_input *input = sp5_controller_create(91, &fake.input);
    CHECK(input != NULL);
    uint8_t wire[34];
    gamepad(wire,
            (gamepad_fields){.wire_id = 0, .active_mask = 1, .buttons = 0, .triggers = {0, 0}});
    sp5_input_evidence event = {.session_id = 91, .host_received = fake.now};
    CHECK(sp5_controller_receive(input, (sp5_bytes){wire, sizeof(wire)}, false, &event).status ==
          SP5_OK);
    uint8_t gyro[24] = {0, 0, 0,    20,   6, 0, 0, 0x55, 0, 2, 0, 0,
                        0, 0, 0x34, 0x43, 0, 0, 0, 0,    0, 0, 0, 0};
    CHECK(sp5_controller_receive(input, (sp5_bytes){gyro, sizeof(gyro)}, false, &event).status ==
          SP5_OK);
    CHECK(fake.controllers[0].state.has_motion);
    CHECK(fake.controllers[0].state.gyro[0] > 3.14159F &&
          fake.controllers[0].state.gyro[0] < 3.14160F);
    uint8_t touch[28] = {0, 0, 0, 24, 5,    0,    0, 0x55, 0,    1,    0, 0, 42,   0,
                         0, 0, 0, 0,  0x80, 0x3e, 0, 0,    0x40, 0x3f, 0, 0, 0x80, 0x3f};
    CHECK(sp5_controller_receive(input, (sp5_bytes){touch, sizeof(touch)}, false, &event).status ==
          SP5_OK);
    CHECK(fake.controllers[0].state.has_touch && fake.controllers[0].state.touches[0].active);
    CHECK(fake.controllers[0].state.touches[0].contact_id == 42);
    CHECK(fake.controllers[0].state.touches[0].x == 0.25F &&
          fake.controllers[0].state.touches[0].y == 0.75F);
    touch[9] = 2; /* UP */
    CHECK(sp5_controller_receive(input, (sp5_bytes){touch, sizeof(touch)}, false, &event).status ==
          SP5_OK);
    CHECK(!fake.controllers[0].state.touches[0].active);
    CHECK(sp5_controller_close(input).status == SP5_OK);
    CHECK(!fake.controllers[0].live && !fake.controllers[0].state.has_motion &&
          !fake.controllers[0].state.has_touch);
    sp5_controller_destroy(input);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    if (strcmp(argv[1], "translation") == 0) {
        return translation();
    }
    if (strcmp(argv[1], "controllers") == 0) {
        return controllers();
    }
    if (strcmp(argv[1], "independent") == 0) {
        return independent_delivery();
    }
    if (strcmp(argv[1], "motion_touch") == 0) {
        return motion_touch();
    }
    return 2;
}
