#ifndef INPUT_REBIND_H
#define INPUT_REBIND_H

#include <stdbool.h>

typedef enum {
    INPUT_TYPE_NONE,
    INPUT_TYPE_KEYBOARD,
    INPUT_TYPE_MOUSE,
    INPUT_TYPE_GAMEPAD,
    INPUT_TYPE_AXIS,
} InputType;

typedef struct {
    InputType     type;
    unsigned      id;        // button index for lookup
    float         dead_zone; // for gamepad axis
    unsigned char device;    // for gamepads
    char          sign;      // for gamepad axis
    const char   *name;
} InputAction;

typedef enum {
    INPUT_BUTTON_STATE_NONE,
    INPUT_BUTTON_STATE_PRESSED,
    INPUT_BUTTON_STATE_RELEASED,
    INPUT_BUTTON_STATE_HELD,
} InputButtonState;

typedef struct {
    InputAction value;
    bool ok;
} InputListenResult;

// To use gamepad axis as buttons, this needs to be called
void InputUpdateAxis();

// Read axis state as button if UpdateAxis has been called at update beginning
InputButtonState InputAxisState(char device, char axis);

// Read float value from inputs
float InputGetValue(InputAction input);

// Read float value for directions -1 to 1
char InputGetValueAxis(InputAction negative, InputAction positive);

// Scans all inputs. First one released is reported.
InputListenResult InputListenRebind();

bool InputIsActionPressed(InputAction input);
bool InputIsActionReleased(InputAction input);
bool InputIsActionDown(InputAction input);

#endif // INPUT_REBIND_H
