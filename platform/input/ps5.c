#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "ps5.h"

#include "firmware/13.60.h"
#include "runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Exact sourced UserService declarations from tools/ps5-probes/input_system.c:
 * leolobato/ps5-mcp 84c0f7b payload/system_ps5.c. The final link must supply
 * -lSceUserService; this console-only object deliberately keeps them external. */
int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int32_t *user_id);
int sceUserServiceTerminate(void);
int scePadInit(void);
int scePadSetLoginUserNumber(int32_t handle, int32_t user_number);

#define SP5_PS5_HID_PATH "/dev/hid"
#define SP5_PS5_HID_ADD UINT32_C(0xc018482a)
#define SP5_PS5_HID_REPORT UINT32_C(0x8018482c)
#define SP5_PS5_HID_REMOVE UINT32_C(0x80104850)
#define SP5_PS5_HID_REMOVE_OPERATION 5U

_Static_assert(sizeof(sp5_ps5_pad_request) == 24, "sourced ioctl header size");
_Static_assert(offsetof(sp5_ps5_pad_request, input) == 8, "sourced input pointer offset");
_Static_assert(offsetof(sp5_ps5_pad_request, output) == 16, "sourced output pointer offset");
_Static_assert(sizeof(sp5_ps5_remove_request) == 16, "sourced removal size");
_Static_assert(offsetof(sp5_ps5_remove_request, handle) == 8, "sourced removal handle offset");
_Static_assert(sizeof(sp5_ps5_pad_descriptor) == 168, "sourced descriptor size");
_Static_assert(sizeof(sp5_ps5_neutral_report) == 160, "sourced report size");
_Static_assert(sizeof(float) == 4, "sourced float32 orientation quaternion");
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "sourced PS5 little endian ABI");

enum {
    REPORT_BUTTONS = 0x0c,
    REPORT_LEFT_X = 0x10,
    REPORT_LEFT_Y = 0x11,
    REPORT_RIGHT_X = 0x12,
    REPORT_RIGHT_Y = 0x13,
    REPORT_LEFT_TRIGGER = 0x14,
    REPORT_RIGHT_TRIGGER = 0x15,
    REPORT_FLAGS = 0x17,
    /* Touch and motion layout as written by libScePad's
     * scePadVirtualDeviceInsertData on 13.60 (disassembled from the mapped
     * library, tools/ps5-probes/module_dump.c): ScePadData touch count,
     * per-finger id and u16 x/y, and the orientation/gyro/accel floats. */
    REPORT_TOUCH_COUNT = 0x1c,
    REPORT_TOUCH = 0x20, /* 8 bytes per finger: id, 3 pad, u16 x, u16 y */
    REPORT_ORIENTATION_X = 0x78,
    REPORT_ORIENTATION = 0x84, /* quaternion w */
    REPORT_GYRO = 0x88,        /* x, y, z rad/s */
    REPORT_ACCEL = 0x94,       /* x, y, z in g */
    TOUCH_WIDTH = 1920,        /* DualSense touchpad resolution */
    TOUCH_HEIGHT = 1080
};

enum {
    HID_UP = 0x00000010,
    HID_RIGHT = 0x00000020,
    HID_DOWN = 0x00000040,
    HID_LEFT = 0x00000080,
    HID_L2 = 0x00000100,
    HID_R2 = 0x00000200,
    HID_L1 = 0x00000400,
    HID_R1 = 0x00000800,
    HID_TRIANGLE = 0x00001000,
    HID_CIRCLE = 0x00002000,
    HID_CROSS = 0x00004000,
    HID_SQUARE = 0x00008000,
    HID_PS = 0x00010000,
    HID_TOUCHPAD = 0x00100000,
    HID_L3 = 0x00000002,
    HID_R3 = 0x00000004,
    HID_CREATE = 0x00000001,
    HID_OPTIONS = 0x00000008
};

typedef struct {
    uint32_t portable;
    uint32_t console;
} button_mapping;

/* Moonlight canonical mask (core/input/controller.c:8-10) and the sourced HID
 * report masks exercised by input_system.c. */
