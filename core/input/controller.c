#include "controller.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Facts, not upstream implementation: protocol-notes §§9/12, mcc 41cedb8
 * Input.h:87-109,157-197; Limelight.h:720-742,777-834. Buttons retain the
 * canonical mask (guide/PS=0x400, touchpad click=0x100000). Platform adapters
 * own sourced console mappings/polarity; no unverified PS5 interface appears. */
_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "Input wire floats require IEEE754 binary32");
typedef struct {
    bool live, arrival;
    uint8_t wire_id;
    uint16_t capabilities;
    sp5_controller_state state;
} controller;
struct sp5_controller_input {
    uint64_t session_id;
    sp5_input_ops ops;
    controller pads[SP5_MAX_CONTROLLERS];
    bool closed, released, feedback_pending;
    sp5_controller_feedback feedback;
    sp5_session *session;
    uint8_t certificate[SP5_CERTIFICATE_CAPACITY];
    size_t certificate_size;
};
static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}
static void put16(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}
static float net_float(const uint8_t *p)
{
    uint32_t bits = le32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static int16_t signed_axis(const uint8_t *p)
{
    uint16_t value = le16(p);
    /* Avoid implementation-defined unsigned-to-signed conversion. */
    return (int16_t)(value <= INT16_MAX ? (int32_t)value : (int32_t)value - 65536);
}
static bool authorized(const sp5_controller_input *in)
{
    if (in->closed)
        return false;
    if (in->session == NULL)
        return true;
    /* Input owner: immutable lease/ID and atomic state, no management snapshot. */
    return sp5_session_authorized_id(
        in->session, (sp5_bytes){in->certificate, in->certificate_size}, in->session_id);
}
static sp5_result failure(sp5_input_evidence *event, sp5_reason reason)
{
    if (event != NULL) {
        event->delivery = SP5_INPUT_DELIVERY_FAILED;
        event->reason = reason;
        event->console_delivered = (sp5_timestamp){0};
        event->delivery_boundary_ref = NULL;
    }
    return sp5_failure(reason);
}
sp5_controller_input *sp5_controller_create(uint64_t session_id, const sp5_input_ops *ops)
{
    if (session_id == 0 || ops == NULL || ops->add == NULL || ops->deliver == NULL ||
        ops->remove == NULL || ops->release_all == NULL)
        return NULL;
    sp5_controller_input *in = calloc(1, sizeof(*in));
    if (in == NULL)
        return NULL;
    in->session_id = session_id;
    in->ops = *ops;
    return in;
}
sp5_controller_input *sp5_controller_create_bound(sp5_session *session, sp5_bytes cert,
                                                  const sp5_input_ops *ops)
{
    if (cert.size > SP5_CERTIFICATE_CAPACITY || !sp5_session_authorized(session, cert))
        return NULL;
    sp5_controller_input *in = sp5_controller_create(sp5_session_read(session).id, ops);
    if (in == NULL)
        return NULL;
    memcpy(in->certificate, cert.data, cert.size);
    in->certificate_size = cert.size;
    in->session = session;
    return in;
}
static size_t find(const sp5_controller_input *in, uint8_t wire)
{
    for (size_t i = 0; i < SP5_MAX_CONTROLLERS; ++i)
        if (in->pads[i].live && in->pads[i].wire_id == wire)
            return i;
    return SP5_MAX_CONTROLLERS;
}
static sp5_result add(sp5_controller_input *in, uint8_t wire, size_t *slot)
{
    *slot = find(in, wire);
    if (*slot < SP5_MAX_CONTROLLERS)
        return sp5_success();
    for (size_t i = 0; i < SP5_MAX_CONTROLLERS; ++i)
        if (!in->pads[i].live) {
            if (!authorized(in))
                return sp5_failure(SP5_REASON_OWNERSHIP);
            sp5_result result = in->ops.add(in->ops.context, in->session_id, (uint32_t)i);
            if (result.status != SP5_OK)
                return result;
            in->pads[i] = (controller){.live = true, .wire_id = wire};
            *slot = i;
            return sp5_success();
        }
    return sp5_failure(SP5_REASON_CAPACITY);
}
static sp5_result deliver(sp5_controller_input *in, size_t slot, const sp5_controller_state *state,
                          bool metrics, sp5_input_evidence *event)
{
    if (!authorized(in))
        return failure(event, SP5_REASON_OWNERSHIP);
    event->controller_id = (uint32_t)slot;
    event->delivery_attempted = true;
    sp5_result result = in->ops.deliver(in->ops.context, state, metrics, event);
    if (result.status != SP5_OK)
        return failure(event, result.reason);
    in->pads[slot].state = *state;
    return result;
}
static sp5_result gamepad(sp5_controller_input *in, const uint8_t *p, bool metrics,
                          sp5_input_evidence *event)
{
    uint16_t wire = le16(p + 10), mask = le16(p + 12);
    if (wire >= 16)
        return failure(event, SP5_REASON_INVALID_ARGUMENT);
    size_t slot = find(in, (uint8_t)wire);
    if ((mask & (1U << wire)) == 0) {
        if (slot == SP5_MAX_CONTROLLERS)
            return sp5_success();
        event->controller_id = (uint32_t)slot;
        if (!authorized(in))
            return failure(event, SP5_REASON_OWNERSHIP);
        sp5_result result = in->ops.remove(in->ops.context, in->session_id, (uint32_t)slot);
        if (result.status != SP5_OK)
            return failure(event, result.reason);
        in->pads[slot] = (controller){0};
        if (in->feedback_pending && in->feedback.controller_id == slot)
            in->feedback_pending = false;
        return result;
    }
    sp5_result result = add(in, (uint8_t)wire, &slot);
    if (result.status != SP5_OK)
        return failure(event, result.reason);
    sp5_controller_state state = in->pads[slot].state;
    state.buttons = le16(p + 16) | ((uint32_t)le16(p + 30) << 16);
    state.triggers[0] = (uint16_t)(p[18] * 257U);
    state.triggers[1] = (uint16_t)(p[19] * 257U);
    for (size_t i = 0; i < 4; ++i)
        state.axes[i] = signed_axis(p + 20 + 2 * i);
    return deliver(in, slot, &state, metrics, event);
}
static float normalized(float value)
{
    return value < 0 ? 0 : (value > 1 ? 1 : value);
}
static sp5_result surface(sp5_controller_input *in, size_t slot, const uint8_t *p, bool metrics,
                          sp5_input_evidence *event)
{
    uint8_t type = p[9], surface_id = p[11];
    float x = net_float(p + 16), y = net_float(p + 20), pressure = net_float(p + 24);
    if (type > 7 || surface_id > 1 || !isfinite(x) || !isfinite(y) || !isfinite(pressure))
        return failure(event, SP5_REASON_INVALID_ARGUMENT);
    if (type == 0 || type == 5 || type == 6)
        return failure(event, SP5_REASON_UNSUPPORTED);
    sp5_controller_state state = in->pads[slot].state;
    uint32_t id = le32(p + 12);
    size_t contact = SP5_MAX_TOUCHES;
    for (size_t i = 0; i < SP5_MAX_TOUCHES; ++i)
        if (state.touches[i].active && state.touches[i].contact_id == id &&
            state.touches[i].touchpad_index == surface_id)
            contact = i;
    if (type == 7) {
        memset(state.touches, 0, sizeof(state.touches));
    } else if (type == 2 || type == 4) {
        if (contact < SP5_MAX_TOUCHES)
            state.touches[contact] = (sp5_touch){0};
    } else {
        if (contact == SP5_MAX_TOUCHES)
            for (size_t i = 0; i < SP5_MAX_TOUCHES; ++i)
                if (!state.touches[i].active) {
                    contact = i;
                    break;
                }
        if (contact == SP5_MAX_TOUCHES)
            return failure(event, SP5_REASON_CAPACITY);
        state.touches[contact] = (sp5_touch){.active = true,
                                             .contact_id = id,
                                             .x = normalized(x),
                                             .y = normalized(y),
                                             .pressure = normalized(pressure),
                                             .touchpad_index = surface_id};
    }
    state.has_touch = true;
    return deliver(in, slot, &state, metrics, event);
}
static sp5_result motion(sp5_controller_input *in, size_t slot, const uint8_t *p, bool metrics,
                         sp5_input_evidence *event)
{
    if (p[9] != 1 && p[9] != 2)
        return failure(event, SP5_REASON_UNSUPPORTED);
    sp5_controller_state state = in->pads[slot].state;
    for (size_t i = 0; i < 3; ++i) {
        float value = net_float(p + 12 + 4 * i);
        if (!isfinite(value))
            return failure(event, SP5_REASON_INVALID_ARGUMENT);
        if (p[9] == 1)
            state.acceleration[i] = value;
        else
            state.gyro[i] = value * 0.01745329251994329577F;
    }
    state.has_motion = true;
    return deliver(in, slot, &state, metrics, event);
}
sp5_result sp5_controller_receive(sp5_controller_input *in, sp5_bytes packet, bool metrics,
                                  sp5_input_evidence *event)
{
    if (event != NULL) {
        event->delivery = SP5_INPUT_DELIVERY_UNCONFIRMED;
        event->reason = SP5_REASON_DELIVERY_UNCONFIRMED;
        event->console_delivered = (sp5_timestamp){0};
        event->delivery_boundary_ref = NULL;
    }
    if (in == NULL || event == NULL || packet.data == NULL || packet.size < 8 ||
        packet.size > SP5_CONTROL_PAYLOAD_LIMIT || event->session_id != in->session_id ||
        be32(packet.data) != packet.size - 4)
        return failure(event, SP5_REASON_INVALID_ARGUMENT);
    if (!authorized(in))
        return failure(event, SP5_REASON_OWNERSHIP);
    const uint8_t *p = packet.data;
    uint32_t magic = le32(p + 4);
    size_t expected;
    switch (magic) {
    case 0x0c:
        expected = 34;
        break;
    case 0x55000004:
        expected = 16;
        break;
    case 0x55000005:
        expected = 28;
        break;
    case 0x55000006:
        expected = 24;
        break;
    case 0x55000007:
        expected = 12;
        break;
    case 0x0d:
        expected = 10;
        break;
    default:
        return failure(event, SP5_REASON_UNSUPPORTED);
    }
    if (packet.size != expected)
        return failure(event, SP5_REASON_INVALID_ARGUMENT);
    if (magic == 0x0c)
        return gamepad(in, p, metrics, event);
    if (magic == 0x0d)
        return le16(p + 8) <= 1 ? sp5_success() : failure(event, SP5_REASON_INVALID_ARGUMENT);
    if (p[8] >= 16)
        return failure(event, SP5_REASON_INVALID_ARGUMENT);
    size_t slot = find(in, p[8]);
    if (magic == 0x55000004) {
        if (p[9] > 4)
            return failure(event, SP5_REASON_UNSUPPORTED);
        sp5_result result = add(in, p[8], &slot);
        if (result.status != SP5_OK)
            return failure(event, result.reason);
        in->pads[slot].arrival = true;
        in->pads[slot].capabilities = le16(p + 10);
        event->controller_id = (uint32_t)slot;
        return sp5_success();
    }
    if (slot == SP5_MAX_CONTROLLERS)
        return failure(event, SP5_REASON_OWNERSHIP);
    if (magic == 0x55000005)
        return surface(in, slot, p, metrics, event);
    if (magic == 0x55000006)
        return motion(in, slot, p, metrics, event);
    /* Battery is understood but the portable adapter has no battery operation. */
    return p[9] <= 5 && (p[10] <= 100 || p[10] == 255)
               ? sp5_success()
               : failure(event, SP5_REASON_INVALID_ARGUMENT);
}
sp5_result sp5_controller_receive_control(sp5_controller_input *in, const sp5_control_event *packet,
                                          bool metrics, sp5_input_evidence *event)
{
    if (packet == NULL || packet->type != 0x0206 || packet->payload_size < 8 ||
        packet->payload_size > sizeof(packet->payload) ||
        packet->delivery == SP5_CONTROL_UNSEQUENCED)
        return failure(event, SP5_REASON_INVALID_ARGUMENT);
    const uint8_t *p = packet->payload;
    uint32_t magic = le32(p + 4);
    bool sensor = magic == 0x55000006;
    bool transient =
        sensor || (magic == 0x55000005 && packet->payload_size == 28 && (p[9] == 0 || p[9] == 3));
    if (!transient && packet->delivery != SP5_CONTROL_RELIABLE)
        return failure(event, SP5_REASON_INVALID_ARGUMENT);
    if (packet->channel != 0) {
        if (packet->payload_size < 12)
            return failure(event, SP5_REASON_INVALID_ARGUMENT);
        unsigned id = magic == 0x0c ? le16(p + 10) : p[8];
        if (id >= 16 || packet->channel != (sensor ? 32U : 16U) + id || magic == 0x0d)
            return failure(event, SP5_REASON_INVALID_ARGUMENT);
    }
    return sp5_controller_receive(in, (sp5_bytes){p, packet->payload_size}, metrics, event);
}
sp5_result sp5_controller_forward_feedback(sp5_controller_input *in, sp5_control *control)
{
    if (in == NULL || control == NULL || !authorized(in))
        return sp5_failure(SP5_REASON_OWNERSHIP);
    if (!in->feedback_pending) {
        if (in->ops.poll_feedback == NULL)
            return sp5_failure(SP5_REASON_UNSUPPORTED);
        sp5_result result = in->ops.poll_feedback(in->ops.context, &in->feedback);
        if (result.status != SP5_OK)
            return result;
        in->feedback_pending = true;
    }
    uint32_t slot = in->feedback.controller_id;
    if (slot >= SP5_MAX_CONTROLLERS || !in->pads[slot].live) {
        in->feedback_pending = false;
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    const controller *pad = &in->pads[slot];
    if (pad->arrival && (pad->capabilities & 2U) == 0) {
        in->feedback_pending = false;
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    }
    uint8_t payload[10] = {0xee, 0xff, 0xc0, 0};
    put16(payload + 4, pad->wire_id);
    put16(payload + 6, in->feedback.low_frequency);
    put16(payload + 8, in->feedback.high_frequency);
    if (!authorized(in))
        return sp5_failure(SP5_REASON_OWNERSHIP);
    sp5_result result = sp5_control_send(control, 0x010b, (sp5_bytes){payload, sizeof(payload)});
    if (result.status == SP5_OK)
        in->feedback_pending = false;
    return result;
}
sp5_result sp5_controller_motion_request(sp5_controller_input *in, sp5_control *control,
                                         sp5_controller_motion_config config)
{
    if (in == NULL || control == NULL || config.controller_id >= SP5_MAX_CONTROLLERS ||
        (config.motion_type != 1 && config.motion_type != 2))
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (!authorized(in) || !in->pads[config.controller_id].live)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    const controller *pad = &in->pads[config.controller_id];
    if (!pad->arrival || (pad->capabilities & (config.motion_type == 1 ? 0x10U : 0x20U)) == 0)
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    /* notes §§9/12.5; mcc 41cedb8 Limelight.h:813-822,
     * ControlStream.c:1074-1080: host requests rate/type before client sends. */
    uint8_t payload[5];
    put16(payload, pad->wire_id);
    put16(payload + 2, config.rate_hz);
    payload[4] = config.motion_type;
    return sp5_control_send(control, 0x5501, (sp5_bytes){payload, sizeof(payload)});
}
sp5_result sp5_controller_close(sp5_controller_input *in)
{
    if (in == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    in->closed = true;
    in->feedback_pending = false;
    if (in->released)
        return sp5_success();
    sp5_result result = in->ops.release_all(in->ops.context, in->session_id);
    if (result.status == SP5_OK) {
        memset(in->pads, 0, sizeof(in->pads));
        in->released = true;
        return result;
    }
    /* Preserve the safety error but attempt remaining safe neutralize/remove
     * operations once. A failure never reopens admission or waits on media. */
    bool all_removed = true;
    for (size_t i = 0; i < SP5_MAX_CONTROLLERS; ++i)
        if (in->pads[i].live) {
            sp5_controller_state neutral = {0};
            sp5_input_evidence event = {.session_id = in->session_id, .controller_id = (uint32_t)i};
            sp5_result delivered = in->ops.deliver(in->ops.context, &neutral, false, &event);
            sp5_result removed = in->ops.remove(in->ops.context, in->session_id, (uint32_t)i);
            if (removed.status == SP5_OK)
                in->pads[i] = (controller){0};
            else
                all_removed = false;
            if (delivered.status != SP5_OK)
                all_removed = false;
        }
    in->released = all_removed;
    return result;
}
void sp5_controller_destroy(sp5_controller_input *in)
{
    if (in == NULL)
        return;
    /* Owners inspect close() before destruction; void destroy is last-resort
     * cleanup and cannot turn a failed platform release into delivery evidence. */
    (void)sp5_controller_close(in);
    memset(in, 0, sizeof(*in));
    free(in);
}

sp5_controller_input *sp5_controller_create_reserved(sp5_session *session, sp5_bytes cert,
                                                     const sp5_input_ops *ops)
{
    uint64_t id = sp5_session_read(session).id;
    if (cert.size > SP5_CERTIFICATE_CAPACITY || !sp5_session_admitted_id(session, cert, id))
        return NULL;
    sp5_controller_input *in = sp5_controller_create(id, ops);
    if (in == NULL)
        return NULL;
    memcpy(in->certificate, cert.data, cert.size);
    in->certificate_size = cert.size;
    in->session = session;
    return in;
}
