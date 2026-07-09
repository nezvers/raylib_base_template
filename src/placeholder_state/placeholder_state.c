
#include "raylib.h"
#include "../app_state/app_state.h"
#include "../screen_state/screen_state.h"


// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
                               /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_state_placeholder = {Enter, Exit, Update, Draw, NULL, "Placeholder"};

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
    DrawText("raylib", 30, 30, 40, BLACK);
}