static const button_mapping button_mappings[] = {
    {0x0001U, HID_UP},         {0x0002U, HID_DOWN},    {0x0004U, HID_LEFT},
    {0x0008U, HID_RIGHT},      {0x0010U, HID_OPTIONS}, {0x0020U, HID_CREATE},
    {0x0040U, HID_L3},         {0x0080U, HID_R3},      {0x0100U, HID_L1},
    {0x0200U, HID_R1},         {0x0400U, HID_PS},      {0x1000U, HID_CROSS},
    {0x2000U, HID_CIRCLE},     {0x4000U, HID_SQUARE},  {0x8000U, HID_TRIANGLE},
    {0x100000U, HID_TOUCHPAD},
};

static sp5_result failure(sp5_reason reason)
{
    return sp5_failure(reason);
}

static sp5_result failed_evidence(sp5_input_evidence *evidence, sp5_reason reason)
{
    if (evidence != NULL) {
        evidence->delivery = SP5_INPUT_DELIVERY_FAILED;
        evidence->reason = reason;
        evidence->console_delivered = (sp5_timestamp){0};
        evidence->delivery_boundary_ref = NULL;
    }
    return failure(reason);
}

static uint32_t known_buttons(void)
{
    uint32_t mask = 0;
    for (size_t index = 0; index < sizeof(button_mappings) / sizeof(button_mappings[0]); ++index)
        mask |= button_mappings[index].portable;
    return mask;
}

static bool map_buttons(const sp5_controller_state *state, uint32_t *buttons)
{
    if ((state->buttons & ~known_buttons()) != 0)
        return false;
    uint32_t mask = 0;
    for (size_t index = 0; index < sizeof(button_mappings) / sizeof(button_mappings[0]); ++index) {
        if ((state->buttons & button_mappings[index].portable) != 0)
            mask |= button_mappings[index].console;
    }
    /* input_system.c R3_MODE_GAME sets these digital bits together with the
     * proportional L2/R2 bytes. */
    if (state->triggers[0] != 0)
        mask |= HID_L2;
    if (state->triggers[1] != 0)
        mask |= HID_R2;
    *buttons = mask;
    return true;
}

static uint8_t stick_byte(int16_t axis)
{
    return (uint8_t)(((int32_t)axis + INT32_C(32768)) >> 8);
}

/* Moonlight sends stick Y with up positive (XInput convention); the console
 * report has 0 at the top. Centre stays 0x80, matching report_neutral. */
static uint8_t stick_byte_y(int16_t axis)
{
    int32_t value = (INT32_C(32768) - (int32_t)axis) >> 8;
    return (uint8_t)(value > 255 ? 255 : value);
}

static uint8_t trigger_byte(uint16_t trigger)
{
    return (uint8_t)(trigger >> 8);
}

static void report_neutral(sp5_ps5_neutral_report *report)
{
    const float orientation = 1.0f;
    memset(report, 0, sizeof(*report));
    report->bytes[REPORT_LEFT_X] = 0x80;
    report->bytes[REPORT_LEFT_Y] = 0x80;
    report->bytes[REPORT_RIGHT_X] = 0x80;
    report->bytes[REPORT_RIGHT_Y] = 0x80;
    report->bytes[REPORT_FLAGS] = 1;
    memcpy(report->bytes + REPORT_ORIENTATION, &orientation, sizeof(orientation));
}

