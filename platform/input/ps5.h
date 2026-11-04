#ifndef SUNSHINE_PLATFORM_INPUT_PS5_H
#define SUNSHINE_PLATFORM_INPUT_PS5_H

#include "platform.h"

#include <stdbool.h>
#include <stdint.h>

/* PROVISIONAL production adapter for firmware 13.60, not device-qualified.
 *
 * Source of the virtual-device path: tools/ps5-probes/input_system.c and
 * tools/ps5-probes/input_system.h (T042), derived from leolobato/ps5-mcp
 * 84c0f7b316772b13cd62e0121c154afc3ed2482c docs/vpad-abi.md and
 * payload/system_ps5.c. The retained T041/T042 reports are
 * specs/001-moonlight-streaming-host/research-r3-findings.md and
 * tools/ps5-probes/README.md.
 *
 * Fixed ABI reused without invention:
 * - /dev/hid O_RDWR; add ioctl 0xc018482a (PadRequest, descriptor 168 B),
 *   report ioctl 0x8018482c (PadRequest, 160 B), remove ioctl 0x80104850
 *   (operation 5, handle at offset 8).
 * - UserService Initialize/GetForegroundUser/Terminate exactly as the probe's
 *   sourced declarations. The final link must provide -lSceUserService.
 * - Controller IDs 0..3 are assigned by core in connection order; the adapter
 *   stores one owned handle per ID and never removes another session's handle.
 * - Only the exact retained firmware build is admitted before /dev/hid is
 *   opened; unsupported firmware returns UNSUPPORTED without touching ioctls.
 *
 * Delivery truth: a returned-zero report ioctl is driver acceptance only. No
 * console-consumption boundary or timestamp is sourced, so deliver() always
 * reports CONFIRMED==false with DELIVERY_UNCONFIRMED and console_delivered
 * absent. Network ACK, ioctl return zero, or application effects are not ACK.
 * deliver() sends exactly the supplied complete state (no hidden hold/ACK
 * transaction); add/remove/release_all supply explicit neutral reports around
 * controller lifecycle, including neutral-before-remove.
 *
 * Mapping in this provisional adapter:
 * - Moonlight canonical buttons -> the sourced HID report button bits used by
 *   input_system.c: Up 0x10, Right 0x20, Down 0x40, Left 0x80, L1 0x400,
 *   R1 0x800, L2 0x100, R2 0x200, L3 0x2, R3 0x4, Create 0x1, Options 0x8,
 *   Triangle 0x1000, Circle 0x2000, Cross 0x4000, Square 0x8000,
 *   PS 0x10000, touchpad click 0x100000.
 * - Provenance is deliberately narrow: only Cross and D-pad are
 *   owner-corroborated. Create, Options, PS, touchpad click, shoulders,
 *   stick clicks, proportional sticks/triggers, rumble, motion and touch
 *   remain unverified.
 * - Sticks are 16-bit signed -> one byte (0x80 centre); L2/R2 16-bit -> one
 *   byte plus their digital bits. Up to two touch points (normalized ->
 *   1920x1080) and gyro (rad/s) / acceleration (m/s^2 -> g) go to the report
 *   fields libScePad's scePadVirtualDeviceInsertData uses on 13.60
 *   (disassembled); the orientation stays the identity quaternion.
 * - Rumble feedback has no sourced virtual-controller output report;
 *   poll_feedback returns STAGE_UNAVAILABLE explicitly.
 *
 * release_all() neutralizes every owned handle first, parks handles in reverse
 * slot order, is idempotent after success, and never waits for
 * media/network/statistics. Parking rather than DeleteDevice keeps one neutral
 * virtual pad registered across sessions, so the console/game does not raise a
 * controller-disconnected dialog at teardown. A later add() reuses the parked
 * handle; the descriptor, /dev/hid fd and UserService binding stay open until
 * the process exits. A failed add clears its slot and, when it was the last controller, closes the
 * HID/UserService resources unless a parked pad owns them; its one
 * neutral/removal attempt follows the probe's no-uncertain-retry rule even if
 * the driver reports a cleanup error.
 * No allocation, per-frame logging, unbounded retry or timing measurement occurs.
 * The caller serializes access; one bounded ioctl/neutral/removal attempt is made
 * per operation. Platform calls themselves have no proven deadline.
 */
#define SP5_PS5_INPUT_PROVENANCE_REF                                                               \
    "T042: only Cross and D-pad owner-corroborated; Create, Options/PS/touchpad, proportional, "   \
    "rumble, motion, touch unverified"

enum {
    SP5_PS5_INPUT_HID_MAX = SP5_MAX_CONTROLLERS,
    SP5_PS5_INPUT_DESCRIPTOR_BYTES = 168,
    SP5_PS5_INPUT_REPORT_BYTES = 160
};

typedef struct {
    uint32_t operation_or_handle;
    uint32_t reserved;
    void *input;
    void *output;
} sp5_ps5_pad_request;

typedef struct {
    uint8_t operation;
    uint8_t reserved[7];
    uint32_t handle;
    uint32_t reserved2;
} sp5_ps5_remove_request;

typedef struct {
    uint8_t bytes[SP5_PS5_INPUT_DESCRIPTOR_BYTES];
} sp5_ps5_pad_descriptor;

typedef struct {
    uint8_t bytes[SP5_PS5_INPUT_REPORT_BYTES];
} sp5_ps5_neutral_report;

typedef struct {
    int32_t handle;
    uint64_t session_id;
    bool live;
    /* Orientation estimate for the report (a virtual pad gets no console
     * sensor fusion): quaternion x, y, z, w, last gravity direction from the
     * accelerometer, and the previous gyro sample time. */
    uint64_t motion_session;
    float orientation[4];
    float gravity[3];
    bool has_gravity;
    uint64_t last_gyro_us;
} sp5_ps5_input_controller;

typedef struct {
    int hid;
    int32_t user_id;
    bool user_initialized;
    sp5_reason cleanup_error;
    uint64_t accepted_reports;
    uint64_t failed_reports;
    sp5_ps5_input_controller controllers[SP5_PS5_INPUT_HID_MAX];
    /* Handles neutralized at session end but deliberately left registered.
     * Removing the virtual pad makes the console/game raise a controller-
     * disconnected dialog while the title is still running. Parking the handle
     * keeps one pad connected across sessions; the next session reuses it. */
    int32_t parked[SP5_PS5_INPUT_HID_MAX];
    size_t parked_count;
    int32_t external_handle;
} sp5_ps5_input;

void sp5_ps5_input_init(sp5_ps5_input *input);
void sp5_ps5_input_heartbeat(sp5_ps5_input *input);
/* Take ownership of a pad handed over by a previous host: hid is that
 * host's /dev/hid descriptor, which keeps the pad registered. The handle is
 * parked, so it is reused by the next session and the descriptor stays open
 * until this process exits or hands it on. Returns false on bad arguments or
 * when a descriptor is already open. */
bool sp5_ps5_input_adopt(sp5_ps5_input *input, int hid, int32_t handle);
sp5_input_ops sp5_ps5_input_ops(sp5_ps5_input *input);

#endif
