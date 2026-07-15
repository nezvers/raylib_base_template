#include "../app_state/app_state.h"
#include "../screen_state/screen_state.h"
#include "../settings_state/settings_state.h"
#include "../audio_state/audio_state.h"
#include "../scene_anim/scene_anim.h"   // AnimText / AnimPhase / SceneAnim intro player
#include <math.h>

// raygui implementation is compiled ONCE in main_menu.c - plain include here.
#include "raygui.h"

// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
static void Gui();
                               /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_state_platformer = {Enter, Exit, Update, Draw, Gui, "Platformer"};


// #include "raylib.h"

static unsigned int score;

// ============================================================================
//  INTRO TEXT: "GAME ON" appears then disappears (see scene_anim.h, and
//  main_menu.c for the full integration example). One AnimText, played as an
//  ANIM_INTRO on Enter. Its intro table both fades IN then fades OUT, so the
//  text shows and then vanishes on the same intro clock (TextAlpha multiplies
//  in * (1-out), see scene_anim.c). No boxes/art/global beats needed.
// ============================================================================
typedef struct {
    AnimPhase introTextPhases[2];
    AnimPhase introGlobalPhases[1];
    AnimText introTexts[1];
    SceneAnim introAnim;
    SceneAnimPlayer player;
} IntroAnim_t;
static IntroAnim_t intro_animation;

// ============================================================================
//  PAUSE MENU. ESC toggles it (the state machine has no stacking, so pause is
//  an in-state flag - same idea as main_menu's two-page menuPage). While
//  paused the level stops updating but keeps drawing (frozen frame) under a
//  dimming overlay; the same pausePlayer plays the texts IN on pause and OUT
//  on resume (gameplay resumes immediately, the outro runs over live play).
// ============================================================================
typedef struct {
    AnimPhase pauseTitleIntro[1];
    AnimPhase pauseSubIntro[1];
    AnimPhase pauseTitleOutro[1];
    AnimPhase pauseSubOutro[1];
    AnimText pauseTexts[2];
    SceneAnim pauseAnim;
    SceneAnimPlayer player;
} PauseAnim_t;
static PauseAnim_t pause_animation;

static void InitAnimations() {
    // Intro
    {
        intro_animation.introTextPhases[0] = (AnimPhase){ TP_FADE_IN,  0.30f, 1.10f, sineEaseOutf };   // fade up
        intro_animation.introTextPhases[1] = (AnimPhase){ TP_FADE_OUT, 2.00f, 2.80f, sineEaseInf };    // then fade away

        intro_animation.introGlobalPhases[0] = (AnimPhase){ GP_UNFADE_BLACK, 0.00f, 0.60f, sineEaseOutf };  // black -> level reveal
        intro_animation.introTexts[0] = (AnimText){ "GAME ON", 0.15f, {0.5f, 0.42f}, RAYWHITE, introTextPhases, 2,  NULL, 0 };

        intro_animation.introAnim = (SceneAnim){
            .texts = intro_animation.introTexts,
            .textCount = 1,
            .introGlobal = intro_animation.introGlobalPhases,
            .introGlobalCount = 1,
            // no shape beats, no zoom boxes, no decor art
        };
    }

    // Pause
    {
        pause_animation.pauseTitleIntro[0] = (AnimPhase){ TP_SLIDE_IN, 0.00f, 0.50f, sineEaseOutf };   // title first
        pause_animation.pauseSubIntro[0] = (AnimPhase){ TP_SLIDE_IN, 0.20f, 0.70f, sineEaseOutf };     // subtitle trails behind
        pause_animation.pauseTitleOutro[0] = (AnimPhase){ TP_SLIDE_OUT, 0.10f, 0.55f, sineEaseInf };   // reverse stagger: sub leaves
        pause_animation.pauseSubOutro[0] = (AnimPhase){ TP_SLIDE_OUT, 0.00f, 0.45f, sineEaseInf };     //   first, title follows

        pause_animation.pauseTexts[0] = (AnimText){ "PAUSE MENU",              0.11f, {0.5f, 0.08f}, RAYWHITE,
                                                    pause_animation.pauseTitleIntro, 1, pause_animation.pauseTitleOutro, 1 };
        pause_animation.pauseTexts[1] = (AnimText){ "still, all the buttons!", 0.056f, {0.5f, 0.21f}, LIGHTGRAY,
                                                    pause_animation.pauseSubIntro,   1, pause_animation.pauseSubOutro,   1 };
        pause_animation.pauseAnim = (SceneAnim){.texts = pause_animation.pauseTexts, .textCount = 2};
    }
}

// Pause GUI shows one of two pages, mirroring main_menu's MenuPage.
typedef enum { PAUSE_PAGE_MAIN = 0, PAUSE_PAGE_OPTIONS } PausePage;
static PausePage pausePage = PAUSE_PAGE_MAIN;
static bool  paused   = false;
static float pauseDim = 0.0f;   // 0..1 dark-overlay strength (eased toward paused)