static bool report_state(const sp5_controller_state *state, sp5_ps5_neutral_report *report)
{
    uint32_t buttons;
    if (!map_buttons(state, &buttons))
        return false;
    report_neutral(report);
    for (size_t index = 0; index < 4; ++index)
        report->bytes[REPORT_BUTTONS + index] = (uint8_t)(buttons >> (8U * index));
    report->bytes[REPORT_LEFT_X] = stick_byte(state->axes[0]);
    report->bytes[REPORT_LEFT_Y] = stick_byte_y(state->axes[1]);
    report->bytes[REPORT_RIGHT_X] = stick_byte(state->axes[2]);
    report->bytes[REPORT_RIGHT_Y] = stick_byte_y(state->axes[3]);
    report->bytes[REPORT_LEFT_TRIGGER] = trigger_byte(state->triggers[0]);
    report->bytes[REPORT_RIGHT_TRIGGER] = trigger_byte(state->triggers[1]);
    if (state->has_touch) {
        uint8_t count = 0;
        for (size_t index = 0; index < SP5_MAX_TOUCHES && count < 2; ++index) {
            const sp5_touch *touch = &state->touches[index];
            if (!touch->active)
                continue;
            uint8_t *finger = report->bytes + REPORT_TOUCH + 8U * count;
            uint16_t x = (uint16_t)(touch->x * (float)(TOUCH_WIDTH - 1) + 0.5f);
            uint16_t y = (uint16_t)(touch->y * (float)(TOUCH_HEIGHT - 1) + 0.5f);
            finger[0] = (uint8_t)(touch->contact_id & 0x7fU);
            memcpy(finger + 4, &x, sizeof(x));
            memcpy(finger + 6, &y, sizeof(y));
            ++count;
        }
        report->bytes[REPORT_TOUCH_COUNT] = count;
    }
    if (state->has_motion) {
        /* Moonlight motion uses SDL axes, which match the DualSense: gyro is
         * already rad/s in the portable state, acceleration is m/s^2. */
        for (size_t axis = 0; axis < 3; ++axis) {
            float gyro = state->gyro[axis];
            float accel = state->acceleration[axis] / 9.80665f;
            memcpy(report->bytes + REPORT_GYRO + 4U * axis, &gyro, sizeof(gyro));
            memcpy(report->bytes + REPORT_ACCEL + 4U * axis, &accel, sizeof(accel));
        }
    }
    return true;
}

static void descriptor_init(sp5_ps5_pad_descriptor *descriptor, int32_t user)
{
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->bytes[0] = 3;
    for (size_t index = 0; index < 4; ++index)
        descriptor->bytes[0x1c + index] = (uint8_t)((uint32_t)user >> (8U * index));
    descriptor->bytes[0x20] = 1;
}

static int submit_report(const sp5_ps5_input *input, int32_t handle,
                         const sp5_ps5_neutral_report *report)
{
    sp5_ps5_pad_request request = {
        .operation_or_handle = (uint32_t)handle,
        .reserved = 0,
        .input = (void *)report->bytes,
        .output = NULL,
    };
    return ioctl(input->hid, SP5_PS5_HID_REPORT, &request);
}

static sp5_ps5_input *active_context;

static size_t live_count(const sp5_ps5_input *input)
{
    size_t count = 0;
    for (size_t index = 0; index < SP5_PS5_INPUT_HID_MAX; ++index) {
        if (input->controllers[index].live)
            ++count;
    }
    return count;
}

/* A parked pad stays registered but neutral between sessions. Keeping it
 * registered avoids the console's "controller disconnected" dialog after
 * teardown while the game is still running. */
static int32_t take_parked(sp5_ps5_input *input)
{
    for (size_t index = 0; index < SP5_PS5_INPUT_HID_MAX; ++index) {
        if (input->parked[index] >= 0) {
            int32_t handle = input->parked[index];
            input->parked[index] = -1;
            --input->parked_count;
            return handle;
        }
    }
    return -1;
}

static bool park_handle(sp5_ps5_input *input, int32_t handle)
{
    /* Idempotent: a takeover host reuses one external handle every session,
     * and parking it twice would eventually fill the table and delete it. */
    for (size_t index = 0; index < SP5_PS5_INPUT_HID_MAX; ++index) {
        if (input->parked[index] == handle)
            return true;
    }
    for (size_t index = 0; index < SP5_PS5_INPUT_HID_MAX; ++index) {
        if (input->parked[index] < 0) {
            input->parked[index] = handle;
            ++input->parked_count;
            return true;
        }
    }
    return false;
}

static void delete_handle(sp5_ps5_input *input, int32_t handle)
{
    sp5_ps5_remove_request remove = {
        .operation = (uint8_t)SP5_PS5_HID_REMOVE_OPERATION,
        .handle = (uint32_t)handle,
    };
    (void)ioctl(input->hid, SP5_PS5_HID_REMOVE, &remove);
}

