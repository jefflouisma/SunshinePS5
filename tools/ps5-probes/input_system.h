#ifndef R3_INPUT_SYSTEM_H
#define R3_INPUT_SYSTEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t operation_or_handle;
    uint32_t reserved;
    void *input;
    void *output;
} PadRequest;

typedef struct {
    uint8_t operation;
    uint8_t reserved[7];
    uint32_t handle;
    uint32_t reserved2;
} RemoveRequest;

typedef struct {
    uint8_t bytes[168];
} PadDescriptor;

typedef struct {
    uint8_t bytes[160];
} NeutralReport;

typedef struct {
    uint16_t x, y;
    uint8_t id;
    uint8_t reserved[3];
} PadTouch;

typedef struct {
    uint32_t buttons;
    uint8_t sticks[4];
    uint8_t triggers[2];
    uint8_t analog_reserved[2];
    float motion[10];
    uint8_t touch_count;
    uint8_t touch_reserved[7];
    PadTouch touch[2];
    int32_t connected;
    uint64_t timestamp_us;
    uint32_t extension_unit_id;
    uint8_t extension_reserved;
    uint8_t extension_length;
    uint8_t extension_data[10];
    uint8_t connected_count;
    uint8_t reserved[2];
    uint8_t device_unique_length;
    uint8_t device_unique_data[12];
} PadData;

typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2;
    uint8_t reserved[2];
    float motion[8];
    uint16_t touch_x, touch_y;
    uint8_t touch_id, touch_count;
    uint8_t touch_reserved[2];
} InputState;

typedef enum {
    R3_MODE_NEUTRAL,
    R3_MODE_NAVIGATION,
    R3_MODE_CONTROLS,
    R3_MODE_ASSIGN_FOUR,
    R3_MODE_READBACK,
    R3_MODE_TYPED,
    R3_MODE_STOP_HELD,
    R3_MODE_GAME,
    R3_MODE_NEUTRAL_OBSERVE
} ProbeMode;

void descriptor_init(PadDescriptor *descriptor, int32_t user);
void neutral_init(NeutralReport *report);
InputState experiment_state(ProbeMode mode, unsigned slot, uint64_t elapsed);
unsigned input_controller_count(ProbeMode mode);
void report_set_state(NeutralReport *report, const InputState *state);
bool update_slot_report(NeutralReport *report, InputState *previous, const InputState *state);
void typed_data_set_state(PadData *data, const InputState *state, uint64_t timestamp_us);

#endif
