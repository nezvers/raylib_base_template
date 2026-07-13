#include "../app_state/app_state.h"
#include "../screen_state/screen_state.h"

// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
                               /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_state_platformer = {Enter, Exit, Update, Draw, NULL, "Platformer"};


// #include "raylib.h"

static unsigned int score;

// TODO: temporary declarations
void LevelLoad_1();
void LevelDestroy();
void LevelUpdate();
void LevelDraw();

static void Enter(){
    ScreenState *screen_state = ScreenStateGet();
    screen_state->clear_color = DARKGRAY;
    
    LevelLoad_1();
}

static void Exit(){
    LevelDestroy();
}

static void Update(){
    if (IsKeyPressed(KEY_R)){
        // Restart
        Enter();
    }
    LevelUpdate();

    // ESC quits. main.c disabled raylib's default ESC=quit (SetExitKey(KEY_NULL))
    // so each state owns ESC; request a clean shutdown via the app-state flag.
    if (IsKeyPressed(KEY_ESCAPE)) AppStateRequestQuit();
}

static void Draw(){
    // ScreenState *screen_state = ScreenStateGet();
    // Vector2 target_size = ScreenStateTargetSize();
    LevelDraw();
}

