#include "input.h"
#include "raylib.h"
#include <assert.h>


#define KEYBOARD_COUNT 349
#define MOUSE_COUNT 7
#define DEVICE_COUNT 4
#define GAMEPAD_BUTTON_COUNT 18
#define AXIS_COUNT 6
#define DEAD_ZONE 0.5f

typedef struct {
    InputButtonState state;
    float value;
    char sign;
} AxisState;

// Inner buffer carrying information about axis to be able read as buttons
static AxisState axis_state[DEVICE_COUNT * AXIS_COUNT];

// Logic for comparing axis previous value to determine its button state
static void UpdateAxisState(char device, char axis, float dead_zone);

static inline char GetAxisIndex(char device, char axis) {
    return device * AXIS_COUNT + axis;
}

static inline char CalculateAxisSign(float value, float dead_zone) {
    char result = value > dead_zone ? 1 : value < -dead_zone ? -1 : 0;
    return result;
}

static void UpdateAxisState(char device, char axis, float dead_zone) {
    char axis_index = GetAxisIndex(device, axis);
    float value = GetGamepadAxisMovement(device, axis);
    float value_abs = (value < 0.f) ? -value : value;
    char value_sign = CalculateAxisSign(value, dead_zone);

    float buffer_value = axis_state[axis_index].value;
    char buffer_sign = CalculateAxisSign(buffer_value, dead_zone);

    if (value_sign != buffer_sign) {
        if (value_abs < dead_zone) {
            axis_state[axis_index].state = INPUT_BUTTON_STATE_RELEASED;
        } else {
            axis_state[axis_index].state = INPUT_BUTTON_STATE_PRESSED;
            axis_state[axis_index].sign = value_sign;
        }
    }
    else if (value_abs > dead_zone) {
        axis_state[axis_index].state = INPUT_BUTTON_STATE_HELD;
        axis_state[axis_index].sign = value_sign;
    }
    else if (value_abs < dead_zone) {
        axis_state[axis_index].state = INPUT_BUTTON_STATE_NONE;
    }
    axis_state[axis_index].value = value;
}

/* ----------- PUBLIC ------------------- */

// Update axis state to read them also as button state
void InputUpdateAxis() {
    for (int device = 0; device < DEVICE_COUNT; device += 1) {
        if (!IsGamepadAvailable(device)) { return; }
        for (int axis = 0; axis < AXIS_COUNT; axis += 1) {
            // TODO: read deadzone from settings
            UpdateAxisState(device, axis, DEAD_ZONE);
        }
    }
}

// Read axis state as button if UpdateAxis has been called at update beginning
InputButtonState InputAxisState(char device, char axis) {
    assert(device < DEVICE_COUNT);
    assert(axis < AXIS_COUNT);
    char axis_index = GetAxisIndex(device, axis);
    InputButtonState result = axis_state[axis_index].state;
}

// Read float value from inputs
float InputGetValue(InputAction input) {
    switch(input.type) {
        case INPUT_TYPE_KEYBOARD:
            return IsKeyDown(input.id) ? 1 : 0;
        case INPUT_TYPE_MOUSE:
            return IsMouseButtonDown(input.id) ? 1 : 0;
        case INPUT_TYPE_GAMEPAD:
            return IsGamepadButtonDown(input.device, input.id);
        case INPUT_TYPE_AXIS:
            char axis_index = GetAxisIndex(input.device, input.id);
            float value = axis_state[axis_index].value;

            float value_abs = (value < 0.f) ? -1 : value;
            if (value_abs < input.dead_zone) { return 0; }

            char value_sign = CalculateAxisSign(value, input.dead_zone);
            if (value_sign != input.sign) { return 0; }
            // TODO: improve logic
            // lerp from dead_zone
            return ((value_abs - input.dead_zone) / (1 - input.dead_zone)) * value_sign;
        case INPUT_TYPE_NONE:
            return 0;
        default:
            assert(false);
    }
}

// Read float value for directions -1 to 1
char InputGetValueAxis(InputAction negative, InputAction positive) {
    return InputGetValue(positive) - InputGetValue(negative);
}

