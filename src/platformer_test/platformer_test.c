#include "../app_state/app_state.h"
#include "../screen_state/screen_state.h"
#include "../scene_anim/scene_anim.h"   // AnimText / AnimPhase / SceneAnim intro player

// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
                               /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_state_platformer = {Enter, Exit, Update, Draw, NULL, "Platformer"};


// #include "raylib.h"

static unsigned int score;

// ============================================================================
//  INTRO TEXT: "GAME ON" appears then disappears (see scene_anim.h, and
//  main_menu.c for the full integration example). One AnimText, played as an
//  ANIM_INTRO on Enter. Its intro table both fades IN then fades OUT, so the
//  text shows and then vanishes on the same intro clock (TextAlpha multiplies
//  in * (1-out), see scene_anim.c). No boxes/art/global beats needed.
// ============================================================================
static const AnimPhase introTextPhases[] = {
    { TP_FADE_IN,  0.30f, 1.10f, sineEaseOutf },   // fade up
    { TP_FADE_OUT, 2.00f, 2.80f, sineEaseInf  },   // then fade away
};
static AnimText introTexts[] = {
    { "GAME ON", 0.15f, {0.5f, 0.42f}, RAYWHITE, introTextPhases, 2,  NULL, 0 },
};
static const SceneAnim introAnim = {
    .texts = introTexts, .textCount = 1,
    // no global/shape beats, no zoom boxes, no decor art
};
static SceneAnimPlayer introPlayer;

// TODO: temporary declarations
void LevelLoad_1();
void LevelDestroy();
void LevelUpdate();
void LevelDraw();

static void Enter(){
    ScreenState *screen_state = ScreenStateGet();
    screen_state->clear_color = DARKGRAY;

    LevelLoad_1();

    // Kick off the "GAME ON" intro (appears, then fades out).
    SceneAnimStart(&introPlayer, &introAnim, ANIM_INTRO);
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

    SceneAnimUpdate(&introPlayer, GetFrameTime());   // advance intro text (no-op once done)

    // ESC quits. main.c disabled raylib's default ESC=quit (SetExitKey(KEY_NULL))
    // so each state owns ESC; request a clean shutdown via the app-state flag.
    if (IsKeyPressed(KEY_ESCAPE)) AppStateRequestQuit();
}

static void Draw(){
    // ScreenState *screen_state = ScreenStateGet();
    // Vector2 target_size = ScreenStateTargetSize();
    LevelDraw();

    // "GAME ON" intro text, drawn on top of the level (game space).
    SceneAnimDrawTexts(&introPlayer);
}