// TODO: temporary declarations
typedef void RestartFcn(void);
void LevelSetRestartCallback(RestartFcn *callback);
void LevelLoad_1();
void LevelDestroy();
void LevelUpdate();
void LevelDraw();

static void PauseOpen(){
    paused    = true;
    pausePage = PAUSE_PAGE_MAIN;
    SceneAnimStart(&pause_animation.player, &pause_animation.pauseAnim, ANIM_INTRO);
}

static void PauseClose(){
    paused = false;
    SceneAnimStart(&pause_animation.player, &pause_animation.pauseAnim, ANIM_OUTRO);   // texts slide back out
}

static void Enter(){
    ScreenState *screen_state = ScreenStateGet();
    screen_state->clear_color = DARKGRAY;

    LevelLoad_1();
    LevelSetRestartCallback(Enter);

    // Kick off the "GAME ON" intro (appears, then fades out).
    InitAnimations();
    SceneAnimStart(&intro_animation.player, &intro_animation.introAnim, ANIM_INTRO);

    // Fresh pause state (Enter is also the KEY_R restart).
    paused    = false;
    pausePage = PAUSE_PAGE_MAIN;
    pauseDim  = 0.0f;
    pause_animation.player.spec = NULL;   // nothing to play/draw until the first ESC
}

static void Exit(){
    LevelDestroy();
}

static void Update(){
    // ESC: options page -> back to pause main page; otherwise toggle pause.
    // main.c disabled raylib's default ESC=quit (SetExitKey(KEY_NULL)) so each
    // state owns ESC; here it never quits - QUIT lives in the pause menu.
    if (IsKeyPressed(KEY_ESCAPE)){
        if (paused && pausePage == PAUSE_PAGE_OPTIONS){
            AudioPlayButton();
            pausePage = PAUSE_PAGE_MAIN;
        }
        else if (paused) PauseClose();
        else             PauseOpen();
    }

    if (!paused){
        if (IsKeyPressed(KEY_R)){
            // Restart
            Enter();
        }
        LevelUpdate();
        SceneAnimUpdate(&intro_animation.player, GetFrameTime());   // advance intro text (no-op once done)
    }

    // Pause texts animate on their own clock: intro while paused, outro over
    // live gameplay right after resume (no-op once finished / never started).
    if (pause_animation.player.spec) SceneAnimUpdate(&pause_animation.player, GetFrameTime());

    // Ease the dim overlay toward its target (1 when paused, 0 when playing).
    float dimTarget = paused ? 1.0f : 0.0f;
    pauseDim += (dimTarget - pauseDim) * fminf(1.0f, 10.0f*GetFrameTime());
}

static void Draw(){
    Vector2 game_size = ScreenStateTargetSize();
    LevelDraw();

    // "GAME ON" intro text, drawn on top of the level (game space).
    SceneAnimDrawTexts(&intro_animation.player);

    // Pause overlay: dim the frozen game, then the animated pause texts.
    if (pauseDim > 0.01f)
        DrawRectangle(0, 0, (int)game_size.x, (int)game_size.y,
                      Fade(BLACK, 0.55f*pauseDim));
    if (pause_animation.player.spec) SceneAnimDrawTexts(&pause_animation.player);

    // Entry fade-in: GP_UNFADE_BLACK eases 0->1, so the remaining blackness is
    // 1-amount (missing row would read 1 = no overlay). Drawn last, over all.
    float black = 1.0f - SceneAnimGlobalAmount(&intro_animation.player, GP_UNFADE_BLACK);
    if (black > 0.001f)
        DrawRectangle(0, 0, (int)game_size.x, (int)game_size.y,
                      Fade(BLACK, black));
}

