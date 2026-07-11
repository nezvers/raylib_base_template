#include "raylib.h"
#include "../app_state/app_state.h"
#include "../screen_state/screen_state.h"
#include <stddef.h>

// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
static void Gui();
                               /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_mouse_coordinates = {Enter, Exit, Update, Draw, Gui, "Placeholder"};

static void Enter(){

}

static void Exit(){

}

static void Update(){

}

static void Draw(){
    ScreenState *screen_state = ScreenStateGet();
    Vector2 target_size = ScreenStateTargetSize();
    DrawRectangle(0, 0, target_size.x, target_size.y, BLACK);
    DrawRectangle(10, 10, target_size.x -20, target_size.y -20, RAYWHITE);
    // DrawText("raylib", 30, 30, 40, BLACK);

    // Draw() is rendered on ScreenState.target in game resolution, then scaled for the screen
    Vector2 mouse_pos = Screen2Target(GetMousePosition());
    DrawCircleV(mouse_pos, 5, LIME);
}

static void Gui() {

    DrawCircleLinesV(GetMousePosition(), 10, LIME);
}