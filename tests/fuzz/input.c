/* T112 synthetic harness: a bounded stream of controller packets against one
 * manager on the fake platform (tests/fakes/platform.c); no console input.
 *
 * Input: [u8 setup] then up to RECORD_LIMIT records
 * [u8 channel][u8 delivery][u16 length LE][packet]. Delivery bit7 sends the
 * packet straight to sp5_controller_receive; otherwise it travels as an
 * authenticated control event (type 0x0206) through receive_control, with
 * the low two bits as the transport delivery. Setup bit0/bit1 inject one fake
 * add/deliver failure to reach the partial-application paths. Packet layouts
 * follow protocol-notes.md (gamepad 0x0C, arrival/touch/motion/battery). */
#include "../fakes/platform.h"
#include "controller.h"
#include "harness.h"
#include <math.h>
#include <string.h>

enum { RECORD_LIMIT = 32, INPUT_LIMIT = 16384 };

static bool unit_interval(float value)
{
    return isfinite(value) && value >= 0.0F && value <= 1.0F;
}

static void check_controllers(const sp5_fake_platform *fake)
{
    size_t live = 0;
    for (size_t i = 0; i < SP5_MAX_CONTROLLERS; ++i) {
        const sp5_controller_state *state = &fake->controllers[i].state;
        live += fake->controllers[i].live ? 1 : 0;
        for (size_t t = 0; t < SP5_MAX_TOUCHES; ++t) {
            const sp5_touch *touch = &state->touches[t];
            fuzz_require(!touch->active ||
                         (unit_interval(touch->x) && unit_interval(touch->y) &&
                          unit_interval(touch->pressure) && touch->touchpad_index <= 1));
        }
        for (size_t axis = 0; axis < 3; ++axis) {
            fuzz_require(isfinite(state->gyro[axis]) && isfinite(state->acceleration[axis]));
        }
    }
    fuzz_require(live <= SP5_MAX_CONTROLLERS);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > INPUT_LIMIT) {
        return 0;
    }
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    if ((data[0] & 1) != 0) {
        fake.fail_next[SP5_FAKE_INPUT_ADD] = SP5_REASON_PLATFORM;
    }
    if ((data[0] & 2) != 0) {
        fake.fail_next[SP5_FAKE_INPUT_DELIVER] = SP5_REASON_PLATFORM;
    }
    sp5_controller_input *input = sp5_controller_create(1, &fake.input);
    fuzz_require(input != NULL);
    sp5_bytes whole = {data, size};
    size_t at = 1;
    for (unsigned count = 0; count < RECORD_LIMIT && size - at >= 4; ++count) {
        uint8_t channel = data[at];
        uint8_t delivery = data[at + 1];
        size_t length = (size_t)data[at + 2] | ((size_t)data[at + 3] << 8);
        at += 4;
        if (length > size - at) {
            length = size - at;
        }
        sp5_bytes packet = {data + at, length};
        fuzz_span(whole, packet);
        at += length;
        sp5_input_evidence evidence = {.session_id = 1, .event_id = count + 1};
        if ((delivery & 0x80) != 0) {
            (void)sp5_controller_receive(input, packet, (delivery & 0x40) != 0, &evidence);
        } else if (length <= SP5_CONTROL_PAYLOAD_LIMIT) {
            sp5_control_event event = {.type = 0x0206,
                                       .channel = channel,
                                       .delivery = (sp5_control_delivery)(delivery % 3),
                                       .sequence = count,
                                       .payload_size = length};
            if (length != 0) {
                memcpy(event.payload, packet.data, length);
            }
            (void)sp5_controller_receive_control(input, &event, (delivery & 0x40) != 0, &evidence);
        }
        fuzz_require(evidence.delivery != SP5_INPUT_DELIVERY_CONFIRMED || fake.confirm_delivery);
        check_controllers(&fake);
    }
    fuzz_require(sp5_controller_close(input).status == SP5_OK);
    for (size_t i = 0; i < SP5_MAX_CONTROLLERS; ++i) {
        fuzz_require(!fake.controllers[i].live && fake.controllers[i].state.buttons == 0);
    }
    sp5_controller_destroy(input);
    return 0;
}