static sp5_reason close_resources(sp5_ps5_input *input)
{
    sp5_reason reason = SP5_REASON_NONE;
    /* A parked pad owns the descriptor and the user binding. Keep both open
     * until the process exits so the console never sees a disconnect. */
    if (input->hid >= 0 && input->parked_count == 0) {
        if (close(input->hid) == 0) {
            input->hid = -1;
        } else {
            reason = SP5_REASON_PLATFORM;
        }
    }
    if (input->user_initialized && input->parked_count == 0) {
        if (sceUserServiceTerminate() == 0) {
            input->user_initialized = false;
            input->user_id = -1;
        } else {
            reason = SP5_REASON_PLATFORM;
        }
    }
    if (input->hid < 0 && !input->user_initialized && active_context == input)
        active_context = NULL;
    /* M3: a later successful (or already-empty) cleanup clears the diagnostic. */
    input->cleanup_error = reason;
    return reason;
}

static sp5_result ensure_ready(sp5_ps5_input *input)
{
    if (active_context != NULL && active_context != input)
        return failure(SP5_REASON_OWNERSHIP);
    active_context = input;
    bool opened_user = false;
    if (!input->user_initialized) {
        if (sceUserServiceInitialize(NULL) != 0)
            return failure(SP5_REASON_PLATFORM);
        input->user_initialized = true;
        opened_user = true;
    }
    if (input->user_id < 0) {
        int32_t user_id = -1;
        if (sceUserServiceGetForegroundUser(&user_id) != 0 || user_id < 0) {
            if (opened_user) {
                input->user_initialized = false;
                input->user_id = -1;
                (void)sceUserServiceTerminate();
            }
            return failure(SP5_REASON_PLATFORM);
        }
        input->user_id = user_id;
    }
    if (input->hid < 0) {
        int fd = open(SP5_PS5_HID_PATH, O_RDWR);
        if (fd < 0) {
            if (opened_user) {
                input->user_initialized = false;
                input->user_id = -1;
                (void)sceUserServiceTerminate();
            }
            return failure(SP5_REASON_PLATFORM);
        }
        input->hid = fd;
    }
    return sp5_success();
}

static sp5_result input_add(void *context, uint64_t session_id, uint32_t controller_id)
{
    sp5_ps5_input *input = context;
    if (input == NULL || session_id == 0 || controller_id >= SP5_PS5_INPUT_HID_MAX)
        return failure(SP5_REASON_INVALID_ARGUMENT);
    sp5_ps5_input_controller *slot = &input->controllers[controller_id];
    if (slot->live)
        return slot->session_id == session_id ? sp5_success() : failure(SP5_REASON_OWNERSHIP);
    if (!sp5_ps5_firmware_supported())
        return failure(SP5_REASON_UNSUPPORTED);
    sp5_result ready = ensure_ready(input);
    if (ready.status != SP5_OK)
        return ready;

    /* A takeover handoff reuses the retiring host's already-assigned pad
     * handle. No AddDevice means the console never creates or prompts for a
     * new controller. */
    /* A handle the driver rejects was removed outside this host; drop it and
     * register a new pad rather than failing every later session. */
    if (input->external_handle >= 0) {
        sp5_ps5_neutral_report neutral;
        report_neutral(&neutral);
        if (submit_report(input, input->external_handle, &neutral) == 0) {
            slot->handle = input->external_handle;
            slot->session_id = session_id;
            slot->live = true;
            return sp5_success();
        }
        input->external_handle = -1;
    }

    /* Reuse a pad parked by the previous session: the console never observes
     * a disconnect, so the running game keeps its controller assignment. */
    for (int32_t reused = take_parked(input); reused >= 0; reused = take_parked(input)) {
        sp5_ps5_neutral_report neutral;
        report_neutral(&neutral);
        if (submit_report(input, reused, &neutral) != 0)
            continue;
        slot->handle = reused;
        slot->session_id = session_id;
        slot->live = true;
        return sp5_success();
    }

    sp5_ps5_pad_descriptor descriptor;
    descriptor_init(&descriptor, input->user_id);
    int32_t candidate = -1;
    sp5_ps5_pad_request request = {
        .operation_or_handle = 3,
        .reserved = 0,
        .input = descriptor.bytes,
        .output = &candidate,
    };
    int rc = ioctl(input->hid, SP5_PS5_HID_ADD, &request);
    if (rc != 0 || candidate < 0)
        goto fail;
    for (size_t index = 0; index < SP5_PS5_INPUT_HID_MAX; ++index) {
        if (input->controllers[index].live && input->controllers[index].handle == candidate) {
            /* Do not orphan the registration the driver just reported. The
             * duplicate's existing slot stays authoritative; a later deliver
             * surfaces any resulting ownership loss. */
            sp5_ps5_remove_request remove = {
                .operation = (uint8_t)SP5_PS5_HID_REMOVE_OPERATION,
                .handle = (uint32_t)candidate,
            };
            (void)ioctl(input->hid, SP5_PS5_HID_REMOVE, &remove);
            goto fail;
        }
    }
    slot->handle = candidate;
    slot->session_id = session_id;
    slot->live = true;
    /* Bind the virtual pad to the logged-in player number. Without this the
     * console can leave a newly registered pad unassigned when a physical
     * controller already owns player 1, so reports are accepted but ignored. */
    (void)scePadInit();
    (void)scePadSetLoginUserNumber(candidate, 1);
    sp5_ps5_neutral_report neutral;
    report_neutral(&neutral);
    if (submit_report(input, candidate, &neutral) != 0) {
        sp5_ps5_remove_request remove = {
            .operation = (uint8_t)SP5_PS5_HID_REMOVE_OPERATION,
            .handle = (uint32_t)candidate,
        };
        (void)ioctl(input->hid, SP5_PS5_HID_REMOVE, &remove);
        /* The removal attempt consumes the local handle even if the driver
         * reported failure, matching the probe's no-uncertain-retry rule. */
        *slot = (sp5_ps5_input_controller){.handle = -1};
        goto fail;
    }
    return sp5_success();

fail:
    if (live_count(input) == 0)
        (void)close_resources(input);
    return failure(SP5_REASON_PLATFORM);
}

