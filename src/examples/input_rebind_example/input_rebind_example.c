#include "raylib.h"
#include "raygui.h"
#include "../app_state/app_state.h"
#include "../screen_state/screen_state.h"
#include <stddef.h> // used for NULL

#include "../input/input.h"


// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
static void Gui();
                               /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_input_example = {Enter, Exit, Update, Draw, Gui, "SFX Example"};


// Represents game inputs
InputAction input_list[] = {
    {.type = INPUT_TYPE_KEYBOARD, .id = KEY_W,     .name = "Up"},
    {.type = INPUT_TYPE_KEYBOARD, .id = KEY_S,     .name = "Down"},
    {.type = INPUT_TYPE_KEYBOARD, .id = KEY_A,     .name = "Left"},
    {.type = INPUT_TYPE_KEYBOARD, .id = KEY_D,     .name = "Right"},
    {.type = INPUT_TYPE_KEYBOARD, .id = KEY_SPACE, .name = "Jump"},
};

InputAction *selected_rebind = NULL;
#define INPUT_COUNT (sizeof(input_list)/ sizeof(input_list[0]))


// Timers for visualized inputs
typedef struct {
    float pressed;
    float down;
    float released;
} InputTimer;

InputTimer timers[INPUT_COUNT];

// Update timers for visualization
static void UpdateTimers(InputAction *input, InputTimer *timer, float delta_time);

static void Enter(){
    
}

static void Exit(){
    
}

static void Update(){
    // To update pressed/ released/ down for analog inputs
    InputUpdateAxis();

    float delta_time = GetFrameTime();
    for (int i = 0; i < INPUT_COUNT; i += 1) {
        UpdateTimers(&input_list[i], &timers[i], delta_time);
    }

    if (selected_rebind == NULL) { return; }
    InputListenResult result = InputListenRebind();
    if (!result.ok) { return; }

    const char *name = selected_rebind->name;
    *selected_rebind = result.value;
    selected_rebind->name = name;
    selected_rebind = NULL;
}

static void Draw(){
    ScreenState *screen_state = ScreenStateGet();
    Vector2 target_size = ScreenStateTargetSize();
    
    DrawText("Name", 10, 10, 20, BLACK);
    DrawText("P", 200, 10, 20, BLACK);
    DrawText("D", 225, 10, 20, BLACK);
    DrawText("R", 250, 10, 20, BLACK);

    if (selected_rebind != NULL) {
        DrawText(selected_rebind->name, 300, 10, 20, BLACK);
    }

    const char ROW_HEIGHT = 25;
    for (int i = 0; i < INPUT_COUNT; i += 1) {
        InputAction *input = &input_list[i];
        DrawText(input->name, 10, 35 + ROW_HEIGHT * i, 20, BLACK);
        DrawRectangleRec((Rectangle){200, 35 + ROW_HEIGHT * i, 20, 20}, ColorAlpha(LIME, timers[i].pressed));
        DrawRectangleRec((Rectangle){225, 35 + ROW_HEIGHT * i, 20, 20}, ColorAlpha(LIME, timers[i].down));
        DrawRectangleRec((Rectangle){250, 35 + ROW_HEIGHT * i, 20, 20}, ColorAlpha(LIME, timers[i].released));

        // SELECT
        const char *text = (selected_rebind == input) ? "Waiting" : "Rebind";
        if (GuiButton((Rectangle){275, 35 + ROW_HEIGHT * i, 50, 20}, text)) {
            if (selected_rebind == NULL) {
                selected_rebind = input;
            }
        }
    }
}

static void Gui() {
}

static void UpdateTimers(InputAction *input, InputTimer *timer, float delta_time) {
    timer->pressed -= delta_time * 4;
    if (timer->pressed < 0) {
        timer->pressed = 0;
    }
    
    timer->released -= delta_time * 4;
    if (timer->released < 0) {
        timer->released = 0;
    }
    
    timer->down -= delta_time * 4;
    if (timer->down < 0) {
        timer->down = 0;
    }

    if (InputIsActionPressed(*input)){
        timer->pressed = 1;
    }

    if (InputIsActionReleased(*input)){
        timer->released = 1;
    }

    if (InputIsActionDown(*input)){
        timer->down = 1;
    }
}