// ----------------------------------------------------------------------------
//  Gui: SCREEN SPACE pause menu, only while paused. Same anchored right-hand
//  column and DESIRED vs EFFECTIVE scale logic as main_menu.c (see the long
//  comments there); MAIN page = RESUME/OPTIONS/QUIT, OPTIONS page = the same
//  Settings-bound widgets (they all write into the Settings singleton, so
//  nothing here duplicates state with the main menu).
// ----------------------------------------------------------------------------
static void Gui()
{
    if (!paused) return;

    ScreenState *ss = ScreenStateGet();
    Rectangle vp = ss->dest_rect;   // game region in REAL screen pixels

    Settings *settings = SettingsGet();
    const float scales[3] = { 1.0f, 2.0f, 3.0f };
    int desired = (int)scales[settings->gui_scale_wish];   // 1, 2 or 3

    const float game_top_margin = 120.0f;  // clears the pause title when contained
    const float screen_margin   = 20.0f;
    // Column height at s=1 (see main_menu.c for the calc rules):
    //   MAIN:    resume48 +options48 +quit36 = 132.
    //   OPTIONS: back48 +vollbl22 +slider32 +modelbl20 +modetog48 +persist32
    //            +difftog48 +difflbl32 +scalelbl20 +scaletog36 = 338.
    const float LAYOUT_UNITS = (pausePage == PAUSE_PAGE_MAIN) ? 132.0f : 338.0f;

    float contained_top = vp.y + game_top_margin;
    float fit_contained = vp.height - game_top_margin - screen_margin;
    float fit_expanded  = ss->height - 2.0f*screen_margin;
    int effective = desired;
    while (effective > 1 &&
           LAYOUT_UNITS * effective > fit_contained &&
           LAYOUT_UNITS * effective > fit_expanded) effective--;

    float col_h = LAYOUT_UNITS * effective;
    float col_top = (col_h <= fit_contained)
                        ? contained_top
                        : (ss->height - col_h) * 0.5f;

    float s = (float)effective;
    settings->gui_scale = s;

    int baseSize = GuiGetFont().baseSize;
    GuiSetStyle(DEFAULT, TEXT_SIZE, baseSize * effective);
    GuiSetIconScale(effective);

    float w = 220.0f * s;
    float h = 36.0f  * s;
    float gap = 12.0f * s;
    float rh = 20.0f * s;
    float x = vp.x + vp.width - w - 40.0f;
    float y = col_top;

    if (pausePage == PAUSE_PAGE_MAIN)
    {
        if (GuiButton((Rectangle){ x, y, w, h }, "RESUME"))
        {
            AudioPlayButton();
            PauseClose();
        }
        y += h + gap;

        if (GuiButton((Rectangle){ x, y, w, h }, "OPTIONS"))
        {
            AudioPlayButton();
            pausePage = PAUSE_PAGE_OPTIONS;
        }
        y += h + gap;

        if (GuiButton((Rectangle){ x, y, w, h }, "QUIT (-> main menu)"))
        {
            AudioPlayButton();
            AppStateTransition(&app_state_main_menu);
        }

        DrawText("Press ESC to resume", 20, 20, 20, RAYWHITE);
    }
    else // PAUSE_PAGE_OPTIONS - same widgets as main_menu's OPTIONS page
    {
        if (GuiButton((Rectangle){ x, y, w, h }, "< BACK"))
        {
            AudioPlayButton();
            pausePage = PAUSE_PAGE_MAIN;
        }
        y += h + gap;

        GuiLabel((Rectangle){ x, y, w, rh }, TextFormat("Volume: %.0f%%", settings->music_volume*100.0f));
        y += rh + 2.0f*s;

        float prevVol = settings->music_volume;
        GuiSlider((Rectangle){ x + 10.0f*s, y, w - 20.0f*s, rh }, "0", "100", &settings->music_volume, 0.0f, 1.0f);
        SettingsApplyVolume();
        if (settings->music_volume != prevVol) AudioPlayVolumePreview();
        y += rh + gap;

        GuiLabel((Rectangle){ x, y, w, rh }, "Window Mode:");
        y += rh;
        int prevMode = settings->window_mode;
        int mode = prevMode;
        GuiToggleGroup((Rectangle){ x, y, (w - gap*2.0f)/3.0f, h },
                       "Windowed;Fullscreen;Borderless", &mode);
        if (mode != prevMode) {
            AudioPlayButton();
            SettingsApplyWindowMode(mode);
        }
        y += h + gap;

        if (GuiCheckBox((Rectangle){ x, y, rh, rh }, "Persist settings on quit", &settings->persist))
            AudioPlayButton();
        y += rh + gap;

        int prevDiff = settings->difficulty;
        GuiToggleGroup((Rectangle){ x, y, (w - gap*2.0f)/3.0f, h }, "Easy;Normal;Hard", &settings->difficulty);
        if (settings->difficulty != prevDiff) AudioPlayButton();
        y += h + gap;

        GuiLabel((Rectangle){ x, y, w, rh }, TextFormat("Difficulty index: %i", settings->difficulty));
        y += rh + gap;

        const char *names[3] = { "Small", "Medium", "Large" };
        GuiLabel((Rectangle){ x, y, w, rh },
                 effective == desired ? "GUI Scale:"
                                      : TextFormat("GUI Scale: (%s fits)", names[effective - 1]));
        y += rh;
        int prevScaleWish = settings->gui_scale_wish;
        GuiToggleGroup((Rectangle){ x, y, (w - gap*2.0f)/3.0f, h }, "Small;Medium;Large", &settings->gui_scale_wish);
        if (settings->gui_scale_wish != prevScaleWish) AudioPlayButton();

        DrawText("Press ESC or click BACK to return", 20, 20, 20, RAYWHITE);
    }
}