static sp5_result input_deliver(void *context, const sp5_controller_state *state, bool metrics,
                                sp5_input_evidence *evidence)
{
    sp5_ps5_input *input = context;
    (void)metrics;
    if (input == NULL || state == NULL || evidence == NULL)
        return failure(SP5_REASON_INVALID_ARGUMENT);
    if (evidence->controller_id >= SP5_PS5_INPUT_HID_MAX)
        return failed_evidence(evidence, SP5_REASON_INVALID_ARGUMENT);
    const sp5_ps5_input_controller *slot = &input->controllers[evidence->controller_id];
    if (!slot->live || slot->session_id != evidence->session_id)
        return failed_evidence(evidence, SP5_REASON_OWNERSHIP);
    sp5_ps5_neutral_report report;
    if (!report_state(state, &report))
        return failed_evidence(evidence, SP5_REASON_UNSUPPORTED);
    evidence->delivery_attempted = true;
    if (submit_report(input, slot->handle, &report) != 0) {
        ++input->failed_reports;
        return failed_evidence(evidence, SP5_REASON_PLATFORM);
    }
    ++input->accepted_reports;
    evidence->delivery = SP5_INPUT_DELIVERY_UNCONFIRMED;
    evidence->reason = SP5_REASON_DELIVERY_UNCONFIRMED;
    evidence->console_delivered = (sp5_timestamp){0};
    evidence->delivery_boundary_ref = NULL;
    return sp5_success();
}

static sp5_result input_poll_feedback(void *context, sp5_controller_feedback *feedback)
{
    if (context == NULL || feedback == NULL)
        return failure(SP5_REASON_INVALID_ARGUMENT);
    *feedback = (sp5_controller_feedback){0};
    /* No sourced virtual-device output report or reverse rumble callback exists
     * (T042 evidence ledger). Do not guess an output report. */
    return failure(SP5_REASON_STAGE_UNAVAILABLE);
}