InputListenResult InputListenRebind() {
    for (int button = 0; button < KEYBOARD_COUNT; button += 1) {
        if (IsKeyReleased(button)) {
            InputListenResult result = {
                .value = (InputAction){
                    .type = INPUT_TYPE_KEYBOARD,
                    .id = button,
                },
                .ok = true,
            };
            return result;
        }
    }
    for (int button = 0; button < MOUSE_COUNT; button += 1) {
        if (IsMouseButtonReleased(button)) {
            InputListenResult result = {
                .value = (InputAction){
                    .type = INPUT_TYPE_MOUSE,
                    .id = button,
                },
                .ok = true,
            };
            return result;
        }
    }
    for (int device = 0; device < DEVICE_COUNT; device += 1) {
        if (!IsGamepadAvailable(device)) { continue;}
        for (int button = 0; button < GAMEPAD_BUTTON_COUNT; button += 1) {
            if (IsGamepadButtonReleased(device, button)) {
                InputListenResult result = {
                    .value = (InputAction){
                        .type = INPUT_TYPE_GAMEPAD,
                        .id = button,
                        .device = device,
                    },
                    .ok = true,
                };
                return result;
            }
        }
        for (int axis = 0; axis < AXIS_COUNT; axis += 1) {
            char axis_index = GetAxisIndex(device, axis);
            if (axis_state[axis_index].state != INPUT_BUTTON_STATE_RELEASED) { continue; }
            char value_sign = axis_state[axis_index].sign;
            if (value_sign > 0) {
                InputListenResult result = {
                    .value = (InputAction){
                        .type = INPUT_TYPE_AXIS,
                        .device = device,
                        .id = axis,
                        .sign = 1,
                        .dead_zone = DEAD_ZONE,
                    },
                    .ok = true,
                };
                return result;
            }
            else {
                InputListenResult result = {
                    .value = (InputAction){
                        .type = INPUT_TYPE_AXIS,
                        .device = device,
                        .id = axis,
                        .sign = -1,
                        .dead_zone = DEAD_ZONE,
                    },
                    .ok = true,
                };
                return result;
            }
        }
    }
    return (InputListenResult){
        .ok = false,
    };
}

bool InputIsActionPressed(InputAction input) {
    switch(input.type) {
        case INPUT_TYPE_KEYBOARD:
            return IsKeyPressed(input.id);
        case INPUT_TYPE_MOUSE:
            return IsMouseButtonPressed(input.id);
        case INPUT_TYPE_GAMEPAD:
            return IsGamepadButtonPressed(input.device, input.id);
        case INPUT_TYPE_AXIS:
            char axis_index = GetAxisIndex(input.device, input.id);
            if (axis_state[axis_index].state != INPUT_BUTTON_STATE_PRESSED) { return false; }
            float value = axis_state[axis_index].value;
            char value_sign = CalculateAxisSign(value, input.dead_zone);
            return value_sign == input.sign;
        default:
            assert(false);
    }
}

bool InputIsActionReleased(InputAction input) {
    switch(input.type) {
        case INPUT_TYPE_KEYBOARD:
            return IsKeyReleased(input.id);
        case INPUT_TYPE_MOUSE:
            return IsMouseButtonReleased(input.id);
        case INPUT_TYPE_GAMEPAD:
            return IsGamepadButtonReleased(input.device, input.id);
        case INPUT_TYPE_AXIS:
            char axis_index = GetAxisIndex(input.device, input.id);
            if (axis_state[axis_index].state != INPUT_BUTTON_STATE_RELEASED) { return false; }
            float value = axis_state[axis_index].value;
            return axis_state[axis_index].sign == input.sign;
        default:
            assert(false);
    }
}

bool InputIsActionDown(InputAction input) {
    switch(input.type) {
        case INPUT_TYPE_KEYBOARD:
            return IsKeyDown(input.id);
        case INPUT_TYPE_MOUSE:
            return IsMouseButtonDown(input.id);
        case INPUT_TYPE_GAMEPAD:
            return IsGamepadButtonDown(input.device, input.id);
        case INPUT_TYPE_AXIS:
            char axis_index = GetAxisIndex(input.device, input.id);
            if (axis_state[axis_index].state != INPUT_BUTTON_STATE_RELEASED) { return false; }
            float value = axis_state[axis_index].value;
            char value_sign = CalculateAxisSign(value, input.dead_zone);
            return value_sign == input.sign;
        default:
            assert(false);
    }
}