static sp5_result input_remove(void *context, uint64_t session_id, uint32_t controller_id)
{
    sp5_ps5_input *input = context;
    if (input == NULL || session_id == 0 || controller_id >= SP5_PS5_INPUT_HID_MAX)
        return failure(SP5_REASON_INVALID_ARGUMENT);
    sp5_ps5_input_controller *slot = &input->controllers[controller_id];
    if (!slot->live) {
        /* M3: an already-empty remove may still close partial resources and
         * clear a previous cleanup diagnostic. */
        if (live_count(input) == 0) {
            sp5_reason cleanup = close_resources(input);
            if (cleanup != SP5_REASON_NONE)
                return failure(cleanup);
        }
        return sp5_success();
    }
    if (slot->session_id != session_id)
        return failure(SP5_REASON_OWNERSHIP);

    sp5_reason reason = SP5_REASON_NONE;
    sp5_ps5_neutral_report neutral;
    report_neutral(&neutral);
    if (submit_report(input, slot->handle, &neutral) != 0)
        reason = SP5_REASON_PLATFORM;
    /* Park instead of DeleteDevice. The device/report handle stays valid, so
     * the next session can reuse it and the console never raises the
     * controller-disconnected dialog at session end. */
    int32_t handle = slot->handle;
    *slot = (sp5_ps5_input_controller){.handle = -1};
    if (!park_handle(input, handle))
        delete_handle(input, handle);
    if (live_count(input) == 0 && input->parked_count == 0) {
        sp5_reason cleanup = close_resources(input);
        if (cleanup != SP5_REASON_NONE)
            reason = cleanup;
    }
    return reason == SP5_REASON_NONE ? sp5_success() : failure(reason);
}

static sp5_result input_release_all(void *context, uint64_t session_id)
{
    sp5_ps5_input *input = context;
    if (input == NULL || session_id == 0)
        return failure(SP5_REASON_INVALID_ARGUMENT);
    sp5_reason reason = SP5_REASON_NONE;

    /* Neutralize every owned handle first, even after an earlier error. */
    for (size_t index = 0; index < SP5_PS5_INPUT_HID_MAX; ++index) {
        sp5_ps5_input_controller *slot = &input->controllers[index];
        if (!slot->live || slot->session_id != session_id)
            continue;
        sp5_ps5_neutral_report neutral;
        report_neutral(&neutral);
        if (submit_report(input, slot->handle, &neutral) != 0)
            reason = SP5_REASON_PLATFORM;
    }
    /* Park in reverse connection/slot order; the handles stay registered so
     * teardown does not look like a controller disconnect to the console. */
    for (size_t index = SP5_PS5_INPUT_HID_MAX; index > 0; --index) {
        sp5_ps5_input_controller *slot = &input->controllers[index - 1];
        if (!slot->live || slot->session_id != session_id)
            continue;
        int32_t handle = slot->handle;
        *slot = (sp5_ps5_input_controller){.handle = -1};
        if (!park_handle(input, handle))
            delete_handle(input, handle);
    }
    if (live_count(input) == 0 && input->parked_count == 0) {
        sp5_reason cleanup = close_resources(input);
        if (cleanup != SP5_REASON_NONE)
            reason = cleanup;
    }
    return reason == SP5_REASON_NONE ? sp5_success() : failure(reason);
}

void sp5_ps5_input_init(sp5_ps5_input *input)
{
    if (input == NULL)
        return;
    memset(input, 0, sizeof(*input));
    input->hid = -1;
    input->user_id = -1;
    input->cleanup_error = SP5_REASON_NONE;
    for (size_t index = 0; index < SP5_PS5_INPUT_HID_MAX; ++index) {
        input->controllers[index].handle = -1;
        input->parked[index] = -1;
    }
    input->parked_count = 0;
    input->external_handle = -1;
}

bool sp5_ps5_input_adopt(sp5_ps5_input *input, int hid, int32_t handle)
{
    if (input == NULL || hid < 0 || handle < 0 || input->hid >= 0)
        return false;
    if (!park_handle(input, handle))
        return false;
    input->hid = hid;
    return true;
}

/* Keep a parked pad's kernel registration from timing out while no session is
 * sending reports. Called only when the host is idle. */
void sp5_ps5_input_heartbeat(sp5_ps5_input *input)
{
    if (input == NULL || input->hid < 0)
        return;
    sp5_ps5_neutral_report neutral;
    report_neutral(&neutral);
    for (size_t index = 0; index < SP5_PS5_INPUT_HID_MAX; ++index) {
        if (input->parked[index] >= 0)
            (void)submit_report(input, input->parked[index], &neutral);
    }
}

sp5_input_ops sp5_ps5_input_ops(sp5_ps5_input *input)
{
    if (input == NULL)
        return (sp5_input_ops){0};
    return (sp5_input_ops){
        .context = input,
        .add = input_add,
        .deliver = input_deliver,
        .poll_feedback = input_poll_feedback,
        .remove = input_remove,
        .release_all = input_release_all,
    };
}
